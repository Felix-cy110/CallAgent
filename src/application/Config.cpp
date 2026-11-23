#include "callagent/Config.hpp"
#include "callagent/Files.hpp"
#include <cstdlib>
#include <set>

namespace callagent {
namespace {
fs::path Home() { const auto* home = std::getenv("HOME"); if (!home || !*home) throw Error("config", "HOME is required"); return fs::weakly_canonical(home); }
fs::path EnvironmentPath(const char* name, const fs::path& fallback) { const auto* value = std::getenv(name); return value && *value ? fs::weakly_canonical(value) : fallback; }
fs::path Absolute(const Json& value) {
  const fs::path path(value.get<std::string>());
  if (!path.is_absolute()) throw Error("config", "Configuration paths must be absolute");
  return path.lexically_normal();
}
unsigned short Port(const Json& value) { const auto port = value.get<int>(); if (port < 1 || port > 65535) throw Error("config", "Port must be 1..65535"); return static_cast<unsigned short>(port); }
}
fs::path DefaultConfigPath() { return Home() / ".callagent/config.json"; }
Json DefaultConfig() {
  const auto home = Home();
  const auto codex = EnvironmentPath("CODEX_HOME", home / ".codex");
  const auto claude = EnvironmentPath("CLAUDE_CONFIG_DIR", home / ".claude");
  const fs::path bundled = "/Applications/Codex.app/Contents/Resources/codex";
  return {{"version", 1}, {"stateDirectory", (home / ".callagent/runtime").string()}, {"port", 47831}, {"refreshSeconds", 30},
    {"providers", {
      {"codex", {{"enabled", true}, {"directory", codex.string()}, {"executable", fs::exists(bundled) ? bundled.string() : "codex"}, {"endpoint", (codex / "ipc/ipc.sock").string()}}},
      {"claude", {{"enabled", true}, {"directory", claude.string()}}},
      {"workbuddy", {{"enabled", true}, {"directory", (home / ".workbuddy").string()}, {"debugPort", 50641}, {"autoEnableDebugPort", true}}}
    }}, {"publishRoots", Json::array()}};
}
Config LoadConfig(const fs::path& file, const fs::path& executable) {
  const auto document = ReadJson(file, 64 * 1024);
  if (document.at("version") != 1) throw Error("config", "Unsupported config version");
  Config config; config.file = fs::absolute(file).lexically_normal(); config.executable = fs::canonical(executable);
  config.stateDirectory = Absolute(document.at("stateDirectory")); config.port = Port(document.at("port"));
  config.refreshSeconds = document.at("refreshSeconds");
  if (config.refreshSeconds < 5 || config.refreshSeconds > 3600) throw Error("config", "refreshSeconds must be 5..3600");
  for (const auto kind : {ProviderKind::Codex, ProviderKind::Claude, ProviderKind::WorkBuddy}) {
    const auto& row = document.at("providers").at(ToString(kind));
    ProviderConfig provider; provider.kind = kind; provider.enabled = row.at("enabled"); provider.directory = Absolute(row.at("directory"));
    provider.executable = row.value("executable", "");
    if (row.contains("endpoint")) provider.endpoint = Absolute(row.at("endpoint"));
    if (row.contains("debugPort")) provider.debugPort = Port(row.at("debugPort"));
    if (kind == ProviderKind::WorkBuddy) {
      provider.autoEnableDebugPort = row.value("autoEnableDebugPort", true);
      if (!provider.executable.empty() && !provider.executable.is_absolute()) throw Error("config", "WorkBuddy executable must be an absolute path");
    }
    config.providers.push_back(provider);
  }
  std::set<fs::path> unique;
  for (const auto& row : document.at("publishRoots")) {
    const auto directory = Absolute(row.at("directory"));
    if (!unique.insert(directory).second) throw Error("config", "Duplicate publication directory");
    config.publishRoots.push_back({ParseProvider(row.at("host")), directory});
  }
  return config;
}
}
