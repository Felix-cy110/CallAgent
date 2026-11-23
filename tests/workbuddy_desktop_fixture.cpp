#include <hv/HttpServer.h>
#include <hv/hlog.h>
#include <hv/json.hpp>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>
using Json = nlohmann::json;
namespace fs = std::filesystem;
namespace { volatile std::sig_atomic_t signals = 0; void Terminate(int) { signals = 1; } }
int main(int argc, char** argv) {
  (void)argc; hlog_disable();
  const char* fixtureRoot = std::getenv("CALLAGENT_FIXTURE_ROOT");
  if (!fixtureRoot) return 2;
  const fs::path root(fixtureRoot), executable = fs::canonical(argv[0]);
  const char* value = std::getenv("WORKBUDDY_REMOTE_DEBUGGING_PORT");
  const int port = value ? std::stoi(value) : 0;
  auto log = [&](Json row) { row["pid"] = getpid(); std::ofstream(root / "events.jsonl", std::ios::app) << row.dump() << '\n'; };
  std::signal(SIGTERM, Terminate); std::signal(SIGINT, Terminate);
  log({{"event", "launch"}, {"port", port}});
  if (port && fs::exists(root / "replacement-exits")) return 0;
  hv::HttpService router;
  router.GET("/json/list", [&](HttpRequest*, HttpResponse* response) {
    auto path = (executable.parent_path() / "../Resources/app.asar/renderer/index.html").lexically_normal().string();
    for (std::size_t pos = 0; (pos = path.find(' ', pos)) != std::string::npos; pos += 3) path.replace(pos, 1, "%20");
    response->body = Json::array({{{"type", "page"}, {"title", fs::exists(root / "wrong-renderer") ? "Other" : "WorkBuddy"},
      {"url", "file://" + path + "?fixture=true#fragment"}}}).dump();
    return 200;
  });
  hv::HttpServer server; std::strcpy(server.host, "127.0.0.1"); server.port = port; server.service = &router;
  if (port && server.start() != 0) return 3;
  while (!fs::exists(root / ("exit-" + std::to_string(getpid())))) {
    if (signals) {
      signals = 0; log({{"event", "terminate"}});
      if (!fs::exists(root / "hold-exit")) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  if (port) server.stop();
  log({{"event", "exit"}});
  return 0;
}
