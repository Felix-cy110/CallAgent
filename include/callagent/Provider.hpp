#pragma once
#include "callagent/Config.hpp"
#include <memory>
namespace callagent {
class Provider {
 public:
  virtual ~Provider() = default;
  virtual ProviderKind Kind() const = 0;
  virtual std::vector<Session> List() = 0;
  virtual Json Read(const Session& session, int limit) = 0;
  virtual Json Send(const Session& session, const std::string& requestId, const std::string& text) = 0;
  virtual Json Receipt(const Session& session, const Json& receipt) = 0;
};
std::unique_ptr<Provider> MakeCodex(const ProviderConfig& config);
std::unique_ptr<Provider> MakeClaude(const ProviderConfig& config);
std::unique_ptr<Provider> MakeWorkBuddy(const ProviderConfig& config);
void InstallClaudeBridge(const fs::path& directory);
void RenewClaudeBridge(const fs::path& directory, const std::string& challenge);
}
