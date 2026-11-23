#pragma once
#include "callagent/Types.hpp"
namespace callagent {
class WorkBuddyRepository {
 public:
  explicit WorkBuddyRepository(fs::path directory) : directory_(std::move(directory)) {}
  std::string Account() const;
  std::vector<Session> List() const;
  void Validate(const Session& session) const;
 private:
  fs::path directory_;
};
}
