#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <type_traits>
#include <utility>

namespace vnmaki {

namespace detail {

/// A queued unit of work with its result type erased.
class Job {
 public:
  Job() = default;
  Job(const Job&) = delete;
  Job& operator=(const Job&) = delete;
  Job(Job&&) = delete;
  Job& operator=(Job&&) = delete;
  virtual ~Job() = default;

  /// Runs the work and stores its result or exception for the handle.
  virtual void run() = 0;
};

template <typename Result, typename Fn>
class TypedJob final : public Job {
 public:
  explicit TypedJob(Fn fn, std::promise<Result> promise)
      : fn_(std::move(fn)), promise_(std::move(promise)) {}

  void run() override {
    try {
      if constexpr (std::is_void_v<Result>) {
        std::invoke(fn_);
        promise_.set_value();
      } else {
        promise_.set_value(std::invoke(fn_));
      }
    } catch (...) {
      promise_.set_exception(std::current_exception());
    }
  }

 private:
  Fn fn_;
  std::promise<Result> promise_;
};

}  // namespace detail

/// Worker count for a machine with `hardware_threads` hardware threads (0 if unknown): one
/// fewer, leaving a thread for the main thread, but never fewer than one.
[[nodiscard]] std::size_t worker_count_for_hardware(unsigned hardware_threads) noexcept;

/// The result of a submitted job, owned by whoever submitted it.
template <typename Result>
class JobHandle {
 public:
  JobHandle() = default;

  /// True once the job has finished (with a result or an exception). Never blocks.
  [[nodiscard]] bool ready() const {
    return future_.wait_for(std::chrono::seconds{0}) == std::future_status::ready;
  }

  /// Blocks until the job has finished.
  void wait() const { future_.wait(); }

  /// Blocks until the job has finished, then returns its result or rethrows the exception
  /// the job threw. Consumes the result, so call it once.
  Result get() { return future_.get(); }

 private:
  friend class JobSystem;
  explicit JobHandle(std::future<Result> future) : future_(std::move(future)) {}

  std::future<Result> future_;
};

/// A pool of worker threads that runs jobs (e.g. asset decoding) off the main thread.
class JobSystem {
 public:
  /// Starts `worker_count_for_hardware(std::thread::hardware_concurrency())` workers.
  JobSystem();
  /// Starts `worker_count` workers.
  explicit JobSystem(std::size_t worker_count);
  JobSystem(const JobSystem&) = delete;
  JobSystem& operator=(const JobSystem&) = delete;
  JobSystem(JobSystem&&) = delete;
  JobSystem& operator=(JobSystem&&) = delete;
  ~JobSystem();

  /// False when the engine was built without threads (VNMAKI_ENABLE_THREADS=OFF).
  [[nodiscard]] static bool threads_supported() noexcept;

  /// Number of worker threads running jobs.
  [[nodiscard]] std::size_t worker_count() const noexcept;

  /// Queues `fn` to run on a worker thread. Safe to call from any thread.
  template <typename Fn>
    requires std::invocable<std::decay_t<Fn>&>
  [[nodiscard]] auto submit(Fn&& fn) -> JobHandle<std::invoke_result_t<std::decay_t<Fn>&>> {
    using Result = std::invoke_result_t<std::decay_t<Fn>&>;
    std::promise<Result> promise;
    JobHandle<Result> handle{promise.get_future()};
    enqueue(std::make_unique<detail::TypedJob<Result, std::decay_t<Fn>>>(std::forward<Fn>(fn),
                                                                           std::move(promise)));
    return handle;
  }

 private:
  struct Impl;

  void enqueue(std::unique_ptr<detail::Job> job);

  std::unique_ptr<Impl> impl_;
};

}  // namespace vnmaki
