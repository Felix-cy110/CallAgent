#include "callagent/WorkBuddyRepository.hpp"
#include "callagent/Files.hpp"
#include <sqlite3.h>
#include <memory>
namespace callagent {
std::string WorkBuddyRepository::Account() const {
  const auto row = ReadJson(directory_ / "storage/skeleton/account-snapshot.json", 64 * 1024).at("primary");
  const auto uid = row.at("uid").get<std::string>();
  if (row.at("version") != 1 || uid.empty() || uid.size() > 256 || uid.find_first_of("\r\n\t") != std::string::npos) throw Error("account", "WorkBuddy current account is unavailable");
  return uid;
}
std::vector<Session> WorkBuddyRepository::List() const {
  const auto account = Account(); const auto path = directory_ / "workbuddy.db"; RejectSymlinks(path);
  sqlite3* raw = nullptr;
  const int opened = sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
  if (opened != SQLITE_OK) throw Error("database", "Cannot open WorkBuddy read-only catalogue");
  sqlite3_busy_timeout(raw, 1000); sqlite3_stmt* statement = nullptr;
  const int prepared = sqlite3_prepare_v2(raw, "SELECT id,cwd,COALESCE(custom_title,title,''),updated_at FROM sessions WHERE user_id=? AND transport='local' AND deleted_at IS NULL ORDER BY updated_at DESC,id ASC LIMIT 500", -1, &statement, nullptr);
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> query(statement, sqlite3_finalize);
  if (prepared != SQLITE_OK || sqlite3_bind_text(statement, 1, account.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) throw Error("database", "Unsupported WorkBuddy catalogue schema");
  auto text = [&](int column) { const auto* value = sqlite3_column_text(statement, column); return value ? std::string(reinterpret_cast<const char*>(value), sqlite3_column_bytes(statement, column)) : std::string{}; };
  std::vector<Session> sessions; int code;
  while ((code = sqlite3_step(statement)) == SQLITE_ROW) sessions.push_back({ProviderKind::WorkBuddy, Json::array({directory_.string(), account}).dump(),
    text(0), text(2), text(1), sqlite3_column_int64(statement, 3), "native.updatedAt"});
  if (code != SQLITE_DONE) throw Error("database", "WorkBuddy catalogue read failed");
  if (Account() != account) throw Error("account_changed", "WorkBuddy account changed during catalogue read");
  return sessions;
}
void WorkBuddyRepository::Validate(const Session& session) const {
  for (const auto& current : List()) if (current.Key() == session.Key()) {
    if (current.cwd != session.cwd) throw Error("identity", "WorkBuddy session directory changed"); return;
  }
  throw Error("identity", "WorkBuddy session is deleted, outside the catalogue limit, or belongs to a different account");
}
}
