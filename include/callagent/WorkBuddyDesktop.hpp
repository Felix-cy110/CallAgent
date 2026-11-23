#pragma once
#include "callagent/Config.hpp"
#include <functional>
#include <memory>

namespace callagent {
// An explicit installation is preserved; an empty path discovers the user's installation on each call.
fs::path ResolveWorkBuddyExecutable(const fs::path& configured, const fs::path& home = {});
class WorkBuddySupervisor {
 public:
  WorkBuddySupervisor(ProviderConfig config, std::function<void()> onReady);
  ~WorkBuddySupervisor();
  void Start();
  void Stop();
  Json Snapshot() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
