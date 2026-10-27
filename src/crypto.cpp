#include "crypto.h"

#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#endif

#define OK(st) ((NTSTATUS)(st) >= 0)

static const uint8_t MAGIC[8] = { 'R', 'D', 'S', 'K', '1', 0, 0, 0 };
constexpr ULONG PUB_LEN = sizeof(BCRYPT_ECCKEY_BLOB) + 64;   // P-256 X||Y
constexpr uint32_t MAX_FRAME = 64u << 20;

static std::string to_hex(const uint8_t* p, size_t n) {
    static const char* H = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += H[p[i] >> 4]; s += H[p[i] & 15]; }
    return s;
}
static std::vector<uint8_t> from_hex(const std::string& s) {
    auto v = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back((uint8_t)(v(s[i]) << 4 | v(s[i + 1])));
    return out;
}

// SHA-256(label | viewerPub | hostPub | sharedSecret) -> 32 bytes
static bool derive(BCRYPT_SECRET_HANDLE secret, const char* label,
                   const uint8_t* vpub, const uint8_t* hpub, uint8_t out[32]) {
    std::vector<uint8_t> prepend(label, label + strlen(label));
    prepend.insert(prepend.end(), vpub, vpub + PUB_LEN);
    prepend.insert(prepend.end(), hpub, hpub + PUB_LEN);
    BCryptBuffer bufs[2];
    bufs[0].BufferType = KDF_HASH_ALGORITHM;
    bufs[0].cbBuffer = (ULONG)((wcslen(BCRYPT_SHA256_ALGORITHM) + 1) * sizeof(wchar_t));
    bufs[0].pvBuffer = (PVOID)BCRYPT_SHA256_ALGORITHM;
    bufs[1].BufferType = KDF_SECRET_PREPEND;
    bufs[1].cbBuffer = (ULONG)prepend.size();
    bufs[1].pvBuffer = prepend.data();
    BCryptBufferDesc desc{ BCRYPTBUFFER_VERSION, 2, bufs };
    ULONG n = 0;
    return OK(BCryptDeriveKey(secret, BCRYPT_KDF_HASH, &desc, out, 32, &n, 0)) && n == 32;
}

SecureChannel::~SecureChannel() {
    if (tx_) BCryptDestroyKey(tx_);
    if (rx_) BCryptDestroyKey(rx_);
    if (aes_) BCryptCloseAlgorithmProvider(aes_, 0);
}

bool SecureChannel::handshake(bool initiator) {
    BCRYPT_ALG_HANDLE ecdh = nullptr;
    BCRYPT_KEY_HANDLE mine = nullptr, peer = nullptr;
    BCRYPT_SECRET_HANDLE secret = nullptr;
    bool ok = false;
    uint8_t kv2h[32], kh2v[32], kcode[32];

    do {
        if (!OK(BCryptOpenAlgorithmProvider(&ecdh, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0))) break;
        if (!OK(BCryptGenerateKeyPair(ecdh, &mine, 256, 0)) || !OK(BCryptFinalizeKeyPair(mine, 0))) break;

        uint8_t out[8 + PUB_LEN], in[8 + PUB_LEN];
        ULONG n = 0;
        memcpy(out, MAGIC, 8);
        if (!OK(BCryptExportKey(mine, nullptr, BCRYPT_ECCPUBLIC_BLOB, out + 8, PUB_LEN, &n, 0)) || n != PUB_LEN) break;
        if (!send_all(s_, out, sizeof(out)) || !recv_all(s_, in, sizeof(in))) break;
        if (memcmp(in, MAGIC, 8) != 0) break;

        auto* hdr = (BCRYPT_ECCKEY_BLOB*)(in + 8);
        if (hdr->dwMagic != BCRYPT_ECDH_PUBLIC_P256_MAGIC || hdr->cbKey != 32) break;
        if (!OK(BCryptImportKeyPair(ecdh, nullptr, BCRYPT_ECCPUBLIC_BLOB, &peer, in + 8, PUB_LEN, 0))) break;
        if (!OK(BCryptSecretAgreement(mine, peer, &secret, 0))) break;

        const uint8_t* vpub = initiator ? out + 8 : in + 8;
        const uint8_t* hpub = initiator ? in + 8 : out + 8;
        if (!derive(secret, "v2h", vpub, hpub, kv2h) ||
            !derive(secret, "h2v", vpub, hpub, kh2v) ||
            !derive(secret, "code", vpub, hpub, kcode)) break;

        if (!OK(BCryptOpenAlgorithmProvider(&aes_, BCRYPT_AES_ALGORITHM, nullptr, 0))) break;
        if (!OK(BCryptSetProperty(aes_, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                                  sizeof(BCRYPT_CHAIN_MODE_GCM), 0))) break;
        if (!OK(BCryptGenerateSymmetricKey(aes_, &tx_, nullptr, 0, initiator ? kv2h : kh2v, 32, 0))) break;
        if (!OK(BCryptGenerateSymmetricKey(aes_, &rx_, nullptr, 0, initiator ? kh2v : kv2h, 32, 0))) break;

        std::string h = to_hex(kcode, 4);
        code_ = h.substr(0, 4) + "-" + h.substr(4, 4);
        ok = true;
    } while (false);

    SecureZeroMemory(kv2h, sizeof(kv2h));
    SecureZeroMemory(kh2v, sizeof(kh2v));
    if (secret) BCryptDestroySecret(secret);
    if (peer) BCryptDestroyKey(peer);
    if (mine) BCryptDestroyKey(mine);
    if (ecdh) BCryptCloseAlgorithmProvider(ecdh, 0);
    return ok;
}

bool SecureChannel::send(uint8_t type, const void* data, uint32_t len) {
    std::lock_guard<std::mutex> lk(txMx_);
    if (!tx_ || len + 1 > MAX_FRAME) return false;
    uint32_t plen = len + 1;
    txBuf_.resize(4 + 16 + (size_t)plen);
    uint8_t* body = txBuf_.data() + 20;
    body[0] = type;
    if (len) memcpy(body + 1, data, len);

    uint8_t nonce[12] = {};
    memcpy(nonce + 4, &txCtr_, 8);
    ++txCtr_;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO ai;
    BCRYPT_INIT_AUTH_MODE_INFO(ai);
    ai.pbNonce = nonce; ai.cbNonce = 12;
    ai.pbTag = txBuf_.data() + 4; ai.cbTag = 16;
    ULONG n = 0;
    if (!OK(BCryptEncrypt(tx_, body, plen, &ai, nullptr, 0, body, plen, &n, 0))) return false;
    memcpy(txBuf_.data(), &plen, 4);
    return send_all(s_, txBuf_.data(), txBuf_.size());
}

bool SecureChannel::recv(uint8_t& type, std::vector<uint8_t>& payload) {
    uint32_t plen;
    uint8_t tag[16];
    if (!rx_ || !recv_all(s_, &plen, 4) || plen == 0 || plen > MAX_FRAME) return false;
    if (!recv_all(s_, tag, 16)) return false;
    rxBuf_.resize(plen);
    if (!recv_all(s_, rxBuf_.data(), plen)) return false;

    uint8_t nonce[12] = {};
    memcpy(nonce + 4, &rxCtr_, 8);
    ++rxCtr_;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO ai;
    BCRYPT_INIT_AUTH_MODE_INFO(ai);
    ai.pbNonce = nonce; ai.cbNonce = 12;
    ai.pbTag = tag; ai.cbTag = 16;
    ULONG n = 0;
    if (!OK(BCryptDecrypt(rx_, rxBuf_.data(), plen, &ai, nullptr, 0, rxBuf_.data(), plen, &n, 0)))
        return false;   // tampered or out of order
    type = rxBuf_[0];
    payload.assign(rxBuf_.begin() + 1, rxBuf_.end());
    return true;
}

std::string random_hex(int bytes) {
    std::vector<uint8_t> b(bytes);
    BCryptGenRandom(nullptr, b.data(), (ULONG)bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return to_hex(b.data(), b.size());
}

std::string random_id() {
    uint32_t r;
    BCryptGenRandom(nullptr, (PUCHAR)&r, sizeof(r), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return std::to_string(100000000u + r % 900000000u);
}

std::string hash_password(const std::string& pw, const std::string& saltHex) {
    BCRYPT_ALG_HANDLE h = nullptr;
    if (!OK(BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)))
        return {};
    std::vector<uint8_t> salt = from_hex(saltHex);
    uint8_t out[32];
    NTSTATUS st = BCryptDeriveKeyPBKDF2(h, (PUCHAR)pw.data(), (ULONG)pw.size(),
                                        salt.data(), (ULONG)salt.size(), 100000, out, 32, 0);
    BCryptCloseAlgorithmProvider(h, 0);
    return OK(st) ? to_hex(out, 32) : std::string();
}

bool equal_ct(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char d = 0;
    for (size_t i = 0; i < a.size(); ++i) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}
