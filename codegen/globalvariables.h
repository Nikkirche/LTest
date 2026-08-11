#pragma once
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

#include <string>

#include "llvm/IR/Module.h"

namespace ltest {

inline bool IsLtestGlobal(const llvm::GlobalValue& value) {
  if (value.getName().starts_with("llvm.")) {
    return true;
  }
  auto demangled = llvm::demangle(value.getName().str());
  return demangled.rfind("ltest::", 0) == 0 ||
         demangled.find(" ltest::") != std::string::npos;
}

inline bool IsCxxAbiFunctionLocalStatic(const llvm::GlobalValue& value) {
  auto name = value.getName();
  // Itanium C++ ABI: _ZZ is function-local static storage, _ZGV is its guard.
  // Resetting those would replay singleton initialization without modeling its
  // process-lifetime side effects.
  return name.starts_with("_ZZ") || name.starts_with("_ZGV");
}

struct GlobalReplacer {
  GlobalReplacer(llvm::Module& M) : M(M) {}
  void Run() {
    llvm::SmallVector<llvm::Constant*> entries;
    auto pointer = llvm::PointerType::get(M.getContext(), 0);
    // first we init with init values, and only after it we call constructors
    for (auto& g : M.globals()) {
      bool ext = g.isExternallyInitialized();
      if (ext || g.isConstant() || g.hasAppendingLinkage() ||
          !g.hasInitializer() || IsLtestGlobal(g) ||
          IsCxxAbiFunctionLocalStatic(g)) {
        continue;
      }
      auto init = g.getInitializer();
      auto f = llvm::Function::Create(
          llvm::FunctionType::get(llvm::Type::getVoidTy(M.getContext()), {},
                                  false),
          llvm::GlobalValue::InternalLinkage, "", M);
      f->addFnAttr(llvm::Attribute::NoInline);
      auto bb = llvm::BasicBlock::Create(M.getContext(), "", f);
      llvm::IRBuilder<> builder(M.getContext());
      builder.SetInsertPoint(bb);
      builder.CreateStore(init, &g);
      builder.CreateRetVoid();
      entries.push_back(llvm::ConstantExpr::getPointerCast(f, pointer));
    }
    for (auto& f : M) {
      if (!f.isDeclaration() && f.getSection() == "text.start" &&
          !IsLtestGlobal(f)) {
        entries.push_back(llvm::ConstantExpr::getPointerCast(&f, pointer));
      }
    }
    auto type = llvm::ArrayType::get(pointer, entries.size());
    auto gv = new llvm::GlobalVariable(
        M, type, true, llvm::GlobalValue::InternalLinkage,
        llvm::ConstantArray::get(type, entries), "ltest_init_array");
    gv->setSection("ltest_init");
    appendToCompilerUsed(M, {gv});
  }
  llvm::Module& M;
};

struct GlobalVarsPass final : public llvm::PassInfoMixin<GlobalVarsPass> {
  llvm::PreservedAnalyses run(llvm::Module& M,
                              llvm::ModuleAnalysisManager& AM) {
    GlobalReplacer replacer(M);
    replacer.Run();
    if (verifyModule(M, &llvm::errs())) {
      llvm::report_fatal_error("module verification failed", false);
    }
    return llvm::PreservedAnalyses::none();
  };
};

}  // namespace ltest
