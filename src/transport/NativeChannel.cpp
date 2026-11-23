#include "callagent/NativeChannel.hpp"
#include <algorithm>
#include <cerrno>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <signal.h>
#include <unistd.h>
#include <array>
#include <cstring>
extern char** environ;

namespace callagent {
namespace {
void Wait(int fd, short events, Clock::time_point deadline) {
  for (;;) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (remaining <= 0) throw Error("timeout", "Native IPC timed out");
    pollfd descriptor{fd, events, 0};
    const int result = poll(&descriptor, 1, static_cast<int>(std::min<std::int64_t>(remaining, 30000)));
    if (result < 0 && errno == EINTR) continue;
    if (result < 0 || (result > 0 && !(descriptor.revents & events))) throw Error("disconnected", "Native IPC closed");
    if (result > 0) return;
  }
}
void Nonblocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); fcntl(fd, F_SETFD, FD_CLOEXEC); }
struct Spawned { int input, output, pid; };
Spawned Spawn(const std::string& executable, const std::vector<std::string>& args, const std::string& envName, const std::string& envValue) {
  int input[2], output[2];
  if (pipe(input)) throw Error("process", "Cannot open process pipe");
  if (pipe(output)) { close(input[0]); close(input[1]); throw Error("process", "Cannot open process pipe"); }
  for (int fd : {input[0], input[1], output[0], output[1]}) fcntl(fd, F_SETFD, FD_CLOEXEC);
  posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  for (int fd : {input[0], input[1], output[0], output[1]}) posix_spawn_file_actions_addclose(&actions, fd);
  std::vector<std::string> arguments{executable}; arguments.insert(arguments.end(), args.begin(), args.end());
  std::vector<char*> argv; for (auto& value : arguments) argv.push_back(value.data()); argv.push_back(nullptr);
  std::vector<std::string> environment;
  for (char** entry = environ; *entry; ++entry) if (envName.empty() || !std::string(*entry).starts_with(envName + "=")) environment.emplace_back(*entry);
  if (!envName.empty()) environment.push_back(envName + "=" + envValue);
  std::vector<char*> env; for (auto& value : environment) env.push_back(value.data()); env.push_back(nullptr);
  pid_t pid = 0; const auto error = posix_spawnp(&pid, executable.c_str(), &actions, nullptr, argv.data(), env.data());
  posix_spawn_file_actions_destroy(&actions); close(input[0]); close(output[1]);
  if (error) { close(input[1]); close(output[0]); throw Error("process", "Cannot start native reader: " + executable); }
  Nonblocking(input[1]); Nonblocking(output[0]); return {output[0], input[1], pid};
}
void Stop(int pid) {
  if (pid <= 0) return;
  const auto status = waitpid(pid, nullptr, WNOHANG);
  if (status == pid || (status < 0 && errno == ECHILD)) return;
  kill(pid, SIGTERM);
  // Only the read-only child belongs to us. Never signal a Desktop process.
  for (int count = 0; count < 10; ++count) { if (waitpid(pid, nullptr, WNOHANG) == pid) return; poll(nullptr, 0, 10); }
  kill(pid, SIGKILL); while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
}
}
NativeChannel::NativeChannel(int input, int output, int pid, bool prefix) : input_(input), output_(output), pid_(pid), lengthPrefix_(prefix) {}
NativeChannel::~NativeChannel() { close(input_); if (output_ != input_) close(output_); Stop(pid_); }
std::unique_ptr<NativeChannel> NativeChannel::Process(const std::string& executable, const std::vector<std::string>& args, const std::string& envName, const std::string& envValue) {
  const auto child = Spawn(executable, args, envName, envValue);
  return std::unique_ptr<NativeChannel>(new NativeChannel(child.input, child.output, child.pid, false));
}
std::unique_ptr<NativeChannel> NativeChannel::Socket(const fs::path& path, bool prefix) {
  sockaddr_un address{}; address.sun_family = AF_UNIX;
  if (path.string().size() >= sizeof(address.sun_path)) throw Error("endpoint", "IPC path exceeds system limit");
  std::strcpy(address.sun_path, path.c_str());
  int fd = socket(AF_UNIX, SOCK_STREAM, 0); if (fd < 0) throw Error("endpoint", "Cannot create IPC socket");
  try {
    Nonblocking(fd);
#ifdef __APPLE__
    int noSignal = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal));
#endif
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      if (errno != EINPROGRESS) throw Error("unavailable", "Desktop IPC is unavailable");
      Wait(fd, POLLOUT, Clock::now() + std::chrono::seconds(5));
      int error = 0; socklen_t size = sizeof(error); getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size);
      if (error) throw Error("unavailable", "Desktop IPC connection failed");
    }
#ifdef __APPLE__
    uid_t uid = 0; gid_t gid = 0;
    if (getpeereid(fd, &uid, &gid) || uid != getuid()) throw Error("peer", "Desktop IPC belongs to another user");
#else
    ucred peer{}; socklen_t size = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) || peer.uid != getuid()) throw Error("peer", "Desktop IPC belongs to another user");
#endif
    return std::unique_ptr<NativeChannel>(new NativeChannel(fd, fd, 0, prefix));
  } catch (...) { close(fd); throw; }
}
void NativeChannel::Send(const Json& frame, Clock::time_point deadline) {
  std::string bytes = frame.dump();
  if (bytes.size() > kMaxWireBytes) throw Error("size_limit", "IPC frame is too large");
  if (lengthPrefix_) { std::string header; auto size = static_cast<std::uint32_t>(bytes.size()); for (int shift : {0, 8, 16, 24}) header += static_cast<char>(size >> shift); bytes = header + bytes; }
  else bytes += '\n';
  std::size_t offset = 0;
  while (offset < bytes.size()) { Wait(output_, POLLOUT, deadline); const auto count = write(output_, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue; if (count <= 0) throw Error("disconnected", "Native IPC write failed"); offset += static_cast<std::size_t>(count); }
}
Json NativeChannel::Receive(Clock::time_point deadline) {
  for (;;) {
    std::size_t size = std::string::npos, prefix = 0;
    if (lengthPrefix_ && buffered_.size() >= 4) { size = 0; for (int i = 0; i < 4; ++i) size |= static_cast<std::size_t>(static_cast<unsigned char>(buffered_[i])) << (8 * i); prefix = 4; }
    else if (!lengthPrefix_) size = buffered_.find('\n');
    if (size != std::string::npos) {
      if (size > kMaxWireBytes) throw Error("size_limit", "Native IPC response exceeds limit");
      if (buffered_.size() >= prefix + size + (lengthPrefix_ ? 0 : 1)) {
        const auto text = buffered_.substr(prefix, size); buffered_.erase(0, prefix + size + (lengthPrefix_ ? 0 : 1)); return Json::parse(text);
      }
    }
    Wait(input_, POLLIN, deadline); char buffer[8192]; auto count = read(input_, buffer, sizeof(buffer));
    if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue; if (count <= 0) throw Error("disconnected", "Native IPC reader exited");
    buffered_.append(buffer, static_cast<std::size_t>(count)); if (buffered_.size() > kMaxWireBytes + 8192) throw Error("size_limit", "Native IPC buffer exceeds limit");
  }
}
std::string RunCapture(const std::string& executable, const std::vector<std::string>& args) {
  const auto child = Spawn(executable, args, {}, {}); close(child.output); std::string result;
  try { const auto deadline = Clock::now() + std::chrono::seconds(3); char buffer[4096];
    for (;;) { pollfd fd{child.input, POLLIN, 0}; if (Clock::now() >= deadline) throw Error("timeout", "Process discovery timed out");
      if (poll(&fd, 1, 100) <= 0) continue; const auto count = read(child.input, buffer, sizeof(buffer));
      if (!count) break; if (count < 0) { if (errno == EAGAIN || errno == EINTR) continue; throw Error("process", "Process discovery read failed"); }
      result.append(buffer, static_cast<std::size_t>(count)); if (result.size() > 256 * 1024) throw Error("size_limit", "Process discovery exceeds limit");
    }
    close(child.input); Stop(child.pid); return result;
  } catch (...) { close(child.input); Stop(child.pid); throw; }
}
}
