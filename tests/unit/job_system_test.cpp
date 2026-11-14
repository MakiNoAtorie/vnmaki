#include "engine/jobs/job_system.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

namespace {

// Bounds every wait in these tests, so a bug fails a test instead of hanging it.
constexpr auto kTimeout = std::chrono::seconds{10};

// Number of jobs a test queues behind a busy worker.
constexpr int kQueuedJobs = 10;

/// Polls `handle` until it is ready. Returns false after kTimeout.
template <typename Result>
bool wait_until_ready(const vnmaki::JobHandle<Result>& handle) {
  const auto deadline = std::chrono::steady_clock::now() + kTimeout;
  while (!handle.ready()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return true;
}

}  // namespace

TEST_CASE("a submitted job's result is handed back through its handle") {
  vnmaki::JobSystem jobs;

  constexpr int kDecodedWidth = 640;

  vnmaki::JobHandle<int> handle = jobs.submit([] { return kDecodedWidth; });

  CHECK(handle.valid());
  CHECK(handle.get() == kDecodedWidth);
  CHECK_FALSE(handle.valid());  // get() consumed the result.
  CHECK_FALSE(vnmaki::JobHandle<int>{}.valid());
}

TEST_CASE("polling a handle does not block and reports when the job has finished") {
  if (!vnmaki::JobSystem::threads_supported()) {
    return;  // Needs the job to run while this thread keeps going.
  }
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

TEST_CASE("a waiter can inspect a job's exception however late the worker cleans up") {
  // Regression: the waiter's reads of the exception must not race with the worker releasing
  // the job. Many short jobs make the two interleave in every order; TSan checks each one.
  vnmaki::JobSystem jobs;
  constexpr int kRounds = 500;

  for (int round = 0; round < kRounds; ++round) {
    vnmaki::JobHandle<int> handle =
        jobs.submit([]() -> int { throw std::runtime_error{"corrupt image"}; });
    CHECK_THROWS_WITH_AS(handle.get(), "corrupt image", std::runtime_error);
  }
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
    return all_arrived.wait_for(lock, kTimeout, [&] { return arrived == kWorkers; });
  };
  std::vector<vnmaki::JobHandle<bool>> handles;
  handles.reserve(kWorkers);
  for (std::size_t i = 0; i < kWorkers; ++i) {
    handles.push_back(jobs.submit(rendezvous));
  }

  for (vnmaki::JobHandle<bool>& handle : handles) {
    CHECK(handle.get());
  }
}

TEST_CASE("jobs can be submitted from several threads and from inside a job") {
  if (!vnmaki::JobSystem::threads_supported()) {
    return;
  }
  vnmaki::JobSystem jobs{2};
  constexpr int kThreads = 4;
  constexpr int kJobsPerThread = 50;
  std::vector<std::vector<vnmaki::JobHandle<int>>> handles(kThreads);

  std::vector<std::thread> submitters;
  submitters.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    submitters.emplace_back([&jobs, &handles, t] {
      handles.at(static_cast<std::size_t>(t)).reserve(kJobsPerThread);
      for (int i = 0; i < kJobsPerThread; ++i) {
        handles.at(static_cast<std::size_t>(t)).push_back(jobs.submit([t, i] { return t * i; }));
      }
    });
  }
  for (std::thread& submitter : submitters) {
    submitter.join();
  }
  constexpr int kFollowUpResult = -5;
  vnmaki::JobHandle<vnmaki::JobHandle<int>> outer =
      jobs.submit([&jobs] { return jobs.submit([] { return kFollowUpResult; }); });

  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kJobsPerThread; ++i) {
      CHECK(handles.at(static_cast<std::size_t>(t)).at(static_cast<std::size_t>(i)).get() == t * i);
    }
  }
  CHECK(outer.get().get() == kFollowUpResult);
}

TEST_CASE("shutting down with drain runs every queued job first") {
  vnmaki::JobSystem jobs{1};
  // The first job keeps the only worker busy, so the rest are still queued at shutdown.
  vnmaki::JobHandle<int> slow = jobs.submit([] {
    constexpr auto kDecodeTime = std::chrono::milliseconds{50};
    std::this_thread::sleep_for(kDecodeTime);
    return -1;
  });
  std::vector<vnmaki::JobHandle<int>> queued;
  queued.reserve(kQueuedJobs);
  for (int i = 0; i < kQueuedJobs; ++i) {
    queued.push_back(jobs.submit([i] { return i; }));
  }

  jobs.shutdown(vnmaki::ShutdownMode::kDrain);

  CHECK(slow.ready());
  for (int i = 0; i < kQueuedJobs; ++i) {
    vnmaki::JobHandle<int>& handle = queued.at(static_cast<std::size_t>(i));
    CHECK(handle.ready());
    CHECK(handle.get() == i);
  }
}

TEST_CASE("shutting down with cancel cancels queued jobs and lets the running one finish") {
  if (!vnmaki::JobSystem::threads_supported()) {
    return;  // Needs the job to run while this thread keeps going.
  }
  vnmaki::JobSystem jobs{1};
  constexpr int kRunningResult = 7;
  std::promise<void> started;
  std::promise<void> gate;
  std::shared_future<void> gate_opened = gate.get_future().share();
  vnmaki::JobHandle<int> running = jobs.submit([&started, gate_opened] {
    started.set_value();
    gate_opened.wait();
    return kRunningResult;
  });
  started.get_future().wait();
  std::vector<vnmaki::JobHandle<int>> queued;
  queued.reserve(kQueuedJobs);
  for (int i = 0; i < kQueuedJobs; ++i) {
    queued.push_back(jobs.submit([i] { return i; }));
  }

  // shutdown() waits for the running job, so call it from another thread and release the
  // running job only once the queued jobs have been cancelled.
  std::thread shutting_down{[&jobs] { jobs.shutdown(vnmaki::ShutdownMode::kCancel); }};
  const bool cancelled_while_running = wait_until_ready(queued.back());
  gate.set_value();
  shutting_down.join();

  CHECK(cancelled_while_running);
  CHECK(running.get() == kRunningResult);
  for (vnmaki::JobHandle<int>& handle : queued) {
    CHECK(handle.ready());
    CHECK_THROWS_AS(handle.get(), vnmaki::JobCancelled);
  }
}

TEST_CASE("a job submitted after shutdown is cancelled") {
  vnmaki::JobSystem jobs;
  jobs.shutdown(vnmaki::ShutdownMode::kDrain);

  vnmaki::JobHandle<int> late = jobs.submit([] { return 1; });

  REQUIRE(wait_until_ready(late));
  CHECK_THROWS_AS(late.get(), vnmaki::JobCancelled);
}

TEST_CASE("destroying the job system cancels queued jobs and releases what they captured") {
  if (!vnmaki::JobSystem::threads_supported()) {
    return;  // Needs the job to run while this thread keeps going.
  }
  std::optional<vnmaki::JobSystem> jobs{std::in_place, 1};
  const auto asset = std::make_shared<int>(0);
  std::promise<void> started;
  std::promise<void> gate;
  std::shared_future<void> gate_opened = gate.get_future().share();
  vnmaki::JobHandle<int> running = jobs->submit([&started, gate_opened, asset] {
    started.set_value();
    gate_opened.wait();
    return *asset;
  });
  started.get_future().wait();
  vnmaki::JobHandle<int> queued = jobs->submit([asset] { return *asset + 1; });

  std::thread destroying{[&jobs] { jobs.reset(); }};
  const bool cancelled_while_running = wait_until_ready(queued);
  gate.set_value();
  destroying.join();

  CHECK(cancelled_while_running);
  CHECK(running.get() == 0);
  CHECK_THROWS_AS(queued.get(), vnmaki::JobCancelled);
  CHECK(asset.use_count() == 1);
}

TEST_CASE("workers run jobs off the submitting thread") {
  if (!vnmaki::JobSystem::threads_supported()) {
    return;
  }
  vnmaki::JobSystem jobs{1};

  vnmaki::JobHandle<std::thread::id> handle =
      jobs.submit([] { return std::this_thread::get_id(); });

  CHECK(handle.get() != std::this_thread::get_id());
}

TEST_CASE("a job system with no workers runs each job inline on the submitting thread") {
  vnmaki::JobSystem jobs{0};

  vnmaki::JobHandle<std::thread::id> handle =
      jobs.submit([] { return std::this_thread::get_id(); });
  vnmaki::JobHandle<int> failing =
      jobs.submit([]() -> int { throw std::runtime_error{"corrupt image"}; });

  CHECK(jobs.worker_count() == 0);
  CHECK(handle.ready());
  CHECK(handle.get() == std::this_thread::get_id());
  CHECK(failing.ready());
  CHECK_THROWS_WITH_AS(failing.get(), "corrupt image", std::runtime_error);
}

TEST_CASE("an inline job system cancels jobs submitted after shutdown") {
  vnmaki::JobSystem jobs{0};
  jobs.shutdown(vnmaki::ShutdownMode::kDrain);
  bool ran = false;

  vnmaki::JobHandle<void> late = jobs.submit([&ran] { ran = true; });

  CHECK(late.ready());
  CHECK_THROWS_AS(late.get(), vnmaki::JobCancelled);
  CHECK_FALSE(ran);
}

TEST_CASE("without thread support every job system runs jobs inline") {
  if (vnmaki::JobSystem::threads_supported()) {
    return;
  }
  vnmaki::JobSystem jobs{4};

  vnmaki::JobHandle<std::thread::id> handle =
      jobs.submit([] { return std::this_thread::get_id(); });

  CHECK(jobs.worker_count() == 0);
  CHECK(vnmaki::JobSystem{}.worker_count() == 0);
  CHECK(handle.ready());
  CHECK(handle.get() == std::this_thread::get_id());
}
