#pragma once
#include <sqlite3.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace atchim {
class SQLiteDatabase {
    sqlite3 *db_ = nullptr;
public:
    explicit SQLiteDatabase(const std::filesystem::path &path,
                            int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) {
        if (sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
            const std::string error = db_ ? sqlite3_errmsg(db_) : "Cannot open database";
            sqlite3_close(db_);
            throw std::runtime_error(error);
        }
        sqlite3_busy_timeout(db_, 5000);
    }
    ~SQLiteDatabase() { sqlite3_close(db_); }
    SQLiteDatabase(const SQLiteDatabase &) = delete;
    SQLiteDatabase &operator=(const SQLiteDatabase &) = delete;
    sqlite3 *get() const { return db_; }
    void execute(std::string_view sql) {
        if (sqlite3_exec(db_, std::string(sql).c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db_));
    }
};
class Statement {
    sqlite3_stmt *stmt_ = nullptr;
    void check(int rc) {
        if (rc != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(stmt_)));
    }
public:
    Statement(sqlite3 *db, std::string_view sql) {
        if (sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &stmt_, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;
    sqlite3_stmt *get() const { return stmt_; }
    void bindText(int i, std::string_view value) {
        check(sqlite3_bind_text(stmt_, i, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT));
    }
    void bindInt(int i, int value) { check(sqlite3_bind_int(stmt_, i, value)); }
    int step() { return sqlite3_step(stmt_); }
    std::string columnText(int i) const {
        const auto *text = sqlite3_column_text(stmt_, i);
        return text ? std::string(reinterpret_cast<const char *>(text), sqlite3_column_bytes(stmt_, i)) : "";
    }
    void reset() { check(sqlite3_reset(stmt_)); check(sqlite3_clear_bindings(stmt_)); }
};
inline bool tableHasColumn(SQLiteDatabase &db, std::string_view table, std::string_view column) {
    Statement stmt(db.get(), "SELECT name FROM pragma_table_info(?1) WHERE name = ?2 COLLATE NOCASE;");
    stmt.bindText(1, table);
    stmt.bindText(2, column);
    return stmt.step() == SQLITE_ROW;
}
inline void createAtchimSchema(SQLiteDatabase &db) {
    // Versioned migration: ordinary writes avoid DDL and its write lock.
    {
        Statement version(db.get(), "PRAGMA user_version;");
        if (version.step() == SQLITE_ROW && sqlite3_column_int(version.get(), 0) >= 1) return;
    }
    db.execute("BEGIN IMMEDIATE;");
    try {
        db.execute("CREATE TABLE IF NOT EXISTS HISTORY(ID INTEGER PRIMARY KEY AUTOINCREMENT, COMMAND TEXT NOT NULL, HOST TEXT DEFAULT 'local', TIME DATETIME DEFAULT CURRENT_TIMESTAMP);");
        if (!tableHasColumn(db, "HISTORY", "CWD")) db.execute("ALTER TABLE HISTORY ADD COLUMN CWD TEXT;");
        if (!tableHasColumn(db, "HISTORY", "EXIT_STATUS")) db.execute("ALTER TABLE HISTORY ADD COLUMN EXIT_STATUS INTEGER;");
        db.execute("CREATE INDEX IF NOT EXISTS IDX_HISTORY_TIME ON HISTORY(TIME); CREATE INDEX IF NOT EXISTS IDX_HISTORY_HOST ON HISTORY(HOST);");
        db.execute("CREATE INDEX IF NOT EXISTS IDX_HISTORY_COMMAND ON HISTORY(COMMAND COLLATE BINARY);");
        db.execute("PRAGMA user_version = 1;");
        db.execute("COMMIT;");
    } catch (...) { db.execute("ROLLBACK;"); throw; }
}
}
