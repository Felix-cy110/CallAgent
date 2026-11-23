#pragma once
#include "callagent/Config.hpp"
namespace callagent {
class Publisher {
 public:
  explicit Publisher(Config config) : config_(std::move(config)) {}
  Json Publish(const std::vector<Session>& sessions) const;
 private:
  void PublishRoot(const PublishRoot& root, const std::vector<Session>& sessions) const;
  Config config_;
};
}
