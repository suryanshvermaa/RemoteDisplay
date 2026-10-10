#pragma once
// Per-device identity: every viewer owns a permanent ECDSA P-256 key pair. When it connects it
// proves possession of the private key by signing this session's handshake transcript, so the host
// can recognise a device it approved before WITHOUT any shared secret being replayed:
//  - the host stores only the device's public key fingerprint (not secret),
//  - a recorded Identify packet is useless in another session (the signature covers that session),
//  - knowing the pairing key alone is not enough to be remembered; the device key is required.
#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "protocol.h"
#include "session.h"

#pragma comment(lib, "bcrypt.lib")

namespace rd {

using DeviceId = std::array<uint8_t, 16>;

constexpr size_t kPubKeyLen = 64;
constexpr size_t kSigLen = 64;
constexpr size_t kDeviceNameLen = 48;
constexpr size_t kIdentifyLen = kPubKeyLen + kSigLen + kDeviceNameLen;  // payload of PacketType::Identify

inline DeviceId IdFromPub(const uint8_t pub[kPubKeyLen]) {
    uint8_t h[32];
    DeviceId id{};
    if (crypto::Sha256(pub, kPubKeyLen, h)) memcpy(id.data(), h, id.size());
    return id;
}

inline std::string IdToHex(const DeviceId& id) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (uint8_t b : id) { s += hex[b >> 4]; s += hex[b & 15]; }
    return s;
}

inline bool IdFromHex(const std::string& s, DeviceId& id) {
    if (s.size() != id.size() * 2) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        auto nib = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
        int a = nib(s[2 * i]), b = nib(s[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        id[i] = uint8_t(a * 16 + b);
    }
    return true;
}

// What the device signs: binds the signature to THIS handshake.
inline bool IdentifyDigest(const uint8_t transcriptHash[32], uint8_t out[32]) {
    std::vector<uint8_t> m = {'r', 'd', '-', 'd', 'e', 'v', 'i', 'c', 'e'};
    m.insert(m.end(), transcriptHash, transcriptHash + 32);
    return crypto::Sha256(m.data(), m.size(), out);
}

inline bool VerifyDeviceSignature(const uint8_t pub[kPubKeyLen], const uint8_t digest[32], const uint8_t sig[kSigLen]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) < 0) return false;
    uint8_t blob[8 + kPubKeyLen];
    BCRYPT_ECCKEY_BLOB hdr = {BCRYPT_ECDSA_PUBLIC_P256_MAGIC, 32};
    memcpy(blob, &hdr, 8);
    memcpy(blob + 8, pub, kPubKeyLen);
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob, sizeof(blob), 0) >= 0 &&
              BCryptVerifySignature(key, nullptr, const_cast<PUCHAR>(digest), 32, const_cast<PUCHAR>(sig), kSigLen, 0) >= 0;
    if (key) BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

class DeviceIdentity {
public:
    DeviceIdentity() = default;
    DeviceIdentity(const DeviceIdentity&) = delete;
    DeviceIdentity& operator=(const DeviceIdentity&) = delete;
    ~DeviceIdentity() { Reset(); }

    bool Generate() {
        Reset();
        if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) < 0) return false;
        if (BCryptGenerateKeyPair(alg_, &key_, 256, 0) < 0 || BCryptFinalizeKeyPair(key_, 0) < 0) return false;
        return CachePub();
    }

    // `blob` is a BCRYPT_ECCPRIVATE_BLOB as produced by ExportPrivate().
    bool Load(const std::vector<uint8_t>& blob) {
        Reset();
        if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) < 0) return false;
        if (BCryptImportKeyPair(alg_, nullptr, BCRYPT_ECCPRIVATE_BLOB, &key_, const_cast<PUCHAR>(blob.data()),
                                ULONG(blob.size()), 0) < 0)
            return false;
        return CachePub();
    }

    std::vector<uint8_t> ExportPrivate() const {
        ULONG n = 0;
        if (BCryptExportKey(key_, nullptr, BCRYPT_ECCPRIVATE_BLOB, nullptr, 0, &n, 0) < 0) return {};
        std::vector<uint8_t> b(n);
        if (BCryptExportKey(key_, nullptr, BCRYPT_ECCPRIVATE_BLOB, b.data(), n, &n, 0) < 0) return {};
        b.resize(n);
        return b;
    }

    bool Sign(const uint8_t digest[32], uint8_t sig[kSigLen]) const {
        ULONG n = 0;
        return key_ && BCryptSignHash(key_, nullptr, const_cast<PUCHAR>(digest), 32, sig, kSigLen, &n, 0) >= 0 && n == kSigLen;
    }

    bool valid() const { return key_ != nullptr; }
    const uint8_t* pub() const { return pub_; }
    DeviceId id() const { return IdFromPub(pub_); }

    // Builds the Identify payload for a session: pub | signature(transcript) | name.
    bool BuildIdentify(const Session& s, const std::string& name, uint8_t out[kIdentifyLen]) const {
        uint8_t digest[32];
        memset(out, 0, kIdentifyLen);
        if (!valid() || !IdentifyDigest(s.transcriptHash(), digest)) return false;
        memcpy(out, pub_, kPubKeyLen);
        if (!Sign(digest, out + kPubKeyLen)) return false;
        memcpy(out + kPubKeyLen + kSigLen, name.data(), std::min(name.size(), kDeviceNameLen - 1));
        return true;
    }

private:
    bool CachePub() {
        uint8_t blob[8 + kPubKeyLen];
        ULONG n = 0;
        if (BCryptExportKey(key_, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof(blob), &n, 0) < 0 || n != sizeof(blob)) return false;
        memcpy(pub_, blob + 8, kPubKeyLen);
        return true;
    }
    void Reset() {
        if (key_) BCryptDestroyKey(key_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
        key_ = nullptr;
        alg_ = nullptr;
    }
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_KEY_HANDLE key_ = nullptr;
    uint8_t pub_[kPubKeyLen] = {};
};

// Host side: checks an Identify packet's payload against the session it arrived in.
inline bool ParseIdentify(const uint8_t* payload, size_t len, const Session& s, DeviceId& id, std::string& name) {
    if (len < kIdentifyLen) return false;
    uint8_t digest[32];
    if (!IdentifyDigest(s.transcriptHash(), digest)) return false;
    if (!VerifyDeviceSignature(payload, digest, payload + kPubKeyLen)) return false;
    id = IdFromPub(payload);
    name.clear();
    for (size_t i = 0; i < kDeviceNameLen - 1 && payload[kPubKeyLen + kSigLen + i]; ++i) {
        const uint8_t c = payload[kPubKeyLen + kSigLen + i];
        if (c >= 0x20 && c != 0x7F) name += char(c);  // printable only: this text is shown in a prompt
    }
    return true;
}

}  // namespace rd
