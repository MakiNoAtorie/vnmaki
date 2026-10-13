#include "engine/jobs/job_system.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace vnmaki {

struct JobSystem::Impl {
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::unique_ptr<detail::Job>> queue;
  bool stopping = false;
  std::vector<std::thread> workers;

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
};

std::size_t worker_count_for_hardware(unsigned hardware_threads) noexcept {
  return hardware_threads > 1 ? std::size_t{hardware_threads} - 1 : 1;
}

JobSystem::JobSystem() : JobSystem(worker_count_for_hardware(std::thread::hardware_concurrency())) {}

JobSystem::JobSystem(std::size_t worker_count) : impl_(std::make_unique<Impl>()) {
  impl_->workers.reserve(worker_count);
  for (std::size_t i = 0; i < worker_count; ++i) {
    impl_->workers.emplace_back([impl = impl_.get()] { impl->work(); });
  }
}

JobSystem::~JobSystem() {
  {
    const std::scoped_lock lock{impl_->mutex};
    impl_->stopping = true;
  }
  impl_->wake.notify_all();
  for (std::thread& worker : impl_->workers) {
    worker.join();
  }
}

bool JobSystem::threads_supported() noexcept { return VNMAKI_JOBS_THREADS != 0; }

std::size_t JobSystem::worker_count() const noexcept { return impl_->workers.size(); }

void JobSystem::enqueue(std::unique_ptr<detail::Job> job) {
  {
    const std::scoped_lock lock{impl_->mutex};
    impl_->queue.push_back(std::move(job));
  }
  impl_->wake.notify_one();
}

}  // namespace vnmaki
