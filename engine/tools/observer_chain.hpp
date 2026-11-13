#pragma once

// ScriptRuntimeOptions has one observer slot. A host that wants more than one observer (the log
// and the story graph trace) puts them in a chain and sets the chain.

#include <initializer_list>
#include <string_view>
#include <vector>

#include "engine/runtime/script_runtime.hpp"

namespace vnmaki {

/// Passes every call on to each observer, in the order given. The observers must outlive it.
class ObserverChain final : public ScriptObserver {
 public:
  ObserverChain(std::initializer_list<ScriptObserver*> observers) : observers_(observers) {}

  void on_label_entered(std::string_view label) override {
    for (ScriptObserver* observer : observers_) {
      observer->on_label_entered(label);
    }
  }
  void on_label_resumed(std::string_view label) override {
    for (ScriptObserver* observer : observers_) {
      observer->on_label_resumed(label);
    }
  }
  void on_blocked(const BlockingRequest& request, const ScriptPosition& position) override {
    for (ScriptObserver* observer : observers_) {
      observer->on_blocked(request, position);
    }
  }
  void on_scene_command(std::string_view command) override {
    for (ScriptObserver* observer : observers_) {
      observer->on_scene_command(command);
    }
  }
  void on_audio_command(std::string_view command) override {
    for (ScriptObserver* observer : observers_) {
      observer->on_audio_command(command);
    }
  }
  void on_print(std::string_view text) override {
    for (ScriptObserver* observer : observers_) {
      observer->on_print(text);
    }
  }
  void on_warning(std::string_view message) override {
    for (ScriptObserver* observer : observers_) {
      observer->on_warning(message);
    }
  }

 private:
  std::vector<ScriptObserver*> observers_;
};

}  // namespace vnmaki
