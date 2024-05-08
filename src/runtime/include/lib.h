#pragma once
#include <coroutine>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "stable_vector.h"

extern "C" {
// Let's begin from C-style API to make LLVM calls easier.

// This structure must be equal to the clone in LLVM pass.
struct CoroPromise {
  using Handle = std::coroutine_handle<CoroPromise>;

  int has_ret_val{};
  int ret_val{};
  int suspension_points;
  Handle child_hdl{};
};

struct CoroPromise;
using Handle = std::coroutine_handle<CoroPromise>;

// Clones the existing task. Cloners are created with macro.
// TODO: suspention points
typedef Handle (*TaskCloner)(void* this_ptr, void* args_ptr);

// Task describes coroutine to run.
// Entrypoint receives list of task builders as input.

// Task describes a coroutine to run.
struct Task {
  // Task meta info.
  // Does not change in the task runtime.
  // Stored only for tasks, which where generated by launcher.
  struct Meta {
    // Name of the task.
    std::string name;
    // Pointer to the arguments, points to the std::tuple.
    std::shared_ptr<void> args;
    // Args as strings.
    std::vector<std::string> str_args;
  };

  // Constructs a non root task from handler.
  Task(Handle hdl);
  // Constructs a root task from handler, cloner.
  Task(Handle hdl, TaskCloner cloner);

  Task(const Task&) = delete;
  Task(Task&&);
  Task& operator=(Task&&);

  // Resumes task until next suspension point.
  // If task calls another coroutine during execution
  // then has_child() returns true after this call.
  //
  // Panics if has_ret_val() == true.
  void Resume();

  // Returns true if the task called another coroutine task.
  // Scheduler must check result of this function after each resume.
  bool HasChild();

  // Returns child of the task.
  //
  // Panics if has_child() == false.
  Task GetChild();

  // Must be called by scheduler after current
  // child task is terminated.
  void ClearChild();

  // Returns true if the task is terminated.
  bool IsReturned();

  // Get return value of the task.
  //
  // Panics if has_ret_val() == false.
  int GetRetVal();

  // Returns the task name.
  const std::string& GetName() const;

  // Returns the task args.
  void* GetArgs() const;

  // Returns the args as strings.
  const std::vector<std::string>& GetStrArgs() const;

  // Sets the meta information.
  void SetMeta(std::shared_ptr<Meta> meta);

  // Starts this task from the beginning.
  // Returns a task with new handle.
  Task StartFromTheBeginning(void* state);

  ~Task();

  // Returns the number of suspension points(approximation)
  // that have been placed inside this task
  // Useful to approximate the number of Resumes
  size_t GetSuspensionPoints() const;

 private:
  std::shared_ptr<Meta> meta{};
  size_t suspension_points;
  TaskCloner cloner;
  Handle hdl;
};

// Creates a task. Builders are created with macro.
typedef Task (*TaskBuilder)(void* this_ptr);

// Runtime token.
// Target method could use token generator and StackfulTask will account this.
struct Token {
  bool parked{};
  void Reset();
};

// StackfulTask is a Task wrapper which contains the stack inside, so resume
// method resumes the last subtask.
struct StackfulTask {
  explicit StackfulTask(TaskBuilder builder, void* this_state);
  explicit StackfulTask(Task);

  StackfulTask(const StackfulTask&) = delete;
  StackfulTask(StackfulTask&&);

  // Returns the root task name.
  const std::string& GetName() const;

  // Returns the root task args.
  void* GetArgs() const;

  // Return the root task args as strings.
  const std::vector<std::string>& GetStrArgs() const;

  // Sets token for this task.
  void SetToken(std::shared_ptr<Token> token);

  // Returns true is the task was parked.
  bool IsParked();

  // Start the task from the beginning.
  void StartFromTheBeginning(void*);

  // Resume method resumes the last subtask.
  virtual void Resume();

  // Haven't the first task finished yet?
  virtual bool IsReturned();

  // Returns whether this thread is waiting
  virtual bool IsSuspended() const;

  // Returns whether synchronous task
  // if so, then this task should be presented as inv_req, inv_follow_up and
  // res_req, res_follow_up in a history
  virtual bool IsBlocking() const;

  void Terminate();

  // Returns the value that was returned from the first task, have to be called
  // only when IsReturned is true
  // TODO: after a while int will be replaced with the trait
  [[nodiscard]] virtual int GetRetVal() const;

  [[nodiscard]] size_t GetSuspensionPoints() const;

  virtual ~StackfulTask();

  // TODO: snapshot method might be useful.
 protected:
  // Need this constructor for tests
  StackfulTask();

 private:
  std::shared_ptr<Token> token{};
  // Keeps all spawned tasks.
  StableVector<Task> spawned_tasks{};
  // Current stack.
  std::vector<std::reference_wrapper<Task>> stack{};
  int last_returned_value{};
};

StackfulTask* GetCurrentTask();

void SetCurrentTask(StackfulTask*);
}

struct Invoke {
  explicit Invoke(const StackfulTask& task, int thread_id);

  [[nodiscard]] const StackfulTask& GetTask() const;

  int thread_id;

 private:
  std::reference_wrapper<const StackfulTask> task;
};

struct Response {
  Response(const StackfulTask& task, int result, int thread_id);

  [[nodiscard]] const StackfulTask& GetTask() const;

  int result;
  int thread_id;

 private:
  std::reference_wrapper<const StackfulTask> task;
};

// Types for dual data structures
// Use them in the pattern matching
struct RequestInvoke : Invoke {
  RequestInvoke(const StackfulTask& task, int thread_id)
      : Invoke(task, thread_id) {}
};
struct RequestResponse : Response {
  RequestResponse(const StackfulTask& task, int result, int thread_id)
      : Response(task, result, thread_id) {}
};

struct FollowUpInvoke : Invoke {
  FollowUpInvoke(const StackfulTask& task, int thread_id)
      : Invoke(task, thread_id) {}
};
struct FollowUpResponse : Response {
  FollowUpResponse(const StackfulTask& task, int result, int thread_id)
      : Response(task, result, thread_id) {}
};

using HistoryEvent =
    std::variant<Invoke, Response, RequestInvoke, RequestResponse,
                 FollowUpInvoke, FollowUpResponse>;
