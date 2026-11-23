#pragma once
#include "callagent/Provider.hpp"
#include "callagent/Publisher.hpp"
#include "callagent/WorkBuddyDesktop.hpp"
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
namespace callagent {
class Service {
 public:
  explicit Service(Config config, std::vector<std::unique_ptr<Provider>> providers = {});
  ~Service();
  Json Handle(const Json& request);
  void Start();
 private:
  struct Slot { std::unique_ptr<Provider> provider; std::mutex mutex; };
  void Refresh(std::stop_token stop);
  Session Target(const std::string& key);
  Slot& Adapter(ProviderKind kind);
  Json Send(const Json& request);
  Json Receipt(const Json& request);
  Json Snapshot() const;
  Config config_; Publisher publisher_;
  std::map<ProviderKind, std::unique_ptr<Slot>> providers_;
  mutable std::mutex stateMutex_; std::mutex sendMutex_, wakeMutex_;
  std::condition_variable wake_;
  Json state_, publication_ = Json::array();
  std::vector<Session> selected_;
  bool requested_ = false;
  std::unique_ptr<WorkBuddySupervisor> workbuddySupervisor_;
  std::jthread scanner_, heartbeat_;
};
void Serve(const Config& config);
Json ClientRequest(const Config& config, const Json& request);
}
