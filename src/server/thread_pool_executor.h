// A minimal fixed-size thread pool implementing net_http's EventExecutor.
//
// net_http requires an executor to run request callbacks off the I/O thread.
// This is a plain worker pool; Color's own serialization (a mutex in the
// interceptor) handles the fact that the conversation state is single-threaded.
#ifndef COLOR_THREAD_POOL_EXECUTOR_H
#define COLOR_THREAD_POOL_EXECUTOR_H

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "net_http/server/public/httpserver_interface.h"

namespace color {

class ThreadPoolExecutor : public net_http::EventExecutor {
 public:
  explicit ThreadPoolExecutor(int num_threads) {
    for (int i = 0; i < num_threads; ++i)
      workers_.emplace_back([this] { run(); });
  }

  ~ThreadPoolExecutor() override {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
  }

  void Schedule(std::function<void()> fn) override {
    {
      std::lock_guard<std::mutex> lk(mu_);
      queue_.push(std::move(fn));
    }
    cv_.notify_one();
  }

 private:
  void run() {
    for (;;) {
      std::function<void()> fn;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
        if (stop_ && queue_.empty()) return;
        fn = std::move(queue_.front());
        queue_.pop();
      }
      fn();
    }
  }

  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> queue_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
};

}  // namespace color

#endif  // COLOR_THREAD_POOL_EXECUTOR_H
