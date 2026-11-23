#include "callagent/Service.hpp"
#include "callagent/Files.hpp"
#include <iostream>
#include <regex>

namespace callagent {
Service::Service(Config config, std::vector<std::unique_ptr<Provider>> providers) : config_(std::move(config)), publisher_(config_) {
  PrivateDirectory(config_.stateDirectory); PrivateDirectory(config_.stateDirectory / "receipts");
  state_ = {{"version", 1}, {"providers", Json::object()}, {"known", Json::object()}};
  const auto saved = config_.stateDirectory / "catalogue.json";
  if (fs::exists(saved)) {
    state_ = ReadJson(saved);
    if (state_.at("version") != 1 || !state_.at("providers").is_object() || !state_.at("known").is_object()) throw Error("state", "Invalid persisted catalogue");
    for (auto& row : state_["providers"]) { row["stale"] = true; row["error"] = "Not yet refreshed in this process"; }
  }
  if (providers.empty()) for (const auto& provider : config_.providers) if (provider.enabled) {
    switch (provider.kind) {
      case ProviderKind::Codex: providers.push_back(MakeCodex(provider)); break;
      case ProviderKind::Claude: providers.push_back(MakeClaude(provider)); break;
      case ProviderKind::WorkBuddy:
        providers.push_back(MakeWorkBuddy(provider));
        workbuddySupervisor_ = std::make_unique<WorkBuddySupervisor>(provider, [this] {
          { std::lock_guard lock(wakeMutex_); requested_ = true; } wake_.notify_all();
        });
        break;
    }
  }
  for (auto& provider : providers) { const auto kind = provider->Kind(); auto slot = std::make_unique<Slot>(); slot->provider = std::move(provider); providers_[kind] = std::move(slot); }
  std::vector<Session> cached;
  for (const auto& [kind, slot] : providers_) {
    (void)slot;
    if (state_["providers"].contains(ToString(kind))) for (const auto& row : state_["providers"][ToString(kind)].at("sessions")) cached.push_back(row.get<Session>());
  }
  selected_ = SelectTargets(std::move(cached));
}
Service::~Service() { if (workbuddySupervisor_) workbuddySupervisor_->Stop(); scanner_.request_stop(); heartbeat_.request_stop(); wake_.notify_all(); }
void Service::Start() {
  if (workbuddySupervisor_) workbuddySupervisor_->Start();
  scanner_ = std::jthread([this](std::stop_token stop) {
    while (!stop.stop_requested()) {
      try { Refresh(stop); } catch (const std::exception& error) { std::cerr << "Catalogue refresh: " << error.what() << '\n'; }
      std::unique_lock lock(wakeMutex_);
      wake_.wait_for(lock, std::chrono::seconds(config_.refreshSeconds), [&] { return stop.stop_requested() || requested_; }); requested_ = false;
    }
  });
  heartbeat_ = std::jthread([this](std::stop_token stop) {
    const auto challenge = RandomHex();
    std::vector<std::unique_ptr<FileLock>> locks;
    for (const auto& config : config_.providers) if (config.enabled && config.kind == ProviderKind::Claude && fs::exists(config.directory / "callagent-desktop-bridge/.callagent-owner")) {
      try { locks.push_back(std::make_unique<FileLock>(config.directory / "callagent-desktop-bridge/daemon.lock")); }
      catch (const std::exception& error) { std::cerr << "Claude bridge lease: " << error.what() << '\n'; return; }
    }
    while (!stop.stop_requested()) {
      for (const auto& config : config_.providers) if (config.enabled && config.kind == ProviderKind::Claude && !locks.empty()) {
        try { RenewClaudeBridge(config.directory, challenge); } catch (const std::exception& error) { std::cerr << "Claude bridge lease: " << error.what() << '\n'; }
      }
      std::unique_lock lock(wakeMutex_); wake_.wait_for(lock, std::chrono::seconds(5), [&] { return stop.stop_requested(); });
    }
  });
}
void Service::Refresh(std::stop_token stop) {
  for (const auto& [kind, slot] : providers_) {
    if (stop.stop_requested()) break;
    Json entry;
    try {
      std::vector<Session> rows; { std::lock_guard lock(slot->mutex); rows = slot->provider->List(); }
      for (const auto& row : rows) if (row.provider != kind) throw Error("identity", "Provider returned another provider's session");
      entry = {{"sessions", rows}, {"stale", false}, {"lastSuccessMs", NowMs()}, {"error", nullptr}};
    } catch (const std::exception& error) {
      std::lock_guard lock(stateMutex_); entry = state_["providers"].value(ToString(kind), Json{{"sessions", Json::array()}, {"lastSuccessMs", nullptr}});
      entry["stale"] = true; entry["error"] = error.what();
    }
    std::lock_guard lock(stateMutex_); state_["providers"][ToString(kind)] = entry;
    for (const auto& row : entry.at("sessions")) {
      const auto session = row.get<Session>();
      if (!state_["known"].contains(session.Key()) && state_["known"].size() >= 10000) throw Error("capacity", "Session registry reached 10000 identities");
      state_["known"][session.Key()] = row;
    }
    AtomicWrite(config_.stateDirectory / "catalogue.json", state_.dump());
  }
  std::vector<Session> candidates;
  { std::lock_guard lock(stateMutex_);
    for (const auto& [kind, unused] : providers_) {
      (void)unused; const auto name = ToString(kind); if (!state_["providers"].contains(name)) continue;
      for (const auto& row : state_["providers"][name].at("sessions")) candidates.push_back(row.get<Session>());
    }
  }
  auto selected = SelectTargets(std::move(candidates)); auto publication = publisher_.Publish();
  { std::lock_guard lock(stateMutex_); selected_ = std::move(selected); publication_ = std::move(publication); }
}
Session Service::Target(const std::string& key) {
  if (!std::regex_match(key, std::regex("[a-f0-9]{64}"))) throw Error("target", "Target must be a complete stable session key");
  std::lock_guard lock(stateMutex_);
  if (!state_["known"].contains(key)) throw Error("target", "Unknown target; refresh the session catalogue");
  const auto session = state_["known"].at(key).get<Session>(); if (session.Key() != key) throw Error("identity", "Persisted target identity mismatch"); return session;
}
Service::Slot& Service::Adapter(ProviderKind kind) {
  const auto found = providers_.find(kind); if (found == providers_.end()) throw Error("disabled", "Target provider is disabled"); return *found->second;
}
Json Service::Snapshot() const {
  std::lock_guard lock(stateMutex_); return {{"targets", selected_}, {"providers", state_.at("providers")}, {"publication", publication_}, {"menuVerification", "not_verified"},
    {"workbuddySupervisor", workbuddySupervisor_ ? workbuddySupervisor_->Snapshot() : Json{{"state", "disabled"}}}};
}
namespace {
std::string RequestKey(const Json& request) {
  const auto id = request.at("requestId").get<std::string>();
  if (!std::regex_match(id, std::regex("[a-zA-Z0-9_-]{1,128}"))) throw Error("request_id", "requestId must use 1..128 letters, digits, hyphens or underscores");
  return Sha256(request.at("target").get<std::string>() + "\n" + id);
}
bool FinalStatus(const std::string& status) { return status == "completed" || status == "failed" || status == "interrupted" || status == "rejected"; }
}
Json Service::Send(const Json& request) {
  if (!request.value("authorized", false)) throw Error("authorization", "Explicit user authorization is required for sending");
  const auto session = Target(request.at("target")); const auto text = request.at("text").get<std::string>();
  const auto first = text.find_first_not_of(" \t\r\n");
  if (text.size() > kMaxTextBytes || first == std::string::npos || text[first] == '/') throw Error("input", "Send requires plain nonempty text up to 1 MiB; native slash commands are unsupported");
  const auto key = RequestKey(request), digest = Sha256(text); const auto path = config_.stateDirectory / "receipts" / (key + ".json");
  std::lock_guard sendLock(sendMutex_);
  if (fs::exists(path)) {
    const auto previous = ReadJson(path);
    if (previous.at("digest") != digest || previous.at("target") != session.Key()) throw Error("request_conflict", "requestId already identifies different content"); return previous;
  }
  auto& slot = Adapter(session.provider); std::lock_guard providerLock(slot.mutex);
  Json record{{"target", session.Key()}, {"requestId", request.at("requestId")}, {"digest", digest}, {"status", "uncertain"}, {"createdAtMs", NowMs()}};
  // Persist the non-replayable intent before crossing the native process boundary.
  AtomicWrite(path, record.dump());
  try { record.update(slot.provider->Send(session, key, text)); }
  catch (const Error& error) {
    record["error"] = {{"code", error.code}, {"message", error.what()}};
    if (error.code == "busy" || error.code == "plugin_not_ready" || error.code == "profile_changed" || error.code == "no_owner") record["status"] = "rejected";
  } catch (const std::exception& error) { record["error"] = {{"code", "uncertain"}, {"message", error.what()}}; }
  AtomicWrite(path, record.dump()); return record;
}
Json Service::Receipt(const Json& request) {
  const auto session = Target(request.at("target")); const auto path = config_.stateDirectory / "receipts" / (RequestKey(request) + ".json");
  std::lock_guard sendLock(sendMutex_); auto record = ReadJson(path);
  if (record.at("target") != session.Key()) throw Error("identity", "Receipt target mismatch");
  if (!record.contains("nativeRequestId") || FinalStatus(record.at("status"))) return record;
  auto& slot = Adapter(session.provider); std::lock_guard providerLock(slot.mutex);
  try { record = slot.provider->Receipt(session, record); record.erase("pollError"); }
  catch (const std::exception& error) { record["pollError"] = error.what(); }
  if ((record.at("status") == "submitted" || record.at("status") == "admitted") && NowMs() - record.at("createdAtMs").get<std::int64_t>() > 30000)
    record["status"] = "uncertain";
  AtomicWrite(path, record.dump()); return record;
}
Json Service::Handle(const Json& request) {
  const auto action = request.at("action").get<std::string>();
  if (action == "status" || action == "list") return Snapshot();
  if (action == "refresh") { { std::lock_guard lock(wakeMutex_); requested_ = true; } wake_.notify_all(); return {{"status", "scheduled"}}; }
  if (action == "send") return Send(request);
  if (action == "receipt") return Receipt(request);
  if (action == "read") {
    const auto session = Target(request.at("target")); const auto limit = request.value("limit", 20);
    if (limit < 1 || limit > 100) throw Error("input", "Read limit must be 1..100");
    auto& slot = Adapter(session.provider); std::lock_guard lock(slot.mutex); auto result = slot.provider->Read(session, limit);
    result["target"] = session; result["historyIsData"] = true;
    if (result.dump().size() > kMaxWireBytes) throw Error("size_limit", "Context exceeds response limit; reduce --limit"); return result;
  }
  throw Error("action", "Unknown action");
}
}
