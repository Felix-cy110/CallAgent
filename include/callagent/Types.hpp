#pragma once
#include <hv/json.hpp>
#include <chrono>
#include <filesystem>
#include <string>
#include <stdexcept>
#include <cstdint>
#include <vector>

namespace callagent {
using Json = nlohmann::json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
inline constexpr std::size_t kMaxWireBytes = 8 * 1024 * 1024;
inline constexpr std::size_t kMaxTextBytes = 1024 * 1024;
enum class ProviderKind { Codex, Claude, WorkBuddy };
std::string ToString(ProviderKind kind);
ProviderKind ParseProvider(const std::string& name);
std::string DisplayName(ProviderKind kind);
struct Session {
  ProviderKind provider;
  std::string profile, nativeId, title, cwd;
  std::int64_t activityMs = 0;
  std::string activitySource;
  std::string Key() const;
};
void to_json(Json& out, const Session& session);
void from_json(const Json& in, Session& session);
std::vector<Session> SelectTargets(std::vector<Session> sessions);
std::string ShortTitle(const std::string& title);
std::int64_t NowMs();
std::string Sha256(const std::string& text);
std::string RandomHex(std::size_t bytes = 32);
std::string DialogueText(const Json& blocks);
class Error : public std::runtime_error {
 public:
  const std::string code;
  Error(std::string code, const std::string& message) : std::runtime_error(message), code(std::move(code)) {}
};
}
