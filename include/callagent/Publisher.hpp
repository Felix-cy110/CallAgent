#pragma once
#include "callagent/Config.hpp"
namespace callagent {
class Publisher {
 public:
  explicit Publisher(Config config) : config_(std::move(config)) {}
  Json Publish() const;
 private:
  void PublishRoot(const PublishRoot& root) const;
  Config config_;
};
}
