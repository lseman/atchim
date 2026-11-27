// atchim_server.cpp
#include <fcntl.h>
#include <sqlite3.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <syslog.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "atchim/db.hpp"

namespace {
constexpr std::string_view SOCKET_PATH = "/tmp/atchim_socket";
constexpr size_t BUFFER_SIZE = 1024;
constexpr size_t MAX_THREADS = 10;
constexpr size_t MAX_COMMAND_LENGTH = 4096;
volatile sig_atomic_t shutdown_requested = 0;

std::string getEnvOrDefault(const char *name, std::string_view fallback) {
    const char *value = std::getenv(name);
    return value ? value : std::string(fallback);
}

std::filesystem::path databasePath() {
    if (const char *db = std::getenv("ATCHIM_DB")) {
        return db;
    }
    const char *home = std::getenv("HOME");
    if (!home)
        throw std::runtime_error("HOME not set");
    return std::filesystem::path(home) / ".atchim.db";
}

void requestShutdown(int) { shutdown_requested = 1; }

void installShutdownHandler(int signal_number) {
    struct sigaction action{};
    action.sa_handler = requestShutdown;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(signal_number, &action, nullptr) == -1) {
        throw std::system_error(errno, std::system_category(),
                                "Failed to install signal handler");
    }
}
} // namespace

class Daemon {
  public:
    static void daemonize() {
        auto fork_and_exit = []() {
            const pid_t pid = fork();
            if (pid < 0)
                throw std::runtime_error("Fork failed");
            if (pid > 0)
                exit(EXIT_SUCCESS);
        };
        fork_and_exit();
        if (setsid() < 0)
            throw std::runtime_error("Setsid failed");
        signal(SIGCHLD, SIG_IGN);
        signal(SIGHUP, SIG_IGN);
        fork_and_exit();
        umask(0);
        std::filesystem::current_path("/");
        for (int x = sysconf(_SC_OPEN_MAX); x >= 0; x--)
            close(x);
        [[maybe_unused]] auto null_fd = open("/dev/null", O_RDWR);
        dup2(0, 1);
        dup2(0, 2);
    }
};

class AsyncLogger {
  public:
    explicit AsyncLogger(const std::filesystem::path &db_path)
        : db_(db_path), stop_flag_(false) {
        createTable();
        worker_ = std::thread([this]() { databaseThreadFunction(); });
    }
    ~AsyncLogger() {
        {
            std::lock_guard lock(queue_mutex_);
            stop_flag_ = true;
        }
        cv_.notify_one();
        if (worker_.joinable())
            worker_.join();
    }
    void logCommand(std::string command) {
        if (command.size() > MAX_COMMAND_LENGTH) {
            syslog(LOG_WARNING, "Discarded long command (%zu bytes)",
                   command.size());
            return;
        }
        {
            std::lock_guard lock(queue_mutex_);
            command_queue_.push(std::move(command));
        }
        cv_.notify_one();
    }

  private:
    void createTable() {
        atchim::createAtchimSchema(db_);
    }
    void logToDatabase(std::string_view message) {
        while (!message.empty() &&
               (message.back() == '\n' || message.back() == '\r')) {
            message.remove_suffix(1);
        }
        auto [host, command] = parseMessage(message);
        const std::string sql =
            "INSERT INTO HISTORY (COMMAND, HOST) VALUES (?, ?);";
        sqlite3_stmt *stmt = nullptr;
        auto db_handle = db_.get();

        if (sqlite3_prepare_v2(db_handle, sql.c_str(), -1, &stmt, nullptr) !=
            SQLITE_OK) {
            syslog(LOG_ERR, "Prepare fail: %s", sqlite3_errmsg(db_handle));
            return;
        }

        sqlite3_bind_text(stmt, 1, command.data(), command.size(),
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, host.data(), host.size(), SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            syslog(LOG_ERR, "Exec fail: %s (command: %.100s)",
                   sqlite3_errmsg(db_handle), command.data());
        }
        sqlite3_finalize(stmt);
    }
    void databaseThreadFunction() {
        while (true) {
            std::string message;
            {
                std::unique_lock lock(queue_mutex_);
                cv_.wait(lock, [this]() {
                    return !command_queue_.empty() || stop_flag_;
                });
                if (stop_flag_ && command_queue_.empty())
                    break;
                message = std::move(command_queue_.front());
                command_queue_.pop();
            }
            logToDatabase(message);
        }
    }
    static std::pair<std::string_view, std::string_view>
    parseMessage(std::string_view message) {
        auto tab_pos = message.find('\t');
        if (tab_pos == std::string_view::npos)
            return {"local", message};
        return {message.substr(0, tab_pos), message.substr(tab_pos + 1)};
    }
    atchim::SQLiteDatabase db_;
    std::queue<std::string> command_queue_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> stop_flag_;
};

class UnixSocket {
  public:
    UnixSocket() = default;

    explicit UnixSocket(int fd) : fd_(fd) {
        if (fd_ == -1)
            throw std::system_error(errno, std::system_category(),
                                    "Invalid socket");
    }

    static UnixSocket create() {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd == -1)
            throw std::system_error(errno, std::system_category(),
                                    "Socket creation failed");
        return UnixSocket(fd);
    }

    ~UnixSocket() {
        if (fd_ != -1)
            close(fd_);
    }
    UnixSocket(UnixSocket &&o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    UnixSocket &operator=(UnixSocket &&o) noexcept {
        if (this != &o) {
            if (fd_ != -1)
                close(fd_);
            fd_ = o.fd_;
            o.fd_ = -1;
        }
        return *this;
    }
    int get() const { return fd_; }

  private:
    int fd_ = -1;
};

class SocketServer {
  public:
    SocketServer(const std::filesystem::path &socket_path,
                 std::shared_ptr<AsyncLogger> logger)
        : socket_path_(socket_path), logger_(std::move(logger)),
          server_socket_(UnixSocket::create()), running_(true) {
        const std::string path_str = socket_path_.string();
        if (path_str.empty() ||
            path_str.length() > sizeof(sockaddr_un::sun_path) - 1) {
            throw std::invalid_argument("Invalid socket path");
        }

        std::filesystem::remove(socket_path_);
        struct sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, path_str.c_str(),
                     sizeof(address.sun_path) - 1);
        socklen_t addrLen =
            offsetof(sockaddr_un, sun_path) + path_str.length() + 1;

        int opt = 1;
        setsockopt(server_socket_.get(), SOL_SOCKET, SO_REUSEADDR, &opt,
                   sizeof(opt));

        if (bind(server_socket_.get(), reinterpret_cast<sockaddr *>(&address),
                 addrLen) == -1)
            throw std::system_error(errno, std::system_category(),
                                    "Bind failed");

        if (listen(server_socket_.get(), SOMAXCONN) == -1)
            throw std::system_error(errno, std::system_category(),
                                    "Listen failed");

        for (size_t i = 0; i < MAX_THREADS; ++i)
            threads_.emplace_back(&SocketServer::workerThread, this);
    }

    ~SocketServer() {
        shutdown();
        std::filesystem::remove(socket_path_);
    }

    void run() {
        while (!shutdown_requested) {
            int client_fd = accept(server_socket_.get(), nullptr, nullptr);
            if (client_fd == -1) {
                if (shutdown_requested)
                    break;
                syslog(LOG_ERR, "Accept failed: %s", strerror(errno));
                continue;
            }
            {
                std::lock_guard lock(queue_mutex_);
                client_queue_.emplace(client_fd);
            }
            client_queue_cv_.notify_one();
        }
    }

    void shutdown() {
        running_ = false;
        client_queue_cv_.notify_all();
        for (auto &t : threads_)
            if (t.joinable())
                t.join();
    }

  private:
    void workerThread() {
        while (running_) {
            UnixSocket client;
            {
                std::unique_lock lock(queue_mutex_);
                client_queue_cv_.wait(lock, [this] {
                    return !running_ || !client_queue_.empty();
                });
                if (!running_ && client_queue_.empty())
                    return;
                client = std::move(client_queue_.front());
                client_queue_.pop();
            }
            handleClient(std::move(client));
        }
    }

    void handleClient(UnixSocket client) {
        try {
            std::array<char, BUFFER_SIZE> buffer{};
            std::string message;
            while (true) {
                ssize_t n = read(client.get(), buffer.data(), buffer.size());
                if (n > 0) {
                    message.append(buffer.data(), n);
                    if (message.find('\n') != std::string::npos)
                        break;
                } else if (n == 0) {
                    break;
                } else if (errno != EINTR) {
                    throw std::system_error(errno, std::system_category(),
                                            "Read error");
                }
            }
            message = message.substr(0, message.find('\n'));
            if (!message.empty())
                logger_->logCommand(std::move(message));
        } catch (const std::exception &e) {
            syslog(LOG_ERR, "Client error: %s", e.what());
        }
    }

    std::filesystem::path socket_path_;
    std::shared_ptr<AsyncLogger> logger_;
    UnixSocket server_socket_;
    std::queue<UnixSocket> client_queue_;
    std::mutex queue_mutex_;
    std::condition_variable client_queue_cv_;
    std::vector<std::thread> threads_;
    std::atomic<bool> running_;
};

int main() try {
    Daemon::daemonize();
    installShutdownHandler(SIGTERM);
    installShutdownHandler(SIGINT);

    auto logger = std::make_shared<AsyncLogger>(databasePath());
    SocketServer server(getEnvOrDefault("ATCHIM_SOCKET", SOCKET_PATH),
                        std::move(logger));
    server.run();
    server.shutdown();
    return EXIT_SUCCESS;
} catch (const std::exception &e) {
    syslog(LOG_ERR, "Fatal: %s", e.what());
    return EXIT_FAILURE;
}
