              << "] Client connected.\n";

    char buffer[RECV_BUFFER_SIZE];
    std::string pending;
    bool keepRunning = true;

    while (keepRunning) {
        const int received = recv(clientSocket, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }

        pending.append(buffer, static_cast<std::size_t>(received));

        if (pending.size() > MAX_LINE_SIZE &&
            pending.find('\n') == std::string::npos) {
            sendAll(clientSocket, "ERR command too large\r\n");
            break;
        }

        std::size_t newline;
        while ((newline = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (line.size() > MAX_LINE_SIZE) {
                if (!sendAll(clientSocket, "ERR command too large\r\n")) {
                    keepRunning = false;
                }
                keepRunning = false;
                break;
            }

            const auto tokens = tokenize(line);
            const std::string response = handleCommand(tokens);

            if (!sendAll(clientSocket, response)) {
                keepRunning = false;
                break;
            }

            if (!tokens.empty() && tokens[0] == "QUIT") {
                keepRunning = false;
                break;
            }
        }
    }

    shutdown(clientSocket, SD_BOTH);
    closesocket(clientSocket);
    std::cout << "[Thread " << std::this_thread::get_id()
              << "] Client disconnected.\n";
}

} // namespace

int main() {
    loadSnapshot();

    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        std::cerr << "socket() failed: " << WSAGetLastError() << '\n';
        WSACleanup();
        return 1;
    }

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    serverAddress.sin_port = htons(PORT);

    if (bind(listenSocket,
             reinterpret_cast<sockaddr*>(&serverAddress),
             sizeof(serverAddress)) == SOCKET_ERROR) {
        std::cerr << "bind() failed: " << WSAGetLastError() << '\n';
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "listen() failed: " << WSAGetLastError() << '\n';
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    std::cout << "Mini Redis server listening on port " << PORT << '\n';

    while (true) {
        sockaddr_in clientAddress{};
        int addressLength = sizeof(clientAddress);

        SOCKET clientSocket = accept(
            listenSocket,
            reinterpret_cast<sockaddr*>(&clientAddress),
            &addressLength
        );

        if (clientSocket == INVALID_SOCKET) {
            std::cerr << "accept() failed: " << WSAGetLastError() << '\n';
            continue;
        }

        try {
            std::thread(handleClient, clientSocket).detach();
        } catch (const std::system_error& error) {
            std::cerr << "Could not start client thread: "
                      << error.what() << '\n';
            closesocket(clientSocket);
        }
    }

    // The current server runs until the process is terminated.
    closesocket(listenSocket);
    WSACleanup();
    return 0;
}