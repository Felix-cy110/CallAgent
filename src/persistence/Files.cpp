#include "callagent/Files.hpp"
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>

namespace callagent {
void RejectSymlinks(const fs::path& path) {
  if (!path.is_absolute() || path.lexically_normal() != path) throw Error("unsafe_path", "An absolute normalized path is required");
  for (auto p = path; p != p.root_path(); p = p.parent_path()) {
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(p, ec))) throw Error("unsafe_path", "Symlink is not allowed: " + p.string());
  }
}
std::string ReadFile(const fs::path& path, std::size_t limit) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw Error("read_failed", "Cannot read: " + path.string());
  std::string out; char buffer[8192];
  while (input.read(buffer, sizeof(buffer)) || input.gcount()) {
    if (out.size() + static_cast<std::size_t>(input.gcount()) > limit) throw Error("size_limit", "File exceeds read limit: " + path.string());
    out.append(buffer, static_cast<std::size_t>(input.gcount()));
  }
  if (input.bad()) throw Error("read_failed", "File read failed");
  return out;
}
Json ReadJson(const fs::path& path, std::size_t limit) { return Json::parse(ReadFile(path, limit)); }
void PrivateDirectory(const fs::path& path) {
  RejectSymlinks(path); fs::create_directories(path);
  if (chmod(path.c_str(), 0700)) throw Error("permissions", "Cannot protect state directory");
}
void AtomicWrite(const fs::path& path, const std::string& bytes, bool executable) {
  RejectSymlinks(path);
  const auto temporary = path.parent_path() / ("." + path.filename().string() + "." + RandomHex(8));
  const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, executable ? 0700 : 0600);
  if (fd < 0) throw Error("write_failed", "Cannot create: " + path.string());
  try {
    std::size_t offset = 0;
    while (offset < bytes.size()) { auto count = write(fd, bytes.data() + offset, bytes.size() - offset); if (count < 0 && errno == EINTR) continue;
      if (count <= 0) throw Error("write_failed", "Cannot write: " + path.string()); offset += static_cast<std::size_t>(count); }
    if (fsync(fd)) throw Error("write_failed", "Cannot persist file");
    if (rename(temporary.c_str(), path.c_str())) throw Error("write_failed", "Cannot replace file");
    close(fd);
    const int parent = open(path.parent_path().c_str(), O_RDONLY | O_CLOEXEC);
    if (parent >= 0) { fsync(parent); close(parent); }
  } catch (...) { close(fd); unlink(temporary.c_str()); throw; }
}
FileLock::FileLock(const fs::path& path) {
  RejectSymlinks(path);
  fd_ = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd_ < 0) throw Error("lock_failed", "Cannot open lock");
  if (flock(fd_, LOCK_EX | LOCK_NB)) { close(fd_); fd_ = -1; throw Error("already_running", "Another CallAgent process owns this state directory"); }
}
FileLock::~FileLock() { if (fd_ >= 0) close(fd_); }
}
