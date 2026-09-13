#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <string>

#pragma comment(lib, "ws2_32.lib")

int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        std::cerr << "socket() failed: " << WSAGetLastError() << "\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(6379);
    inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);

    if (connect(sock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "connect() failed: " << WSAGetLastError()
                   << " (is the server running?)\n";
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    std::cout << "Connected to 127.0.0.1:6379\n";
    std::cout << "Type commands like: SET foo bar / GET foo / DEL foo / QUIT\n";

    std::string line;
    char buffer[4096];

    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) {
            break; // Ctrl+Z / EOF
        }
        if (line.empty()) {
            continue;
        }

        std::string toSend = line + "\r\n";
        send(sock, toSend.c_str(), (int)toSend.size(), 0);

        int bytesReceived = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (bytesReceived <= 0) {
            std::cout << "Server closed the connection.\n";
            break;
        }
        buffer[bytesReceived] = '\0';
        std::cout << buffer;

        // stop CTRLoop if we sent QUIT ourselves
        std::string upperLine = line;
        for (auto& c : upperLine) c = toupper(c);
        if (upperLine == "QUIT") {
            break;
        }
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}
