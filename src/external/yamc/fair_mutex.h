/*
 * fair_mutex.h
 *
 * MIT License
 *
 * Copyright (c) 2017 yohhoy
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#ifndef YAMC_FAIR_MUTEX_HPP_
#define YAMC_FAIR_MUTEX_HPP_

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>


namespace yamc {

/*
 * fairness (FIFO locking) mutex
 *
 * - yamc::fair::mutex
 * - yamc::fair::recursive_mutex
 * - yamc::fair::timed_mutex
 * - yamc::fair::recursive_timed_mutex
 */
namespace fair {

class mutex {
  std::size_t next_ = 0;
  std::size_t curr_ = 0;
  std::condition_variable cv_;
  std::mutex mtx_;

public:
  mutex() = default;
  ~mutex() = default;

  mutex(const mutex&) = delete;
  mutex& operator=(const mutex&) = delete;

  void lock()
  {
    std::unique_lock<decltype(mtx_)> lk(mtx_);
    const std::size_t request = next_++;
    while (request != curr_) {
      cv_.wait(lk);
    }
  }

  bool try_lock()
  {
    std::lock_guard<decltype(mtx_)> lk(mtx_);
    if (next_ != curr_)
      return false;
    ++next_;
    return true;
  }

  void unlock()
  {
    std::lock_guard<decltype(mtx_)> lk(mtx_);
    ++curr_;
    cv_.notify_all();
  }
};


class recursive_mutex {
  std::size_t next_ = 0;
  std::size_t curr_ = 0;
  std::size_t ncount_ = 0;
  std::thread::id owner_;
  std::condition_variable cv_;
  std::mutex mtx_;

public:
  recursive_mutex() = default;
  ~recursive_mutex() = default;

  recursive_mutex(const recursive_mutex&) = delete;
  recursive_mutex& operator=(const recursive_mutex&) = delete;

  void lock()
  {
    const auto tid = std::this_thread::get_id();
    std::unique_lock<decltype(mtx_)> lk(mtx_);
    if (owner_ == tid) {
      assert(0 < ncount_);
      ++ncount_;
      return;
    }
    const std::size_t request = next_++;
    while (request != curr_) {
      cv_.wait(lk);
    }
    assert(ncount_ == 0 && owner_ == std::thread::id());
    ncount_ = 1;
    owner_ = tid;
  }

  bool try_lock()
  {
    const auto tid = std::this_thread::get_id();
    std::lock_guard<decltype(mtx_)> lk(mtx_);
    if (owner_ == tid) {
      assert(0 < ncount_);
      ++ncount_;
      return true;
    }
    if (next_ != curr_)
      return false;
    ++next_;
    assert(ncount_ == 0 && owner_ == std::thread::id());
    ncount_ = 1;
    owner_ = tid;
    return true;
  }

  void unlock()
  {
    std::lock_guard<decltype(mtx_)> lk(mtx_);
    assert(0 < ncount_ && owner_ == std::this_thread::get_id());
    if (--ncount_ == 0) {
      ++curr_;
      owner_ = std::thread::id();
      cv_.notify_all();
    }
  }
};


namespace detail {

class timed_mutex_impl {
public:
  struct node {
    node* next;
    node* prev;
  };

  node queue_;   // q.next = front(), q.prev = back()
  node locked_;  // placeholder node of 'locked' state
  std::condition_variable cv_;
  std::mutex mtx_;

private:
  bool wq_empty()
  {
    return queue_.next == &queue_;
  }

  void wq_push_back(node* p)
  {
    node* back = queue_.prev;
    back->next = queue_.prev = p;
    p->next = &queue_;
    p->prev = back;
  }

  void wq_erase(node* p)
  {
    p->next->prev= p->prev;
    p->prev->next = p->next;
  }

  void wq_pop_front()
  {
    wq_erase(queue_.next);
  }

  void wq_replace_front(node* p)
  {
    // q.push_front() + q.push_front(p)
    node* front = queue_.next;
    assert(front != p);
    *p = *front;
    queue_.next = front->next->prev = p;
  }

public:
  timed_mutex_impl()
    : queue_{&queue_, &queue_} {}
  ~timed_mutex_impl() = default;

  std::unique_lock<std::mutex> internal_lock()
  {
    return std::unique_lock<std::mutex>(mtx_);
  }

  void impl_lock(std::unique_lock<std::mutex>& lk)
  {
    if (!wq_empty()) {
      node request;
      wq_push_back(&request);
      while (queue_.next != &request) {
        cv_.wait(lk);
      }
      wq_replace_front(&locked_);
    } else {
      wq_push_back(&locked_);
    }
  }

  bool impl_try_lock()
  {
    if (!wq_empty()) {
      return false;
    }
    wq_push_back(&locked_);
    return true;
  }

  void impl_unlock()
  {
    assert(queue_.next == &locked_);
    wq_pop_front();
    cv_.notify_all();
  }

  template<typename Clock, typename Duration>
  bool impl_try_lockwait(std::unique_lock<std::mutex>& lk, const std::chrono::time_point<Clock, Duration>& tp)
  {
    if (!wq_empty()) {
      node request;
      wq_push_back(&request);
      while (queue_.next != &request) {
        if (cv_.wait_until(lk, tp) == std::cv_status::timeout) {
          if (queue_.next == &request)  // re-check predicate
            break;
          wq_erase(&request);
          return false;
        }
      }
      wq_replace_front(&locked_);
    } else {
      wq_push_back(&locked_);
    }
    return true;
  }
};

} // namespace detail


class timed_mutex {
  detail::timed_mutex_impl impl_;

public:
  timed_mutex() = default;
  ~timed_mutex() = default;

  timed_mutex(const timed_mutex&) = delete;
  timed_mutex& operator=(const timed_mutex&) = delete;

  void lock()
  {
    auto lk = impl_.internal_lock();
    impl_.impl_lock(lk);
  }

  bool try_lock()
  {
    auto lk = impl_.internal_lock();
    return impl_.impl_try_lock();
  }

  void unlock()
  {
    auto lk = impl_.internal_lock();
    impl_.impl_unlock();
  }

  template<typename Rep, typename Period>
  bool try_lock_for(const std::chrono::duration<Rep, Period>& duration)
  {
    const auto tp = std::chrono::steady_clock::now() + duration;
    auto lk = impl_.internal_lock();
    return impl_.impl_try_lockwait(lk, tp);
  }

  template<typename Clock, typename Duration>
  bool try_lock_until(const std::chrono::time_point<Clock, Duration>& tp)
  {
    auto lk = impl_.internal_lock();
    return impl_.impl_try_lockwait(lk, tp);
  }
};


class recursive_timed_mutex {
  std::size_t ncount_ = 0;
  std::thread::id owner_ = {};
  detail::timed_mutex_impl impl_;

public:
  recursive_timed_mutex() = default;
  ~recursive_timed_mutex() = default;

  recursive_timed_mutex(const recursive_timed_mutex&) = delete;
  recursive_timed_mutex& operator=(const recursive_timed_mutex&) = delete;

  void lock()
  {
    const auto tid = std::this_thread::get_id();
    auto lk = impl_.internal_lock();
    if (owner_ == tid) {
      assert(0 < ncount_);
      ++ncount_;
    } else {
      impl_.impl_lock(lk);
      ncount_ = 1;
      owner_ = tid;
    }
  }

  bool try_lock()
  {
    const auto tid = std::this_thread::get_id();
    auto lk = impl_.internal_lock();
    if (owner_ == tid) {
      assert(0 < ncount_);
      ++ncount_;
      return true;
    }
    if (!impl_.impl_try_lock())
      return false;
    ncount_ = 1;
    owner_ = tid;
    return true;
  }

  void unlock()
  {
    auto lk = impl_.internal_lock();
    assert(0 < ncount_ && owner_ == std::this_thread::get_id());
    if (--ncount_ == 0) {
      impl_.impl_unlock();
      owner_ = std::thread::id();
    }
  }

  template<typename Rep, typename Period>
  bool try_lock_for(const std::chrono::duration<Rep, Period>& duration)
  {
    const auto tp = std::chrono::steady_clock::now() + duration;
    return try_lock_until(tp);  // delegate
  }

  template<typename Clock, typename Duration>
  bool try_lock_until(const std::chrono::time_point<Clock, Duration>& tp)
  {
    const auto tid = std::this_thread::get_id();
    auto lk = impl_.internal_lock();
    if (owner_ == tid) {
      assert(0 < ncount_);
      ++ncount_;
      return true;
    }
    if (!impl_.impl_try_lockwait(lk, tp))
      return false;
    ncount_ = 1;
    owner_ = tid;
    return true;
  }
};

} // namespace fair
} // namespace yamc

#endif
