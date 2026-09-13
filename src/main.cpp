#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <chrono>
#include <optional>

#pragma comment(lib, "ws2_32.lib")

struct Entry {
    std::string value;
    std::optional<std::chrono::steady_clock::time_point> expiry; // no value = never expires
};

std::unordered_map<std::string, Entry> store;
std::mutex storeMutex;   // protects every access to 'store' above

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

bool isKeyValid(const std::string& key) {
    auto it = store.find(key);
    if(it == store.end()) {
        return false;
    }

    if(it->second.expiry.has_value() && std::chrono::steady_clock::now() >= it->second.expiry.value()) {
        store.erase(it);
        return false;
    }
    return true;
}

// ---- Process one command, return the response string to send back ----
// NOTE: every place this touches 'store' must lock storeMutex first.
std::string handleCommand(const std::vector<std::string>& tokens) {
    if (tokens.empty()) {
        return "ERR empty command\r\n";
    }

    const std::string& cmd = tokens[0];

    if (cmd == "SET") {
        if (tokens.size() != 3) {
            return "ERR usage: SET key value\r\n";
        }
        std::lock_guard<std::mutex> lock(storeMutex);
        store[tokens[1]] = {tokens[2], std::nullopt}; 
        return "OK\r\n";
    }
    else if (cmd == "GET") {
        if (tokens.size() != 2) {
            return "ERR usage: GET key\r\n";
        }
        std::lock_guard<std::mutex> lock(storeMutex);
        if(!isKeyValid(tokens[1])) {
            return "(nil)\r\n";
        }

        auto it = store.find(tokens[1]);
        return it->second.value + "\r\n";
    }
    else if (cmd == "DEL") {
        if (tokens.size() != 2) {
            return "ERR usage: DEL key\r\n";
        }
        std::lock_guard<std::mutex> lock(storeMutex);
        size_t erased = store.erase(tokens[1]);
        return (erased > 0 ? "1\r\n" : "0\r\n");
    }
    else if (cmd == "EXISTS") {
        if (tokens.size() != 2) {
            return "ERR usage: EXISTS key\r\n";
        }
        std::lock_guard<std::mutex> lock(storeMutex);
        if(!isKeyValid(tokens[1])) {
            return "0\r\n";
        }
        return "1\r\n";
    }
    else if (cmd == "QUIT") {
        return "BYE\r\n";
    }
    else if (cmd == "EXPIRE") {
        if (tokens.size() != 3) {
            return "ERR usage: EXPIRE key seconds\r\n";
        }
        std::lock_guard<std::mutex> lock(storeMutex);
        if(!isKeyValid(tokens[1])) {
            return "0\r\n";
        }

        try {
            int seconds = std::stoi(tokens[2]);
            auto it = store.find(tokens[1]);
            it->second.expiry = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
            return "1\r\n";
        } catch (const std::invalid_argument&) {
            return "ERR invalid seconds value\r\n";
        } catch (const std::out_of_range&) {
            return "ERR seconds value out of range\r\n";
        }
    }
    else if (cmd == "TTL") {
        if (tokens.size() != 2) {
            return "ERR usage: TTL key\r\n";
        }
        std::lock_guard<std::mutex> lock(storeMutex);
        if(!isKeyValid(tokens[1])) {
            return "-2\r\n"; // key does not exist
        }

        auto it = store.find(tokens[1]);
        if(!it->second.expiry.has_value()) {
            return "-1\r\n"; // key exists but has no expiry
        }

        auto now = std::chrono::steady_clock::now();
        auto expiryTime = it->second.expiry.value();
        auto ttl = std::chrono::duration_cast<std::chrono::seconds>(expiryTime - now).count();
        
        if(ttl < 0) {
            store.erase(it);
            return "-2\r\n"; // key has expired
        }

        return std::to_string(ttl) + "\r\n";

    }
    else {
        return "ERR unknown command '" + cmd + "'\r\n";
    }
}

// ---- Handles ONE client's entire session, runs on its own thread ----
void handleClient(SOCKET clientSocket) {
    std::cout << "[Thread " << std::this_thread::get_id() << "] Client connected.\n";

    char buffer[4096];
    while (true) {
        int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
        if (bytesReceived <= 0) {
            break;
        }
        buffer[bytesReceived] = '\0';

        std::string line(buffer);
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

    std::cout << "[Thread " << std::this_thread::get_id() << "] Client disconnected.\n";
    closesocket(clientSocket);
}

int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        std::cerr << "socket() failed: " << WSAGetLastError() << "\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(6379);

    if (bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "bind() failed: " << WSAGetLastError() << "\n";
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "listen() failed: " << WSAGetLastError() << "\n";
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    std::cout << "Mini Redis server listening on port 6379...\n";
    std::cout << "(Now accepting MULTIPLE clients concurrently. Ctrl+C to stop.)\n";

    // ---- Main accept loop: runs forever, spawns one thread per client ----
    while (true) {
        sockaddr_in clientAddr{};
        int clientAddrSize = sizeof(clientAddr);
        SOCKET clientSocket = accept(listenSocket, (sockaddr*)&clientAddr, &clientAddrSize);

        if (clientSocket == INVALID_SOCKET) {
            std::cerr << "accept() failed: " << WSAGetLastError() << "\n";
            continue; // keep serving other clients even if one accept() call fails
        }

        // Spawn a thread to handle this client, detach so it runs independently
        // and cleans up its own resources when handleClient() returns.
        std::thread(handleClient, clientSocket).detach();
    }

    // unreachable in this simple version (Ctrl+C kills the process directly)
    closesocket(listenSocket);
    WSACleanup();
    return 0;
}
