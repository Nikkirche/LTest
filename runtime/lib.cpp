#include "include/lib.h"

#include <atomic>
#include <cassert>
#include <string>
#include <utility>
#include <vector>

#include "coro_ctx_guard.h"
#include "logger.h"
#include "value_wrapper.h"

// See comments in the lib.h.
Task this_coro{};
int this_thread_id = -1;

boost::context::fiber_context sched_ctx;
std::optional<CoroutineStatus> coroutine_status;
bool ltest_initialized = false;

namespace {
std::atomic<std::uint64_t> g_dual_event_seqno{0};
}  // namespace

namespace ltest {
std::vector<TaskBuilder> task_builders{};
}

// Test failure tracking for litmus tests, which could expectedly fail.
namespace ltest {
namespace {
bool test_failed{false};
std::string test_failure_message{};
}  // namespace

void SetTestFailure(std::string message) {
  test_failure_message = std::move(message);
  test_failed = true;
}

bool HasTestFailure() { return test_failed; }

const std::string& GetTestFailureMessage() { return test_failure_message; }

void ClearTestFailure() {
  test_failed = false;
  test_failure_message.clear();
}
}  // namespace ltest

Task CoroBase::GetPtr() { return shared_from_this(); }

void CoroBase::EmitDualEvent(DualEventKind kind, ValueWrapper result) {
  pending_dual_events_.push_back(
      DualEvent{kind, g_dual_event_seqno.fetch_add(1, std::memory_order_relaxed),
                std::move(result)});
}

std::vector<CoroBase::DualEvent> CoroBase::DrainDualEvents() {
  std::vector<DualEvent> out;
  out.swap(pending_dual_events_);
  return out;
}

void CoroBase::Resume(size_t resumed_thread_id) {
  this_coro = this->GetPtr();
  this_thread_id = resumed_thread_id;
  assert(!this_coro->IsReturned() && this_coro->ctx);
  // debug(stderr, "name: %s\n",
  // std::string(this_coro->GetPtr()->GetName()).c_str());
  auto coro = this_coro.get();  // std::shared_ptr also can be interleaved
  // NOTE(kmitkin): Guard below prevents us from call CoroYield in the scheduler
  // coroutine, area that protected by it should be as small as possible to
  // reduce errors
  {
    ltest::CoroCtxGuard guard{};
    boost::context::fiber_context([coro](boost::context::fiber_context&& ctx) {
      sched_ctx = std::move(ctx);
      coro->ctx = std::move(coro->ctx).resume();
      return std::move(sched_ctx);
    }).resume();
  }
  this_coro.reset();
  this_thread_id = -1;
}

void CoroBase::setWakeupCondition(std::function<bool()> cond) {
  wakeup_condition_ = std::move(cond);
}

void CoroBase::clearWakeupCondition() { wakeup_condition_ = nullptr; }

bool CoroBase::hasWakeupCondition() const {
  return static_cast<bool>(wakeup_condition_);
}

bool CoroBase::checkWakeupCondition() const {
  return wakeup_condition_ ? wakeup_condition_() : false;
}

int CoroBase::GetId() const { return id; }

ValueWrapper CoroBase::GetRetVal() const {
  assert(IsReturned());
  return ret;
}

CoroBase::~CoroBase() {
  // The coroutine must be returned if we want to restart it.
  // We can't just Terminate() it because it is the runtime responsibility to
  // decide, in which order the tasks should be terminated.
  assert(IsReturned() && "Task not returned at destruction");
}

std::string_view CoroBase::GetName() const { return name; }

bool CoroBase::IsReturned() const { return finish_kind_ != FinishKind::Running; }

CoroBase::FinishKind CoroBase::GetFinishKind() const {
  return finish_kind_;
}

bool CoroBase::FinishedNormally() const {
  return finish_kind_ == FinishKind::ReturnedNormally;
}

void CoroBase::MarkFinishedNormally() {
  finish_kind_ = FinishKind::ReturnedNormally;
}

void CoroBase::MarkFinishedNormallyIfRunning() {
  if (!IsReturned()) {
    MarkFinishedNormally();
  }
}

extern "C" void CoroYield() {
  if (!ltest_coro_ctx) [[unlikely]] {
    return;
  }
  assert(this_coro && sched_ctx);
  boost::context::fiber_context([](boost::context::fiber_context&& ctx) {
    this_coro->ctx = std::move(ctx);
    return std::move(sched_ctx);
  }).resume();
}

extern "C" void CoroutineStatusChange(char* name, bool start) {
  // assert(!coroutine_status.has_value());
  coroutine_status.emplace(name, start);
  CoroYield();
}

void CoroBase::DestroyContext() {
  if (ctx) {
    ctx = boost::context::fiber_context{};
  }
}

void CoroBase::Terminate() {
  finish_kind_ = FinishKind::ReturnedNormally;
  fstate = {};
  clearWakeupCondition();
  DestroyContext();
}

void CoroBase::TerminateWith(const ValueWrapper& value) {
  Terminate();
  ret = value;
}
