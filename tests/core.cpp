#include "callagent/Service.hpp"
#include "callagent/Files.hpp"
#include <atomic>
#include <functional>
#include <iostream>
#include <map>
#include <thread>
#include <unistd.h>

using namespace callagent;
namespace {
void Check(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
template<class F> void Reject(F fn, const std::string& code) {
  try { fn(); } catch (const Error& error) { Check(error.code == code, "Unexpected error " + error.code + ", expected " + code); return; }
  throw std::runtime_error("Expected rejection: " + code);
}
struct Temp {
  fs::path path = fs::canonical(fs::temp_directory_path()) / ("callagent-test-" + RandomHex(8));
  Temp() { PrivateDirectory(path); } ~Temp() { fs::remove_all(path); }
};
Session Example(ProviderKind kind, int number, std::int64_t activity = 1000) {
  return {kind, "profile", std::to_string(number), "本机会话标题超过十个字符", "/project", activity, "fixture"};
}
struct State { std::atomic<int> sends{0}; std::atomic<bool> fail{false}, uncertain{false}; std::atomic<int> base{0}; };
class FixtureProvider final : public Provider {
 public:
  FixtureProvider(ProviderKind kind, State& state) : kind_(kind), state_(state) {}
  ProviderKind Kind() const override { return kind_; }
  std::vector<Session> List() override {
    if (state_.fail) throw Error("fixture_unavailable", "Fixture offline");
    std::vector<Session> result; for (int n = 0; n < 5; ++n) result.push_back(Example(kind_, n + state_.base, 1000 + n)); return result;
  }
  Json Read(const Session& session, int) override { return {{"nativeId", session.nativeId}, {"messages", Json::array({{{"role", "user"}, {"text", "send this elsewhere"}}})}}; }
  Json Send(const Session&, const std::string&, const std::string&) override {
    ++state_.sends; if (state_.uncertain) throw Error("disconnected", "Receipt lost"); return {{"status", "accepted"}, {"nativeRequestId", "native"}};
  }
  Json Receipt(const Session&, const Json& row) override { auto result = row; result["status"] = "completed"; return result; }
 private:
  ProviderKind kind_; State& state_;
};
std::vector<std::unique_ptr<Provider>> Providers(State& state) {
  std::vector<std::unique_ptr<Provider>> providers;
  for (auto kind : {ProviderKind::Codex, ProviderKind::Claude, ProviderKind::WorkBuddy}) providers.push_back(std::make_unique<FixtureProvider>(kind, state));
  return providers;
}
Json Await(Service& service, const std::function<bool(const Json&)>& predicate) {
  const auto deadline = Clock::now() + std::chrono::seconds(5);
  while (Clock::now() < deadline) { auto snapshot = service.Handle({{"action", "status"}}); if (predicate(snapshot)) return snapshot; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
  throw std::runtime_error("Fixture service timeout");
}
void Configuration() {
  Temp temp; auto document = DefaultConfig();
  Check(document["providers"]["workbuddy"]["autoEnableDebugPort"] == true, "WorkBuddy supervision must default on");
  Check(!document["providers"]["workbuddy"].contains("executable"), "Default installation must remain discoverable");
  const auto file = temp.path / "config.json";
  auto& workbuddy = document["providers"]["workbuddy"];
  workbuddy["autoEnableDebugPort"] = false; workbuddy["executable"] = "/explicit/WorkBuddy.app/Contents/MacOS/Electron";
  AtomicWrite(file, document.dump());
  auto loaded = LoadConfig(file, "/bin/echo");
  Check(!loaded.providers.back().autoEnableDebugPort && loaded.providers.back().executable == workbuddy["executable"].get<std::string>(), "Explicit WorkBuddy settings changed");
  workbuddy.erase("autoEnableDebugPort"); AtomicWrite(file, document.dump());
  Check(LoadConfig(file, "/bin/echo").providers.back().autoEnableDebugPort, "Existing configuration did not gain the supervision default");
  workbuddy["executable"] = "relative/Electron"; AtomicWrite(file, document.dump());
  Reject([&] { LoadConfig(file, "/bin/echo"); }, "config");
  const auto installation = temp.path / "Applications/WorkBuddy.app/Contents/MacOS/Electron";
  fs::create_directories(installation.parent_path()); AtomicWrite(installation, "fixture");
#ifdef __APPLE__
  Check(ResolveWorkBuddyExecutable({}, temp.path) == installation, "User Applications discovery failed");
#endif
  const auto explicitFile = temp.path / "explicit"; AtomicWrite(explicitFile, "fixture");
  Check(ResolveWorkBuddyExecutable(explicitFile, temp.path) == explicitFile, "Automatic discovery overrode explicit installation");
  Reject([&] { ResolveWorkBuddyExecutable(temp.path / "missing"); }, "waiting_installation");
}
void Domain() {
  Check(ShortTitle("一二三四五六七八九十尾巴") == "一二三四五六七八九十", "CJK grapheme truncation");
  Check(ShortTitle("123456789👨‍👩‍👧‍👦尾") == "123456789👨‍👩‍👧‍👦", "Joined emoji was split");
  Check(ShortTitle("123456789e\xcc\x81tail") == "123456789e\xcc\x81", "Combining accent was split");
  Check(ShortTitle(" \n\t") == "未命名会话", "Empty title fallback");
  std::vector<Session> rows;
  for (auto kind : {ProviderKind::Codex, ProviderKind::Claude, ProviderKind::WorkBuddy}) for (int n = 4; n >= 0; --n) rows.push_back(Example(kind, n));
  rows.push_back(rows.front()); auto selected = SelectTargets(rows); Check(selected.size() == 9, "Per-provider quota");
  Check(selected[0].nativeId == "0" && selected[3].nativeId == "0", "Stable tie ordering");
  auto changed = selected[0]; const auto key = changed.Key(); changed.title = "Renamed"; changed.activityMs = 9999; Check(changed.Key() == key, "Title/rank changed identity");
  changed.profile += "2"; Check(changed.Key() != key, "Profile not included in identity");
  Check(SelectTargets({rows[0], rows[1]}).size() == 2, "Missing sessions must not be padded");
  auto label = Example(ProviderKind::Codex, 1); label.title = "一二三四五六七八九十尾巴";
  Check(Json(label)["displayLabel"] == "AT codex 一二三四五六七八九十" && Json(label)["title"] == label.title, "Candidate label lost full title or truncation");
}
fs::path LegacyPackage(const fs::path& root, const Session& session) {
  const auto directory = root / ("at-" + ToString(session.provider) + "-" + session.Key().substr(0, 40));
  fs::create_directories(directory / "scripts"); fs::create_directories(directory / "agents");
  const std::map<std::string, std::string> files{{"SKILL.md", "Legacy session Skill"}, {"target.json", Json(session).dump()},
    {"scripts/at", "#!/bin/sh\nlegacy command\n"}, {"agents/openai.yaml", "interface: {}\n"}};
  Json manifest{{"owner", "callagent-skills-v1"}, {"key", session.Key()}, {"files", Json::object()}};
  for (const auto& [name, bytes] : files) { AtomicWrite(directory / name, bytes); manifest["files"][name] = Sha256(bytes); }
  AtomicWrite(directory / ".callagent-owned.json", manifest.dump()); return directory;
}
void Publishing() {
  Temp temp; Config config; config.stateDirectory = temp.path / "state"; config.file = temp.path / "config.json"; config.executable = "/path/with ' quote/callagent";
  const auto root = config.stateDirectory / "generated/codex";
  fs::create_directories(root / "personal"); AtomicWrite(root / "personal/SKILL.md", "user content");
  fs::create_directories(root / "at-codex-personal"); AtomicWrite(root / "at-codex-personal/SKILL.md", "unowned AT");
  std::vector<fs::path> legacy;
  for (auto kind : {ProviderKind::Codex, ProviderKind::Claude, ProviderKind::WorkBuddy})
    for (int n = 0; n < 3; ++n) legacy.push_back(LegacyPackage(root, Example(kind, n)));
  Publisher publisher(config); auto result = publisher.Publish();
  Check(result[0]["status"] == "files_written", "Initial single Skill publish and migration");
  const auto directory = root / "at";
  for (const auto& old : legacy) Check(!fs::exists(old), "Unmodified legacy package was not removed");
  for (auto host : {"codex", "claude", "workbuddy"}) {
    const auto generated = config.stateDirectory / "generated" / host;
    std::size_t owned = 0;
    for (const auto& entry : fs::directory_iterator(generated)) if (fs::exists(entry.path() / ".callagent-owned.json")) ++owned;
    Check(owned == 1 && fs::exists(generated / "at/SKILL.md"), "Each host needs one AT even without sessions");
  }
  Check(!fs::exists(directory / "target.json"), "Shared Skill must not bind a session");
  Check(fs::is_regular_file(directory / "LICENSE") && fs::is_regular_file(directory / "NOTICE"), "Missing Skill license notices");
  const auto marker = directory / ".callagent-owned.json";
  const auto timestamp = fs::file_time_type::clock::now() - std::chrono::hours(1); fs::last_write_time(marker, timestamp);
  result = publisher.Publish(); Check(result[0]["status"] == "files_written", "Repeated publication failed");
  Check(fs::last_write_time(marker) == timestamp, "Unchanged Skill was rewritten");
  const auto helper = ReadFile(directory / "scripts/at");
  config.file = temp.path / "other config.json"; result = Publisher(config).Publish();
  Check(result[0]["status"] == "files_written" && ReadFile(directory / "scripts/at") != helper, "Configuration change did not update package");
  AtomicWrite(directory / "SKILL.md", "local modification"); result = publisher.Publish();
  Check(result[0]["status"] == "error" && ReadFile(directory / "SKILL.md") == "local modification", "Modified generated skill must be retained");
  Check(ReadFile(root / "personal/SKILL.md") == "user content" && ReadFile(root / "at-codex-personal/SKILL.md") == "unowned AT", "User Skill modified");
  const auto symlink = temp.path / "link"; fs::create_directory_symlink(root, symlink); Reject([&] { AtomicWrite(symlink / "new", "x"); }, "unsafe_path");
}
void PublicationConflicts() {
  for (const std::string scenario : {"edited", "added", "missing", "symlink", "collision", "identity"}) {
    Temp temp; Config config; config.stateDirectory = temp.path / "state"; config.file = temp.path / "config.json"; config.executable = "/bin/echo";
    const auto root = config.stateDirectory / "generated/codex";
    const auto old = LegacyPackage(root, Example(ProviderKind::Codex, 1));
    if (scenario == "edited") AtomicWrite(old / "SKILL.md", "user edit");
    if (scenario == "added") AtomicWrite(old / "notes.txt", "user note");
    if (scenario == "missing") fs::remove(old / "scripts/at");
    if (scenario == "symlink") {
      fs::remove(old / "scripts/at"); AtomicWrite(temp.path / "user-script", "keep me");
      fs::create_symlink(temp.path / "user-script", old / "scripts/at");
    }
    if (scenario == "collision") { fs::create_directories(root / "at"); AtomicWrite(root / "at/SKILL.md", "personal AT"); }
    if (scenario == "identity") {
      auto manifest = ReadJson(old / ".callagent-owned.json"); manifest["key"] = Example(ProviderKind::Codex, 2).Key();
      AtomicWrite(old / ".callagent-owned.json", manifest.dump());
    }
    auto result = Publisher(config).Publish();
    Check(result[0]["status"] == "error" && fs::exists(old / "SKILL.md"), "Unsafe migration changed legacy package: " + scenario);
    Check(!fs::exists(root / "at/.callagent-owned.json"), "Failed preflight changed destination: " + scenario);
    if (scenario == "collision") Check(ReadFile(root / "at/SKILL.md") == "personal AT", "Unowned AT overwritten");
    if (scenario == "edited") Check(ReadFile(old / "SKILL.md") == "user edit", "Legacy user edit lost");
    if (scenario == "added") Check(ReadFile(old / "notes.txt") == "user note", "Legacy user note lost");
    if (scenario == "symlink") Check(ReadFile(temp.path / "user-script") == "keep me", "Symlink destination changed");
  }
}
void Runtime() {
  Temp temp; Config config; config.stateDirectory = temp.path / "state"; config.file = temp.path / "config.json"; config.executable = "/bin/echo"; config.refreshSeconds = 3600;
  State state; std::string first;
  {
    Service service(config, Providers(state)); service.Start(); const auto initial = Await(service, [](const Json& row) { return row.at("targets").size() == 9; });
    first = initial["targets"][0]["key"];
    const auto marker = config.stateDirectory / "generated/codex/at/.callagent-owned.json";
    const auto timestamp = fs::file_time_type::clock::now() - std::chrono::hours(1); fs::last_write_time(marker, timestamp);
    Check(service.Handle({{"action", "read"}, {"target", first}}).at("historyIsData") == true, "Context trust marker"); Check(state.sends == 0, "Reading triggered sending");
    Json request{{"action", "send"}, {"target", first}, {"requestId", "operation-1"}, {"text", "hello"}};
    Reject([&] { service.Handle(request); }, "authorization"); request["authorized"] = true;
    auto receipt = service.Handle(request); Check(receipt["status"] == "accepted" && state.sends == 1, "Native receipt");
    service.Handle(request); Check(state.sends == 1, "Duplicate send"); request["text"] = "different"; Reject([&] { service.Handle(request); }, "request_conflict");
    Check(service.Handle({{"action", "receipt"}, {"target", first}, {"requestId", "operation-1"}})["status"] == "completed", "Receipt polling");
    request["text"] = "unknown result"; request["requestId"] = "operation-2"; state.uncertain = true;
    Check(service.Handle(request)["status"] == "uncertain", "Lost native acknowledgement"); service.Handle(request); Check(state.sends == 2, "Uncertain send was retried");
    state.base = 10; service.Handle({{"action", "refresh"}});
    Await(service, [&](const Json& row) { return row["targets"][0]["key"] != first; });
    Check(service.Handle({{"action", "read"}, {"target", first}})["nativeId"] == "4", "Selected target rebound to new rank");
    Check(fs::last_write_time(marker) == timestamp, "Ranking changes rewrote the shared Skill");
    state.fail = true; service.Handle({{"action", "refresh"}});
    const auto stale = Await(service, [](const Json& row) { return row["providers"]["workbuddy"]["stale"] == true; });
    Check(stale["targets"].size() == 9, "Temporary failure erased successful catalogue");
  }
  Service restarted(config, Providers(state));
  const auto repeated = restarted.Handle({{"action", "send"}, {"target", first}, {"requestId", "operation-2"}, {"text", "unknown result"}, {"authorized", true}});
  Check(repeated["status"] == "uncertain" && state.sends == 2, "Restart replayed uncertain send");
}
}
int main() {
  try { Configuration(); Domain(); Publishing(); PublicationConflicts(); Runtime(); std::cout << "Domain, publication and runtime invariants passed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
