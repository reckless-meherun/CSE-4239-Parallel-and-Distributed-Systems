/*
 * client.cpp
 * ----------
 * Interactive client for the knock-knock server.
 *
 * Usage:
 *   ./client                 -> connects to 127.0.0.1:8079
 *   ./client <ip>            -> connects to <ip>:8079
 *   ./client <ip> <port>     -> connects to <ip>:<port>
 *
 * Protocol (text lines):
 *   - When a server line contains "<input>", the client should send one line
 *     the user types (without quotes, newline auto-appended).
 *   - Other lines are informational and are just printed.
 *
 * Build:
 *   g++ -std=c++17 -Wall -Wextra -O2 client.cpp -o client
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>

namespace {

// Default port (must match your server)
constexpr int kDefaultPort = 8079;

// Receive exactly one line (ending with '\n'); strips '\r'. Returns false on EOF/error.
bool recv_line(int fd, std::string& line) {
    line.clear();
    char ch = 0;
    while (true) {
        ssize_t n = ::recv(fd, &ch, 1, 0);
        if (n <= 0) {
            // n == 0 -> orderly shutdown by peer; n < 0 -> error/timeout
            return false;
        }
        if (ch == '\r') continue;   // normalize CRLF to LF
        if (ch == '\n') break;
        line.push_back(ch);
        if (line.size() > 8192) {   // safety cap to avoid unbounded growth
            break;
        }
    }
    return true;
}

// Send a whole line; appends '\n' if missing. Returns false on socket error.
bool send_line(int fd, const std::string& s) {
    std::string out = s;
    if (out.empty() || out.back() != '\n') out.push_back('\n');

    const char* p = out.data();
    size_t left = out.size();
    while (left > 0) {
        ssize_t n = ::send(fd, p, left, 0);
        if (n < 0) {
            if (errno == EINTR) continue; // interrupted: retry
            return false;                  // real error
        }
        if (n == 0) return false;         // shouldn't happen for TCP, treat as error
        p    += n;
        left -= static_cast<size_t>(n);
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    // ---- Parse command-line arguments (ip and optional port) ----
    std::string host = "127.0.0.1";
    int port = kDefaultPort;

    if (argc >= 2) {
        host = argv[1];
    }
    if (argc >= 3) {
        try {
            port = std::stoi(argv[2]);
            if (port <= 0 || port > 65535) {
                std::cerr << "Port must be in 1..65535\n";
                return 1;
            }
        } catch (...) {
            std::cerr << "Invalid port: " << argv[2] << "\n";
            return 1;
        }
    }

    // ---- Create socket ----
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::perror("socket");
        return 1;
    }

    // ---- Build server address ----
    sockaddr_in serv{};
    serv.sin_family = AF_INET;
    serv.sin_port   = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &serv.sin_addr) <= 0) {
        std::cerr << "Invalid IPv4 address: " << host << "\n";
        ::close(sock);
        return 1;
    }

    // ---- Connect ----
    if (::connect(sock, reinterpret_cast<sockaddr*>(&serv), sizeof(serv)) < 0) {
        std::perror("connect");
        ::close(sock);
        return 1;
    }

    std::cout << "Connected to " << host << ":" << port
              << ". Type your responses when prompted.\n";

    // ---- Conversation loop ----
    std::string line;
    while (true) {
        // Read a line from the server
        if (!recv_line(sock, line)) {
            std::cout << "Connection closed by server.\n";
            break;
        }

        // If the server expects input, it includes "<input>" (lowercase) in the line
        std::size_t pos = line.find("<input>");
        if (pos != std::string::npos) {
            // Show the message without the marker
            std::string display = line;
            display.erase(pos, 7); // remove "<input>"
            std::cout << "Server: " << display << std::endl;

            // Read one line from the user and send it back
            std::string reply;
            std::cout << "Client: ";
            if (!std::getline(std::cin, reply)) {
                // stdin closed; end session gracefully
                break;
            }
            if (!send_line(sock, reply)) {
                std::cerr << "Send failed.\n";
                break;
            }
        } else {
            // Informational line (punchline, corrections, etc.)
            std::cout << "Server: " << line << std::endl;

            // Optional: if server tells you it has no more jokes, you can exit
            if (line.find("I have no more jokes to tell") != std::string::npos) {
                break;
            }
        }
    }

    ::close(sock);
    return 0;
}
