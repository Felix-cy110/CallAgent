#include "callagent/WorkBuddyRpc.hpp"
#include <algorithm>
#include "callagent/NativeChannel.hpp"
#include <hv/HttpClient.h>
#include <hv/WebSocketClient.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#endif

namespace callagent {
namespace {
constexpr char kProtocol[] = "workbuddy.local-rpc.v2";
struct ProcessIdentity {
  int pid = 0, parent = 0; unsigned uid = 0;
  std::uint64_t seconds = 0, microseconds = 0; std::string executable;
  bool operator==(const ProcessIdentity&) const = default;
};
ProcessIdentity Inspect(int pid) {
#ifdef __APPLE__
  proc_bsdinfo info{}; std::array<char, PROC_PIDPATHINFO_MAXSIZE> path{};
  if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info) || proc_pidpath(pid, path.data(), path.size()) <= 0)
    throw Error("peer", "Cannot verify WorkBuddy process");
  return {pid, static_cast<int>(info.pbi_ppid), info.pbi_uid, info.pbi_start_tvsec, info.pbi_start_tvusec, path.data()};
#else
  (void)pid; throw Error("unsupported_platform", "WorkBuddy process verification currently requires macOS");
#endif
}
int Listener(unsigned short port) {
  const auto text = RunCapture("/usr/sbin/lsof", {"-nP", "-Fpn", "-iTCP:" + std::to_string(port), "-sTCP:LISTEN"});
  int pid = 0; bool endpoint = false; std::istringstream input(text); std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) continue;
    if (line[0] == 'p') { const int next = std::stoi(line.substr(1)); if (pid && next != pid) throw Error("peer", "WorkBuddy listener has multiple owners"); pid = next; }
    if (line[0] == 'n') { if (line.substr(1) != "127.0.0.1:" + std::to_string(port)) throw Error("peer", "WorkBuddy listener is not loopback only"); endpoint = true; }
  }
  if (!pid || !endpoint) throw Error("unavailable", "WorkBuddy Local RPC bootstrap is unavailable; open Desktop with its configured debug port"); return pid;
}
class Peer {
 public:
  Peer(unsigned short port, const fs::path& executable, int ancestor = 0) : port_(port) {
    identity_ = Inspect(Listener(port));
    auto current = identity_; const auto expected = fs::weakly_canonical(executable).string();
    for (int depth = 0; depth < 32; ++depth) {
      if (current.uid != getuid()) throw Error("peer", "WorkBuddy belongs to another user");
      if ((!ancestor || current.pid == ancestor) && fs::weakly_canonical(current.executable).string() == expected) { desktop_ = current; return; }
      if (current.parent <= 1) break; current = Inspect(current.parent);
    }
    throw Error("peer", "WorkBuddy listener is outside the configured desktop process tree");
  }
  int DesktopPid() const { return desktop_.pid; }
  void Verify() const {
    if (Listener(port_) != identity_.pid || Inspect(identity_.pid) != identity_ || Inspect(desktop_.pid) != desktop_) throw Error("peer", "WorkBuddy process identity changed");
  }
 private:
  unsigned short port_; ProcessIdentity identity_, desktop_;
};
unsigned short EndpointPort(const std::string& endpoint, bool debugger) {
  std::smatch match; const std::regex pattern(debugger ? R"(^ws://127\.0\.0\.1:([0-9]{1,5})/devtools/page/[a-zA-Z0-9-]+$)" : R"(^ws://127\.0\.0\.1:([0-9]{1,5})/rpc$)");
  if (!std::regex_match(endpoint, match, pattern)) throw Error("endpoint", "Invalid WorkBuddy RPC endpoint");
  const auto port = std::stoi(match[1]); if (port < 1 || port > 65535) throw Error("endpoint", "Invalid WorkBuddy RPC port"); return static_cast<unsigned short>(port);
}
class Socket {
 public:
  Socket(const std::string& endpoint, std::shared_ptr<Peer> peer, const std::string& protocol = {}) : peer_(std::move(peer)) {
    socket_.setConnectTimeout(4000);
    socket_.onopen = [this, protocol] {
      socket_.channel->setMaxReadBufsize(kMaxWireBytes); socket_.channel->setMaxWriteBufsize(kMaxWireBytes);
      std::lock_guard lock(mutex_); open_ = protocol.empty() || socket_.getHttpResponse()->GetHeader("Sec-WebSocket-Protocol") == protocol; failed_ = !open_; changed_.notify_all();
    };
    socket_.onclose = [this] { std::lock_guard lock(mutex_); failed_ = true; changed_.notify_all(); };
    socket_.onmessage = [this](const std::string& bytes) {
      const auto frame = bytes.size() <= kMaxWireBytes ? Json::parse(bytes, nullptr, false) : Json();
      std::lock_guard lock(mutex_);
      if (!frame.is_object() || socket_.opcode() != WS_OPCODE_TEXT) failed_ = true;
      else if (frame.contains("type") || (frame.contains("id") && frame.at("id") == waitingId_)) {
        if (frames_.size() >= 16) failed_ = true; else frames_.push_back(frame);
      }
      changed_.notify_all();
    };
    try {
      peer_->Verify(); http_headers headers; if (!protocol.empty()) headers["Sec-WebSocket-Protocol"] = protocol;
      if (socket_.open(endpoint.c_str(), headers) != 0) throw Error("unavailable", "WorkBuddy WebSocket could not open");
      std::unique_lock lock(mutex_);
      if (!changed_.wait_for(lock, std::chrono::seconds(4), [this] { return open_ || failed_; }) || failed_) throw Error("unavailable", "WorkBuddy WebSocket connection failed");
    } catch (...) { socket_.close(); socket_.stop(); throw; }
  }
  ~Socket() { socket_.close(); socket_.stop(); }
  void Send(const Json& frame) {
    peer_->Verify(); const auto bytes = frame.dump(); if (bytes.size() > kMaxWireBytes) throw Error("size_limit", "WorkBuddy request is too large");
    { std::lock_guard lock(mutex_); if (!open_ || failed_) throw Error("unavailable", "WorkBuddy connection closed"); }
    socket_.loop()->runInLoop([this, bytes] { if (socket_.send(bytes) < 0) { std::lock_guard lock(mutex_); failed_ = true; changed_.notify_all(); } });
  }
  Json Receive(Clock::time_point deadline) {
    std::unique_lock lock(mutex_);
    if (!changed_.wait_until(lock, deadline, [this] { return failed_ || !frames_.empty(); }) || failed_) throw Error("uncertain", "WorkBuddy RPC reply is unavailable");
    auto frame = frames_.front(); frames_.pop_front(); return frame;
  }
  Json Call(const std::string& method, const Json& params, bool jsonrpc = true) {
    int id; { std::lock_guard lock(mutex_); id = ++waitingId_; }
    Json frame{{"id", id}, {"method", method}, {"params", params}}; if (jsonrpc) frame["jsonrpc"] = "2.0";
    Send(frame); const auto deadline = Clock::now() + std::chrono::seconds(6);
    for (;;) { const auto reply = Receive(deadline); if (reply.value("id", Json()) != id) continue;
      if (reply.contains("error")) throw Error("native_rejected", "WorkBuddy RPC rejected " + method); return reply.at("result"); }
  }
 private:
  hv::WebSocketClient socket_; std::shared_ptr<Peer> peer_;
  std::mutex mutex_; std::condition_variable changed_; std::deque<Json> frames_;
  bool open_ = false, failed_ = false; int waitingId_ = 0;
};
std::string Base64(const unsigned char* data, std::size_t size) {
  std::string text(4 * ((size + 2) / 3), '\0'); EVP_EncodeBlock(reinterpret_cast<unsigned char*>(text.data()), data, static_cast<int>(size));
  for (auto& c : text) { if (c == '+') c = '-'; if (c == '/') c = '_'; }
  while (text.ends_with('=')) text.pop_back(); return text;
}
std::string Proof(const std::string& ticket, const std::vector<std::string>& fields) {
  std::string bytes;
  for (const auto& field : fields) { for (int shift : {24, 16, 8, 0}) bytes += static_cast<char>(static_cast<std::uint32_t>(field.size()) >> shift); bytes += field; }
  std::array<unsigned char, EVP_MAX_MD_SIZE> output{}; unsigned size = 0;
  if (!HMAC(EVP_sha256(), ticket.data(), static_cast<int>(ticket.size()), reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), output.data(), &size)) throw Error("crypto", "WorkBuddy HMAC failed");
  return Base64(output.data(), size);
}
std::string FileUrl(const std::string& raw) {
  const auto url = raw.substr(0, raw.find_first_of("?#"));
  if (!url.starts_with("file://")) return {}; std::string out;
  for (std::size_t i = 7; i < url.size(); ++i) {
    if (url[i] != '%') out += url[i];
    else { if (i + 2 >= url.size() || url.substr(i + 1, 2).find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return {};
      out += static_cast<char>(std::stoi(url.substr(i + 1, 2), nullptr, 16)); i += 2; }
  }
  return out;
}
}
struct WorkBuddyRpc::Impl { std::unique_ptr<Socket> socket; std::set<std::string> methods; };
WorkBuddyRpc::WorkBuddyRpc(const ProviderConfig& config) : impl_(std::make_unique<Impl>()) {
  auto desktop = std::make_shared<Peer>(config.debugPort, config.executable);
  HttpRequest request; request.method = HTTP_GET; request.url = "http://127.0.0.1:" + std::to_string(config.debugPort) + "/json/list"; request.timeout = 4;
  HttpResponse response; hv::HttpClient client;
  if (client.send(&request, &response) != 0 || response.status_code != 200 || response.body.size() > 256 * 1024) throw Error("unavailable", "WorkBuddy debugger discovery failed");
  const auto targets = Json::parse(response.body); if (!targets.is_array() || targets.size() > 128) throw Error("identity", "Invalid WorkBuddy renderer list");
  const auto renderer = fs::weakly_canonical(config.executable.parent_path() / "../Resources/app.asar/renderer/index.html");
  std::string endpoint;
  for (const auto& target : targets) if (target.value("type", "") == "page" && target.value("title", "") == "WorkBuddy" && FileUrl(target.value("url", "")) == renderer.string()) {
    if (!endpoint.empty()) throw Error("identity", "Ambiguous WorkBuddy renderer"); endpoint = target.at("webSocketDebuggerUrl");
  }
  if (endpoint.empty()) throw Error("unavailable", "WorkBuddy renderer is unavailable or does not match the configured installation");
  if (EndpointPort(endpoint, true) != config.debugPort) throw Error("peer", "WorkBuddy debugger port changed");
  Json descriptor;
  { Socket bootstrap(endpoint, desktop);
    const auto result = bootstrap.Call("Runtime.evaluate", {{"expression", "window.__wbInvoke(\"wb:extensions:$resolveLocalRpc\",undefined,\"edge-sync\",\"invokeSendPrompt\")"}, {"awaitPromise", true}, {"returnByValue", true}}, false);
    if (result.contains("exceptionDetails")) throw Error("authorization", "WorkBuddy did not authorize Local RPC"); descriptor = result.at("result").at("value");
  }
  if (descriptor.at("extensionId") != "edge-sync" || descriptor.at("protocolVersion") != 2 || !descriptor.at("methods").is_array()) throw Error("protocol", "Unsupported WorkBuddy Local RPC descriptor");
  endpoint = descriptor.at("endpoint"); auto ticket = descriptor.at("ticket").get<std::string>(); descriptor.erase("ticket");
  struct Cleanse { std::string& text; ~Cleanse() { OPENSSL_cleanse(text.data(), text.size()); } } cleanse{ticket};
  if (ticket.empty() || ticket.size() > 4096) throw Error("authorization", "Invalid WorkBuddy ticket");
  auto peer = std::make_shared<Peer>(EndpointPort(endpoint, false), config.executable, desktop->DesktopPid());
  impl_->socket = std::make_unique<Socket>(endpoint, peer, kProtocol);
  std::array<unsigned char, 16> random{}; if (RAND_bytes(random.data(), random.size()) != 1) throw Error("crypto", "Cannot generate WorkBuddy nonce");
  const auto nonce = Base64(random.data(), random.size());
  impl_->socket->Send({{"type", "session_hello"}, {"protocol_min", 2}, {"protocol_max", 2}, {"client_nonce", nonce}, {"ticket_id", Sha256(ticket).substr(0, 16)},
    {"client", {{"kind", "integration"}, {"id", "callagent"}, {"version", "0.2.0"}}}});
  const auto deadline = Clock::now() + std::chrono::seconds(6); const auto challenge = impl_->socket->Receive(deadline);
  if (challenge.at("type") != "session_challenge" || challenge.at("protocol") != 2) throw Error("authorization", "WorkBuddy challenge rejected");
  const auto server = challenge.at("server_nonce").get<std::string>(); if (server.empty() || server.size() > 128) throw Error("authorization", "Invalid WorkBuddy nonce");
  const auto expected = Proof(ticket, {"wbipc-s", "2", endpoint, nonce, server}); const auto actual = challenge.value("server_proof", "");
  if (expected.size() != actual.size() || CRYPTO_memcmp(expected.data(), actual.data(), expected.size())) throw Error("authorization", "WorkBuddy proof mismatch");
  impl_->socket->Send({{"type", "session_prove"}, {"client_proof", Proof(ticket, {"wbipc-c", "2", endpoint, nonce, server})}});
  const auto ack = impl_->socket->Receive(deadline);
  if (ack.at("type") != "session_hello_ack" || !ack.at("methods").is_array() || ack.at("methods").size() > 128) throw Error("authorization", "WorkBuddy authorization failed");
  for (const auto& method : ack.at("methods")) {
    if (!method.is_string() || std::find(descriptor.at("methods").begin(), descriptor.at("methods").end(), method) == descriptor.at("methods").end()) throw Error("authorization", "WorkBuddy method scope changed");
    impl_->methods.insert(method.get<std::string>());
  }
}
WorkBuddyRpc::~WorkBuddyRpc() = default;
Json WorkBuddyRpc::Call(const std::string& method, const Json& params) {
  const auto full = "edge-sync/" + method;
  if (!impl_->methods.contains(full)) throw Error("unsupported", "WorkBuddy method not authorized: " + full);
  return impl_->socket->Call(full, params);
}
Json WorkBuddyRpc::Native(const std::string& channel, const Json& args) {
  const auto response = Call("invokeWbAny", {{"channel", channel}, {"argsJson", args.dump()}});
  if (!response.value("ok", false)) throw Error("native_rejected", "WorkBuddy read rejected: " + channel);
  return response.at("data").value("result", Json());
}
}
