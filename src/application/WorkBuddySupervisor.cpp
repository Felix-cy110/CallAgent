#include "callagent/WorkBuddyDesktop.hpp"
#include "callagent/Files.hpp"
#include "callagent/NativeChannel.hpp"
#include "callagent/WorkBuddyRepository.hpp"
#include <hv/HttpClient.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#ifdef __APPLE__
#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <libproc.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/proc.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace callagent {
namespace {
bool DesktopPath(const fs::path& path) {
  return path.is_absolute() && path.filename() == "Electron" && path.parent_path().filename() == "MacOS" &&
    path.parent_path().parent_path().filename() == "Contents" && path.parent_path().parent_path().parent_path().filename() == "WorkBuddy.app";
}
}
fs::path ResolveWorkBuddyExecutable(const fs::path& configured, const fs::path& home) {
  if (!configured.empty()) {
    // RPC fixtures and explicitly configured installations need not have the canonical bundle name.
    if (!configured.is_absolute()) throw Error("config", "WorkBuddy executable must be an absolute path");
    if (!fs::is_regular_file(configured)) throw Error("waiting_installation", "Configured WorkBuddy installation is unavailable");
    return fs::canonical(configured);
  }
#ifdef __APPLE__
  const auto* environmentHome = std::getenv("HOME");
  const auto base = home.empty() ? fs::path(environmentHome ? environmentHome : "") : home;
  if (!base.is_absolute()) throw Error("config", "WorkBuddy discovery requires an absolute home directory");
  for (const auto& applications : {base / "Applications", fs::path("/Applications")}) {
    const auto candidate = applications / "WorkBuddy.app/Contents/MacOS/Electron";
    if (fs::is_regular_file(candidate)) {
      const auto executable = fs::canonical(candidate);
      if (DesktopPath(executable)) return executable;
    }
  }
  throw Error("waiting_installation", "Open an installed WorkBuddy desktop to connect");
#else
  (void)home;
  throw Error("unsupported_platform", "Automatic WorkBuddy discovery currently requires macOS");
#endif
}

#ifdef __APPLE__
namespace {
struct Process {
  int pid = 0, parent = 0;
  unsigned uid = 0;
  std::uint64_t seconds = 0, microseconds = 0;
  fs::path executable;
  bool operator==(const Process&) const = default;
};
std::optional<Process> Inspect(int pid) {
  proc_bsdinfo info{}; std::array<char, PROC_PIDPATHINFO_MAXSIZE> path{};
  if (pid <= 0 || proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info) ||
      proc_pidpath(pid, path.data(), path.size()) <= 0 || info.pbi_status == SZOMB) return {};
  return Process{pid, static_cast<int>(info.pbi_ppid), info.pbi_uid, info.pbi_start_tvsec, info.pbi_start_tvusec, path.data()};
}
bool SameProcess(const Process& expected) {
  const auto current = Inspect(expected.pid);
  // Orphaned helpers can acquire a new parent while remaining the same process.
  return current && current->uid == expected.uid && current->seconds == expected.seconds &&
    current->microseconds == expected.microseconds && current->executable == expected.executable;
}
std::vector<Process> Processes() {
  const int bytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
  if (bytes <= 0 || bytes > 8 * 1024 * 1024) throw Error("process", "Cannot enumerate desktop processes");
  std::vector<int> ids(static_cast<std::size_t>(bytes) / sizeof(int) + 256);
  for (;;) {
    const int count = proc_listpids(PROC_ALL_PIDS, 0, ids.data(), static_cast<int>(ids.size() * sizeof(int)));
    if (count <= 0) throw Error("process", "Cannot enumerate desktop processes");
    if (count < static_cast<int>(ids.size() * sizeof(int))) { ids.resize(count / sizeof(int)); break; }
    if (ids.size() > 2 * 1024 * 1024) throw Error("process", "Desktop process catalogue exceeds limit");
    ids.resize(ids.size() * 2);
  }
  std::vector<Process> rows;
  for (int pid : ids) if (const auto row = Inspect(pid)) rows.push_back(*row);
  return rows;
}
bool Descendant(int pid, int ancestor, const std::vector<Process>& rows) {
  for (int depth = 0; pid > 1 && depth < 32; ++depth) {
    if (pid == ancestor) return true;
    const auto found = std::find_if(rows.begin(), rows.end(), [pid](const auto& row) { return row.pid == pid; });
    if (found == rows.end() || found->parent == pid || found->uid != getuid()) return false;
    pid = found->parent;
  }
  return false;
}
struct Listener { int pid = 0; bool occupied = false, loopback = true; };
Listener FindListener(unsigned short port) {
  const auto output = RunCapture("/usr/sbin/lsof", {"-nP", "-Fpn", "-iTCP:" + std::to_string(port), "-sTCP:LISTEN"});
  Listener listener; std::istringstream lines(output); std::string line; bool address = false;
  while (std::getline(lines, line)) {
    if (line.empty()) continue;
    if (line.front() == 'p') {
      int pid = 0; const auto [end, code] = std::from_chars(line.data() + 1, line.data() + line.size(), pid);
      if (code != std::errc() || end != line.data() + line.size() || pid <= 0) throw Error("peer", "Invalid debug listener identity");
      if (listener.occupied && listener.pid != pid) listener.loopback = false;
      listener.pid = pid; listener.occupied = true;
    } else if (line.front() == 'n') {
      address = true;
      if (line.substr(1) != "127.0.0.1:" + std::to_string(port)) listener.loopback = false;
    }
  }
  listener.loopback = listener.loopback && address;
  return listener;
}
bool PortFree(unsigned short port) {
  const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket < 0) throw Error("socket", "Cannot check debug port availability");
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const bool free = ::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
  ::close(socket); return free;
}
std::string FileUrl(const std::string& raw) {
  const auto url = raw.substr(0, raw.find_first_of("?#"));
  if (!url.starts_with("file://")) return {};
  std::string path;
  for (std::size_t i = 7; i < url.size(); ++i) {
    if (url[i] != '%') path += url[i];
    else {
      if (i + 2 >= url.size()) return {};
      unsigned value = 0; const auto [end, code] = std::from_chars(url.data() + i + 1, url.data() + i + 3, value, 16);
      if (code != std::errc() || end != url.data() + i + 3 || value == 0) return {};
      path += static_cast<char>(value); i += 2;
    }
  }
  return path;
}
bool Ready(unsigned short port, const fs::path& executable, const Process& desktop, const Listener& listener) {
  HttpRequest request; request.method = HTTP_GET;
  request.url = "http://127.0.0.1:" + std::to_string(port) + "/json/list";
  request.AllowRedirect(false); request.SetRetry(0); request.SetTimeout(2); request.SetConnectTimeout(1);
  request.SetHeader("x-codebuddy-request", "1");
  HttpResponse response; hv::HttpClient client;
  if (client.send(&request, &response) != 0 || response.status_code != 200 || response.body.size() > 256 * 1024) return false;
  const auto confirmed = FindListener(port);
  if (!confirmed.loopback || confirmed.pid != listener.pid || !SameProcess(desktop)) return false;
  const auto rows = Json::parse(response.body, nullptr, false);
  if (!rows.is_array() || rows.size() > 128) return false;
  const auto expected = (executable.parent_path() / "../Resources/app.asar/renderer/index.html").lexically_normal();
  int matches = 0;
  for (const auto& row : rows) {
    if (!row.is_object() || !row.value("type", Json()).is_string() || !row.value("title", Json()).is_string() || !row.value("url", Json()).is_string()) continue;
    if (row.at("type") == "page" && row.at("title") == "WorkBuddy" && FileUrl(row.at("url")) == expected.string()) ++matches;
  }
  return matches == 1;
}
struct Desktop {
  std::optional<Process> process;
  bool ready = false, listening = false, conflict = false, ambiguous = false;
};
Desktop FindDesktop(const fs::path& executable, unsigned short port) {
  const auto rows = Processes(); std::vector<Process> roots;
  for (const auto& row : rows) if (row.uid == getuid() && row.executable == executable) roots.push_back(row);
  const auto candidates = roots;
  std::erase_if(roots, [&](const auto& row) {
    return std::any_of(candidates.begin(), candidates.end(), [&](const auto& other) { return row.pid != other.pid && Descendant(row.pid, other.pid, rows); });
  });
  Desktop result;
  if (roots.size() > 1) { result.ambiguous = true; return result; }
  if (!roots.empty()) result.process = roots.front();
  const auto listener = FindListener(port);
  if (!listener.occupied) { result.conflict = !PortFree(port); return result; }
  const auto owner = Inspect(listener.pid);
  if (!result.process || !listener.loopback || !owner || owner->uid != getuid() || !Descendant(listener.pid, result.process->pid, rows)) {
    result.conflict = true; return result;
  }
  result.listening = true;
  result.ready = Ready(port, executable, *result.process, listener);
  return result;
}
int LaunchDesktop(const fs::path& executable, unsigned short port) {
  std::vector<std::string> environment;
  for (auto entry = environ; entry && *entry; ++entry)
    if (!std::string_view(*entry).starts_with("WORKBUDDY_REMOTE_DEBUGGING_PORT=")) environment.emplace_back(*entry);
  environment.push_back("WORKBUDDY_REMOTE_DEBUGGING_PORT=" + std::to_string(port));
  std::vector<char*> envp; for (auto& value : environment) envp.push_back(value.data()); envp.push_back(nullptr);
  auto path = executable.string(); std::array<char*, 2> args{path.data(), nullptr};
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions)) throw Error("process", "Cannot prepare WorkBuddy launch");
  struct Cleanup { posix_spawn_file_actions_t* actions; ~Cleanup() { posix_spawn_file_actions_destroy(actions); } } cleanup{&actions};
  for (int fd = 0; fd < 3; ++fd) if (posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd == 0 ? O_RDONLY : O_WRONLY, 0))
    throw Error("process", "Cannot isolate WorkBuddy streams");
  posix_spawnattr_t attributes;
  if (posix_spawnattr_init(&attributes)) throw Error("process", "Cannot prepare WorkBuddy process attributes");
  struct AttributeCleanup { posix_spawnattr_t* value; ~AttributeCleanup() { posix_spawnattr_destroy(value); } } attributeCleanup{&attributes};
  // libhv listeners need not be CLOEXEC. The desktop must not keep the Client's sockets or locks alive.
  short flags = POSIX_SPAWN_CLOEXEC_DEFAULT;
#ifdef POSIX_SPAWN_SETSID
  flags |= POSIX_SPAWN_SETSID;
#endif
  if (posix_spawnattr_setflags(&attributes, flags)) throw Error("process", "Cannot isolate WorkBuddy process descriptors");
  int pid = 0;
  if (posix_spawn(&pid, path.c_str(), &actions, &attributes, args.data(), envp.data())) throw Error("launch_failed", "Cannot launch WorkBuddy with its local debug port");
  return pid;
}
}
#endif

struct WorkBuddySupervisor::Impl {
  ProviderConfig config;
  std::function<void()> onReady;
  mutable std::mutex mutex;
  Json state;
  std::mutex waitMutex;
  std::condition_variable_any wake;
  std::jthread worker;
  Impl(ProviderConfig value, std::function<void()> callback) : config(std::move(value)), onReady(std::move(callback)),
    state{{"state", config.enabled && config.autoEnableDebugPort ? "starting" : "disabled"}, {"debugPort", config.debugPort}, {"restartCount", 0}} {}
  void Set(const std::string& value, const std::string& detail = {}, int pid = 0, const fs::path& executable = {}) {
    bool ready;
    {
      std::lock_guard lock(mutex); ready = value == "ready" && state.at("state") != "ready";
      state["state"] = value; state["detail"] = detail; state["pid"] = pid; state["executable"] = executable.string();
    }
    if (ready && onReady) onReady();
  }
  void Wait(std::stop_token stop, std::chrono::milliseconds duration) {
    std::unique_lock lock(waitMutex); wake.wait_for(lock, stop, duration, [] { return false; });
  }
#ifdef __APPLE__
  bool StopDesktop(const Process& desktop, std::stop_token stop) {
    const auto rows = Processes(); std::vector<Process> owned;
    for (const auto& row : rows) if (row.uid == getuid() && Descendant(row.pid, desktop.pid, rows)) owned.push_back(row);
    if (stop.stop_requested() || !SameProcess(desktop) || ::kill(desktop.pid, SIGTERM) != 0) return false;
    const auto deadline = Clock::now() + std::chrono::seconds(12);
    while (!stop.stop_requested() && Clock::now() < deadline) {
      if (std::none_of(owned.begin(), owned.end(), SameProcess)) return true;
      Wait(stop, std::chrono::milliseconds(100));
    }
    return false; // Never force-kill a desktop that refuses to exit.
  }
  void Run(std::stop_token stop) {
    std::unique_ptr<FileLock> lock; fs::path lockedExecutable;
    std::optional<Process> attempted;
    std::vector<int> children;
    while (!stop.stop_requested()) {
      for (auto child = children.begin(); child != children.end();) {
        int status = 0;
        if (waitpid(*child, &status, WNOHANG) != 0) child = children.erase(child); else ++child;
      }
      try {
        if (!config.enabled || !config.autoEnableDebugPort) { Set("disabled"); return; }
        const auto executable = ResolveWorkBuddyExecutable(config.executable);
        if (!DesktopPath(executable)) throw Error("invalid_installation", "Automatic restart only accepts WorkBuddy.app/Contents/MacOS/Electron");
        WorkBuddyRepository repository(config.directory); std::string account;
        try { account = repository.Account(); }
        catch (const std::exception&) { Set("waiting_account", "Waiting for WorkBuddy login", 0, executable); Wait(stop, std::chrono::seconds(5)); continue; }
        if (lockedExecutable != executable || !lock) {
          lock.reset(); lockedExecutable.clear();
          const auto file = fs::canonical(fs::temp_directory_path()) / ("callagent-workbuddy-" + std::to_string(getuid()) + "-" + Sha256(executable.string()) + ".lock");
          try { lock = std::make_unique<FileLock>(file); }
          catch (const Error& error) { if (error.code != "already_running") throw; Set("managed_elsewhere", "Another CallAgent Client manages this installation"); Wait(stop, std::chrono::seconds(5)); continue; }
          lockedExecutable = executable;
        }
        const auto desktop = FindDesktop(executable, config.debugPort);
        if (stop.stop_requested()) break;
        if (desktop.ambiguous) { Set("ambiguous_desktop", "Multiple WorkBuddy main processes; waiting without restarting"); }
        else if (desktop.conflict) { Set("port_conflict", "Debug port is occupied outside the expected loopback desktop listener"); }
        else if (!desktop.process) { Set("waiting_desktop", "Waiting for the user to open WorkBuddy", 0, executable); }
        else if (desktop.ready) { Set("ready", {}, desktop.process->pid, executable); }
        else if (desktop.listening) { Set("waiting_ready", "Existing debug listener has no usable renderer yet", desktop.process->pid, executable); }
        else if (attempted && SameProcess(*attempted)) { Set("restart_not_completed", "Previous restart attempt did not produce a usable debug entry; waiting for a new desktop process", desktop.process->pid, executable); }
        else {
          // Recheck identity, account and port immediately before the only destructive step.
          const auto fresh = FindDesktop(executable, config.debugPort);
          if (stop.stop_requested()) break;
          if (fresh.ambiguous || fresh.conflict || fresh.listening || !fresh.process || *fresh.process != *desktop.process ||
              repository.Account() != account || ResolveWorkBuddyExecutable(config.executable) != executable) continue;
          attempted = *desktop.process;
          Set("restarting", "Requesting desktop exit to enable the local debug port", desktop.process->pid, executable);
          if (!StopDesktop(*desktop.process, stop)) {
            if (stop.stop_requested()) break;
            Set("restart_not_completed", "Desktop did not exit; no force kill or replacement was attempted", desktop.process->pid, executable);
          } else {
            if (stop.stop_requested()) break;
            const auto after = FindDesktop(executable, config.debugPort);
            if (stop.stop_requested()) break;
            if (!after.process && !after.ambiguous && !after.conflict && repository.Account() == account &&
                ResolveWorkBuddyExecutable(config.executable) == executable) {
              const auto pid = LaunchDesktop(executable, config.debugPort); children.push_back(pid);
              attempted = Inspect(pid); // A failed replacement never creates a standing right to relaunch.
              { std::lock_guard guard(mutex); state["restartCount"] = state.at("restartCount").get<int>() + 1; }
              Set("waiting_ready", "Started one replacement desktop with the local debug port", pid, executable);
            }
          }
        }
        Wait(stop, std::chrono::seconds(5));
      } catch (const Error& error) { Set(error.code, error.what()); Wait(stop, std::chrono::seconds(5)); }
      catch (const std::exception& error) { Set("error", error.what()); Wait(stop, std::chrono::seconds(5)); }
    }
    Set("stopped");
  }
#else
  void Run(std::stop_token) { Set("unsupported_platform", "Automatic WorkBuddy supervision currently requires macOS"); }
#endif
};
WorkBuddySupervisor::WorkBuddySupervisor(ProviderConfig config, std::function<void()> onReady) : impl_(std::make_unique<Impl>(std::move(config), std::move(onReady))) {}
WorkBuddySupervisor::~WorkBuddySupervisor() { Stop(); }
void WorkBuddySupervisor::Start() {
  if (!impl_->config.enabled || !impl_->config.autoEnableDebugPort || impl_->worker.joinable()) return;
  impl_->worker = std::jthread([this](std::stop_token stop) { impl_->Run(stop); });
}
void WorkBuddySupervisor::Stop() {
  impl_->worker.request_stop(); impl_->wake.notify_all();
  if (impl_->worker.joinable()) impl_->worker.join();
}
Json WorkBuddySupervisor::Snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->state; }
}
