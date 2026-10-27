#pragma once
#include "common.h"
#include <bcrypt.h>

// Encrypted, framed message channel over a TCP socket.
//
// Handshake: both sides send an ephemeral ECDH P-256 public key, derive one
// AES-256-GCM key per direction with SHA-256 over (label | both keys | secret).
// Wire frame: [u32 len][16-byte GCM tag][ciphertext of (type byte + payload)],
// nonce = 64-bit per-direction counter, so replays/reordering fail auth.
//
// `code` is a short value both ends can compare out-of-band to rule out a
// man-in-the-middle at the relay.
class SecureChannel {
public:
    explicit SecureChannel(SOCKET s) : s_(s) {}
    ~SecureChannel();
    SecureChannel(const SecureChannel&) = delete;
    SecureChannel& operator=(const SecureChannel&) = delete;

    bool handshake(bool initiator);
    bool send(uint8_t type, const void* data, uint32_t len);   // thread-safe
    bool recv(uint8_t& type, std::vector<uint8_t>& payload);   // single reader
    SOCKET socket() const { return s_; }
    const std::string& code() const { return code_; }

private:
    SOCKET s_;
    BCRYPT_ALG_HANDLE aes_ = nullptr;
    BCRYPT_KEY_HANDLE tx_ = nullptr, rx_ = nullptr;
    uint64_t txCtr_ = 0, rxCtr_ = 0;
    std::mutex txMx_;
    std::vector<uint8_t> txBuf_, rxBuf_;
    std::string code_;
};

std::string random_hex(int bytes);
std::string random_id();                                     // 9 digits
std::string hash_password(const std::string& pw, const std::string& saltHex);
bool equal_ct(const std::string& a, const std::string& b);   // constant time
