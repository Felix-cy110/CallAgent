#include "callagent/Service.hpp"
#include "callagent/Files.hpp"
#include <csignal>
#include <hv/hlog.h>
#include <iostream>
#include <map>
#include <set>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace {
callagent::fs::path Executable(const char* argv0) {
#ifdef __APPLE__
  std::uint32_t size = 0; _NSGetExecutablePath(nullptr, &size); std::string path(size, '\0');
  if (_NSGetExecutablePath(path.data(), &size) == 0) return callagent::fs::canonical(path.c_str());
#endif
#ifdef __linux__
  return callagent::fs::canonical("/proc/self/exe");
#endif
  return callagent::fs::canonical(argv0);
}
void Help() {
  std::cout << "CallAgent 0.2.0 — local Desktop sessions\n"
    "  init [--config FILE]                      Create a configuration (no overwrite)\n"
    "  serve [--config FILE]                     Run the single local background service\n"
    "  status | list | refresh                   Inspect catalogue or schedule refresh\n"
    "  read --target KEY [--limit 20]            Read recent dialogue from an existing session\n"
    "  send --target KEY --authorize --request-id ID --text-file FILE\n"
    "  receipt --target KEY --request-id ID      Query delivery receipt; never resends\n"
    "  publish --host HOST --directory DIR      Configure a native Skill scan directory\n"
    "  install-claude-bridge                     Install the local text-delivery bridge\n"
    "Global: --config FILE. Hosts: codex, claude, workbuddy. Paths must be absolute.\n";
}
}
int main(int argc, char** argv) {
  using namespace callagent;
  std::signal(SIGPIPE, SIG_IGN);
  hlog_disable(); // Errors are reported at the CLI/service boundary; avoid implicit cwd log files.
  try {
    std::string action; std::map<std::string, std::string> options; bool authorized = false;
    const std::set<std::string> valued{"--config", "--target", "--limit", "--request-id", "--text-file", "--host", "--directory"};
    for (int i = 1; i < argc; ++i) {
      const std::string argument = argv[i];
      if (argument == "--help" || argument == "-h") { Help(); return 0; }
      if (argument == "--version") { std::cout << "0.2.0\n"; return 0; }
      if (argument == "--authorize") { authorized = true; continue; }
      if (valued.contains(argument)) {
        if (i + 1 >= argc || options.contains(argument)) throw Error("arguments", "Missing or duplicate option: " + argument); options[argument] = argv[++i]; continue;
      }
      if (action.empty() && !argument.starts_with('-')) action = argument;
      else throw Error("arguments", "Unknown argument: " + argument);
    }
    if (action.empty()) { Help(); return 0; }
    auto required = [&](const std::string& option) { if (!options.contains(option)) throw Error("arguments", "Required option: " + option); return options.at(option); };
    const auto file = options.contains("--config") ? fs::absolute(options.at("--config")).lexically_normal() : DefaultConfigPath();
    if (action == "init") {
      if (fs::exists(file)) throw Error("conflict", "Configuration already exists");
      RejectSymlinks(file); fs::create_directories(file.parent_path()); AtomicWrite(file, DefaultConfig().dump(2) + "\n"); std::cout << Json{{"config", file.string()}}.dump(2) << '\n'; return 0;
    }
    auto config = LoadConfig(file, Executable(argv[0]));
    if (action == "serve") { Serve(config); return 0; }
    if (action == "publish") {
      const auto host = ParseProvider(required("--host")); const fs::path root(required("--directory")); RejectSymlinks(root);
      auto document = ReadJson(file); auto& roots = document["publishRoots"];
      for (const auto& row : roots) if (row.at("directory") == root.string()) throw Error("conflict", "Publication directory already configured");
      roots.push_back({{"host", ToString(host)}, {"directory", root.string()}}); AtomicWrite(file, document.dump(2) + "\n");
      std::cout << Json{{"status", "configured"}, {"restartClientRequired", true}, {"menuVerified", false}}.dump(2) << '\n'; return 0;
    }
    if (action == "install-claude-bridge") {
      for (const auto& provider : config.providers) if (provider.kind == ProviderKind::Claude) InstallClaudeBridge(provider.directory);
      std::cout << Json{{"status", "files_installed"}, {"restartClientRequired", true}, {"desktopLoaded", false}}.dump(2) << '\n'; return 0;
    }
    Json request{{"action", action}};
    if (action == "read" || action == "send" || action == "receipt") request["target"] = required("--target");
    if (action == "read" && options.contains("--limit")) {
      std::size_t used = 0; const auto value = std::stoi(options.at("--limit"), &used); if (used != options.at("--limit").size()) throw Error("arguments", "Invalid read limit"); request["limit"] = value;
    }
    if (action == "send" || action == "receipt") request["requestId"] = required("--request-id");
    if (action == "send") { request["authorized"] = authorized; request["text"] = ReadFile(required("--text-file"), kMaxTextBytes); }
    if (action != "status" && action != "list" && action != "refresh" && action != "read" && action != "send" && action != "receipt") throw Error("arguments", "Unknown command: " + action);
    std::cout << ClientRequest(config, request).dump(2) << '\n'; return 0;
  } catch (const Error& error) { std::cerr << Json{{"error", {{"code", error.code}, {"message", error.what()}}}}.dump() << '\n'; }
  catch (const std::exception& error) { std::cerr << Json{{"error", {{"code", "failed"}, {"message", error.what()}}}}.dump() << '\n'; }
  return 1;
}
