
#include <pthread.h>

#include <utility>

#include "runtime/include/blocking_primitives.h"
#include "runtime/include/coro_ctx_guard.h"
#include "runtime/include/os_simulator.h"

#define PTHREAD_MOCK_INIT_ROUTINE                                     \
  if (!ltest_initialized) {                                           \
    return 0;                                                         \
  }                                                                   \
  assert(ltest_coro_ctx && "should be called only from tested code"); \
  ltest::SchedCtxGuard guard;

extern int pthread_create(pthread_t *__restrict __newthread,
                          const pthread_attr_t *__restrict __attr,
                          void *(*__start_routine)(void *),
                          void *__restrict __arg) __THROWNL {
  assert(ltest_coro_ctx);
  assert(!thread_info.has_value());
  ltest::SchedCtxGuard guard;
  std::string name = "NewThread";
  thread_info.emplace(CreatedThreadInfo(
      [__start_routine, __arg](void *) {
        CoroYield();
        return __start_routine(__arg);
      },
      name));
  current_max_thread_id++;
  *__newthread = current_max_thread_id;
  {
    ltest::CoroCtxGuard coro_guard;
    CoroYield();
  }
  return 0;
}

int pthread_join(pthread_t __th, void **__thread_return) {
  assert(ltest_coro_ctx);
  assert(!thread_info.has_value());
  ltest::SchedCtxGuard guard;
  thread_info.emplace(JoinThreadInfo(__th, __thread_return));
  {
    ltest::CoroCtxGuard coro_guard;
    CoroYield();
  }
  return 0;
}

extern void pthread_exit(void *__retval) __attribute__((__noreturn__)) {
  assert(ltest_coro_ctx);
  ltest::SchedCtxGuard guard;
  this_coro->TerminateWith(ValueWrapper(__retval, GetDefaultCompator<void *>(),
                                        &GetThreadResultToString));
  {
    ltest::CoroCtxGuard coro_guard;
    CoroYield();
  }
  throw std::runtime_error(
      "code after CoroYield() here should never be executed, exception is "
      "thrown only to match "
      "noreturn attribute");
}

template <class... Ts>
struct Overloads : Ts... {
  using Ts::operator()...;
};

// we need to call this mutex method in almost each mutex function, because
// mutex can be initialized by macros
static decltype(mutexes)::mapped_type &InsertMutex(pthread_mutex_t *__mutex) {
  // std::cerr << "inserted" << "\n";
  auto it = mutexes.find(__mutex);
  if (it == mutexes.end()) {
    it = mutexes.emplace(__mutex, ltest::mutex()).first;
  }
  return it->second;
}

static decltype(mutexes)::mapped_type &FindMutex(pthread_mutex_t *__mutex,
                                                 const std::string &message) {
  // std::cerr << "inserted" << "\n";
  auto it = mutexes.find(__mutex);
  if (it == mutexes.end()) {
    throw ltest::TestFailure(message);
  }
  return it->second;
}
extern int pthread_mutex_init(pthread_mutex_t *__mutex,
                              const pthread_mutexattr_t *__mutexattr) __THROW {
  assert(ltest_coro_ctx);
  ltest::SchedCtxGuard guard;
  return 0;
}

/* Destroy a mutex.  */
extern int pthread_mutex_destroy(pthread_mutex_t *__mutex) __THROW {
  PTHREAD_MOCK_INIT_ROUTINE
  auto it = mutexes.find(__mutex);
  if (it != mutexes.end()) {
    mutexes.erase(it);
  }
  return 0;
}

/* Try locking a mutex.  */
extern int pthread_mutex_trylock(pthread_mutex_t *__mutex) __THROWNL {
  PTHREAD_MOCK_INIT_ROUTINE
  auto &it = InsertMutex(__mutex);
  return std::visit(Overloads{[](auto &a) {
                      bool res = a.try_lock();
                      if (res) {
                        return 0;
                      }
                      return EBUSY;
                    }},
                    it);
}

/* Lock a mutex.  */
extern int pthread_mutex_lock(pthread_mutex_t *__mutex) __THROWNL {
  PTHREAD_MOCK_INIT_ROUTINE
  auto &it = InsertMutex(__mutex);
  return std::visit(Overloads{[](auto &a) {
                      a.lock();
                      return 0;
                    }},
                    it);
}

/* Unlock a mutex.  */
extern int pthread_mutex_unlock(pthread_mutex_t *__mutex) __THROWNL {
  PTHREAD_MOCK_INIT_ROUTINE
  auto &it = FindMutex(__mutex, "unlock called before lock");
  return std::visit(Overloads{[](auto &a) {
                      a.unlock();
                      return 0;
                    }},
                    it);
}

// we need to call this cond method in almost each cond function, because
// mutex can be initialized by macros
static decltype(cond_variables)::mapped_type &InsertCond(pthread_cond_t *cond) {
  // std::cerr << "inserted" << "\n";
  auto it = cond_variables.find(cond);
  if (it == cond_variables.end()) {
    it = cond_variables.emplace(cond, ltest::condition_variable()).first;
  }
  return it->second;
}

extern int pthread_cond_init(
    pthread_cond_t *__restrict __cond,
    const pthread_condattr_t *__restrict __cond_attr) __THROW {
  return 0;
}

/* Destroy condition variable COND.  */
extern int pthread_cond_destroy(pthread_cond_t *__cond) __THROW {
  PTHREAD_MOCK_INIT_ROUTINE
  auto it = cond_variables.find(__cond);
  if (it != cond_variables.end()) {
    cond_variables.erase(it);
  }
  return 0;
}

extern int pthread_cond_signal(pthread_cond_t *__cond) __THROWNL {
  PTHREAD_MOCK_INIT_ROUTINE
  auto &c = InsertCond(__cond);
  c.notify_one();
  return 0;
}

extern int pthread_cond_broadcast(pthread_cond_t *__cond) __THROWNL {
  PTHREAD_MOCK_INIT_ROUTINE
  auto &c = InsertCond(__cond);
  c.notify_all();
  return 0;
}

extern int pthread_cond_wait(pthread_cond_t *__restrict __cond,
                             pthread_mutex_t *__restrict __mutex) {
  PTHREAD_MOCK_INIT_ROUTINE
  auto &m = InsertMutex(__mutex);
  auto &c = InsertCond(__cond);
  // todo - allow other mutexes
  c.wait(std::get<ltest::mutex>(m));
  return 0;
}

static decltype(shared_mutexes)::mapped_type &InsertSharedLock(
    pthread_rwlock_t *lock) {
  auto it = shared_mutexes.find(lock);
  if (it == shared_mutexes.end()) {
    it = shared_mutexes.emplace(lock, ltest::shared_mutex()).first;
  }
  return it->second;
}

extern int pthread_rwlock_init(
    pthread_rwlock_t *__restrict __rwlock,
    const pthread_rwlockattr_t *__restrict __attr) __THROW {
  PTHREAD_MOCK_INIT_ROUTINE
  return 0;
}

/* Destroy read-write lock RWLOCK.  */
extern int pthread_rwlock_destroy(pthread_rwlock_t *__rwlock) __THROW {
  PTHREAD_MOCK_INIT_ROUTINE
  auto it = shared_mutexes.find(__rwlock);
  if (it != shared_mutexes.end()) {
    shared_mutexes.erase(it);
  }
  return 0;
}

/* Acquire read lock for RWLOCK.  */
extern int pthread_rwlock_rdlock(pthread_rwlock_t *__rwlock) __THROWNL {
  PTHREAD_MOCK_INIT_ROUTINE
  auto &it = InsertSharedLock(__rwlock);
  return std::visit(Overloads{[](auto &a) {
                      a.lock_shared();
                      return 0;
                    }},
                    it);
}

/* Try to acquire read lock for RWLOCK.  */
extern int pthread_rwlock_tryrdlock(pthread_rwlock_t *__rwlock) {
  PTHREAD_MOCK_INIT_ROUTINE
  auto &it = InsertSharedLock(__rwlock);
  throw std::runtime_error("not implemented");
}

/* Acquire write lock for RWLOCK.  */
extern int pthread_rwlock_wrlock(pthread_rwlock_t *__rwlock) __THROWNL {
  PTHREAD_MOCK_INIT_ROUTINE
  auto &it = InsertSharedLock(__rwlock);
  return std::visit(Overloads{[](auto &a) {
                      a.lock();
                      return 0;
                    }},
                    it);
}

/* Try to acquire write lock for RWLOCK.  */
extern int pthread_rwlock_trywrlock(pthread_rwlock_t *__rwlock) {
  PTHREAD_MOCK_INIT_ROUTINE
  throw std::runtime_error("not implemented");
}

/* Unlock RWLOCK.  */
extern int pthread_rwlock_unlock(pthread_rwlock_t *__rwlock) __THROWNL {
  PTHREAD_MOCK_INIT_ROUTINE
  auto &it = InsertSharedLock(__rwlock);
  return std::visit(Overloads{[](auto &a) {
                      a.unlock();
                      return 0;
                    }},
                    it);
}

// omit implementation
extern int pthread_getcpuclockid(pthread_t __thread_id,
                                 __clockid_t *__clock_id) __THROW {
  return 0;
}
