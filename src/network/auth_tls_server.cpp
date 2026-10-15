#include "network/auth_tls_server.h"

#define SECURITY_WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define SCHANNEL_USE_BLACKLISTS
// clang-format off
#include <winsock2.h>
#include <Windows.h>
#include <winternl.h>
#include <security.h>
#include <schannel.h>
#include <wincrypt.h>
#include <ncrypt.h>
// clang-format on

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <syncstream>
#include <thread>
#include <utility>

#include "features/authentication/remote_auth.h"
#include "network/tcp_server.h"
#include "persistence/my_sql_account_store.h"

namespace bubble_fighter::server::network {
namespace {
namespace wire = protocol::auth;
using Clock = std::chrono::steady_clock;
constexpr std::size_t kEncryptedLimit = 65536;
constexpr ULONG kContextFlags =
    ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT |
    ASC_REQ_CONFIDENTIALITY | ASC_REQ_EXTENDED_ERROR | ASC_REQ_ALLOCATE_MEMORY |
    ASC_REQ_STREAM;

[[noreturn]] void TransportError() {
  throw std::runtime_error("Auth TLS transport failed.");
}
struct Winsock {
  Winsock() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) TransportError();
  }
  ~Winsock() { WSACleanup(); }
};
struct Socket {
  SOCKET value = INVALID_SOCKET;
  explicit Socket(SOCKET socket) : value(socket) {
    if (value == INVALID_SOCKET) TransportError();
  }
  ~Socket() { closesocket(value); }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
};
struct CertificateStore {
  HCERTSTORE value = CertOpenStore(
      CERT_STORE_PROV_SYSTEM_W, 0, 0,
      CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG |
          CERT_STORE_READONLY_FLAG,
      L"MY");
  ~CertificateStore() {
    if (value) CertCloseStore(value, 0);
  }
};
struct Certificate {
  PCCERT_CONTEXT value = nullptr;
  ~Certificate() {
    if (value) CertFreeCertificateContext(value);
  }
};
struct Credential {
  CredHandle value{};
  Credential() { SecInvalidateHandle(&value); }
  ~Credential() {
    if (SecIsValidHandle(&value)) FreeCredentialsHandle(&value);
  }
};
struct Context {
  CtxtHandle value{};
  Context() { SecInvalidateHandle(&value); }
  ~Context() {
    if (SecIsValidHandle(&value)) DeleteSecurityContext(&value);
  }
};
struct SecurityBuffer {
  SecBuffer value{0, SECBUFFER_TOKEN, nullptr};
  ~SecurityBuffer() {
    if (value.pvBuffer) FreeContextBuffer(value.pvBuffer);
  }
};
struct ServerCredential {
  Certificate certificate;
  Credential credential;
  explicit ServerCredential(std::string_view thumbprint) {
    const auto fail = [] {
      throw std::invalid_argument("Invalid auth TLS certificate or private key.");
    };
    if (thumbprint.size() != 40) fail();
    std::array<BYTE, 20> hash{};
    const auto nibble = [&](char digit) -> unsigned int {
      if (digit >= '0' && digit <= '9') return digit - '0';
      if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
      if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
      throw std::invalid_argument("Invalid auth TLS certificate or private key.");
    };
    for (std::size_t index = 0; index < hash.size(); ++index) {
      hash[index] = static_cast<BYTE>(
          (nibble(thumbprint[index * 2]) << 4) |
          nibble(thumbprint[index * 2 + 1]));
    }
    CertificateStore store;
    if (!store.value) fail();
    CRYPT_HASH_BLOB blob{static_cast<DWORD>(hash.size()), hash.data()};
    certificate.value = CertFindCertificateInStore(
        store.value, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
        CERT_FIND_HASH, &blob, nullptr);
    if (!certificate.value ||
        CertVerifyTimeValidity(nullptr, certificate.value->pCertInfo) != 0) {
      fail();
    }
    HCRYPTPROV_OR_NCRYPT_KEY_HANDLE private_key = 0;
    DWORD key_spec = 0;
    BOOL release = FALSE;
    if (!CryptAcquireCertificatePrivateKey(
            certificate.value,
            CRYPT_ACQUIRE_SILENT_FLAG | CRYPT_ACQUIRE_COMPARE_KEY_FLAG |
                CRYPT_ACQUIRE_ALLOW_NCRYPT_KEY_FLAG,
            nullptr, &private_key, &key_spec, &release)) {
      fail();
    }
    if (release) {
      if (key_spec == CERT_NCRYPT_KEY_SPEC) NCryptFreeObject(private_key);
      else CryptReleaseContext(private_key, 0);
    }
    TLS_PARAMETERS protocols{};
    protocols.grbitDisabledProtocols =
        SP_PROT_SSL2_SERVER | SP_PROT_SSL3_SERVER | SP_PROT_TLS1_0_SERVER |
        SP_PROT_TLS1_1_SERVER;
    SCH_CREDENTIALS settings{};
    settings.dwVersion = SCH_CREDENTIALS_VERSION;
    settings.cCreds = 1;
    settings.paCred = &certificate.value;
    settings.dwFlags = SCH_USE_STRONG_CRYPTO | SCH_CRED_NO_SYSTEM_MAPPER;
    settings.cTlsParameters = 1;
    settings.pTlsParameters = &protocols;
    TimeStamp expiry{};
    if (AcquireCredentialsHandleW(
            nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_INBOUND,
            nullptr, &settings, nullptr, nullptr, &credential.value,
            &expiry) != SEC_E_OK) {
      fail();
    }
  }
};

void Nonblocking(SOCKET socket) {
  u_long enabled = 1;
  if (ioctlsocket(socket, FIONBIO, &enabled) == SOCKET_ERROR) TransportError();
}
void Wait(SOCKET socket, bool write, Clock::time_point deadline,
          std::stop_token stop) {
  for (;;) {
    if (stop.stop_requested() || Clock::now() >= deadline) TransportError();
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(socket, &ready);
    const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
        deadline - Clock::now());
    timeval interval{0, static_cast<long>(
        std::clamp<std::int64_t>(remaining.count(), 0, 100000))};
    const int result = select(0, write ? nullptr : &ready,
                              write ? &ready : nullptr, nullptr, &interval);
    if (result == SOCKET_ERROR) TransportError();
    if (result) return;
  }
}
void Send(SOCKET socket, std::span<const std::uint8_t> bytes,
          Clock::time_point deadline, std::stop_token stop) {
  while (!bytes.empty()) {
    Wait(socket, true, deadline, stop);
    const int sent = send(socket, reinterpret_cast<const char*>(bytes.data()),
                          static_cast<int>(bytes.size()), 0);
    if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
    if (sent <= 0) TransportError();
    bytes = bytes.subspan(static_cast<std::size_t>(sent));
  }
}
void Receive(SOCKET socket, std::vector<std::uint8_t>& encrypted,
             Clock::time_point deadline, std::stop_token stop) {
  if (encrypted.size() >= kEncryptedLimit) TransportError();
  std::array<std::uint8_t, 16384> block{};
  for (;;) {
    Wait(socket, false, deadline, stop);
    const int received = recv(
        socket, reinterpret_cast<char*>(block.data()),
        static_cast<int>(std::min(block.size(), kEncryptedLimit - encrypted.size())),
        0);
    if (received == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
    if (received <= 0) TransportError();
    encrypted.insert(encrypted.end(), block.begin(), block.begin() + received);
    return;
  }
}
void PreserveExtra(std::vector<std::uint8_t>& encrypted,
                   std::span<const SecBuffer> buffers) {
  std::size_t extra = 0;
  for (const auto& buffer : buffers) {
    if (buffer.BufferType == SECBUFFER_EXTRA) extra = buffer.cbBuffer;
  }
  if (extra > encrypted.size()) TransportError();
  encrypted.erase(encrypted.begin(), encrypted.end() - extra);
}
void Handshake(SOCKET socket, Credential& credential, Context& context,
               std::vector<std::uint8_t>& encrypted, std::stop_token stop) {
  const auto deadline = Clock::now() + std::chrono::seconds(5);
  bool first = true;
  Receive(socket, encrypted, deadline, stop);
  for (;;) {
    if (stop.stop_requested() || Clock::now() >= deadline) TransportError();
    std::array<SecBuffer, 2> input{{
        {static_cast<ULONG>(encrypted.size()), SECBUFFER_TOKEN, encrypted.data()},
        {0, SECBUFFER_EMPTY, nullptr}}};
    SecBufferDesc incoming{SECBUFFER_VERSION,
                           static_cast<ULONG>(input.size()), input.data()};
    SecurityBuffer output;
    SecBufferDesc outgoing{SECBUFFER_VERSION, 1, &output.value};
    ULONG attributes = 0;
    TimeStamp expiry{};
    const auto status = AcceptSecurityContext(
        &credential.value, first ? nullptr : &context.value, &incoming,
        kContextFlags, SECURITY_NATIVE_DREP, &context.value, &outgoing,
        &attributes, &expiry);
    if (status == SEC_E_INCOMPLETE_MESSAGE) {
      Receive(socket, encrypted, deadline, stop);
      continue;
    }
    if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED) TransportError();
    first = false;
    if (output.value.cbBuffer) {
      Send(socket, {static_cast<const std::uint8_t*>(output.value.pvBuffer),
                    output.value.cbBuffer}, deadline, stop);
    }
    PreserveExtra(encrypted, input);
    if (status == SEC_E_OK) {
      SecPkgContext_ConnectionInfo connection{};
      if (!(attributes & ASC_RET_CONFIDENTIALITY) ||
          QueryContextAttributesW(&context.value, SECPKG_ATTR_CONNECTION_INFO,
                                  &connection) != SEC_E_OK ||
          !(connection.dwProtocol & (SP_PROT_TLS1_2_SERVER | SP_PROT_TLS1_3_SERVER))) {
        TransportError();
      }
      return;
    }
    if (encrypted.empty()) Receive(socket, encrypted, deadline, stop);
  }
}

wire::AuthRequest ReadRequest(SOCKET socket, Context& context,
                             std::vector<std::uint8_t>& encrypted,
                             std::stop_token stop) {
  const auto deadline = Clock::now() + std::chrono::seconds(5);
  std::vector<std::uint8_t> plain;
  for (;;) {
    if (stop.stop_requested() || Clock::now() >= deadline) TransportError();
    if (encrypted.empty()) Receive(socket, encrypted, deadline, stop);
    std::array<SecBuffer, 4> buffers{{
        {static_cast<ULONG>(encrypted.size()), SECBUFFER_DATA, encrypted.data()},
        {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr},
        {0, SECBUFFER_EMPTY, nullptr}}};
    SecBufferDesc descriptor{SECBUFFER_VERSION,
                            static_cast<ULONG>(buffers.size()), buffers.data()};
    const auto status = DecryptMessage(&context.value, &descriptor, 0, nullptr);
    if (status == SEC_E_INCOMPLETE_MESSAGE) {
      Receive(socket, encrypted, deadline, stop);
      continue;
    }
    if (status != SEC_E_OK) TransportError();
    for (const auto& buffer : buffers) {
      if (buffer.BufferType != SECBUFFER_DATA) continue;
      if (plain.size() + buffer.cbBuffer > wire::kMaxPayloadBytes + 4) {
        throw std::invalid_argument("Invalid auth request frame.");
      }
      const auto* data = static_cast<const std::uint8_t*>(buffer.pvBuffer);
      if (buffer.cbBuffer) plain.insert(plain.end(), data, data + buffer.cbBuffer);
    }
    PreserveExtra(encrypted, buffers);
    if (plain.size() < 4) continue;
    std::uint32_t size = 0;
    for (std::size_t index = 0; index < 4; ++index) size = (size << 8) | plain[index];
    if (size > wire::kMaxPayloadBytes || plain.size() > size + 4) {
      throw std::invalid_argument("Invalid auth request frame.");
    }
    if (plain.size() == size + 4) {
      // Reject a coalesced second record or any already queued trailing bytes
      // before allowing the handler to perform account work.
      std::uint8_t trailing = 0;
      const int queued = recv(socket, reinterpret_cast<char*>(&trailing), 1,
                               MSG_PEEK);
      if (!encrypted.empty() || queued > 0) {
        throw std::invalid_argument("Invalid auth request frame.");
      }
      return wire::DecodeRequest(plain);
    }
  }
}
void WriteResponse(SOCKET socket, Credential& credential, Context& context,
                   const wire::AuthResponse& response, std::stop_token stop) {
  const auto deadline = Clock::now() + std::chrono::seconds(5);
  SecPkgContext_StreamSizes sizes{};
  if (QueryContextAttributesW(&context.value, SECPKG_ATTR_STREAM_SIZES, &sizes) !=
      SEC_E_OK) TransportError();
  const auto plain = wire::EncodeResponse(response);
  if (plain.size() > sizes.cbMaximumMessage ||
      sizes.cbHeader + plain.size() + sizes.cbTrailer > kEncryptedLimit) {
    TransportError();
  }
  std::vector<std::uint8_t> encrypted(sizes.cbHeader + plain.size() +
                                     sizes.cbTrailer);
  std::copy(plain.begin(), plain.end(), encrypted.begin() + sizes.cbHeader);
  std::array<SecBuffer, 4> buffers{{
      {sizes.cbHeader, SECBUFFER_STREAM_HEADER, encrypted.data()},
      {static_cast<ULONG>(plain.size()), SECBUFFER_DATA,
       encrypted.data() + sizes.cbHeader},
      {sizes.cbTrailer, SECBUFFER_STREAM_TRAILER,
       encrypted.data() + sizes.cbHeader + plain.size()},
      {0, SECBUFFER_EMPTY, nullptr}}};
  SecBufferDesc descriptor{SECBUFFER_VERSION,
                          static_cast<ULONG>(buffers.size()), buffers.data()};
  if (EncryptMessage(&context.value, 0, &descriptor, 0) != SEC_E_OK) TransportError();
  for (const auto& buffer : buffers) {
    if (buffer.cbBuffer) {
      Send(socket, {static_cast<const std::uint8_t*>(buffer.pvBuffer),
                    buffer.cbBuffer}, deadline, stop);
    }
  }
  DWORD shutdown_token = SCHANNEL_SHUTDOWN;
  SecBuffer token{sizeof(shutdown_token), SECBUFFER_TOKEN, &shutdown_token};
  SecBufferDesc control{SECBUFFER_VERSION, 1, &token};
  if (ApplyControlToken(&context.value, &control) != SEC_E_OK) TransportError();
  SecurityBuffer output;
  SecBufferDesc close{SECBUFFER_VERSION, 1, &output.value};
  ULONG attributes = 0;
  TimeStamp expiry{};
  const auto status = AcceptSecurityContext(
      &credential.value, &context.value, nullptr, kContextFlags,
      SECURITY_NATIVE_DREP, nullptr, &close, &attributes, &expiry);
  if (status != SEC_E_OK && status != SEC_I_CONTEXT_EXPIRED) TransportError();
  if (output.value.cbBuffer) {
    Send(socket, {static_cast<const std::uint8_t*>(output.value.pvBuffer),
                  output.value.cbBuffer}, deadline, stop);
  }
  shutdown(socket, SD_SEND);
}
wire::AuthResponse Failure(wire::ErrorCode code) {
  wire::AuthResponse result;
  result.status = wire::Status::kError;
  result.error = code;
  result.message = code == wire::ErrorCode::kBadRequest
                       ? "Invalid account request." : "Authentication failed.";
  return result;
}
void Serve(SOCKET socket, Credential& credential,
           const AuthTlsServer::RequestHandler& handler, std::string_view peer,
           std::stop_token stop) {
  Context context;
  std::vector<std::uint8_t> encrypted;
  Handshake(socket, credential, context, encrypted, stop);
  wire::AuthResponse response;
  try {
    const auto request = ReadRequest(socket, context, encrypted, stop);
    response = handler(request, peer);
  } catch (const std::invalid_argument&) {
    response = Failure(wire::ErrorCode::kBadRequest);
  }
  WriteResponse(socket, credential, context, response, stop);
}
}  // namespace

void AuthTlsServer::ValidateConfiguration(
    std::string_view certificate_thumbprint,
    const std::filesystem::path& database_configuration) {
  ServerCredential certificate(certificate_thumbprint);
  persistence::MySqlAccountStore database(database_configuration);
}
int AuthTlsServer::Run(
    std::uint16_t port, std::string_view listen_address,
    std::string_view certificate_thumbprint,
    const std::filesystem::path& database_configuration, std::stop_token stop,
    const std::function<void(std::uint16_t)>& on_listening) const {
  features::authentication::AttemptLimiter limiter;
  return Run(port, listen_address, certificate_thumbprint,
      RequestHandler{[&](const wire::AuthRequest& request, std::string_view peer) {
        if (request.action == wire::Action::kHealth) return wire::AuthResponse{};
        try {
          persistence::MySqlAccountStore store(database_configuration);
          features::authentication::AccountAuthentication accounts(store);
          return features::authentication::RemoteAuth(limiter).Handle(
              request, accounts, peer, Clock::now());
        } catch (...) {
          return Failure(wire::ErrorCode::kUnavailable);
        }
      }}, stop, on_listening);
}
int AuthTlsServer::Run(
    std::uint16_t port, std::string_view listen_address,
    std::string_view certificate_thumbprint, const RequestHandler& handler,
    std::stop_token stop,
    const std::function<void(std::uint16_t)>& on_listening) const {
  if (!port || !handler) {
    throw std::invalid_argument("Invalid auth listener configuration.");
  }
  const auto address = TcpServer::ParseListenAddress(listen_address);
  Winsock winsock;
  ServerCredential certificate(certificate_thumbprint);
  Socket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  const BOOL exclusive = TRUE;
  if (setsockopt(listener.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) ==
      SOCKET_ERROR) TransportError();
  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_port = htons(port);
  endpoint.sin_addr.s_addr = htonl(address);
  if (bind(listener.value, reinterpret_cast<const sockaddr*>(&endpoint),
           sizeof(endpoint)) == SOCKET_ERROR ||
      listen(listener.value, SOMAXCONN) == SOCKET_ERROR) TransportError();
  Nonblocking(listener.value);
  std::stop_source connections_stop;
  std::stop_callback propagate(stop, [&] { connections_stop.request_stop(); });
  struct Worker {
    std::uint32_t address;
    std::shared_ptr<std::atomic<bool>> done;
    std::jthread thread;
  };
  std::vector<Worker> workers;
  struct StopBeforeJoin {
    std::stop_source& source;
    ~StopBeforeJoin() { source.request_stop(); }
  } stop_before_join{connections_stop};
  if (on_listening) on_listening(port);
  std::osyncstream(std::cout)
      << "Auth TLS listening on " << listen_address << ':' << port << std::endl;
  while (!stop.stop_requested()) {
    std::erase_if(workers, [](const Worker& worker) { return worker.done->load(); });
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(listener.value, &ready);
    timeval interval{0, 100000};
    const int available = select(0, &ready, nullptr, nullptr, &interval);
    if (available == SOCKET_ERROR) TransportError();
    if (!available) continue;
    sockaddr_in peer{};
    int length = sizeof(peer);
    const SOCKET accepted = accept(listener.value,
                                    reinterpret_cast<sockaddr*>(&peer), &length);
    if (accepted == INVALID_SOCKET) {
      if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
      TransportError();
    }
    auto connection = std::make_shared<Socket>(accepted);
    const auto peer_address = ntohl(peer.sin_addr.s_addr);
    if (workers.size() >= 16 ||
        std::count_if(workers.begin(), workers.end(), [&](const Worker& worker) {
          return worker.address == peer_address;
        }) >= 2) continue;
    Nonblocking(accepted);
    const auto peer_text = std::to_string(peer_address >> 24) + "." +
        std::to_string((peer_address >> 16) & 255) + "." +
        std::to_string((peer_address >> 8) & 255) + "." +
        std::to_string(peer_address & 255);
    auto done = std::make_shared<std::atomic<bool>>(false);
    workers.push_back(Worker{peer_address, done,
        std::jthread([&, connection, done, peer_text,
                      token = connections_stop.get_token()] {
          try {
            Serve(connection->value, certificate.credential, handler, peer_text,
                  token);
          } catch (...) {
            // Peer failures never expose credentials, tickets or key material.
          }
          done->store(true);
        })});
  }
  return 0;
}
}  // namespace bubble_fighter::server::network
