#pragma once

#if 0
#define DEBUG_ONLY(x) x
#define BENCHMARK(x) x
#else
#define DEBUG_ONLY(x)
#define BENCHMARK(x)
#endif

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>


constexpr auto ANTCP_SERVER_VERSION = "1.2.1.0";
constexpr auto ANTCP_MAX_PACKET_SIZE = 256;

typedef int AnTcpSizeType;

typedef char AnTcpMessageType;

enum class AnTcpError {
  Success,
  Win32WsaStartupFailed,
  GetAddrInfoFailed,
  SocketCreationFailed,
  SocketBindingFailed,
  SocketListeningFailed
};

class ClientHandler {
private:
  size_t Id;
  SOCKET Socket;
  SOCKADDR_IN SocketInfo;
  std::atomic<bool> &ShouldExit;
  std::unordered_map<AnTcpMessageType,
                     std::function<void(ClientHandler *, AnTcpMessageType,
                                        const void *, int)>> *Callbacks;

  bool IsActive;
  std::jthread Thread;

  std::function<void(ClientHandler *)> *OnClientConnected;
  std::function<void(ClientHandler *)> *OnClientDisconnected;

public:
  ClientHandler(
      SOCKET socket, const SOCKADDR_IN &socketInfo,
      std::atomic<bool> &shouldExit,
      std::unordered_map<AnTcpMessageType,
                         std::function<void(ClientHandler *, AnTcpMessageType,
                                            const void *, int)>> *callbacks,
      std::function<void(ClientHandler *)> *onClientConnected = nullptr,
      std::function<void(ClientHandler *)> *onClientDisconnected = nullptr)
      : Id(static_cast<unsigned int>(socketInfo.sin_addr.S_un.S_addr +
                                     socketInfo.sin_port)),
        Socket(socket), SocketInfo(socketInfo), ShouldExit(shouldExit),
        Callbacks(callbacks), IsActive(true),
        Thread(&ClientHandler::Listen, this),
        OnClientConnected(onClientConnected),
        OnClientDisconnected(onClientDisconnected) {}

  ~ClientHandler() {
    DEBUG_ONLY(std::cout << "[" << Id << "] " << "Deleting Handler: " << Id
                         << std::endl);

    Disconnect();
  }

  ClientHandler(const ClientHandler &) = delete;
  ClientHandler &operator=(const ClientHandler &) = delete;

  constexpr auto GetId() const noexcept { return Id; }

  constexpr bool IsConnected() const noexcept { return !IsActive; }

  template <typename T>
  constexpr bool SendDataVar(AnTcpMessageType type,
                             const T data) const noexcept {
    return SendData(type, &data, sizeof(T));
  }

  template <typename T>
  constexpr bool SendDataPtr(AnTcpMessageType type,
                             const T *data) const noexcept {
    return SendData(type, data, sizeof(T));
  }

  inline bool SendData(AnTcpMessageType type, const void *data,
                       size_t size) const noexcept {
    const int packetSize =
        static_cast<int>(size) + static_cast<int>(sizeof(AnTcpMessageType));
    return send(Socket, reinterpret_cast<const char *>(&packetSize),
                sizeof(decltype(packetSize)), 0) != SOCKET_ERROR &&
           send(Socket, &type, sizeof(AnTcpMessageType), 0) != SOCKET_ERROR &&
           send(Socket, static_cast<const char *>(data), static_cast<int>(size),
                0) != SOCKET_ERROR;
  }

  inline void Disconnect() noexcept {
    if (OnClientDisconnected) {
      (*OnClientDisconnected)(this);
    }

    closesocket(Socket);
    Socket = INVALID_SOCKET;
    IsActive = false;
  }

  inline std::string GetIpAddress() const noexcept {
    char ipAddressBuffer[128]{0};
    inet_ntop(AF_INET, &SocketInfo.sin_addr, ipAddressBuffer, 128);
    return std::string(ipAddressBuffer);
  }

  constexpr unsigned short GetPort() const noexcept {
    return SocketInfo.sin_port;
  }

  constexpr unsigned short GetAddressFamily() const noexcept {
    return SocketInfo.sin_family;
  }

private:
  void Listen() noexcept;

  inline bool ProcessPacket(const char *data, AnTcpMessageType size) noexcept {
    auto msgType = *reinterpret_cast<const AnTcpMessageType *>(data);

    if ((*Callbacks).contains(msgType)) {
      BENCHMARK(const auto packetStart =
                    std::chrono::high_resolution_clock::now());

      (*Callbacks)[msgType](this, msgType, data + sizeof(AnTcpMessageType),
                            size - sizeof(AnTcpMessageType));

      BENCHMARK(
          std::cout << "[" << Id << "] " << "Processing packet of type \""
                    << std::to_string(msgType) << "\" took: "
                    << std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::high_resolution_clock::now() -
                           packetStart)
                    << std::endl);

      return true;
    }

    DEBUG_ONLY(std::cout << "[" << Id << "] " << "\"" << std::to_string(msgType)
                         << "\" is an unknown message type..." << std::endl);

    return false;
  }
};

class AnTcpServer {
private:
  std::string Ip;
  std::string Port;
  std::atomic<bool> ShouldExit;
  SOCKET ListenSocket;
  std::vector<std::unique_ptr<ClientHandler>> Clients;
  std::unordered_map<
      AnTcpMessageType,
      std::function<void(ClientHandler *, AnTcpMessageType, const void *, int)>>
      Callbacks;

  std::function<void(ClientHandler *)> OnClientConnected;
  std::function<void(ClientHandler *)> OnClientDisconnected;

public:
  AnTcpServer(const std::string &ip, unsigned short port)
      : Ip(ip), Port(std::to_string(port)), ShouldExit(false),
        ListenSocket(INVALID_SOCKET), Clients(), Callbacks(),
        OnClientConnected(nullptr), OnClientDisconnected(nullptr) {}

  AnTcpServer(const std::string &ip, const std::string &port)
      : Ip(ip), Port(port), ShouldExit(false), ListenSocket(INVALID_SOCKET),
        Clients(), Callbacks(), OnClientConnected(nullptr),
        OnClientDisconnected(nullptr) {}

  AnTcpServer(const AnTcpServer &) = delete;
  AnTcpServer &operator=(const AnTcpServer &) = delete;

  inline void
  SetOnClientConnected(std::function<void(ClientHandler *)> handlerFunction) {
    OnClientConnected = handlerFunction;
  }

  inline void SetOnClientDisconnected(
      std::function<void(ClientHandler *)> handlerFunction) {
    OnClientDisconnected = handlerFunction;
  }

  inline bool AddCallback(
      AnTcpMessageType type,
      std::function<void(ClientHandler *, AnTcpMessageType, const void *, int)>
          callback) noexcept {
    if (!Callbacks.contains(type)) {
      Callbacks[type] = callback;
      return true;
    }

    return false;
  }

  inline bool RemoveCallback(AnTcpMessageType type) noexcept {
    if (Callbacks.contains(type)) {
      Callbacks.erase(type);
      return true;
    }

    return false;
  }

  inline void Stop() noexcept {
    ShouldExit = true;
    SocketCleanup();
  }

  AnTcpError Run() noexcept;

private:
  constexpr void SocketCleanup() noexcept {
    if (ListenSocket != INVALID_SOCKET) {
      closesocket(ListenSocket);
      ListenSocket = INVALID_SOCKET;
    }
  }

  constexpr void ClientCleanup() noexcept {
    for (size_t i = 0; i < Clients.size(); ++i) {
      if (Clients[i] && !Clients[i]->IsConnected()) {
        if (OnClientDisconnected) {
          OnClientDisconnected(Clients[i].get());
        }

        Clients.erase(Clients.begin() + i);
        i--;
      }
    }
  }
};