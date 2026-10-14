#include "engine/jobs/job_system.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

// VNMAKI_JOBS_THREADS is set by engine/jobs/CMakeLists.txt from VNMAKI_ENABLE_THREADS. With 0,
// no thread is ever created and every job runs inline.
#if VNMAKI_JOBS_THREADS
#include <thread>
#endif

namespace vnmaki {

struct JobSystem::Impl {
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::unique_ptr<detail::Job>> queue;
  bool stopping = false;
#if VNMAKI_JOBS_THREADS
  // Written only by the constructor; empty means jobs run inline.
  std::vector<std::thread> workers;
  // Serializes joining the workers when shutdown() runs on several threads at once.
  std::mutex join_mutex;
#endif

  void work() {
    for (;;) {
      std::unique_ptr<detail::Job> job;
      {
        std::unique_lock lock{mutex};
        wake.wait(lock, [this] { return stopping || !queue.empty(); });
        if (queue.empty()) {
          return;
        }
        job = std::move(queue.front());
        queue.pop_front();
      }
      job->run();
    }
  }

  [[nodiscard]] std::size_t worker_count() const noexcept {
#if VNMAKI_JOBS_THREADS
    return workers.size();
#else
    return 0;
#endif
  }
};

JobCancelled::JobCancelled() : std::runtime_error{"job cancelled"} {}

std::size_t worker_count_for_hardware(unsigned hardware_threads) noexcept {
  return hardware_threads > 1 ? std::size_t{hardware_threads} - 1 : 1;
}

#if VNMAKI_JOBS_THREADS
JobSystem::JobSystem()
    : JobSystem(worker_count_for_hardware(std::thread::hardware_concurrency())) {}
#else
JobSystem::JobSystem() : JobSystem(0) {}
#endif

JobSystem::JobSystem([[maybe_unused]] std::size_t worker_count) : impl_(std::make_unique<Impl>()) {
#if VNMAKI_JOBS_THREADS
  impl_->workers.reserve(worker_count);
  for (std::size_t i = 0; i < worker_count; ++i) {
    impl_->workers.emplace_back([impl = impl_.get()] { impl->work(); });
  }
#endif
}

JobSystem::~JobSystem() { shutdown(ShutdownMode::kCancel); }

void JobSystem::shutdown(ShutdownMode mode) {
  std::deque<std::unique_ptr<detail::Job>> cancelled;
  {
    const std::scoped_lock lock{impl_->mutex};
    impl_->stopping = true;
    if (mode == ShutdownMode::kCancel) {
      cancelled.swap(impl_->queue);
    }
  }
  impl_->wake.notify_all();
  for (std::unique_ptr<detail::Job>& job : cancelled) {
    job->cancel();
  }
  cancelled.clear();

#if VNMAKI_JOBS_THREADS
  const std::scoped_lock lock{impl_->join_mutex};
  for (std::thread& worker : impl_->workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
#endif
}

bool JobSystem::threads_supported() noexcept { return VNMAKI_JOBS_THREADS != 0; }

std::size_t JobSystem::worker_count() const noexcept { return impl_->worker_count(); }

void JobSystem::enqueue(std::unique_ptr<detail::Job> job) {
  std::unique_lock lock{impl_->mutex};
  if (impl_->stopping) {
    lock.unlock();
    job->cancel();  // Nothing will run it any more.
    return;
  }
  if (impl_->worker_count() == 0) {
    lock.unlock();
    job->run();  // Inline mode: run it here, on the submitting thread.
    return;
  }
  impl_->queue.push_back(std::move(job));
  lock.unlock();
  impl_->wake.notify_one();
}

}  // namespace vnmaki
