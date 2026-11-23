#pragma once
#include "callagent/Types.hpp"
namespace callagent {
std::string ReadFile(const fs::path& path, std::size_t limit = kMaxWireBytes);
Json ReadJson(const fs::path& path, std::size_t limit = kMaxWireBytes);
void PrivateDirectory(const fs::path& path);
void AtomicWrite(const fs::path& path, const std::string& bytes, bool executable = false);
void RejectSymlinks(const fs::path& path);
class FileLock {
 public:
  explicit FileLock(const fs::path& path);
  ~FileLock();
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
 private:
  int fd_ = -1;
};
}
