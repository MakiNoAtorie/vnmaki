#pragma once

#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace vnmaki {

/// Rethrown by JobHandle::get() for a job that was cancelled before it started.
class JobCancelled : public std::runtime_error {
 public:
  JobCancelled();
};

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
  /// Drops the work unrun and makes the handle rethrow JobCancelled.
  virtual void cancel() = 0;
};

/// Where a job leaves its outcome for its JobHandle. Shared by the job and the handle.
///
/// Unlike std::promise/std::future, take() moves the exception out under the mutex before
/// rethrowing it. The waiter's thread then holds the last reference to the exception object,
/// so the worker releasing the job later never frees an exception the waiter is still reading.
/// (exception_ptr counts references inside the uninstrumented C++ runtime, so ThreadSanitizer
/// cannot see the ordering that would otherwise make that safe.)
template <typename Result>
class JobState {
 public:
  using Value = std::conditional_t<std::is_void_v<Result>, std::monostate, Result>;

  void succeed(Value value) {
    {
      const std::scoped_lock lock{mutex_};
      value_.emplace(std::move(value));
      done_ = true;
    }
    done_changed_.notify_all();
  }

  void fail(std::exception_ptr error) {
    {
      const std::scoped_lock lock{mutex_};
      error_ = std::move(error);
      done_ = true;
    }
    done_changed_.notify_all();
  }

  [[nodiscard]] bool ready() const {
    const std::scoped_lock lock{mutex_};
    return done_;
  }

  void wait() const {
    std::unique_lock lock{mutex_};
    done_changed_.wait(lock, [this] { return done_; });
  }

  /// Waits, then moves the result out or rethrows the job's exception.
  Result take() {
    std::unique_lock lock{mutex_};
    done_changed_.wait(lock, [this] { return done_; });
    if (error_) {
      std::exception_ptr error = std::exchange(error_, nullptr);
      lock.unlock();
      std::rethrow_exception(std::move(error));
    }
    if constexpr (!std::is_void_v<Result>) {
      Result result = std::move(*value_);
      value_.reset();
      return result;
    }
  }

 private:
  mutable std::mutex mutex_;
  mutable std::condition_variable done_changed_;
  bool done_ = false;
  std::optional<Value> value_;
  std::exception_ptr error_;
};

template <typename Result, typename Fn>
class TypedJob final : public Job {
 public:
  TypedJob(Fn fn, std::shared_ptr<JobState<Result>> state)
      : fn_(std::move(fn)), state_(std::move(state)) {}

  void run() override {
    // Leave the catch block before publishing, so this thread keeps no reference to the
    // exception once the waiter can see it.
    std::optional<typename JobState<Result>::Value> value;
    std::exception_ptr error;
    try {
      if constexpr (std::is_void_v<Result>) {
        std::invoke(fn_);
        value.emplace();
      } else {
        value.emplace(std::invoke(fn_));
      }
    } catch (...) {
      error = std::current_exception();
    }
    if (error) {
      state_->fail(std::move(error));
    } else {
      state_->succeed(std::move(*value));
    }
  }

  void cancel() override { state_->fail(std::make_exception_ptr(JobCancelled{})); }

 private:
  Fn fn_;
  std::shared_ptr<JobState<Result>> state_;
};

}  // namespace detail

/// Worker count for a machine with `hardware_threads` hardware threads (0 if unknown): one
/// fewer, leaving a thread for the main thread, but never fewer than one.
[[nodiscard]] std::size_t worker_count_for_hardware(unsigned hardware_threads) noexcept;

/// What JobSystem::shutdown() does with jobs that are queued but not started.
enum class ShutdownMode : std::uint8_t {
  kDrain,   ///< Run them all before the workers stop. Jobs submitted meanwhile are cancelled.
  kCancel,  ///< Cancel them: their handles rethrow JobCancelled.
};

/// The result of a submitted job, owned by whoever submitted it. Like std::future, a handle
/// is used from one thread at a time, and ready(), wait() and get() need a valid() handle.
template <typename Result>
class JobHandle {
 public:
  JobHandle() = default;

  /// False for a default-constructed handle and after get().
  [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }

  /// True once the job has finished (with a result or an exception). Never blocks.
  [[nodiscard]] bool ready() const { return state_->ready(); }

  /// Blocks until the job has finished.
  void wait() const { state_->wait(); }

  /// Blocks until the job has finished, then returns its result or rethrows the exception
  /// the job threw. Consumes the result: the handle is no longer valid() afterwards.
  Result get() {
    const std::shared_ptr<detail::JobState<Result>> state = std::move(state_);
    return state->take();
  }

 private:
  friend class JobSystem;
  explicit JobHandle(std::shared_ptr<detail::JobState<Result>> state) : state_(std::move(state)) {}

  std::shared_ptr<detail::JobState<Result>> state_;
};

/// A pool of worker threads that runs jobs (e.g. asset decoding) off the main thread.
///
/// Jobs run in submission order (FIFO) on whichever worker is free. A job's result or exception
/// reaches the submitter through its JobHandle; the job's callable is destroyed on the thread
/// that ran or cancelled it.
///
/// Inline mode: with zero workers, or when the engine is built without threads
/// (VNMAKI_ENABLE_THREADS=OFF), submit() runs the job on the calling thread before returning,
/// so the handle is ready at once. Code that waits on handles works unchanged in both modes.
///
/// A job must not wait on another job's handle: with every worker waiting, nothing would run
/// the job they wait for. Chain work by submitting a follow-up job instead.
class JobSystem {
 public:
  /// Starts `worker_count_for_hardware(std::thread::hardware_concurrency())` workers.
  JobSystem();
  /// Starts `worker_count` workers; 0 selects inline mode. Without thread support (see
  /// threads_supported()) no worker is started, whatever the count.
  explicit JobSystem(std::size_t worker_count);
  JobSystem(const JobSystem&) = delete;
  JobSystem& operator=(const JobSystem&) = delete;
  JobSystem(JobSystem&&) = delete;
  JobSystem& operator=(JobSystem&&) = delete;
  /// Shuts down with ShutdownMode::kCancel.
  ~JobSystem();

  /// False when the engine was built without threads (VNMAKI_ENABLE_THREADS=OFF).
  [[nodiscard]] static bool threads_supported() noexcept;

  /// Number of worker threads running jobs; 0 in inline mode.
  [[nodiscard]] std::size_t worker_count() const noexcept;

  /// Stops the workers and waits for them to exit. Jobs already running always finish; `mode`
  /// decides what happens to queued ones. Safe to call more than once and from any thread except
  /// from inside a job.
  void shutdown(ShutdownMode mode);

  /// Queues `fn` to run on a worker thread (or runs it now, in inline mode). Safe to call from
  /// any thread, including from inside a job. After shutdown() the job is cancelled instead.
  template <typename Fn>
    requires std::invocable<std::decay_t<Fn>&>
  [[nodiscard]] auto submit(Fn&& fn) -> JobHandle<std::invoke_result_t<std::decay_t<Fn>&>> {
    using Result = std::invoke_result_t<std::decay_t<Fn>&>;
    auto state = std::make_shared<detail::JobState<Result>>();
    JobHandle<Result> handle{state};
    enqueue(std::make_unique<detail::TypedJob<Result, std::decay_t<Fn>>>(std::forward<Fn>(fn),
                                                                         std::move(state)));
    return handle;
  }

 private:
  struct Impl;

  void enqueue(std::unique_ptr<detail::Job> job);

  std::unique_ptr<Impl> impl_;
};

}  // namespace vnmaki
