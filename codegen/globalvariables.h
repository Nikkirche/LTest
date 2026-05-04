#pragma once
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

#include "llvm/IR/Module.h"

struct GlobalReplacer {
  GlobalReplacer(llvm::Module& M) : M(M) {}
  void Run() {
    llvm::SmallVector<llvm::Constant*> entries;
    auto pointer = llvm::PointerType::get(M.getContext(), 0);
    // first we init with init values, and only after it we call constructors
    for (auto& g : M.globals()) {
      bool ext = g.isExternallyInitialized();
      if (ext || g.isConstant()) {
        continue;
      }
      auto init = g.getInitializer();
      if (!init) {
        continue;
      }
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
      if (!f.isDeclaration() && f.getSection() == "text.start") {
        entries.push_back(llvm::ConstantExpr::getPointerCast(&f, pointer));
      }
    }
    auto type = llvm::ArrayType::get(pointer, entries.size());
    auto gv = new llvm::GlobalVariable(
        M, type, true, llvm::GlobalValue::ExternalLinkage,
        llvm::ConstantArray::get(type, entries), "ltest_init_array");
    gv->setSection("ltest_init");
    appendToCompilerUsed(M, {gv});
    M.dump();
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
    M.dump();
    return llvm::PreservedAnalyses::none();
  };
};