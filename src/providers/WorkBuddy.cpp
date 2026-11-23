#include "callagent/Provider.hpp"
#include "callagent/WorkBuddyRpc.hpp"
#include "callagent/WorkBuddyRepository.hpp"
#include "callagent/WorkBuddyDesktop.hpp"
#include <algorithm>

namespace callagent {
namespace {
std::string NativeRequestId() {
  auto value = RandomHex(16); auto timestamp = static_cast<std::uint64_t>(NowMs());
  for (int i = 11; i >= 0; --i) { value[i] = "0123456789abcdef"[timestamp & 15]; timestamp >>= 4; }
  value[12] = '7'; value[16] = '8'; return value;
}
bool Running(const Json& row) { const auto state = row.value("state", ""); return state != "completed" && state != "cancelled" && state != "error"; }
class WorkBuddy final : public Provider {
 public:
  explicit WorkBuddy(ProviderConfig config) : config_(std::move(config)), repository_(config_.directory) {}
  ProviderKind Kind() const override { return ProviderKind::WorkBuddy; }
  std::vector<Session> List() override {
    const auto rows = repository_.List(); CheckAccount(); return rows;
  }
  Json Read(const Session& session, int limit) override {
    Authorize(session); const auto rows = Requests(session.nativeId); Json messages = Json::array();
    const auto begin = rows.size() > static_cast<std::size_t>(limit) ? rows.size() - limit : 0;
    for (auto i = begin; i < rows.size(); ++i) {
      const auto& row = rows.at(i);
      const auto user = DialogueText(row.value("userMessage", Json::object()).value("content", Json()));
      if (!user.empty()) messages.push_back({{"role", "user"}, {"text", user}});
      const auto assistant = row.value("assistantMessage", Json());
      if (assistant.is_object()) { const auto text = DialogueText(assistant.value("content", Json())); if (!text.empty()) messages.push_back({{"role", "assistant"}, {"text", text}}); }
    }
    repository_.Validate(session); CheckAccount();
    return {{"messages", messages}, {"completeHistory", false}, {"scope", "recent_requests"}, {"limit", limit}};
  }
  Json Send(const Session& session, const std::string& requestId, const std::string& text) override {
    Authorize(session);
    for (const auto& row : Requests(session.nativeId)) if (Running(row)) throw Error("busy", "WorkBuddy session is running");
    if (!Native("wb:conversations:getPendingInfo", Json::array({session.nativeId})).is_null()) throw Error("busy", "WorkBuddy is awaiting input");
    const auto nativeId = NativeRequestId();
    const Json options{{"clientRequestId", requestId}, {"emitUserMessage", true}, {"_meta", {{"codebuddy.ai/conversationRequestId", nativeId}}}};
    Json response;
    try { response = Rpc().Call("invokeSendPrompt", {{"conversationId", session.nativeId}, {"blocks", Json::array({{{"type", "text"}, {"text", text}}})}, {"options", options}}); }
    catch (...) { rpc_.reset(); throw; } // The caller records uncertainty; this method never retries.
    const auto data = response.value("data", Json::object());
    if (!response.value("ok", false) || data.value("conversationId", "") != session.nativeId || data.contains("newConversationId")) throw Error("uncertain", "WorkBuddy did not confirm the existing conversation");
    const auto result = data.value("result", Json());
    return {{"status", result.is_object() && result.contains("errorCode") ? "rejected" : "admitted"}, {"nativeRequestId", nativeId}, {"clientRequestId", requestId}};
  }
  Json Receipt(const Session& session, const Json& receipt) override {
    Authorize(session);
    for (const auto& row : Requests(session.nativeId)) if (row.value("id", "") == receipt.at("nativeRequestId").get<std::string>()) {
      if (row.value("clientRequestId", "") != receipt.at("clientRequestId").get<std::string>()) throw Error("identity", "WorkBuddy receipt request identity mismatch");
      auto result = receipt; const auto state = row.value("state", "");
      result["status"] = state == "completed" ? "completed" : state == "cancelled" ? "interrupted" : state == "error" ? "failed" : "accepted"; return result;
    }
    return receipt;
  }
 private:
  WorkBuddyRpc& Rpc() {
    auto resolved = config_; resolved.executable = ResolveWorkBuddyExecutable(config_.executable);
    if (rpc_ && resolved.executable != connectedExecutable_) rpc_.reset();
    if (!rpc_) { rpc_ = std::make_unique<WorkBuddyRpc>(resolved); connectedExecutable_ = resolved.executable; }
    return *rpc_;
  }
  Json Native(const std::string& channel, const Json& args) {
    try { return Rpc().Native(channel, args); } catch (...) { rpc_.reset(); throw; }
  }
  void CheckAccount() {
    const auto account = repository_.Account(); const auto profile = Native("wb:account:profile", Json::array());
    if (profile.value("uid", "") != account) { rpc_.reset(); throw Error("account_changed", "WorkBuddy RPC account differs from its local catalogue"); }
  }
  void Authorize(const Session& session) {
    repository_.Validate(session); CheckAccount(); const auto response = Rpc().Call("invokeGetInfo", {{"conversationId", session.nativeId}});
    if (!response.value("ok", false)) throw Error("unavailable", "WorkBuddy conversation is unavailable");
    const auto info = response.at("data").at("info");
    if (info.at("id") != session.nativeId || info.value("transport", "") != "local") throw Error("identity", "WorkBuddy conversation identity mismatch");
    const auto space = info.value("space", Json::object()); if (space.contains("cwd") && space.at("cwd") != session.cwd) throw Error("identity", "WorkBuddy conversation directory mismatch");
  }
  Json Requests(const std::string& id) {
    const auto data = Native("wb:conversations:requests", Json::array({id, {{"limit", 100}, {"resultScope", "page"}, {"byteLength", 2 * 1024 * 1024}}}));
    if (!data.at("items").is_array()) throw Error("protocol", "WorkBuddy returned invalid history"); return data.at("items");
  }
  ProviderConfig config_; WorkBuddyRepository repository_; std::unique_ptr<WorkBuddyRpc> rpc_; fs::path connectedExecutable_;
};
}
std::unique_ptr<Provider> MakeWorkBuddy(const ProviderConfig& config) { return std::make_unique<WorkBuddy>(config); }
}
