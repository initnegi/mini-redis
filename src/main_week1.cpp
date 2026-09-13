#include <winsock2.h>
#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <unordered_map>

#pragma comment(lib, "ws2_32.lib")

// ---- Global in-memory store (Week 1: no thread-safety needed yet, single client) ----
std::unordered_map<std::string, std::string> store;

// ---- Split a line like "SET foo bar" into tokens ["SET", "foo", "bar"] ----
std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

// ---- Process one command, return the response string to send back ----
std::string handleCommand(const std::vector<std::string>& tokens) {
    if (tokens.empty()) {
        return "ERR empty command\r\n";
    }

    const std::string& cmd = tokens[0];

    if (cmd == "SET") {
        if (tokens.size() != 3) {
            return "ERR usage: SET key value\r\n";
        }
        store[tokens[1]] = tokens[2];
        return "OK\r\n";
    }
    else if (cmd == "GET") {
        if (tokens.size() != 2) {
            return "ERR usage: GET key\r\n";
        }
        auto it = store.find(tokens[1]);
        if (it == store.end()) {
            return "(nil)\r\n";
        }
        return it->second + "\r\n";
    }
    else if (cmd == "DEL") {
        if (tokens.size() != 2) {
            return "ERR usage: DEL key\r\n";
        }
        size_t erased = store.erase(tokens[1]);
        return (erased > 0 ? "1\r\n" : "0\r\n");
    }
    else if (cmd == "EXISTS") {
        if (tokens.size() != 2) {
            return "ERR usage: EXISTS key\r\n";
        }
        return (store.find(tokens[1]) != store.end() ? "1\r\n" : "0\r\n");
    }
    else if (cmd == "QUIT") {
        return "BYE\r\n";
    }
    else {
        return "ERR unknown command '" + cmd + "'\r\n";
    }
}

int main() {
    // 1. Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    // 2. Create a listening socket
    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        std::cerr << "socket() failed: " << WSAGetLastError() << "\n";
        WSACleanup();
        return 1;
    }

    // 3. Bind to a port
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;   // listen on all local interfaces
    serverAddr.sin_port = htons(6379);         // same default port as real Redis

    if (bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "bind() failed: " << WSAGetLastError() << "\n";
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    // 4. Listen
    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "listen() failed: " << WSAGetLastError() << "\n";
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    std::cout << "Mini Redis server listening on port 6379...\n";

    // 5. Accept ONE client (Week 1: no loop-of-clients, no threading yet)
    sockaddr_in clientAddr{};
    int clientAddrSize = sizeof(clientAddr);
    SOCKET clientSocket = accept(listenSocket, (sockaddr*)&clientAddr, &clientAddrSize);
    if (clientSocket == INVALID_SOCKET) {
        std::cerr << "accept() failed: " << WSAGetLastError() << "\n";
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    std::cout << "Client connected.\n";

    // 6. Read commands from this client in a loop until they disconnect or send QUIT
    char buffer[4096];
    while (true) {
        int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
        if (bytesReceived <= 0) {
            std::cout << "Client disconnected.\n";
            break;
        }
        buffer[bytesReceived] = '\0';

        std::string line(buffer);
        // strip trailing \r\n if present
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }

        auto tokens = tokenize(line);
        std::string response = handleCommand(tokens);

        send(clientSocket, response.c_str(), (int)response.size(), 0);

        if (!tokens.empty() && tokens[0] == "QUIT") {
            break;
        }
    }

    // 7. Cleanup
    closesocket(clientSocket);
    closesocket(listenSocket);
    WSACleanup();
    return 0;
}
