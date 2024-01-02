/**
 * @file: atchim_server.cpp
 * @author: Laio Oriel Seman
 * @contact: laio [at] gos.ufsc.br
 *
 * Description: Server component of the Atchim command logging tool. This server
 * listens for commands from the Atchim client and logs them to a SQLite
 * database.
 */

#include <algorithm>
#include <condition_variable>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <pthread.h>
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

#define SOCKET_PATH "/tmp/atchim_socket"
#define BUFFER_SIZE 1024

/**
 * @brief Daemonizes the process by forking and closing file descriptors.
 * 
 * This function is used to daemonize the process, detaching it from the controlling terminal
 * and running it in the background as a daemon. It performs the following steps:
 * 1. Forks the process.
 * 2. If the fork fails, the function exits with failure status.
 * 3. If the fork succeeds and the parent process is still running, the function exits with success status.
 * 4. The child process becomes the new session leader and process group leader by calling setsid().
 * 5. Ignores the SIGCHLD and SIGHUP signals.
 * 6. Forks the process again.
 * 7. If the second fork fails, the function exits with failure status.
 * 8. If the second fork succeeds and the parent process is still running, the function exits with success status.
 * 9. Sets the file mode creation mask to 0 and changes the current working directory to the root directory.
 * 10. Closes all open file descriptors except for stdin, stdout, and stderr.
 * 11. Opens /dev/null and duplicates the file descriptor to stdin, stdout, and stderr.
 */
void daemonize() {
    pid_t pid = fork();
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS); // Parent exits
    }

    if (setsid() < 0) {
        exit(EXIT_FAILURE);
    }

    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS); // First child exits
    }

    umask(0);
    chdir("/");

    // Close all open file descriptors
    for (int x = sysconf(_SC_OPEN_MAX); x >= 0; x--) {
        close(x);
    }

    open("/dev/null", O_RDWR);
    dup(0);
    dup(0);
}

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
    void logToDatabase(const std::string &message) {
        std::string ssh_host, command;

        // Split the message into ssh_host and command, if possible
        size_t separatorPos = message.find('\t');
        if (separatorPos != std::string::npos) {
            // If \0 is found, split the message
            ssh_host = message.substr(0, separatorPos);
            command = message.substr(separatorPos + 1);
        } else {
            // If \0 is not found, treat the whole message as command and host
            // as "local"
            ssh_host = "local";
            command = message;
        }

        // Remove newlines from command
        command.erase(std::remove(command.begin(), command.end(), '\n'),
                      command.end());

        // Prepare SQL statement
        const std::string sql =
            "INSERT INTO HISTORY (COMMAND, HOST) VALUES (?, ?);";
        sqlite3_stmt *stmt;

        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) ==
            SQLITE_OK) {
            // Bind ssh_host and command to the prepared statement
            sqlite3_bind_text(stmt, 1, command.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, ssh_host.c_str(), -1, SQLITE_TRANSIENT);

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
     * This destructor stops the logging thread, closes the SQLite database
     * connection, and cleans up resources.
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
     * This function creates the table for logging commands if it does not
     * exist.
     */
    void createTable() {
        const std::string sql = "CREATE TABLE IF NOT EXISTS HISTORY("
                                "ID INTEGER PRIMARY KEY AUTOINCREMENT,"
                                "COMMAND TEXT NOT NULL,"
                                "HOST TEXT DEFAULT local,"
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

std::mutex logger_mutex;
AsyncLogger *global_logger = nullptr; // Global logger instance

void initialize_global_logger() {
    const char *homeDir = std::getenv("HOME");
    if (homeDir == nullptr) {
        std::cerr << "HOME environment variable not set" << std::endl;
        exit(EXIT_FAILURE);
    }
    std::string dbPath = std::string(homeDir) + "/.atchim.db";
    global_logger = new AsyncLogger(dbPath);
}

void *handle_client(void *arg) {
    int client_fd = *(int *)arg;
    delete (int *)arg; // Clean up the heap memory

    std::cout << "Client connected" << std::endl;

    char buffer[BUFFER_SIZE];
    while (true) {
        ssize_t read_bytes = read(client_fd, buffer, sizeof(buffer) - 1);

        if (read_bytes > 0) {
            buffer[read_bytes] = '\0'; // Null-terminate the string
            std::lock_guard<std::mutex> lock(
                logger_mutex); // Synchronize access
            global_logger->logCommand(buffer);
        } else if (read_bytes == 0) {
            // Client disconnected
            break;
        } else {
            // An error occurred
            std::cerr << "Read error: " << strerror(errno) << std::endl;
            break;
        }
    }

    std::cout << "Client disconnected" << std::endl;
    close(client_fd);
    return NULL;
}

/**
 * @brief The main function of the atchim_server program.
 *
 * This function is the entry point of the atchim_server program. It sets up a
 * daemon process, creates a Unix domain socket, listens for client connections,
 * and handles client commands.
 *
 * @return int The exit status of the program.
 */

int main() {
    daemonize();

    const char *homeDir = std::getenv("HOME");
    if (homeDir == nullptr) {
        std::cerr << "HOME environment variable not set" << std::endl;
        return 1;
    }
    std::string dbPath = std::string(homeDir) + "/.atchim.db";
    AsyncLogger logger(dbPath); // AsyncLogger instance with database connection
    initialize_global_logger();

    int server_fd;
    struct sockaddr_un address;

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd == -1) {
        exit(EXIT_FAILURE);
    }

    memset(&address, 0, sizeof(struct sockaddr_un));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, SOCKET_PATH, sizeof(address.sun_path) - 1);

    unlink(SOCKET_PATH);
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) == -1) {
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, SOMAXCONN) == -1) {
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    while (true) {
        int *client_fd = new int; // Allocate memory on the heap
        *client_fd = accept(server_fd, NULL, NULL);
        if (*client_fd == -1) {
            delete client_fd; // Clean up the heap memory
            continue;
        }

        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, client_fd) != 0) {
            close(*client_fd);
            delete client_fd; // Clean up the heap memory
        }
    }

    close(server_fd);
    unlink(SOCKET_PATH);
    return 0;
}