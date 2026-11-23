#pragma once
#include "callagent/Config.hpp"
#include <memory>
namespace callagent {
class WorkBuddyRpc {
 public:
  explicit WorkBuddyRpc(const ProviderConfig& config);
  ~WorkBuddyRpc();
  Json Call(const std::string& method, const Json& params);
  Json Native(const std::string& channel, const Json& args);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
