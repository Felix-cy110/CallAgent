#pragma once
#include "callagent/Types.hpp"
namespace callagent {
struct ProviderConfig {
  ProviderKind kind;
  bool enabled = true;
  fs::path directory, executable, endpoint;
  unsigned short debugPort = 50641;
  bool autoEnableDebugPort = true;
};
struct PublishRoot { ProviderKind host; fs::path directory; };
struct Config {
  fs::path file, stateDirectory, executable;
  unsigned short port = 47831;
  int refreshSeconds = 30;
  std::vector<ProviderConfig> providers;
  std::vector<PublishRoot> publishRoots;
};
fs::path DefaultConfigPath();
Json DefaultConfig();
Config LoadConfig(const fs::path& file, const fs::path& executable);
}
