#include "callagent/Provider.hpp"
#include "callagent/NativeChannel.hpp"
#include <algorithm>
#include <set>

namespace callagent {
namespace {
class HistoryReader {
 public:
  explicit HistoryReader(const ProviderConfig& config) : channel_(NativeChannel::Process(config.executable.string(), {"app-server", "--listen", "stdio://"}, "CODEX_HOME", config.directory.string())) {
    Call("initialize", {{"clientInfo", {{"name", "callagent"}, {"version", "0.2.0"}}}, {"capabilities", {{"experimentalApi", true}}}});
    channel_->Send({{"method", "initialized"}, {"params", Json::object()}}, Clock::now() + std::chrono::seconds(5));
  }
  Json Call(const std::string& method, const Json& params) {
    const auto id = ++next_; const auto deadline = Clock::now() + std::chrono::seconds(10);
    channel_->Send({{"id", id}, {"method", method}, {"params", params}}, deadline);
    for (;;) {
      const auto reply = channel_->Receive(deadline);
      if (reply.value("id", Json()) != id) continue;
      if (reply.contains("error")) throw Error("native_rejected", "Codex read operation rejected: " + method);
      return reply.at("result");
    }
  }
 private:
  int next_ = 0;
  std::unique_ptr<NativeChannel> channel_;
};
class DesktopConnection {
 public:
  explicit DesktopConnection(const fs::path& endpoint) : channel_(NativeChannel::Socket(endpoint)) {
    client_ = "initializing-client";
    client_ = Call("initialize", {{"clientType", "callagent"}}, 0).at("result").at("clientId");
  }
  ~DesktopConnection() {
    if (!following_) return;
    try {
      channel_->Send({{"type", "broadcast"}, {"method", "thread-stream-following-changed"}, {"version", 1},
        {"sourceClientId", client_}, {"targetClientIds", Json::array({owner_})},
        {"params", {{"conversationId", session_}, {"hostId", "local"}, {"following", false}}}}, Clock::now() + std::chrono::milliseconds(250));
    } catch (const std::exception&) { /* Best-effort unsubscribe: the native peer may already be disconnected. */ }
  }
  bool Owns(const std::string& session) {
    session_ = session; owner_.clear();
    try { owner_ = Call("thread-owner-discovery", {{"hostId", "local"}, {"conversationId", session}}, 1).at("handledByClientId"); }
    catch (const Error& error) { if (error.code == "no_owner") return false; throw; }
    if (owner_.empty()) throw Error("identity", "Codex did not identify a Desktop owner");
    return true;
  }
  void RequireIdle() {
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    channel_->Send({{"type", "broadcast"}, {"method", "thread-stream-following-changed"}, {"version", 1},
      {"sourceClientId", client_}, {"targetClientIds", Json::array({owner_})},
      {"params", {{"conversationId", session_}, {"hostId", "local"}, {"following", true}}}}, deadline);
    following_ = true;
    for (;;) {
      const auto frame = channel_->Receive(deadline);
      if (frame.value("type", "") == "client-discovery-request") {
        channel_->Send({{"type", "client-discovery-response"}, {"requestId", frame.at("requestId")}, {"response", {{"canHandle", false}}}}, deadline); continue;
      }
      if (frame.value("type", "") != "broadcast" || frame.value("method", "") != "thread-stream-state-changed" || frame.value("sourceClientId", "") != owner_) continue;
      const auto params = frame.at("params");
      if (params.value("conversationId", "") != session_ || params.value("hostId", "") != "local") continue;
      const int version = frame.value("version", -1);
      if (version != 11 && version != 13) throw Error("protocol", "Unsupported Codex Desktop state version");
      const auto change = params.at("change");
      if (change.value("type", "") != "snapshot") continue;
      const auto state = change.at("conversationState");
      if (state.at("id") != session_) throw Error("identity", "Codex Desktop snapshot identity mismatch");
      if (state.value("threadRuntimeStatus", Json::object()).value("type", "") == "active" || !state.value("requests", Json::array()).empty())
        throw Error("busy", "Codex Desktop is running or waiting for approval");
      Json turns;
      const auto history = state.value("turnHistory", Json::object());
      if (history.value("kind", "") == "canonical") {
        turns = Json::array(); const auto data = history.at("history");
        for (const auto& island : data.at("islands")) for (const auto& entry : island.at("entries")) turns.push_back(data.at("entitiesByKey").at(entry.at("value").get<std::string>()));
      } else turns = state.at("turns");
      if (!turns.is_array()) throw Error("protocol", "Invalid Codex Desktop turn state");
      for (const auto& turn : turns) if (turn.value("status", "") == "inProgress") throw Error("busy", "Codex Desktop has an active turn");
      return;
    }
  }
  Json Call(const std::string& method, Json params, int version, bool targeted = false) {
    const auto id = RandomHex(16);
    const auto timeout = method == "thread-owner-discovery" ? 15000 : 30000;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout);
    Json request{{"type", "request"}, {"requestId", id}, {"sourceClientId", client_}, {"method", method}, {"version", version}, {"params", params}, {"timeoutMs", timeout}};
    if (targeted) { request["targetClientId"] = owner_; request["params"]["conversationId"] = session_; }
    channel_->Send(request, deadline);
    for (;;) {
      const auto reply = channel_->Receive(deadline);
      if (reply.value("type", "") == "client-discovery-request") {
        channel_->Send({{"type", "client-discovery-response"}, {"requestId", reply.at("requestId")}, {"response", {{"canHandle", false}}}}, deadline); continue;
      }
      if (reply.value("type", "") != "response" || reply.value("requestId", "") != id) continue;
      if (reply.value("method", "") != method) throw Error("identity", "Codex Desktop response method mismatch");
      if (reply.value("resultType", "") == "error") {
        if (reply.value("error", Json()) == "no-client-found") throw Error("no_owner", "Open this existing session in Codex Desktop");
        throw Error("native_rejected", "Codex Desktop rejected: " + method);
      }
      if (targeted && reply.value("handledByClientId", "") != owner_) throw Error("identity", "Codex Desktop response owner mismatch");
      return reply;
    }
  }
 private:
  std::unique_ptr<NativeChannel> channel_;
  std::string client_, owner_, session_;
  bool following_ = false;
};
Json RecentTurns(HistoryReader& reader, const std::string& id, int limit) {
  auto thread = reader.Call("thread/read", {{"threadId", id}, {"includeTurns", false}}).at("thread");
  if (thread.at("id") != id) throw Error("identity", "Codex history identity mismatch");
  if (thread.value("historyMode", "") == "paginated") {
    const auto page = reader.Call("thread/turns/list", {{"threadId", id}, {"limit", limit}, {"itemsView", "full"}, {"sortDirection", "desc"}});
    auto turns = page.at("data"); std::reverse(turns.begin(), turns.end()); return turns;
  }
  thread = reader.Call("thread/read", {{"threadId", id}, {"includeTurns", true}}).at("thread");
  if (thread.at("id") != id) throw Error("identity", "Codex history identity mismatch");
  auto turns = thread.at("turns");
  if (turns.size() > static_cast<std::size_t>(limit)) turns.erase(turns.begin(), turns.end() - limit);
  return turns;
}
class Codex final : public Provider {
 public:
  explicit Codex(ProviderConfig config) : config_(std::move(config)) {}
  ProviderKind Kind() const override { return ProviderKind::Codex; }
  std::vector<Session> List() override {
    // The app-server is a read-only catalogue process. It never starts/resumes a turn.
    DesktopConnection desktop(config_.endpoint); HistoryReader reader(config_);
    std::vector<Session> candidates; std::string cursor; std::set<std::string> cursors;
    for (int page = 0; page < 5; ++page) {
      Json params{{"limit", 100}, {"sortKey", "updated_at"}, {"sortDirection", "desc"}};
      if (!cursor.empty()) params["cursor"] = cursor;
      const auto result = reader.Call("thread/list", params);
      for (const auto& row : result.at("data")) {
        candidates.push_back({Kind(), config_.directory.string(), row.at("id"), row.value("name", Json()).is_string() ? row.at("name").get<std::string>() : row.value("preview", ""),
          row.value("cwd", ""), row.at("updatedAt").get<std::int64_t>() * 1000, "native.updatedAt"});
      }
      if (result.value("nextCursor", Json()).is_null()) break;
      cursor = result.at("nextCursor"); if (cursor.empty()) break;
      if (!cursors.insert(cursor).second) throw Error("pagination", "Codex repeated catalogue cursor");
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.activityMs != b.activityMs ? a.activityMs > b.activityMs : a.nativeId < b.nativeId; });
    std::vector<Session> sessions; std::set<std::string> discovered; const auto budget = Clock::now() + std::chrono::seconds(45);
    for (const auto& session : candidates) {
      if (!discovered.insert(session.nativeId).second) continue;
      if (Clock::now() >= budget) throw Error("discovery_budget", "Codex Desktop owner discovery exceeded budget; keeping previous catalogue");
      if (desktop.Owns(session.nativeId)) sessions.push_back(session);
      if (sessions.size() == 3) break;
    }
    return sessions;
  }
  Json Read(const Session& session, int limit) override {
    Authorize(session); HistoryReader reader(config_); const auto turns = RecentTurns(reader, session.nativeId, limit);
    Json messages = Json::array();
    for (const auto& turn : turns) for (const auto& item : turn.value("items", Json::array())) {
      const auto type = item.value("type", ""); std::string text;
      if (type == "userMessage") text = DialogueText(item.value("content", Json()));
      else if (type == "agentMessage") text = item.value("text", "");
      else continue;
      if (!text.empty()) messages.push_back({{"role", type == "userMessage" ? "user" : "assistant"}, {"text", text}});
    }
    return {{"messages", messages}, {"completeHistory", false}, {"scope", "recent_turns"}, {"limit", limit}};
  }
  Json Send(const Session& session, const std::string& requestId, const std::string& text) override {
    CheckProfile(session); DesktopConnection desktop(config_.endpoint);
    if (!desktop.Owns(session.nativeId)) throw Error("no_owner", "Target session has no Codex Desktop owner");
    desktop.RequireIdle();
    const Json input = Json::array({{{"type", "text"}, {"text", text}, {"text_elements", Json::array()}}});
    const Json start{{"request", {{"threadId", session.nativeId}, {"input", input}, {"clientUserMessageId", requestId}}},
      {"context", {{"inheritThreadSettings", true}}}};
    const auto reply = desktop.Call("thread-follower-start-turn", {{"turnStart", start}}, 2, true);
    const auto turn = reply.at("result").at("result").at("turn").at("id").get<std::string>();
    if (turn.empty()) throw Error("uncertain", "Codex receipt has no turn identity");
    return {{"status", "accepted"}, {"nativeRequestId", turn}};
  }
  Json Receipt(const Session& session, const Json& receipt) override {
    Authorize(session); HistoryReader reader(config_);
    for (const auto& turn : RecentTurns(reader, session.nativeId, 100)) if (turn.at("id") == receipt.at("nativeRequestId")) {
      const auto status = turn.value("status", ""); auto result = receipt;
      if (status == "completed" || status == "failed" || status == "interrupted") result["status"] = status;
      return result;
    }
    return receipt;
  }
 private:
  void CheckProfile(const Session& session) { if (session.profile != config_.directory.string()) throw Error("profile_changed", "Codex profile changed"); }
  void Authorize(const Session& session) { CheckProfile(session); DesktopConnection desktop(config_.endpoint); if (!desktop.Owns(session.nativeId)) throw Error("no_owner", "Target session has no Codex Desktop owner"); }
  ProviderConfig config_;
};
}
std::unique_ptr<Provider> MakeCodex(const ProviderConfig& config) { return std::make_unique<Codex>(config); }
}
