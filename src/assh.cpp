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

int master_fd_global;
int log_socket = -1; // Global log socket
std::string ssh_host;
#define BUFFER_SIZE 4096

/**
 * @brief Handles the window size change signal.
 * 
 * This function is called when the window size changes and updates the window size
 * of the terminal. It uses the TIOCGWINSZ and TIOCSWINSZ ioctl calls to get and set
 * the window size, respectively.
 * 
 * @param signum The signal number.
 */
void handle_winch(int signum) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        perror("ioctl TIOCGWINSZ");
        return;
    }
    if (ioctl(master_fd_global, TIOCSWINSZ, &ws) == -1) {
        perror("ioctl TIOCSWINSZ");
    }
}

/**
 * @brief Sets up a logging socket connection.
 * 
 * This function creates a socket, connects it to a server, and sets up a logging socket connection.
 * The socket is created using the AF_UNIX domain and SOCK_STREAM type.
 * The connection is established by connecting to the server using the specified address.
 * If the connection fails, an error message is printed and the socket is closed.
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

int main(int argc, char *argv[]) {
    int master_fd, slave_fd;

    if (argc > 1) {
        ssh_host = argv[1]; // Assign the first argument to ssh_host
    } else {
        std::cerr << "SSH host not specified" << std::endl;
        return 1;
    }

    // Set up signal handler for window size changes
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_winch;
    sigaction(SIGWINCH, &sa, NULL);

    // Immediately handle current window size
    handle_winch(SIGWINCH);
    setup_logging_socket(); // Set up the logging socket

    // Allocate a PTY
    master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd < 0 || grantpt(master_fd) < 0 || unlockpt(master_fd) < 0) {
        perror("PTY allocation");
        return 1;
    }

    // Get the slave PTY name
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

    pid_t pid = fork();
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

        // Redirect IO
        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);

        // Execute SSH
        execvp("ssh", argv);
        perror("execvp");
        exit(EXIT_FAILURE);
    } else { // Parent process
        // Forward input and output between the user and the SSH process
        // Set the master PTY to non-blocking
        int flags = fcntl(master_fd, F_GETFL, 0);
        fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);
        struct pollfd fds[2];

        std::string commandBuffer;

        char buffer[BUFFER_SIZE];
        while (true) {
            fds[0].fd = master_fd;
            fds[0].events = POLLIN;
            fds[1].fd = STDIN_FILENO;
            fds[1].events = POLLIN;

            int ret = poll(fds, 2, -1);
            if (ret < 0) {
                perror("poll");
                break;
            }

            if (fds[0].revents & POLLIN) {
                ssize_t len = read(master_fd, buffer, BUFFER_SIZE);
                if (len <= 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        break;
                    }
                } else {
                    write(STDOUT_FILENO, buffer, len);
                }
            }

            if (fds[1].revents & POLLIN) {
                ssize_t len = read(STDIN_FILENO, buffer, BUFFER_SIZE);
                if (len > 0) {
                    // Append to command buffer
                    commandBuffer.append(buffer, len);

                    // Check for newline or command delimiter
                    size_t newlinePos = commandBuffer.find('\n');
                    if (newlinePos != std::string::npos) {
                        // Extract the command
                        std::string command =
                            commandBuffer.substr(0, newlinePos);
                        logToAtchim(command);

                        // Clear the extracted command from the buffer
                        commandBuffer.erase(0, newlinePos + 1);
                    }

                    // Write to PTY
                    write(master_fd, buffer, len);
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    break;
                }
            }
        }

        // Clean up
        close(master_fd);
        waitpid(pid, NULL, 0);
    }

    return 0;
}
