/**
 * @file: assh.cpp
 * @author: Laio Oriel Seman
 * @contact: laio [at] gos.ufsc.br
 *
 * Description: ssh wrapper that logs commands to Atchim
 */

#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <uv.h>
// #include "cpp-terminal/cpp-terminal/terminal.hpp"
//  int master_fd_global;
int log_socket = -1; // Global log socket
std::string ssh_host;
#define BUFFER_SIZE 4096

/**
 * @brief Handles the window size change signal.
 *
 * This function is called when the window size changes and updates the window
 * size of the terminal. It uses the TIOCGWINSZ and TIOCSWINSZ ioctl calls to
 * get and set the window size, respectively.
 *
 * @param signum The signal number.
 */
void handle_winch(int signum, int master_fd = -1) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        perror("ioctl TIOCGWINSZ");
        return;
    }
    if (ioctl(master_fd, TIOCSWINSZ, &ws) == -1) {
        perror("ioctl TIOCSWINSZ");
    }
}

/**
 * @brief Sets up a logging socket connection.
 *
 * This function creates a socket, connects it to a server, and sets up a
 * logging socket connection. The socket is created using the AF_UNIX domain and
 * SOCK_STREAM type. The connection is established by connecting to the server
 * using the specified address. If the connection fails, an error message is
 * printed and the socket is closed.
 */
void setup_logging_socket() {
    struct sockaddr_un address;
    log_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (log_socket == -1) {
        std::cerr << "Socket creation failed" << std::endl;
        return;
    }

    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, "/tmp/atchim_socket");
    if (connect(log_socket, (struct sockaddr *)&address, sizeof(address)) ==
        -1) {
        std::cerr << "Connection to server failed" << std::endl;
        close(log_socket);
        log_socket = -1;
    }
}

/**
 * Sends a command to the Atchim logging server.
 *
 * @param command The command to be logged.
 */
void logToAtchim(const std::string &command) {
    if (log_socket == -1) {
        setup_logging_socket();
        if (log_socket == -1)
            return;
    }

    // Construct the message with the host and command
    std::string message = std::string(ssh_host) + "\t" + command;

    ssize_t total_written = 0;
    size_t to_write = message.length();
    const char *buffer_ptr = message.c_str();

    while (total_written < to_write) {
        ssize_t written = write(log_socket, buffer_ptr + total_written,
                                to_write - total_written);
        if (written == -1) {
            std::cerr << "Failed to send command, error: " << strerror(errno)
                      << std::endl;
            if (errno == EPIPE) {
                std::cerr << "Broken pipe, attempting to reconnect"
                          << std::endl;
                close(log_socket);
                setup_logging_socket();
                if (log_socket == -1)
                    return; // Failed to re-establish connection
                continue;   // Try writing again
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "Failed to send command, error: "
                          << strerror(errno) << std::endl;
                close(log_socket);
                log_socket = -1;
                return;
            }
        } else {
            total_written += written;
        }
    }
}
int master_fd;
pid_t child_pid = -1;

v
/**
 * @brief Callback function for reading data from a stream.
 * 
 * This function is called when data is read from the stream. It handles the read data and performs necessary operations.
 * 
 * @param stream The stream from which the data is read.
 * @param nread The number of bytes read.
 * @param buf The buffer containing the read data.
 */
void read_callback(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    if (nread > 0) {
        // Handle the read data (buf->base)
        std::cout.write(buf->base, nread);
    } else if (nread < 0) {
        if (nread != UV_EOF) {
            std::cerr << "Read error: " << uv_strerror(nread) << std::endl;
        }
        uv_close(reinterpret_cast<uv_handle_t *>(stream), nullptr);
    }

    // Free the buffer memory
    delete[] buf->base;
}


/**
 * @brief Callback function for handling read events on the pty.
 * 
 * This function is called when there is data available to be read from the pty.
 * It reads the data from the pty and writes it to the standard output.
 * 
 * @param handle The uv_poll_t handle associated with the pty.
 * @param status The status of the read operation.
 * @param events The events that triggered the callback.
 */
void on_pty_read(uv_poll_t *handle, int status, int events) {
    if (events & UV_READABLE) {
        char buffer[1024];
        int master_fd = reinterpret_cast<intptr_t>(handle->data);
        ssize_t bytes_read = read(master_fd, buffer, sizeof(buffer));
        if (bytes_read > 0) {
            write(STDOUT_FILENO, buffer, bytes_read);
        }
        // Add error handling
    }
}

std::string commandBuffer; // Global or static command buffer
/**
 * @brief Callback function for handling stdin read events.
 * 
 * This function is called when there is data available to be read from stdin.
 * It reads the data from stdin, processes it, and writes it to the PTY master.
 * It also handles special characters such as arrow keys, backspace, and newline.
 * If the read operation encounters an error or reaches the end of file (EOF),
 * it stops the poll handle, closes the handle, and stops the event loop.
 * 
 * @param handle The uv_poll_t handle for stdin.
 * @param status The status of the read operation.
 * @param events The events that triggered the callback.
 */
void on_stdin_read(uv_poll_t *handle, int status, int events) {
    if (events & UV_READABLE) {
        char buffer[1024];
        ssize_t bytes_read = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (bytes_read > 0) {
            for (int i = 0; i < bytes_read; ++i) {
                char ch = buffer[i];

                // Check for arrow keys (and other escape sequences)
                if (ch == '\x1B' && i + 2 < bytes_read) {
                    if (buffer[i + 1] == '[' &&
                        (buffer[i + 2] == 'A' || buffer[i + 2] == 'B' ||
                         buffer[i + 2] == 'C' || buffer[i + 2] == 'D')) {
                        // It's an arrow key, so skip the next two characters
                        // Write to PTY master (including backspace for terminal
                        // echo)
                        int master_fd =
                            reinterpret_cast<intptr_t>(handle->data);
                        // write from buffer i, i+1 and i+2 to master_fd
                        write(master_fd, &buffer[i], 1);
                        write(master_fd, &buffer[i + 1], 1);
                        write(master_fd, &buffer[i + 2], 1);
                        i += 2;

                        continue;
                    }
                }

                if (ch == '\n') {
                    // Log the command when Enter is pressed
                    // dont write empty commands
                    if (!commandBuffer.empty()) {
                        logToAtchim(commandBuffer);
                        commandBuffer.clear();
                    }
                } else if (ch == 0x08 || ch == 0x7F) {
                    // Handle backspace
                    if (!commandBuffer.empty()) {
                        commandBuffer.pop_back();
                    }
                    // dont log tab
                } else if (ch == '\t') {
                    // do nothing
                } else {
                    // Append regular characters to the command buffer
                    commandBuffer.push_back(ch);
                }

                // Write to PTY master (including backspace for terminal echo)
                int master_fd = reinterpret_cast<intptr_t>(handle->data);
                write(master_fd, &ch, 1);
            }
        } else if (bytes_read == 0 || (bytes_read < 0 && errno != EAGAIN)) {
            // EOF or read error, indicating SSH session has ended
            uv_poll_stop(handle);  // Stop the poll handle
            uv_close((uv_handle_t *)handle, NULL);  // Close the handle
            uv_stop(uv_default_loop());  // Stop the event loop
            return;
        }
    }
}
/**
 * @brief Callback function for uv_walk.
 *
 * This function is called for each active handle or request in the event loop.
 * It closes the given handle.
 *
 * @param handle The handle to be closed.
 * @param arg    User-defined argument (unused in this function).
 */
void on_uv_walk(uv_handle_t *handle, void *arg) { uv_close(handle, nullptr); }

int main(int argc, char *argv[]) {
    pid_t pid;
    int slave_fd;

    if (argc > 1) {
        ssh_host = argv[1]; // Assign the first argument to ssh_host
    } else {
        std::cerr << "SSH host not specified" << std::endl;
        return 1;
    }

    setup_logging_socket(); // Set up the logging socket

    master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd < 0 || grantpt(master_fd) < 0 || unlockpt(master_fd) < 0) {
        perror("PTY allocation");
        return 1;
    }

    handle_winch(SIGWINCH, master_fd); // Set up the window size change handler

    char *slave_pty_name = ptsname(master_fd);
    if (slave_pty_name == nullptr) {
        perror("ptsname");
        return 1;
    }

    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != -1) {
        if (ioctl(master_fd, TIOCSWINSZ, &ws) == -1) {
            perror("ioctl TIOCSWINSZ error");
        }
    } else {
        perror("ioctl TIOCGWINSZ error");
    }

    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) { // Child process
        setsid();

        // Open the slave PTY
        slave_fd = open(slave_pty_name, O_RDWR);
        if (slave_fd < 0) {
            perror("Opening slave PTY");
            return 1;
        }

        if (ioctl(slave_fd, TIOCSCTTY, NULL) < 0) {
            perror("ioctl TIOCSCTTY");
            return 1;
        }
        // Set terminal attributes for line editing and history
        struct termios tattr;
        tcgetattr(STDIN_FILENO, &tattr);
        struct termios raw = tattr;
        raw.c_lflag &= ~(ECHO | ICANON);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

        // Reset SIGINT to default behavior for the SSH process
        signal(SIGINT, SIG_DFL);
        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);

        execvp("ssh", argv);
        perror("execvp");
        exit(EXIT_FAILURE);
    } else { // Parent process
        uv_loop_t *loop = uv_default_loop();

        // Poll handle for PTY master
        uv_poll_t pty_poll_handle;
        uv_poll_init(loop, &pty_poll_handle, master_fd);
        pty_poll_handle.data = (void *)(intptr_t)master_fd;
        uv_poll_start(&pty_poll_handle, UV_READABLE, on_pty_read);

        // Poll handle for STDIN
        uv_poll_t stdin_poll_handle;
        uv_poll_init(loop, &stdin_poll_handle, STDIN_FILENO);
        stdin_poll_handle.data = (void *)(intptr_t)master_fd;
        uv_poll_start(&stdin_poll_handle, UV_READABLE, on_stdin_read);

        // Start the event loop
        uv_run(loop, UV_RUN_DEFAULT);
        // Clean up after loop exit
        uv_loop_close(loop);
        close(master_fd);
    }
    return 0;
}