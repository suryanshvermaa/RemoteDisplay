#pragma once
// Authenticated encryption for the UDP stream, built on Windows CNG (no third-party deps).
//
// Pairing: the host shows a random 128-bit key (the "pairing key"); the user types it into the
// client. Handshake (1 round trip):
//   C -> H  Hs1: cNonce[16] cPub[64]                       (ephemeral ECDH P-256)
//   H -> C  Hs2: hNonce[16] hPub[64] hostMac[32]
//   th      = SHA256(cNonce|cPub|hNonce|hPub)
//   prk     = HMAC(psk, th | ECDH-shared-secret)
//   k_h2c, k_c2h = HMAC(prk, "rd-h2c"/"rd-c2h")[0..16]
//   hostMac = HMAC(psk, "rd-host" | th)        -> proves to the client that the host knows psk
// The client proves it knows psk implicitly: only a holder of psk derives the right keys, so the
// host promotes the new session on the first data packet that authenticates.
// Data packets: WireHeader (AAD) | AES-128-GCM ciphertext | 16-byte tag; nonce = seq; a sliding
// window rejects replays. Because every packet authenticates on its own, the client's IP/port may
// change (NAT rebinding, Wi-Fi <-> LTE) without re-handshaking.
#include <windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace rd {

constexpr uint32_t kWireMagic = 0x33445352;  // 'RSD3'
enum class WireType : uint8_t { Hs1 = 1, Hs2 = 2, Data = 3 };

#pragma pack(push, 1)
struct WireHeader {
    uint32_t magic;
    uint8_t type;  // WireType
    uint8_t rsvd[3];
    uint64_t seq;
};
#pragma pack(pop)
static_assert(sizeof(WireHeader) == 16);

constexpr size_t kTagLen = 16;
constexpr size_t kKeyLen = 16;
constexpr size_t kPubLen = 64;
constexpr size_t kNonceLen = 16;
constexpr size_t kHs1Len = kNonceLen + kPubLen;       // 80
constexpr size_t kHs2Len = kNonceLen + kPubLen + 32;  // 112

namespace crypto {

inline bool Random(void* p, size_t n) {
    return BCryptGenRandom(nullptr, static_cast<PUCHAR>(p), ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}

inline bool Sha256(const uint8_t* d, size_t n, uint8_t out[32]) {
    BCRYPT_ALG_HANDLE h;
    if (BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    bool ok = BCryptHash(h, nullptr, 0, const_cast<PUCHAR>(d), ULONG(n), out, 32) >= 0;
    BCryptCloseAlgorithmProvider(h, 0);
    return ok;
}

inline bool Hmac(const uint8_t* key, size_t kn, const std::vector<uint8_t>& data, uint8_t out[32]) {
    BCRYPT_ALG_HANDLE h;
    if (BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0)
        return false;
    bool ok = BCryptHash(h, const_cast<PUCHAR>(key), ULONG(kn), const_cast<PUCHAR>(data.data()),
                         ULONG(data.size()), out, 32) >= 0;
    BCryptCloseAlgorithmProvider(h, 0);
    return ok;
}

inline bool ConstTimeEq(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; ++i) d |= a[i] ^ b[i];
    return d == 0;
}

// Ephemeral ECDH P-256 key pair.
class Ecdh {
public:
    Ecdh() = default;
    Ecdh(const Ecdh&) = delete;
    Ecdh& operator=(const Ecdh&) = delete;
    ~Ecdh() { Reset(); }

    bool Generate() {
        Reset();
        if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0) < 0) return false;
        if (BCryptGenerateKeyPair(alg_, &key_, 256, 0) < 0) return false;
        if (BCryptFinalizeKeyPair(key_, 0) < 0) return false;
        uint8_t blob[8 + kPubLen];
        ULONG n = 0;
        if (BCryptExportKey(key_, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof(blob), &n, 0) < 0 ||
            n != sizeof(blob))
            return false;
        memcpy(pub, blob + 8, kPubLen);
        return true;
    }

    // Raw shared secret (32 bytes). Fails for invalid peer points (CNG validates the curve point).
    bool Derive(const uint8_t peerPub[kPubLen], uint8_t secret[32]) {
        uint8_t blob[8 + kPubLen];
        BCRYPT_ECCKEY_BLOB hdr = {BCRYPT_ECDH_PUBLIC_P256_MAGIC, 32};
        memcpy(blob, &hdr, 8);
        memcpy(blob + 8, peerPub, kPubLen);
        BCRYPT_KEY_HANDLE peer = nullptr;
        if (BCryptImportKeyPair(alg_, nullptr, BCRYPT_ECCPUBLIC_BLOB, &peer, blob, sizeof(blob), 0) < 0)
            return false;
        BCRYPT_SECRET_HANDLE s = nullptr;
        bool ok = BCryptSecretAgreement(key_, peer, &s, 0) >= 0;
        ULONG n = 0;
        if (ok) ok = BCryptDeriveKey(s, BCRYPT_KDF_RAW_SECRET, nullptr, secret, 32, &n, 0) >= 0 && n == 32;
        if (s) BCryptDestroySecret(s);
        BCryptDestroyKey(peer);
        return ok;
    }

    uint8_t pub[kPubLen] = {};

private:
    void Reset() {
        if (key_) BCryptDestroyKey(key_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
        key_ = nullptr;
        alg_ = nullptr;
    }
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_KEY_HANDLE key_ = nullptr;
};

// AES-128-GCM with an explicit 64-bit sequence number as nonce.
class Aead {
public:
    Aead() = default;
    Aead(const Aead&) = delete;
    Aead& operator=(const Aead&) = delete;
    Aead(Aead&& o) noexcept { *this = std::move(o); }
    Aead& operator=(Aead&& o) noexcept {
        Reset();
        alg_ = o.alg_; key_ = o.key_;
        o.alg_ = nullptr; o.key_ = nullptr;
        return *this;
    }
    ~Aead() { Reset(); }

    bool Init(const uint8_t key[kKeyLen]) {
        Reset();
        if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) return false;
        if (BCryptSetProperty(alg_, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                              sizeof(BCRYPT_CHAIN_MODE_GCM), 0) < 0)
            return false;
        return BCryptGenerateSymmetricKey(alg_, &key_, nullptr, 0, const_cast<PUCHAR>(key), kKeyLen, 0) >= 0;
    }

    bool Seal(uint64_t seq, const uint8_t* aad, size_t aadLen, const uint8_t* in, size_t n, uint8_t* out,
              uint8_t tag[kTagLen]) {
        uint8_t nonce[12] = {};
        memcpy(nonce + 4, &seq, 8);
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO ai;
        BCRYPT_INIT_AUTH_MODE_INFO(ai);
        ai.pbNonce = nonce; ai.cbNonce = 12;
        ai.pbAuthData = const_cast<PUCHAR>(aad); ai.cbAuthData = ULONG(aadLen);
        ai.pbTag = tag; ai.cbTag = kTagLen;
        ULONG res = 0;
        return BCryptEncrypt(key_, const_cast<PUCHAR>(in), ULONG(n), &ai, nullptr, 0, out, ULONG(n), &res, 0) >= 0;
    }

    bool Open(uint64_t seq, const uint8_t* aad, size_t aadLen, const uint8_t* in, size_t n,
              const uint8_t tag[kTagLen], uint8_t* out) {
        uint8_t nonce[12] = {};
        memcpy(nonce + 4, &seq, 8);
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO ai;
        BCRYPT_INIT_AUTH_MODE_INFO(ai);
        ai.pbNonce = nonce; ai.cbNonce = 12;
        ai.pbAuthData = const_cast<PUCHAR>(aad); ai.cbAuthData = ULONG(aadLen);
        ai.pbTag = const_cast<PUCHAR>(tag); ai.cbTag = kTagLen;
        ULONG res = 0;
        return BCryptDecrypt(key_, const_cast<PUCHAR>(in), ULONG(n), &ai, nullptr, 0, out, ULONG(n), &res, 0) >= 0;
    }

private:
    void Reset() {
        if (key_) BCryptDestroyKey(key_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
        key_ = nullptr;
        alg_ = nullptr;
    }
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_KEY_HANDLE key_ = nullptr;
};

}  // namespace crypto

// Pairing key <-> text (RFC 4648 base32, grouped for typing: XXXXX-XXXXX-...).
inline std::string FormatKey(const uint8_t k[kKeyLen]) {
    static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string s;
    uint32_t buf = 0;
    int bits = 0;
    for (size_t i = 0; i < kKeyLen; ++i) {
        buf = (buf << 8) | k[i];
        bits += 8;
        while (bits >= 5) { s += A[(buf >> (bits - 5)) & 31]; bits -= 5; }
    }
    if (bits) s += A[(buf << (5 - bits)) & 31];
    std::string g;
    for (size_t i = 0; i < s.size(); ++i) { if (i && i % 5 == 0) g += '-'; g += s[i]; }
    return g;
}

inline bool ParseKey(const std::string& text, uint8_t k[kKeyLen]) {
    uint32_t buf = 0;
    int bits = 0;
    size_t out = 0;
    for (char c : text) {
        if (c == '-' || c == ' ') continue;
        if (c >= 'a' && c <= 'z') c = char(c - 32);
        int v = (c >= 'A' && c <= 'Z') ? c - 'A' : (c >= '2' && c <= '7') ? c - '2' + 26 : -1;
        if (v < 0) return false;
        buf = (buf << 5) | uint32_t(v);
        bits += 5;
        if (bits >= 8) {
            if (out < kKeyLen) k[out++] = uint8_t(buf >> (bits - 8));
            bits -= 8;
        }
    }
    return out == kKeyLen;
}

// One direction-pair of keys plus replay protection. Created by a handshake.
class Session {
public:
    bool valid() const { return valid_; }

    // plain -> out = WireHeader | ciphertext | tag. Returns total size, or 0 on failure.
    size_t Seal(const uint8_t* plain, size_t n, uint8_t* out) {
        WireHeader h = {kWireMagic, uint8_t(WireType::Data), {}, txSeq_++};
        memcpy(out, &h, sizeof(h));
        if (!tx_.Seal(h.seq, out, sizeof(h), plain, n, out + sizeof(h), out + sizeof(h) + n)) return 0;
        return sizeof(h) + n + kTagLen;
    }

    // wire -> plain. Rejects forgeries and replays. `plain` must hold n - 16 - 16 bytes.
    bool Open(const uint8_t* wire, size_t n, uint8_t* plain, size_t& plainLen) {
        if (n < sizeof(WireHeader) + kTagLen) return false;
        WireHeader h;
        memcpy(&h, wire, sizeof(h));
        if (!NotReplayed(h.seq)) return false;
        const size_t cl = n - sizeof(h) - kTagLen;
        if (!rx_.Open(h.seq, wire, sizeof(h), wire + sizeof(h), cl, wire + sizeof(h) + cl, plain)) return false;
        MarkSeen(h.seq);
        plainLen = cl;
        return true;
    }

    // Hash of this session's handshake; a device signs it to prove who it is (see identity.h).
    const uint8_t* transcriptHash() const { return th_; }

    bool Setup(const uint8_t txKey[kKeyLen], const uint8_t rxKey[kKeyLen], const uint8_t th[32]) {
        memcpy(th_, th, 32);
        valid_ = tx_.Init(txKey) && rx_.Init(rxKey);
        txSeq_ = 0; maxSeq_ = 0; window_ = 0; any_ = false;
        return valid_;
    }

private:
    bool NotReplayed(uint64_t s) const {
        if (!any_ || s > maxSeq_) return true;
        uint64_t d = maxSeq_ - s;
        return d < 64 && !(window_ & (1ull << d));
    }
    void MarkSeen(uint64_t s) {
        if (!any_ || s > maxSeq_) {
            uint64_t shift = any_ ? s - maxSeq_ : 0;
            window_ = shift >= 64 ? 0 : window_ << shift;
            window_ |= 1;
            maxSeq_ = s;
            any_ = true;
        } else {
            window_ |= 1ull << (maxSeq_ - s);
        }
    }

    crypto::Aead tx_, rx_;
    uint8_t th_[32] = {};
    uint64_t txSeq_ = 0, maxSeq_ = 0, window_ = 0;
    bool any_ = false, valid_ = false;
};

namespace detail {
inline bool DeriveSession(const uint8_t psk[kKeyLen], const uint8_t* cNonceAndPub, const uint8_t* hNonceAndPub,
                          const uint8_t shared[32], bool isHost, Session& s, uint8_t th[32]) {
    std::vector<uint8_t> t(cNonceAndPub, cNonceAndPub + kHs1Len);
    t.insert(t.end(), hNonceAndPub, hNonceAndPub + kHs1Len);
    if (!crypto::Sha256(t.data(), t.size(), th)) return false;
    std::vector<uint8_t> m(th, th + 32);
    m.insert(m.end(), shared, shared + 32);
    uint8_t prk[32], h2c[32], c2h[32];
    if (!crypto::Hmac(psk, kKeyLen, m, prk)) return false;
    const char a[] = "rd-h2c", b[] = "rd-c2h";
    if (!crypto::Hmac(prk, 32, {a, a + 6}, h2c) || !crypto::Hmac(prk, 32, {b, b + 6}, c2h)) return false;
    return isHost ? s.Setup(h2c, c2h, th) : s.Setup(c2h, h2c, th);
}

inline bool HostMac(const uint8_t psk[kKeyLen], const uint8_t th[32], uint8_t out[32]) {
    std::vector<uint8_t> m = {'r', 'd', '-', 'h', 'o', 's', 't'};
    m.insert(m.end(), th, th + 32);
    return crypto::Hmac(psk, kKeyLen, m, out);
}
}  // namespace detail

// Host side: consumes Hs1, produces Hs2 and a (not yet active) Session.
inline bool HostRespond(const uint8_t psk[kKeyLen], const uint8_t* hs1, uint8_t* hs2Out, Session& pending) {
    crypto::Ecdh e;
    if (!e.Generate()) return false;
    uint8_t shared[32];
    if (!e.Derive(hs1 + kNonceLen, shared)) return false;
    uint8_t hNonceAndPub[kHs1Len];
    if (!crypto::Random(hNonceAndPub, kNonceLen)) return false;
    memcpy(hNonceAndPub + kNonceLen, e.pub, kPubLen);
    uint8_t th[32], mac[32];
    if (!detail::DeriveSession(psk, hs1, hNonceAndPub, shared, true, pending, th)) return false;
    if (!detail::HostMac(psk, th, mac)) return false;
    memcpy(hs2Out, hNonceAndPub, kHs1Len);
    memcpy(hs2Out + kHs1Len, mac, 32);
    return true;
}

// Client side: Begin() makes Hs1, Finish() verifies Hs2 and yields the Session.
class ClientHandshake {
public:
    bool Begin(uint8_t* hs1Out) {
        if (!ecdh_.Generate() || !crypto::Random(hs1_, kNonceLen)) return false;
        memcpy(hs1_ + kNonceLen, ecdh_.pub, kPubLen);
        memcpy(hs1Out, hs1_, kHs1Len);
        return true;
    }
    bool Finish(const uint8_t psk[kKeyLen], const uint8_t* hs2, Session& s) {
        uint8_t shared[32], th[32], want[32];
        if (!ecdh_.Derive(hs2 + kNonceLen, shared)) return false;
        if (!detail::DeriveSession(psk, hs1_, hs2, shared, false, s, th)) return false;
        if (!detail::HostMac(psk, th, want)) return false;
        return crypto::ConstTimeEq(want, hs2 + kHs1Len, 32);  // wrong pairing key => rejected
    }

private:
    crypto::Ecdh ecdh_;
    uint8_t hs1_[kHs1Len] = {};
};

}  // namespace rd
