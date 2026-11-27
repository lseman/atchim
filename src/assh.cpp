#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <pty.h>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <fnmatch.h>
#include "atchim/db.hpp"

#include <unistd.h>
#include <uv.h>

#define BUFFER_SIZE 4096
class PTYHandler {
  public:
    PTYHandler(const std::string &ssh_host, char **argv)
        : ssh_host_(ssh_host), argv_(argv) {
        global_instance = this;
        if ((master_fd_ = posix_openpt(O_RDWR | O_NOCTTY)) < 0 ||
            grantpt(master_fd_) < 0 || unlockpt(master_fd_) < 0) {
            throw std::runtime_error("PTY allocation failed");
        }
    }

    ~PTYHandler() {
        if (master_fd_ >= 0)
            close(master_fd_);
        global_instance = nullptr;
    }

    void run() {
        setupSignals();
        startChild();
        startEventLoop();
    }

  private:
    void setupSignals() {
        signal(SIGWINCH, handleWinch);
        handleWinch(SIGWINCH);
        signal(SIGINT, handleSigint);
    }

    void startChild() {
        const char *slave = ptsname(master_fd_);
        if (!slave)
            throw std::runtime_error(std::string("ptsname failed: ") +
                                     strerror(errno));

        if (fork() == 0) {
            setsid();
            int slave_fd = open(slave, O_RDWR);
            if (slave_fd < 0)
                exit(1);

            ioctl(slave_fd, TIOCSCTTY, nullptr);
            dup2(slave_fd, STDIN_FILENO);
            dup2(slave_fd, STDOUT_FILENO);
            dup2(slave_fd, STDERR_FILENO);
            close(slave_fd);

            execvp("ssh", argv_);
            exit(127);
        }
    }

    void startEventLoop() {
        uv_loop_t *loop = uv_default_loop();

        if (uv_poll_init(loop, &pty_poll_, master_fd_) != 0 ||
            uv_poll_init(loop, &stdin_poll_, STDIN_FILENO) != 0) {
            throw std::runtime_error("uv_poll_init failed");
        }

        pty_poll_.data = this;
        stdin_poll_.data = this;

        if (uv_poll_start(&pty_poll_, UV_READABLE, onPtyRead) != 0 ||
            uv_poll_start(&stdin_poll_, UV_READABLE, onStdinRead) != 0) {
            throw std::runtime_error("uv_poll_start failed");
        }

        uv_run(loop, UV_RUN_DEFAULT);
        uv_loop_close(loop);
    }

    static void handleWinch(int) {
        if (!global_instance)
            return;
        winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != -1) {
            ioctl(global_instance->master_fd_, TIOCSWINSZ, &ws);
        }
    }

    static void handleSigint(int) {
        if (global_instance && global_instance->master_fd_ >= 0) {
            char ctrl_c = 0x03;
            write(global_instance->master_fd_, &ctrl_c, 1);
        }
    }

    static void onPtyRead(uv_poll_t *h, int, int) {
        auto *self = static_cast<PTYHandler *>(h->data);
        char buf[BUFFER_SIZE];
        ssize_t r = read(self->master_fd_, buf, sizeof(buf));
        if (r > 0)
            write(STDOUT_FILENO, buf, r);
        else
            uv_stop(uv_default_loop());
    }

    static void onStdinRead(uv_poll_t *h, int, int) {
        auto *self = static_cast<PTYHandler *>(h->data);
        char buf[BUFFER_SIZE];
        ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
        if (r <= 0)
            return;

        for (ssize_t i = 0; i < r; ++i) {
            char ch = buf[i];
            if (ch == '\n' && !self->buffer_.empty()) {
                self->logCommand(self->buffer_);
                self->buffer_.clear();
            } else if (ch == 0x7F || ch == 0x08) {
                if (!self->buffer_.empty())
                    self->buffer_.pop_back();
            } else if (std::isprint(static_cast<unsigned char>(ch))) {
                self->buffer_ += ch;
            }
            write(self->master_fd_, &ch, 1);
        }
    }

    void logCommand(const std::string &cmd) {
        if (cmd.empty())
            return;

        const char *record = std::getenv("ATCHIM_RECORD");
        if (record && std::string_view(record) == "0") return;
        const char *ignoreSpace = std::getenv("ATCHIM_IGNORE_SPACE");
        if ((!ignoreSpace || std::string_view(ignoreSpace) == "1") && cmd.front() == ' ') return;
        const char *ignore = std::getenv("ATCHIM_IGNORE");
        const std::string patterns = ignore ? ignore : "";
        size_t offset = 0;
        while (offset < patterns.size()) {
            const auto end = patterns.find('\n', offset);
            const auto pattern = patterns.substr(offset, end - offset);
            if (!pattern.empty() && fnmatch(pattern.c_str(), cmd.c_str(), 0) == 0) return;
            if (end == std::string::npos) break;
            offset = end + 1;
        }
        try {
            const char *configured = std::getenv("ATCHIM_DB");
            const char *home = std::getenv("HOME");
            if (!configured && !home) throw std::runtime_error("HOME is not set; use ATCHIM_DB");
            const auto path = configured ? std::filesystem::path(configured)
                : std::filesystem::path(home) / ".atchim.db";
            atchim::SQLiteDatabase db(path);
            atchim::createAtchimSchema(db);
            atchim::Statement insert(db.get(), "INSERT INTO HISTORY(COMMAND, HOST) VALUES(?1, ?2);");
            insert.bindText(1, cmd);
            insert.bindText(2, ssh_host_);
            if (insert.step() != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db.get()));
        } catch (const std::exception &error) {
            // Logging failure must not terminate the SSH session or escape a C callback.
            std::cerr << "atchim: SSH history write failed: " << error.what() << '\n';
        }
    }

    int master_fd_ = -1;
    std::string ssh_host_;
    std::string buffer_;
    char **argv_;
    uv_poll_t pty_poll_;
    uv_poll_t stdin_poll_;

    static inline PTYHandler *global_instance = nullptr;
};

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: assh <ssh_host> <args...>" << std::endl;
        return 1;
    }
    try {
        PTYHandler p(argv[1], argv);
        p.run();
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
