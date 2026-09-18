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
#include <fstream>

#pragma comment(lib, "ws2_32.lib")

struct Entry {
    std::string value;
    std::optional<std::chrono::steady_clock::time_point> expiry; // no value => never expires
};

struct Node {
    std::string key;
    Entry entry;
    Node* prev;
    Node* next;
};

std::unordered_map<std::string, Node*> store;
std::mutex storeMutex;   // protects every access to 'store' above

Node* dummyHead = new Node();
Node* dummyTail = new Node();

const size_t MAX_KEYS = 3; // can be set according to the need

void removeNode(Node* node){
    Node* before = node->prev;
    Node* after = node->next;

    // remove B from A <---> B <---> C means A <---> C
    before->next = after;
    after->prev = before;
}

void addToFront(Node* node){
    node->next = dummyHead->next;
    node->prev = dummyHead;
    dummyHead->next->prev = node;
    dummyHead->next = node;
}

void moveToFront(Node* node){
    removeNode(node);
    addToFront(node);
}

void saveSnapshot() {
    std::lock_guard<std::mutex> lock(storeMutex);

    std::ofstream outFile("snapshot.txt", std::ios::trunc);
    if (!outFile.is_open()) {
        std::cerr << "Failed to open snapshot.txt for writing\n";
        return;
    }

    for (const auto& pair : store) {
        Node* node = pair.second;
        long long remainingSeconds = -1;  // -1 means "no expiry"

        if (node->entry.expiry.has_value()) {
            auto now = std::chrono::steady_clock::now();
            auto diff = std::chrono::duration_cast<std::chrono::seconds>(node->entry.expiry.value() - now).count();
            if (diff < 0) {
                continue;  // already expired, skip persisting this key entirely
            }
            remainingSeconds = diff;
        }

        outFile << node->key << "\t" << node->entry.value << "\t" << remainingSeconds << "\n";
    }

    outFile.close();
    std::cout << "Snapshot saved (" << store.size() << " keys).\n";
}

void loadSnapshot() {
    std::ifstream inFile("snapshot.txt");
    if (!inFile.is_open()) {
        std::cout << "No snapshot file found, starting with empty store.\n";
        return;
    }

    std::string line;
    int loadedCount = 0;

    while (std::getline(inFile, line)) {
        std::istringstream iss(line);
        std::string key, value, secondsStr;

        if (!std::getline(iss, key, '\t')) continue;
        if (!std::getline(iss, value, '\t')) continue;
        if (!std::getline(iss, secondsStr, '\t')) continue;

        long long remainingSeconds = std::stoll(secondsStr);

        Node* newNode = new Node();
        newNode->key = key;
        newNode->entry.value = value;

        if (remainingSeconds >= 0) {
            newNode->entry.expiry = std::chrono::steady_clock::now() + std::chrono::seconds(remainingSeconds);
        } else {
            newNode->entry.expiry = std::nullopt;
        }

        store[key] = newNode;
        addToFront(newNode);
        loadedCount++;
    }

    inFile.close();
    std::cout << "Snapshot loaded (" << loadedCount << " keys).\n";
}



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

    Node* node = it->second;
    if(node->entry.expiry.has_value() && std::chrono::steady_clock::now() >= node->entry.expiry.value()) {
        removeNode(node);
        store.erase(it);
        delete node;
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

        auto it = store.find(tokens[1]);
        if (it != store.end()) {
            it->second->entry.value = tokens[2];
            it->second->entry.expiry = std::nullopt;
            moveToFront(it->second);
        }
        else{
            Node* newNode = new Node();
            newNode->key = tokens[1];
            newNode->entry = {tokens[2], std::nullopt};
            store[tokens[1]] = newNode;
            addToFront(newNode); 

            if (store.size() > MAX_KEYS) {
                Node* lru = dummyTail->prev;
                removeNode(lru);
                store.erase(lru->key);
                delete lru;
            }
        }
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
        Node* node = it->second;
        moveToFront(node);
        return node->entry.value + "\r\n";
    }
    else if (cmd == "DEL") {
        if (tokens.size() != 2) {
            return "ERR usage: DEL key\r\n";
        }
        std::lock_guard<std::mutex> lock(storeMutex);

        auto it = store.find(tokens[1]);
        if (it == store.end()) {
            return "0\r\n";
        }

        Node* node = it->second;
        removeNode(node);
        store.erase(it);
        delete node;

        return "1\r\n";
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
            Node* node = it->second;
            node->entry.expiry = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
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
        Node* node = it->second;

        if(!node->entry.expiry.has_value()) {
            return "-1\r\n"; // key exists but has no expiry
        }

        auto now = std::chrono::steady_clock::now();
        auto expiryTime = node->entry.expiry.value();
        auto ttl = std::chrono::duration_cast<std::chrono::seconds>(expiryTime - now).count();
        
        if(ttl < 0) {
            removeNode(node);
            store.erase(it);
            delete node;
            return "-2\r\n"; // key has expired
        }

        return std::to_string(ttl) + "\r\n";

    }
    else if (cmd == "SAVE") {
        saveSnapshot();
        return "OK\r\n";
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
    dummyHead->next = dummyTail;
    dummyTail->prev = dummyHead;

    loadSnapshot();

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
