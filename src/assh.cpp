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
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <uv.h>

#define BUFFER_SIZE 4096
#define SOCKET_PATH "/tmp/atchim_socket"

namespace {
std::string socketPath() {
    const char *value = std::getenv("ATCHIM_SOCKET");
    return value ? value : SOCKET_PATH;
}
} // namespace

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

        int sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock == -1)
            return;

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        const std::string path = socketPath();
        if (path.empty() || path.length() > sizeof(sockaddr_un::sun_path) - 1) {
            close(sock);
            return;
        }
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        socklen_t len =
            offsetof(sockaddr_un, sun_path) + strlen(addr.sun_path) + 1;

        if (connect(sock, reinterpret_cast<sockaddr *>(&addr), len) == -1) {
            close(sock);
            return;
        }

        std::string msg = ssh_host_ + "\t" + cmd + "\n";
        const char *data = msg.data();
        size_t remaining = msg.size();

        while (remaining > 0) {
            ssize_t written = write(sock, data, remaining);
            if (written > 0) {
                data += written;
                remaining -= written;
            } else if (errno == EINTR) {
                continue;
            } else {
                break;
            }
        }

        close(sock);
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
