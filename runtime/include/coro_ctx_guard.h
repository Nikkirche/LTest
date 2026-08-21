#pragma once

namespace ltest {

extern bool ltest_coro_ctx;
extern bool ltest_target_construction;

struct CoroCtxGuard {
  CoroCtxGuard();
  ~CoroCtxGuard();
};

struct SchedCtxGuard {
  bool tmp;
  SchedCtxGuard();
  ~SchedCtxGuard();
};

struct TargetConstructionGuard {
  bool previous;
  TargetConstructionGuard();
  ~TargetConstructionGuard();
};

}  // namespace ltest
