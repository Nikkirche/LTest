#include "coro_ctx_guard.h"

/// True in coroutine contexts
/// required for
/// 1. incapsulating CoroYield calls, allowing
/// to call methods annotated with non_atomic in scheduler fiber
/// 2. incapsulating syscall hook

namespace ltest {

bool ltest_coro_ctx = false;

CoroCtxGuard::CoroCtxGuard() { ltest_coro_ctx = true; }

CoroCtxGuard::~CoroCtxGuard() { ltest_coro_ctx = false; }

SchedCtxGuard::SchedCtxGuard() {
  tmp = ltest_coro_ctx;
  ltest_coro_ctx = false;
}

SchedCtxGuard::~SchedCtxGuard() { ltest_coro_ctx = tmp; }

}  // namespace ltest
