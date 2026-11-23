#pragma once
#include "callagent/Types.hpp"
#include <memory>
namespace callagent {
// Each channel owns its descriptors/process. All calls are serialized by its provider.
class NativeChannel {
 public:
  static std::unique_ptr<NativeChannel> Socket(const fs::path& path, bool lengthPrefix = true);
  static std::unique_ptr<NativeChannel> Process(const std::string& executable, const std::vector<std::string>& arguments,
                                               const std::string& environmentName = {}, const std::string& environmentValue = {});
  ~NativeChannel();
  void Send(const Json& frame, Clock::time_point deadline);
  Json Receive(Clock::time_point deadline);
 private:
  NativeChannel(int input, int output, int pid, bool lengthPrefix);
  int input_, output_, pid_;
  bool lengthPrefix_;
  std::string buffered_;
};
std::string RunCapture(const std::string& executable, const std::vector<std::string>& arguments);
}
