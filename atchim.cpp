#include <iostream>
#include <sqlite3.h>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <algorithm>
class AsyncLogger {
private:
    std::queue<std::string> commandQueue;
    std::mutex queueMutex;
    std::condition_variable cv;
    std::thread dbThread;
    bool stopThread = false;
    sqlite3 *db;

    void databaseThreadFunction() {
        while (true) {
            std::unique_lock<std::mutex> lock(queueMutex);
            cv.wait(lock, [this]{ return !commandQueue.empty() || stopThread; });

            if (stopThread && commandQueue.empty()) {
                break;
            }

            std::string command = commandQueue.front();
            commandQueue.pop();
            lock.unlock();

            logToDatabase(command);
        }
    }

    void logToDatabase(const std::string &command) {
        sqlite3_stmt *stmt;
        const std::string sql = "INSERT INTO HISTORY (COMMAND) VALUES (?);";

        // we wanna strip any newlines from the command
        std::string stripped_command = command;
        stripped_command.erase(
            std::remove(stripped_command.begin(), stripped_command.end(), '\n'),
            stripped_command.end());

        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, stripped_command.c_str(), -1, SQLITE_TRANSIENT);

            if (sqlite3_step(stmt) != SQLITE_DONE) {
                std::cerr << "SQL error: " << sqlite3_errmsg(db) << std::endl;
            }

            sqlite3_finalize(stmt);
        } else {
            std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
        }
    }

public:
    AsyncLogger(const std::string &db_name) {
        if (sqlite3_open(db_name.c_str(), &db)) {
            std::cerr << "Can't open database: " << sqlite3_errmsg(db) << std::endl;
            exit(0);
        }
        dbThread = std::thread(&AsyncLogger::databaseThreadFunction, this);
    }

    ~AsyncLogger() {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            stopThread = true;
        }
        cv.notify_one();
        dbThread.join();
        sqlite3_close(db);
    }

    void logCommand(const std::string &command) {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            commandQueue.push(command);
        }
        cv.notify_one();
    }
};

int main(int argc, char *argv[]) {
    if (argc != 3 || std::string(argv[1]) != "start") {
        std::cerr << "Invalid arguments" << std::endl;
        return 1;
    }

    const char *homeDir = std::getenv("HOME");
    if (homeDir == nullptr) {
        std::cerr << "HOME environment variable not set" << std::endl;
        return 1;
    }

    std::string dbPath = std::string(homeDir) + "/.atchim.db";
    AsyncLogger logger(dbPath);  // AsyncLogger instance with database connection

    std::string command = argv[2];
    logger.logCommand(command);

    return 0;
}
