/**
 * @file: atchim.cpp
 * @author: Laio Oriel Seman
 * @contact: laio [at] gos.ufsc.br
 *
 * Description: Client component of the Atchim command logging tool. This client sends
 * commands executed in the user's shell to the Atchim server for logging.
 */


#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string>
#include <iostream>

/**
 * @brief The main function of the program.
 * 
 * @param argc The number of command-line arguments.
 * @param argv An array of command-line arguments.
 * @return int The exit status of the program.
 */
int main(int argc, char *argv[]) {
    if (argc != 3 || std::string(argv[1]) != "start") {
        std::cerr << "Invalid arguments" << std::endl;
        return 1;
    }

    std::string command = argv[2];

    int sock;
    struct sockaddr_un address;

    // Create socket
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) {
        std::cerr << "Socket creation failed" << std::endl;
        return 1;
    }

    // Connect to the server
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, "/tmp/atchim_socket");
    if (connect(sock, (struct sockaddr *)&address, sizeof(address)) == -1) {
        std::cerr << "Connection to server failed" << std::endl;
        close(sock);
        return 1;
    }

    // Send data (command)
    if (write(sock, command.c_str(), command.length()) == -1) {
        std::cerr << "Failed to send command" << std::endl;
        close(sock);
        return 1;
    }

    // Close the socket
    close(sock);
    return 0;
}
