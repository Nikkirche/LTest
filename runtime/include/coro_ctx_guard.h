#pragma once

namespace ltest {

extern bool ltest_coro_ctx;

struct CoroCtxGuard {
  CoroCtxGuard();
  ~CoroCtxGuard();
};

struct SchedCtxGuard {
  bool tmp;
  SchedCtxGuard();
  ~SchedCtxGuard();
};

}  // namespace ltest
