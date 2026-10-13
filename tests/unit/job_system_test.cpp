#include "engine/jobs/job_system.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

TEST_CASE("a submitted job's result is handed back through its handle") {
  vnmaki::JobSystem jobs;

  vnmaki::JobHandle<int> handle = jobs.submit([] { return 6 * 7; });

  CHECK(handle.get() == 42);
}

TEST_CASE("polling a handle does not block and reports when the job has finished") {
  vnmaki::JobSystem jobs;
  std::promise<void> gate;
  std::shared_future<void> gate_opened = gate.get_future().share();

  vnmaki::JobHandle<int> handle = jobs.submit([gate_opened] {
    gate_opened.wait();
    return 1;
  });

  CHECK_FALSE(handle.ready());
  gate.set_value();
  handle.wait();
  CHECK(handle.ready());
  CHECK(handle.get() == 1);
}

TEST_CASE("an exception thrown inside a job is rethrown to whoever waits on the handle") {
  vnmaki::JobSystem jobs;

  vnmaki::JobHandle<int> handle =
      jobs.submit([]() -> int { throw std::runtime_error{"corrupt image"}; });

  handle.wait();
  CHECK(handle.ready());
  CHECK_THROWS_WITH_AS(handle.get(), "corrupt image", std::runtime_error);
}

TEST_CASE("a job without a result reports completion and exceptions") {
  vnmaki::JobSystem jobs;
  int decoded = 0;

  vnmaki::JobHandle<void> ok = jobs.submit([&decoded] { decoded = 1; });
  vnmaki::JobHandle<void> failing = jobs.submit([] { throw std::runtime_error{"truncated"}; });

  ok.get();
  CHECK(decoded == 1);
  CHECK_THROWS_WITH_AS(failing.get(), "truncated", std::runtime_error);
}

TEST_CASE("the default worker count leaves one hardware thread for the main thread") {
  CHECK(vnmaki::worker_count_for_hardware(8) == 7);
  CHECK(vnmaki::worker_count_for_hardware(2) == 1);
  // Never fewer than one worker, even when the hardware reports one thread or is unknown (0).
  CHECK(vnmaki::worker_count_for_hardware(1) == 1);
  CHECK(vnmaki::worker_count_for_hardware(0) == 1);
}

TEST_CASE("a job system uses the default worker count unless told otherwise") {
  if (!vnmaki::JobSystem::threads_supported()) {
    return;
  }
  const vnmaki::JobSystem default_jobs;
  const vnmaki::JobSystem three_jobs{3};

  CHECK(default_jobs.worker_count() ==
        vnmaki::worker_count_for_hardware(std::thread::hardware_concurrency()));
  CHECK(three_jobs.worker_count() == 3);
}

TEST_CASE("every configured worker runs jobs at the same time") {
  if (!vnmaki::JobSystem::threads_supported()) {
    return;
  }
  constexpr std::size_t kWorkers = 3;
  vnmaki::JobSystem jobs{kWorkers};
  std::mutex mutex;
  std::condition_variable all_arrived;
  std::size_t arrived = 0;

  // Each job waits until every job has started. That only happens if each runs on its own
  // worker; the timeout turns a missing worker into a failure instead of a hang.
  auto rendezvous = [&] {
    std::unique_lock lock{mutex};
    ++arrived;
    all_arrived.notify_all();
    return all_arrived.wait_for(lock, std::chrono::seconds{10},
                                [&] { return arrived == kWorkers; });
  };
  std::vector<vnmaki::JobHandle<bool>> handles;
  for (std::size_t i = 0; i < kWorkers; ++i) {
    handles.push_back(jobs.submit(rendezvous));
  }

  for (vnmaki::JobHandle<bool>& handle : handles) {
    CHECK(handle.get());
  }
}
