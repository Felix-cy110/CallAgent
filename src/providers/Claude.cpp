#include "callagent/Provider.hpp"
#include <cctype>
#include "callagent/Files.hpp"
#include "callagent/NativeChannel.hpp"
#include "ClaudeBundle.hpp"
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <charconv>
#include <map>
#include <regex>
#include <set>

namespace callagent {
namespace {
constexpr char kBridge[] = "callagent-desktop-bridge";
bool Id(const std::string& id) { return std::regex_match(id, std::regex("[a-fA-F0-9]{8}(-[a-fA-F0-9]{4}){3}-[a-fA-F0-9]{12}")); }
bool Hex64(const std::string& value) { return std::regex_match(value, std::regex("[a-f0-9]{64}")); }
Json SafeJson(const fs::path& path, std::size_t limit = 16384) { RejectSymlinks(path); return ReadJson(path, limit); }
Json Registration(const fs::path& root, const std::string& id) {
  if (!Id(id)) throw Error("identity", "Invalid Claude session identity");
  const auto directory = root / "sessions"; RejectSymlinks(directory); Json match; std::size_t count = 0;
  if (!fs::is_directory(directory)) throw Error("unavailable", "Claude Desktop session registry is unavailable");
  for (const auto& entry : fs::directory_iterator(directory)) {
    if (++count > 1024) throw Error("size_limit", "Claude registry exceeds limit");
    if (entry.path().extension() != ".json") continue;
    Json row; try { row = SafeJson(entry.path()); } catch (const std::exception&) { throw Error("registry_changing", "Claude registry is incomplete; retain the previous catalogue"); }
    const auto origin = row.value("entrypoint", ""); const auto pid = row.value("pid", 0);
    if (row.value("sessionId", "") != id || (origin != "claude-desktop" && origin != "claude-desktop-3p") ||
        !row.value("hostSessionId", "").starts_with("local_") || pid <= 0 || kill(pid, 0) != 0) continue;
    if (!match.is_null()) throw Error("identity", "Claude session has multiple live owners");
    match = row;
  }
  if (match.is_null()) throw Error("unavailable", "Open this existing session in Claude Desktop Code");
  return match;
}
fs::path Transcript(const fs::path& root, const Json& row) {
  const std::string id = row.at("sessionId"); const auto projects = root / "projects"; RejectSymlinks(projects);
  if (!fs::is_directory(projects)) throw Error("unavailable", "Claude transcript directory is unavailable");
  fs::path found; std::size_t count = 0;
  for (const auto& entry : fs::directory_iterator(projects)) {
    if (++count > 1024) throw Error("size_limit", "Claude project directory exceeds limit");
    RejectSymlinks(entry.path()); if (!entry.is_directory()) continue;
    const auto candidate = entry.path() / (id + ".jsonl"); RejectSymlinks(candidate);
    if (!fs::is_regular_file(candidate)) continue;
    if (!found.empty()) throw Error("identity", "Claude transcript is ambiguous"); found = candidate;
  }
  if (found.empty()) throw Error("unavailable", "Claude transcript is not yet available"); return found;
}
Json Lease(const fs::path& root, const std::string& id) {
  const auto control = root / kBridge; const auto lease = SafeJson(control / "connection.json");
  const auto heartbeat = SafeJson(control / "heartbeats" / (id + ".json")); const auto now = NowMs();
  const auto version = heartbeat.value("engineVersion", ""); unsigned patch = 0;
  bool compatible = version.starts_with("2.1.");
  if (compatible) { const auto value = version.substr(4); const auto parsed = std::from_chars(value.data(), value.data() + value.size(), patch);
    compatible = parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && patch >= 309; }
  if (!compatible || lease.value("version", "") != "1" || lease.value("bundle", "") != claude_bundle::kId ||
      !Hex64(lease.value("challenge", "")) || lease.value("expiresAt", 0LL) <= now || lease.value("expiresAt", 0LL) > now + 20000 ||
      heartbeat.value("version", "") != "1" || heartbeat.value("bundle", "") != claude_bundle::kId ||
      heartbeat.value("challenge", "") != lease.at("challenge").get<std::string>() || heartbeat.value("sessionId", "") != id ||
      heartbeat.value("observedAt", 0LL) < now - 20000 || heartbeat.value("observedAt", 0LL) > now + 2000)
    throw Error("plugin_not_ready", "Claude Desktop bridge is not loaded or its heartbeat has expired");
  return lease;
}
class Claude final : public Provider {
 public:
  explicit Claude(ProviderConfig config) : config_(std::move(config)) {}
  ProviderKind Kind() const override { return ProviderKind::Claude; }
  std::vector<Session> List() override {
    const auto directory = config_.directory / "sessions"; RejectSymlinks(directory);
    if (!fs::is_directory(directory)) throw Error("unavailable", "Claude Desktop registry is unavailable");
    std::set<std::string> ids; std::vector<Session> result; std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(directory)) {
      if (++count > 1024) throw Error("size_limit", "Claude registry exceeds limit");
      if (entry.path().extension() != ".json") continue;
      Json row; try { row = SafeJson(entry.path()); } catch (const std::exception&) { throw Error("registry_changing", "Claude registry is incomplete; retain the previous catalogue"); }
      const auto id = row.value("sessionId", ""); if (!Id(id) || !ids.insert(id).second) continue;
      try {
        const auto native = Registration(config_.directory, id); std::int64_t activity = 0;
        try { const auto stamp = fs::last_write_time(Transcript(config_.directory, native)); activity = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::file_clock::to_sys(stamp).time_since_epoch()).count(); }
        catch (const Error& e) { if (e.code != "unavailable") throw; }
        result.push_back({Kind(), config_.directory.string(), id, native.value("title", ""), native.at("cwd"), activity, "transcript.mtime"});
      } catch (const Error& e) { if (e.code != "unavailable") throw; }
    }
    return result;
  }
  Json Read(const Session& session, int limit) override {
    const auto native = Authorize(session); const auto path = Transcript(config_.directory, native); RejectSymlinks(path);
    const auto bytes = ReadFile(path, 24 * 1024 * 1024); std::map<std::string, Json> records; std::string latest;
    for (std::size_t offset = 0; offset < bytes.size();) {
      const auto end = bytes.find('\n', offset); if (end == std::string::npos) break; // An unfinished final native write is not a record.
      const auto row = Json::parse(bytes.substr(offset, end - offset)); offset = end + 1;
      if (row.value("sessionId", session.nativeId) != session.nativeId) throw Error("identity", "Claude transcript identity mismatch");
      if (row.value("isSidechain", false) || !row.contains("uuid") || !row.contains("parentUuid")) continue;
      latest = row.at("uuid"); records[latest] = row;
    }
    Json messages = Json::array(); std::set<std::string> seen;
    while (!latest.empty() && records.contains(latest) && messages.size() < static_cast<std::size_t>(limit)) {
      if (!seen.insert(latest).second) throw Error("history", "Claude transcript ancestry cycle");
      const auto& row = records.at(latest); const auto role = row.value("type", "");
      if (!row.value("isMeta", false) && (role == "user" || role == "assistant")) {
        const auto text = DialogueText(row.value("message", Json::object()).value("content", Json()));
        if (!text.empty()) messages.push_back({{"role", role}, {"text", text}});
      }
      latest = row.value("parentUuid", Json()).is_string() ? row.at("parentUuid").get<std::string>() : "";
    }
    std::reverse(messages.begin(), messages.end()); Authorize(session);
    return {{"messages", messages}, {"completeHistory", false}, {"scope", "recent_messages"}, {"limit", limit}};
  }
  Json Send(const Session& session, const std::string& requestId, const std::string& text) override {
    const auto row = Authorize(session), lease = Lease(config_.directory, session.nativeId);
    if (row.value("status", "") != "idle") throw Error("busy", "Claude Desktop session is busy");
    const auto control = config_.directory / kBridge; const auto key = Sha256(session.Key() + requestId);
    const auto request = control / "requests" / (key + ".json");
    if (fs::exists(request)) throw Error("uncertain", "Claude request already exists; inspect its receipt");
    const auto endpoint = row.at("messagingSocketPath").get<std::string>(); struct stat socketInfo{};
    if (lstat(endpoint.c_str(), &socketInfo) || !S_ISSOCK(socketInfo.st_mode) || socketInfo.st_uid != getuid()) throw Error("peer", "Claude IPC ownership mismatch");
    std::string lower = endpoint; std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto credentials = SafeJson(config_.directory / "sessions" / (std::to_string(row.at("pid").get<int>()) + "." + Sha256(lower) + ".key"));
    const auto peerToken = credentials.value("peerToken", "");
    if (!std::regex_match(peerToken, std::regex("[a-f0-9]{32}")) ||
        (credentials.contains("procStart") && credentials.at("procStart") != row.value("procStart", Json()))) throw Error("peer", "Claude credential mismatch");
    auto socket = NativeChannel::Socket(endpoint, false); const auto token = RandomHex();
    const Json frame{{"id", key}, {"token", token}, {"challenge", lease.at("challenge")}, {"sessionId", session.nativeId}};
    Json document = frame; document.update({{"text", text}, {"digest", Sha256(text)}, {"expiresAt", NowMs() + 20000}});
    AtomicWrite(request, document.dump()); const auto deadline = Clock::now() + std::chrono::seconds(5);
    socket->Send({{"type", "auth"}, {"token", peerToken}}, deadline);
    auto rawId = RandomHex(16); rawId[12] = '4'; rawId[16] = '8';
    const auto uuid = rawId.substr(0, 8) + "-" + rawId.substr(8, 4) + "-" + rawId.substr(12, 4) + "-" + rawId.substr(16, 4) + "-" + rawId.substr(20);
    socket->Send({{"type", "user"}, {"session_id", session.nativeId}, {"uuid", uuid}, {"msg_id", uuid}, {"from", "callagent"}, {"priority", "next"},
      {"message", {{"role", "user"}, {"content", "CALLAGENT_SESSION_V1:" + frame.dump()}}}}, deadline);
    return {{"status", "submitted"}, {"nativeRequestId", key}};
  }
  Json Receipt(const Session& session, const Json& receipt) override {
    const auto key = receipt.at("nativeRequestId").get<std::string>(); if (!Hex64(key)) throw Error("identity", "Invalid Claude receipt key");
    const auto path = config_.directory / kBridge / "receipts" / (key + ".json");
    if (!fs::exists(path)) return receipt;
    const auto row = SafeJson(path);
    if (row.at("version") != "1" || row.at("id") != key || row.at("sessionId") != session.nativeId) throw Error("identity", "Claude receipt identity mismatch");
    auto result = receipt; const auto status = row.at("status").get<std::string>();
    if (status == "accepted" || status == "rejected" || status == "uncertain") result["status"] = status;
    return result;
  }
 private:
  Json Authorize(const Session& session) {
    if (session.profile != config_.directory.string()) throw Error("profile_changed", "Claude configuration changed");
    auto row = Registration(config_.directory, session.nativeId);
    if (row.at("cwd") != session.cwd) throw Error("identity", "Claude session directory changed"); return row;
  }
  ProviderConfig config_;
};
}
std::unique_ptr<Provider> MakeClaude(const ProviderConfig& config) { return std::make_unique<Claude>(config); }
void InstallClaudeBridge(const fs::path& root) {
  const auto plugin = root / "skills" / kBridge; RejectSymlinks(plugin);
  const auto control = root / kBridge; RejectSymlinks(control);
  if (fs::exists(control) && (!fs::exists(control / ".callagent-owner") || ReadFile(control / ".callagent-owner") != "claude-bridge-v1"))
    throw Error("conflict", "Claude bridge control directory is not owned by CallAgent");
  if (fs::exists(plugin) && (!fs::exists(plugin / ".callagent-owner") || ReadFile(plugin / ".callagent-owner") != "claude-bridge-v1")) throw Error("conflict", "Claude bridge directory is not owned by CallAgent");
  PrivateDirectory(plugin); AtomicWrite(plugin / ".callagent-owner", "claude-bridge-v1");
  PrivateDirectory(plugin / ".claude-plugin"); PrivateDirectory(plugin / "hooks");
  AtomicWrite(plugin / ".claude-plugin/plugin.json", Json{{"name", kBridge}, {"version", "0.1.0"}, {"description", "Local session text delivery"}, {"license", "MIT"}}.dump(2));
  AtomicWrite(plugin / "hooks/register.js", claude_bundle::kScript);
  AtomicWrite(plugin / "hooks/hooks.json", Json{{"modules", Json::array({"./register.js"})}}.dump());
  for (const auto* child : {"", "heartbeats", "requests", "receipts"}) PrivateDirectory(root / kBridge / child);
  AtomicWrite(root / kBridge / ".callagent-owner", "claude-bridge-v1");
}
void RenewClaudeBridge(const fs::path& root, const std::string& challenge) {
  const auto control = root / kBridge;
  if (!fs::exists(control / ".callagent-owner")) return;
  if (ReadFile(control / ".callagent-owner") != "claude-bridge-v1") throw Error("conflict", "Claude bridge control ownership mismatch");
  AtomicWrite(control / "connection.json", Json{{"version", "1"}, {"bundle", claude_bundle::kId}, {"challenge", challenge}, {"expiresAt", NowMs() + 20000}}.dump());
}
}
