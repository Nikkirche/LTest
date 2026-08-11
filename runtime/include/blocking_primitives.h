#pragma once
#include <iostream>
#include <memory_resource>
#include <mutex>
#include <variant>

#include "block_manager.h"
#include "lib.h"
#include "verifying_macro.h"

namespace ltest {

struct mutex {
  as_atomic void lock() {
    while (locked) {
      CoroCtxGuard guard;
      this_coro->SetBlocked(state());
      CoroYield();
    }
    locked = 1;
  }

  as_atomic bool try_lock() {
    if (locked) {
      CoroCtxGuard guard;
      CoroYield();
      return false;
    }
    locked = 1;
    return true;
  }

  as_atomic void unlock() {
    locked = 0;
    // To have the ability schedule any coroutine.
    block_manager.UnblockAllOn(addr());
  }

 private:
  [[nodiscard]] std::intptr_t addr() const {
    return reinterpret_cast<std::intptr_t>(&locked);
  }

  [[nodiscard]] BlockState state() const { return {addr(), locked}; }

  int locked{0};

  friend struct condition_variable;
};

struct condition_variable {
  as_atomic void wait(std::unique_lock<ltest::mutex>& lock) {
    lock.unlock();
    this_coro->SetBlocked({addr(), 1});
    {
      CoroCtxGuard guard;
      CoroYield();
    }
    lock.lock();
  }

  // is needed to match pthread api - there doesn't exists any std::unique_lock
  as_atomic void wait(ltest::mutex& lock) {
    lock.unlock();
    this_coro->SetBlocked({addr(), 1});
    {
      CoroCtxGuard guard;
      CoroYield();
    }
    lock.lock();
  }

  as_atomic void notify_one() { block_manager.UnblockOn(addr(), 1); }

  as_atomic void notify_all() { block_manager.UnblockAllOn(addr()); }

 private:
  [[nodiscard]] std::intptr_t addr() const {
    return reinterpret_cast<std::intptr_t>(&wait_queue_);
  }

  int wait_queue_{0};
};

/**
 *
 * shared_mutex_r is a simple implementation:
 * locked = -1: exclusive lock
 * locked >= 0: number of separating locks
 *
 * Problem: writer starvation (writers can wait forever)
 *
 */
struct shared_mutex_r {
  as_atomic void lock() {
    while (locked != 0) {
      this_coro->SetBlocked(state());
      {
        CoroCtxGuard guard;
        CoroYield();
      }
    }
    locked = -1;
  }
  as_atomic void unlock() {
    locked = 0;
    block_manager.UnblockAllOn(addr());
  }
  as_atomic void lock_shared() {
    while (locked == -1) {
      this_coro->SetBlocked(state());
      {
        CoroCtxGuard guard;
        CoroYield();
      }
    }
    ++locked;
  }
  as_atomic void unlock_shared() {
    --locked;
    block_manager.UnblockAllOn(addr());
  }

 private:
  [[nodiscard]] std::intptr_t addr() const {
    return reinterpret_cast<std::intptr_t>(&locked);
  }

  [[nodiscard]] BlockState state() const { return {addr(), locked}; }

  int locked{0};
};

/**
 *
 * shared_mutex is an advanced implementation with queues:
 *
 * Uses two condition_variables:
 * write_entered_ - to wait for the writer to log in
 * no_readers_ - to wait for readers to finish
 *
 * Be honest with the writers (FIFO)
 */
struct shared_mutex {
  as_atomic void lock() {
    std::unique_lock lock{mutex_};
    while (has_write()) {
      write_entered_.wait(lock);
    }
    write_ = this_thread_id;
    while (reader_count_ > 0) {
      no_readers_.wait(lock);
    }
  }
  // in pthread api, unlock_shared calls under the hood unlock()
  as_atomic void unlock() {
    std::unique_lock lock{mutex_};
    if (write_ == this_thread_id) {
      write_ = -1;
      write_entered_.notify_all();
    }
    else {
      --reader_count_;
      if (has_write() && reader_count_ == 0) {
        no_readers_.notify_one();
      }
    }
  }

  as_atomic void lock_shared() {
    std::unique_lock lock{mutex_};
    while (has_write()) {
      write_entered_.wait(lock);
    }
    ++reader_count_;
  }
  as_atomic void unlock_shared() {
    unlock();
  }

 private:
  [[nodiscard]] bool has_write() const { return write_ != -1ull; }
  int reader_count_{0};
  size_t write_ = -1;

  condition_variable write_entered_;
  condition_variable no_readers_;

  mutex mutex_;
};

inline std::pmr::monotonic_buffer_resource pthread_mock_resource(1000);
inline std::pmr::unordered_map<pthread_mutex_t *, std::variant<mutex>>
    mutexes(&pthread_mock_resource);
inline std::pmr::unordered_map<pthread_rwlock_t *,
                               std::variant<shared_mutex>>
    shared_mutexes(&pthread_mock_resource);
inline std::pmr::unordered_map<pthread_cond_t *, condition_variable>
    cond_variables(&pthread_mock_resource);

}  // namespace ltest
