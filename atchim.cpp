#include <algorithm>
#include <iostream>
#include <sqlite3.h>
#include <string>

class Database {
private:
  sqlite3 *db;
  char *zErrMsg = 0;
  int rc;
  std::string sql;

public:
  Database(const std::string &db_name) {
    rc = sqlite3_open(db_name.c_str(), &db);
    if (rc) {
      std::cerr << "Can't open database: " << sqlite3_errmsg(db) << std::endl;
      exit(0);
    }
    createTable();
  }

  ~Database() { sqlite3_close(db); }

  void createTable() {
    sql =
        "CREATE TABLE IF NOT EXISTS HISTORY("
        "ID INTEGER PRIMARY KEY AUTOINCREMENT,"
        "COMMAND TEXT NOT NULL,"
        "TIME TEXT);";

    rc = sqlite3_exec(db, sql.c_str(), nullptr, 0, &zErrMsg);
    if (rc != SQLITE_OK) {
      std::cerr << "SQL error: " << zErrMsg << std::endl;
      sqlite3_free(zErrMsg);
    }
  }

  void logCommandStart(const std::string &command) {
    sqlite3_stmt *stmt;
    sql = "INSERT INTO HISTORY (COMMAND) VALUES (?);";

    // we wanna strip any newlines from the command
    std::string stripped_command = command;
    stripped_command.erase(
        std::remove(stripped_command.begin(), stripped_command.end(), '\n'),
        stripped_command.end());

    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, stripped_command.c_str(), -1,
                        SQLITE_TRANSIENT);

      if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "SQL error: " << sqlite3_errmsg(db) << std::endl;
      }

      sqlite3_finalize(stmt);
    } else {
      std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db)
                << std::endl;
    }
  }
};

int main(int argc, char *argv[]) {
  if (argc != 3 || std::string(argv[1]) != "start") {
    std::cerr << "Invalid arguments" << std::endl;
    return 1;
  }
  
  // Get the home directory of the current user
  const char *homeDir = std::getenv("HOME");

  if (homeDir == nullptr) {
    std::cerr << "HOME environment variable not set" << std::endl;
    return 1;
  }

  // Construct the database path using the home directory
  std::string dbPath = std::string(homeDir) + "/.atchim.db";

  Database db(dbPath);
  db.logCommandStart(argv[2]);

  return 0;
}
