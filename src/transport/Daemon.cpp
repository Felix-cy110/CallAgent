#include "callagent/Service.hpp"
#include "callagent/Files.hpp"
#include <hv/HttpServer.h>
#include <hv/HttpClient.h>
#include <openssl/crypto.h>
#include <csignal>
#include <cstring>
#include <iostream>
#include <thread>

namespace callagent {
namespace {
volatile std::sig_atomic_t stopped = 0;
void StopSignal(int) { stopped = 1; }
}
void Serve(const Config& config) {
  PrivateDirectory(config.stateDirectory); FileLock lock(config.stateDirectory / "daemon.lock");
  const auto token = RandomHex(); Service service(config); hv::HttpService router;
  router.POST("/v1/request", [&](HttpRequest* request, HttpResponse* response) {
    response->SetHeader("Content-Type", "application/json"); response->SetHeader("Cache-Control", "no-store");
    const auto supplied = request->GetHeader("Authorization"), expected = "Bearer " + token;
    if (!request->GetHeader("Origin").empty() || supplied.size() != expected.size() || CRYPTO_memcmp(supplied.data(), expected.data(), expected.size())) {
      response->status_code = HTTP_STATUS_UNAUTHORIZED; response->body = R"({"ok":false,"error":{"code":"authorization","message":"Local Client authorization required"}})"; return response->status_code;
    }
    try {
      if (request->body.size() > kMaxTextBytes + 16384) throw Error("size_limit", "Request exceeds limit");
      const auto result = service.Handle(Json::parse(request->body)); response->body = Json{{"ok", true}, {"result", result}}.dump();
    } catch (const Error& error) { response->status_code = HTTP_STATUS_BAD_REQUEST; response->body = Json{{"ok", false}, {"error", {{"code", error.code}, {"message", error.what()}}}}.dump(); }
    catch (const std::exception& error) { response->status_code = HTTP_STATUS_BAD_REQUEST; response->body = Json{{"ok", false}, {"error", {{"code", "request_failed"}, {"message", error.what()}}}}.dump(); }
    return response->status_code;
  });
  hv::HttpServer server; std::strcpy(server.host, "127.0.0.1"); server.port = config.port; server.service = &router; server.worker_threads = 4;
  if (server.start() != 0) throw Error("listen", "Cannot bind CallAgent loopback port");
  AtomicWrite(config.stateDirectory / "connection.json", Json{{"port", config.port}, {"token", token}}.dump());
  stopped = 0; const auto previousTerm = std::signal(SIGTERM, StopSignal), previousInt = std::signal(SIGINT, StopSignal);
  service.Start(); std::cerr << "CallAgent listening on 127.0.0.1:" << config.port << '\n';
  while (!stopped) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  server.stop(); fs::remove(config.stateDirectory / "connection.json"); std::signal(SIGTERM, previousTerm); std::signal(SIGINT, previousInt);
}
Json ClientRequest(const Config& config, const Json& body) {
  const auto connection = ReadJson(config.stateDirectory / "connection.json", 4096);
  if (connection.at("port") != config.port) throw Error("config", "Client configuration differs from running daemon");
  HttpRequest request; request.method = HTTP_POST; request.url = "http://127.0.0.1:" + std::to_string(config.port) + "/v1/request";
  request.timeout = 120; request.SetHeader("Authorization", "Bearer " + connection.at("token").get<std::string>()); request.SetHeader("Content-Type", "application/json"); request.body = body.dump();
  HttpResponse response; hv::HttpClient client;
  if (client.send(&request, &response) != 0) throw Error("unavailable", "CallAgent daemon is unavailable; start 'callagent serve'");
  if (response.body.size() > kMaxWireBytes) throw Error("size_limit", "Daemon response exceeds limit");
  const auto result = Json::parse(response.body);
  if (!result.value("ok", false)) throw Error(result.at("error").value("code", "remote"), result.at("error").value("message", "Daemon request failed"));
  return result.at("result");
}
}
