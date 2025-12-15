#include "AnTcpServer.hpp"

AnTcpError AnTcpServer::Run() noexcept {
  WSADATA wsaData{};
  int result = WSAStartup(MAKEWORD(2, 2), &wsaData);

  if (result != 0) {
    DEBUG_ONLY(std::cout << ">> WSAStartup() failed: " << result << std::endl);
    return AnTcpError::Win32WsaStartupFailed;
  }

  addrinfo hints{0};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;

  addrinfo *addrResult{nullptr};
  result = getaddrinfo(Ip.c_str(), Port.c_str(), &hints, &addrResult);

  if (result != 0) {
    DEBUG_ONLY(std::cout << ">> getaddrinfo() failed: " << result << std::endl);
    WSACleanup();
    return AnTcpError::GetAddrInfoFailed;
  }

  ListenSocket = socket(addrResult->ai_family, addrResult->ai_socktype,
                        addrResult->ai_protocol);

  if (ListenSocket == INVALID_SOCKET) {
    DEBUG_ONLY(std::cout << ">> socket() failed: " << WSAGetLastError()
                         << std::endl);
    freeaddrinfo(addrResult);
    WSACleanup();
    return AnTcpError::SocketCreationFailed;
  }

  result = bind(ListenSocket, addrResult->ai_addr,
                static_cast<int>(addrResult->ai_addrlen));
  freeaddrinfo(addrResult);

  if (result == SOCKET_ERROR) {
    DEBUG_ONLY(std::cout << ">> bind() failed: " << WSAGetLastError()
                         << std::endl);
    SocketCleanup();
    WSACleanup();
    return AnTcpError::SocketBindingFailed;
  }

  if (listen(ListenSocket, SOMAXCONN) == SOCKET_ERROR) {
    DEBUG_ONLY(std::cout << ">> listen() failed: " << WSAGetLastError()
                         << std::endl);
    SocketCleanup();
    WSACleanup();
    return AnTcpError::SocketListeningFailed;
  }

  while (!ShouldExit) {
    SOCKADDR_IN clientInfo{0};
    int sockAddrSize = static_cast<int>(sizeof(SOCKADDR_IN));
    SOCKET clientSocket = accept(
        ListenSocket, reinterpret_cast<sockaddr *>(&clientInfo), &sockAddrSize);

    if (clientSocket == INVALID_SOCKET) {
      DEBUG_ONLY(std::cout << ">> accept() failed: " << WSAGetLastError()
                           << std::endl);
      continue;
    }

    ClientCleanup();

    Clients.push_back(std::make_unique<ClientHandler>(
        clientSocket, clientInfo, ShouldExit, &Callbacks, &OnClientConnected,
        &OnClientDisconnected));
  }

  Clients.clear();

  SocketCleanup();
  WSACleanup();
  return AnTcpError::Success;
}

void ClientHandler::Listen() noexcept {
  if (OnClientConnected) {
    (*OnClientConnected)(this);
  }

  AnTcpSizeType packetSize = 0;
  AnTcpSizeType packetOffset = 0;

  char packet[sizeof(AnTcpSizeType) + ANTCP_MAX_PACKET_SIZE]{0};

  while (!ShouldExit) {
    auto packetBytesMissing = packetSize - packetOffset;
    auto maxReceiveSize =
        packetBytesMissing > 0 ? packetBytesMissing : sizeof(AnTcpSizeType);

    auto receivedPacketBytes =
        recv(Socket, packet + packetOffset, maxReceiveSize, 0);
    packetOffset += receivedPacketBytes;

    if (receivedPacketBytes <= 0) {
      break;
    }

    DEBUG_ONLY(std::cout << "[" << Id << "] " << "Received "
                         << std::to_string(receivedPacketBytes) + " bytes"
                         << std::endl);

    if (packetSize == 0) {
      if (packetOffset >= sizeof(AnTcpSizeType)) {
        packetSize = *reinterpret_cast<AnTcpSizeType *>(packet);

        if (packetSize > ANTCP_MAX_PACKET_SIZE) {
          DEBUG_ONLY(std::cout << "[" << Id << "] " << "Packet too big ("
                               << std::to_string(packetSize) << "/"
                               << ANTCP_MAX_PACKET_SIZE
                               << "), disconnecting client..." << std::endl);
          break;
        }

        DEBUG_ONLY(std::cout << "[" << Id << "] " << "New Packet: "
                             << std::to_string(packetSize) + " bytes"
                             << std::endl);
      }
    } else {
      if (packetBytesMissing == 0) {
        if (!ProcessPacket(packet + sizeof(AnTcpSizeType), packetSize)) {
          break;
        }

        packetSize = 0;
        packetOffset = 0;
      } else {
        DEBUG_ONLY(std::cout
                   << "[" << Id << "] "
                   << "Packet Chunk: " << std::to_string(receivedPacketBytes)
                   << " bytes (" << std::to_string(packetOffset) << "/"
                   << std::to_string(packetSize) << ")" << std::endl);
      }
    }
  }

  Disconnect();
}