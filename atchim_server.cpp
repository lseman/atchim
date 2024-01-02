/**
 * @file: atchim_server.cpp
 * @author: Laio Oriel Seman
 * @contact: laio [at] gos.ufsc.br
 *
 * Description: Server component of the Atchim command logging tool. This server listens
 * for commands from the Atchim client and logs them to a SQLite database.
 */

#include <algorithm>
#include <condition_variable>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <queue>
#include <signal.h>
#include <sqlite3.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <syslog.h>
#include <thread>
#include <unistd.h>

/**
 * @brief Handles signals for graceful shutdown.
 *
 * This function is responsible for handling signals, such as SIGTERM,
 * to perform a graceful shutdown of the server. It logs the received
 * signal and shuts down the server by performing necessary cleanup
 * operations.
 *
 * @param sig The signal number.
 */
void handleSignal(int sig) {
    // Handle signals like SIGTERM for graceful shutdown
    syslog(LOG_INFO, "Received signal %d, shutting down.", sig);
    // Perform cleanup...
    exit(EXIT_SUCCESS);
}

/**
 * @class AsyncLogger
 * @brief A class that asynchronously logs commands to a SQLite database.
 */
class AsyncLogger {
  private:
    std::queue<std::string> commandQueue;
    std::mutex queueMutex;
    std::condition_variable cv;
    std::thread dbThread;
    bool stopThread = false;
    sqlite3 *db;

    /**
     * @brief This function is responsible for processing commands from a
     * command queue and logging them to the database.
     *
     * It runs in a separate thread and continuously waits for commands to be
     * added to the queue. Once a command is available, it retrieves the command
     * from the front of the queue, logs it to the database, and repeats the
     * process until the thread is stopped.
     */
    void databaseThreadFunction() {
        while (true) {
            std::unique_lock<std::mutex> lock(queueMutex);
            cv.wait(lock,
                    [this] { return !commandQueue.empty() || stopThread; });

            if (stopThread && commandQueue.empty()) {
                break;
            }

            std::string command = commandQueue.front();
            commandQueue.pop();
            lock.unlock();

            logToDatabase(command);
        }
    }

    /**
     * Logs the given command to the database.
     *
     * @param command The command to be logged.
     */
    void logToDatabase(const std::string &command) {
        sqlite3_stmt *stmt;
        const std::string sql = "INSERT INTO HISTORY (COMMAND) VALUES (?);";

        // we wanna strip any newlines from the command
        std::string stripped_command = command;
        stripped_command.erase(
            std::remove(stripped_command.begin(), stripped_command.end(), '\n'),
            stripped_command.end());

        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) ==
            SQLITE_OK) {
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

  public:
    /**
     * @brief Constructs an AsyncLogger object with the specified database name.
     *
     * @param db_name The name of the database to be opened.
     */
    AsyncLogger(const std::string &db_name) {
        if (sqlite3_open(db_name.c_str(), &db)) {
            std::cerr << "Can't open database: " << sqlite3_errmsg(db)
                      << std::endl;
            exit(0);
        }
        createTable();
        dbThread = std::thread(&AsyncLogger::databaseThreadFunction, this);
    }

    /**
     * @brief Destructor for the AsyncLogger class.
     * 
     * This destructor stops the logging thread, closes the SQLite database connection, and cleans up resources.
     */
    ~AsyncLogger() {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            stopThread = true;
        }
        cv.notify_one();
        dbThread.join();
        sqlite3_close(db);
    }
    
    /**
     * @brief Creates the table for logging commands.
     * 
     * This function creates the table for logging commands if it does not exist.
     */
    void createTable() {
        const std::string sql =
           "CREATE TABLE IF NOT EXISTS HISTORY("
           "ID INTEGER PRIMARY KEY AUTOINCREMENT,"
           "COMMAND TEXT NOT NULL,"
           "TIME DATETIME DEFAULT CURRENT_TIMESTAMP);";

        char *err_msg = nullptr;
        if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err_msg) !=
            SQLITE_OK) {
            std::cerr << "SQL error: " << err_msg << std::endl;
            sqlite3_free(err_msg);
        }
    }


    /**
     * @brief Logs a command and adds it to the command queue.
     * 
     * This function takes a command as input and adds it to the command queue.
     * It also notifies the waiting threads that a new command is available.
     * 
     * @param command The command to be logged and added to the queue.
     */
    void logCommand(const std::string &command) {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            commandQueue.push(command);
        }
        cv.notify_one();
    }
};

/**
 * @brief The main function of the atchim_server program.
 * 
 * This function is the entry point of the atchim_server program. It sets up a daemon process,
 * creates a Unix domain socket, listens for client connections, and handles client commands.
 * 
 * @return int The exit status of the program.
 */
int main() {
    // Double fork
    pid_t pid = fork();
    if (pid < 0)
        exit(EXIT_FAILURE);
    if (pid > 0)
        exit(EXIT_SUCCESS);

    if (setsid() < 0)
        exit(EXIT_FAILURE);
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0)
        exit(EXIT_FAILURE);
    if (pid > 0)
        exit(EXIT_SUCCESS);

    umask(0);
    chdir("/");

    for (int x = sysconf(_SC_OPEN_MAX); x >= 0; x--) {
        close(x);
    }

    open("/dev/null", O_RDWR);
    dup(0);
    dup(0);

    openlog("atchim_daemon", LOG_PID, LOG_DAEMON);
    syslog(LOG_INFO, "Daemon started");

    signal(SIGTERM, handleSignal);

    const char *homeDir = std::getenv("HOME");
    if (homeDir == nullptr) {
        std::cerr << "HOME environment variable not set" << std::endl;
        return 1;
    }
    std::string dbPath = std::string(homeDir) + "/.atchim.db";
    AsyncLogger logger(dbPath); // AsyncLogger instance with database connection

    int server_fd, client_fd;
    struct sockaddr_un address;
    const char *socket_path = "/tmp/atchim_socket";

    // Create socket
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd == -1) {
        std::cerr << "Socket creation failed" << std::endl;
        return 1;
    }

    // Clearing and setting address structure
    memset(&address, 0, sizeof(struct sockaddr_un));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, socket_path, sizeof(address.sun_path) - 1);

    // Bind socket to a name
    unlink(socket_path); // Remove previous socket file if exists
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) == -1) {
        std::cerr << "Socket bind failed" << std::endl;
        close(server_fd);
        return 1;
    }

    // Listen for connections
    if (listen(server_fd, SOMAXCONN) == -1) {
        std::cerr << "Listen failed" << std::endl;
        close(server_fd);
        return 1;
    }

    std::cout << "Server is listening on " << socket_path << std::endl;

    // Accept and handle client connections
    while (true) {
        if ((client_fd = accept(server_fd, NULL, NULL)) == -1) {
            std::cerr << "Accept failed" << std::endl;
            continue;
        }

        char buffer[1024] = {0};
        ssize_t read_bytes = read(client_fd, buffer, sizeof(buffer));
        if (read_bytes > 0) {
            std::cout << "Received command: " << buffer << std::endl;
            logger.logCommand(buffer); // Log command to database
        }

        close(client_fd);
    }

    // Clean up (in practice, this part of code may never be reached)
    close(server_fd);
    unlink(socket_path);
    return 0;
}
