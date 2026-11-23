#include "callagent/Types.hpp"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <unicode/brkiter.h>
#include <unicode/unistr.h>
#include <algorithm>
#include <array>
#include <memory>
#include <set>

namespace callagent {
std::string ToString(ProviderKind kind) {
  switch (kind) { case ProviderKind::Codex: return "codex"; case ProviderKind::Claude: return "claude"; case ProviderKind::WorkBuddy: return "workbuddy"; }
  throw Error("invalid_provider", "Unknown provider");
}
ProviderKind ParseProvider(const std::string& name) {
  for (auto kind : {ProviderKind::Codex, ProviderKind::Claude, ProviderKind::WorkBuddy}) if (ToString(kind) == name) return kind;
  throw Error("invalid_provider", "Unknown provider: " + name);
}
std::string DisplayName(ProviderKind kind) {
  switch (kind) { case ProviderKind::Codex: return "codex"; case ProviderKind::Claude: return "Claude code"; case ProviderKind::WorkBuddy: return "WorkBuddy"; }
  throw Error("invalid_provider", "Unknown provider");
}
std::int64_t NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
namespace {
std::string Hex(const unsigned char* bytes, std::size_t length) {
  std::string out;
  for (std::size_t i = 0; i < length; ++i) { out += "0123456789abcdef"[bytes[i] >> 4]; out += "0123456789abcdef"[bytes[i] & 15]; }
  return out;
}
}
std::string Sha256(const std::string& text) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{}; unsigned length = 0;
  if (EVP_Digest(text.data(), text.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1) throw Error("crypto", "SHA256 failed");
  return Hex(digest.data(), length);
}
std::string RandomHex(std::size_t bytes) {
  if (bytes > 128) throw Error("crypto", "Nonce exceeds limit");
  std::array<unsigned char, 128> data{};
  if (RAND_bytes(data.data(), static_cast<int>(bytes)) != 1) throw Error("crypto", "Cannot generate nonce");
  return Hex(data.data(), bytes);
}
std::string Session::Key() const { return Sha256(Json::array({ToString(provider), profile, nativeId}).dump()); }
void to_json(Json& out, const Session& s) {
  out = {{"key", s.Key()}, {"provider", ToString(s.provider)}, {"profile", s.profile}, {"nativeId", s.nativeId},
    {"title", s.title}, {"cwd", s.cwd}, {"activityMs", s.activityMs}, {"activitySource", s.activitySource}};
}
void from_json(const Json& in, Session& s) {
  s = {ParseProvider(in.at("provider")), in.at("profile"), in.at("nativeId"), in.at("title"), in.at("cwd"), in.at("activityMs"), in.at("activitySource")};
  if (s.nativeId.empty() || s.profile.empty() || s.title.size() > 16384 || s.cwd.size() > 16384 || s.nativeId.size() > 1024)
    throw Error("invalid_session", "Invalid session metadata");
}
std::vector<Session> SelectTargets(std::vector<Session> sessions) {
  std::sort(sessions.begin(), sessions.end(), [](const auto& a, const auto& b) {
    if (a.provider != b.provider) return a.provider < b.provider;
    if (a.activityMs != b.activityMs) return a.activityMs > b.activityMs;
    if (a.nativeId != b.nativeId) return a.nativeId < b.nativeId;
    return a.profile < b.profile;
  });
  std::array<int, 3> counts{}; std::set<std::string> seen; std::vector<Session> out;
  for (const auto& s : sessions) if (seen.insert(s.Key()).second && counts[static_cast<int>(s.provider)] < 3) {
    out.push_back(s); ++counts[static_cast<int>(s.provider)];
  }
  return out;
}
std::string ShortTitle(const std::string& title) {
  auto text = icu::UnicodeString::fromUTF8(title);
  // Titles are presentation data, never Markdown instructions or control bytes.
  for (int32_t i = text.length() - 1; i >= 0; --i) if (text.charAt(i) < 32 || text.charAt(i) == 127) text.setCharAt(i, ' ');
  text.trim(); if (text.isEmpty()) return "未命名会话";
  UErrorCode error = U_ZERO_ERROR;
  std::unique_ptr<icu::BreakIterator> iterator(icu::BreakIterator::createCharacterInstance(icu::Locale::getRoot(), error));
  if (U_FAILURE(error) || !iterator) throw Error("unicode", "Cannot initialize grapheme iterator");
  iterator->setText(text); iterator->first(); int32_t end = 0;
  for (int count = 0; count < 10; ++count) { const auto next = iterator->next(); if (next == icu::BreakIterator::DONE) break; end = next; }
  std::string out; text.tempSubStringBetween(0, end).toUTF8String(out); return out;
}
std::string DialogueText(const Json& blocks) {
  if (blocks.is_string()) return blocks.get<std::string>();
  std::string out;
  if (blocks.is_array()) for (const auto& block : blocks) if (block.is_object() && block.value("type", "") == "text" && block.value("text", Json()).is_string()) {
    if (!out.empty()) out += '\n'; out += block.at("text").get<std::string>();
  }
  return out;
}
}
