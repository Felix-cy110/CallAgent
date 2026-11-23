#include "callagent/Publisher.hpp"
#include "callagent/Files.hpp"
#include "SkillBundle.hpp"
#include <map>
#include <set>
#include <cstdio>
#ifdef __linux__
#include <sys/syscall.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace callagent {
namespace {
constexpr char kOwnership[] = ".callagent-owned.json";
constexpr char kOwner[] = "callagent-skills-v2";
std::string Quote(const std::string& text) {
  std::string value = "'"; for (char c : text) value += c == '\'' ? "'\\''" : std::string(1, c); return value + "'";
}
Json CheckOwned(const fs::path& directory, const std::string& owner) {
  RejectSymlinks(directory);
  const auto manifest = ReadJson(directory / kOwnership, 64 * 1024);
  if (manifest.at("owner") != owner) throw Error("conflict", "Skill is not owned by CallAgent: " + directory.string());
  const auto& files = manifest.at("files");
  if (!files.is_object() || files.empty()) throw Error("conflict", "Generated skill manifest has no files");
  std::set<fs::path> directories;
  for (auto it = files.begin(); it != files.end(); ++it) {
    const fs::path relative(it.key());
    if (relative.empty() || relative.is_absolute() || relative.lexically_normal() != relative || *relative.begin() == ".." ||
        !it.value().is_string() || !fs::is_regular_file(directory / relative)) throw Error("conflict", "Generated skill manifest is incomplete");
    for (auto parent = relative.parent_path(); !parent.empty(); parent = parent.parent_path()) directories.insert(parent);
  }
  for (const auto& entry : fs::recursive_directory_iterator(directory)) {
    RejectSymlinks(entry.path());
    const auto relative = entry.path().lexically_relative(directory);
    if (entry.is_directory() && directories.contains(relative)) continue;
    if (relative == kOwnership) continue;
    const auto name = relative.generic_string();
    if (!files.contains(name) || !entry.is_regular_file() || Sha256(ReadFile(entry.path())) != files.at(name).get<std::string>())
      throw Error("conflict", "Generated skill has local changes: " + entry.path().string());
  }
  return manifest;
}
std::map<std::string, std::string> Content(const Config& config) {
  const auto invoke = Quote(config.executable.string()) + " --config " + Quote(config.file.string());
  std::string skill = skill_bundle::kSkill;
  const std::string prefix = "callagent ";
  for (std::size_t at = 0; (at = skill.find(prefix, at)) != std::string::npos;) {
    skill.replace(at, prefix.size(), invoke + " "); at += invoke.size() + 1;
  }
  const auto script = "#!/bin/sh\nexec " + invoke + " \"$@\"\n";
  return {{"SKILL.md", skill}, {"scripts/at", script}, {"agents/openai.yaml", skill_bundle::kMetadata},
    {"LICENSE", skill_bundle::kLicense}, {"NOTICE", skill_bundle::kNotice}};
}
std::vector<fs::path> LegacyPackages(const fs::path& root) {
  std::vector<fs::path> packages;
  for (const auto& entry : fs::directory_iterator(root)) {
    const auto name = entry.path().filename().string();
    if (!name.starts_with("at-codex-") && !name.starts_with("at-claude-") && !name.starts_with("at-workbuddy-")) continue;
    if (!entry.is_directory() || !fs::exists(entry.path() / kOwnership)) continue;
    const auto manifest = CheckOwned(entry.path(), "callagent-skills-v1");
    const auto key = manifest.at("key").get<std::string>();
    bool matches = false;
    if (key.size() == 64 && key.find_first_not_of("0123456789abcdef") == std::string::npos) {
      for (auto kind : {ProviderKind::Codex, ProviderKind::Claude, ProviderKind::WorkBuddy})
        if (name == "at-" + ToString(kind) + "-" + key.substr(0, 40)) matches = true;
    }
    if (!matches) throw Error("conflict", "Legacy Skill identity mismatch: " + entry.path().string());
    packages.push_back(entry.path());
  }
  return packages;
}
}
void Publisher::PublishRoot(const callagent::PublishRoot& root) const {
  RejectSymlinks(root.directory); fs::create_directories(root.directory);
  const auto target = root.directory / "at"; RejectSymlinks(target);
  // Validate migration and the destination before changing any package in this root.
  const auto legacy = LegacyPackages(root.directory);
  const auto files = Content(config_);
  Json manifest{{"owner", kOwner}, {"skill", "at"}, {"files", Json::object()}};
  for (const auto& [path, bytes] : files) manifest["files"][path] = Sha256(bytes);
  const bool exists = fs::exists(target);
  Json previous;
  if (exists) {
    previous = CheckOwned(target, kOwner);
    if (previous.at("skill") != "at") throw Error("conflict", "Generated Skill identity collision");
  }
  if (previous != manifest) {
    const auto staging = root.directory / (".callagent-stage-" + RandomHex(8));
    PrivateDirectory(staging); PrivateDirectory(staging / "agents"); PrivateDirectory(staging / "scripts");
    try {
      for (const auto& [path, bytes] : files) AtomicWrite(staging / path, bytes, path == "scripts/at");
      AtomicWrite(staging / kOwnership, manifest.dump(2));
      if (!exists) fs::rename(staging, target);
      else {
        // Exchange complete packages atomically; a crash cannot leave half a manifest.
#ifdef __APPLE__
        if (renamex_np(staging.c_str(), target.c_str(), RENAME_SWAP)) throw Error("publish", "Cannot atomically replace Skill package");
#elif defined(__linux__)
        if (syscall(SYS_renameat2, AT_FDCWD, staging.c_str(), AT_FDCWD, target.c_str(), RENAME_EXCHANGE)) throw Error("publish", "Cannot atomically replace Skill package");
#else
        throw Error("unsupported_platform", "Atomic Skill directory exchange is unavailable");
#endif
        fs::remove_all(staging);
      }
    } catch (...) { fs::remove_all(staging); throw; }
  }
  for (const auto& directory : legacy) {
    CheckOwned(directory, "callagent-skills-v1"); fs::remove_all(directory);
  }
}
Json Publisher::Publish() const {
  auto roots = config_.publishRoots;
  for (auto host : {ProviderKind::Codex, ProviderKind::Claude, ProviderKind::WorkBuddy}) roots.push_back({host, config_.stateDirectory / "generated" / ToString(host)});
  Json results = Json::array();
  for (const auto& root : roots) {
    try { PublishRoot(root); results.push_back({{"host", ToString(root.host)}, {"directory", root.directory.string()}, {"status", "files_written"}}); }
    catch (const std::exception& error) { results.push_back({{"host", ToString(root.host)}, {"directory", root.directory.string()}, {"status", "error"}, {"error", error.what()}}); }
  }
  return results;
}
}
