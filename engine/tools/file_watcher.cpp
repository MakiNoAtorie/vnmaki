#include "engine/tools/file_watcher.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <efsw/efsw.hpp>

namespace vnmaki {

class FileWatcher::Os final : public efsw::FileWatchListener {
 public:
  Os(FileWatcher& owner, const std::filesystem::path& root)
      : owner_(&owner), root_(std::filesystem::absolute(root)) {
    const efsw::WatchID id = watcher_.addWatch(root_.string(), this, true);
    if (id < 0) {
      throw std::runtime_error("cannot watch " + root_.string() + ": " +
                               efsw::Errors::Log::getLastErrorLog());
    }
    watcher_.watch();
  }

  // The efsw thread: a directory and a file name, or a rename (two names).
  void handleFileAction(efsw::WatchID /*id*/, const std::string& directory,
                        const std::string& filename, efsw::Action /*action*/,
                        const std::string& old_filename) override {
    report(directory, filename);
    if (!old_filename.empty()) {
      report(directory, old_filename);
    }
  }

 private:
  void report(const std::string& directory, const std::string& filename) {
    const std::filesystem::path relative =
        (std::filesystem::path{directory} / filename).lexically_relative(root_);
    if (relative.empty() || *relative.begin() == "..") {
      return;
    }
    owner_->notify(relative.generic_string());
  }

  FileWatcher* owner_;
  std::filesystem::path root_;
  efsw::FileWatcher watcher_;  // last: its destructor joins the thread that calls report()
};

FileWatcher::FileWatcher(std::chrono::milliseconds window) : window_(window) {}

FileWatcher::FileWatcher(const std::filesystem::path& root, std::chrono::milliseconds window)
    : window_(window), os_(std::make_unique<Os>(*this, root)) {}

FileWatcher::~FileWatcher() = default;

void FileWatcher::notify(std::string path, Clock::time_point at) {
  const std::scoped_lock lock{mutex_};
  pending_.insert(std::move(path));
  last_event_ = at;
}

std::vector<std::string> FileWatcher::poll(Clock::time_point now) {
  const std::scoped_lock lock{mutex_};
  if (pending_.empty() || now - last_event_ < window_) {
    return {};
  }
  std::vector<std::string> batch{pending_.begin(), pending_.end()};
  pending_.clear();
  return batch;
}

}  // namespace vnmaki
