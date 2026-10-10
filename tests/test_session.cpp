#include <cstdio>
#include <vector>

#include "session.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main() {
    uint8_t psk[rd::kKeyLen], wrong[rd::kKeyLen];
    rd::crypto::Random(psk, sizeof(psk));
    memcpy(wrong, psk, sizeof(psk));
    wrong[3] ^= 1;

    // Key text round-trip.
    {
        std::string t = rd::FormatKey(psk);
        uint8_t back[rd::kKeyLen];
        CHECK(rd::ParseKey(t, back) && memcmp(back, psk, sizeof(psk)) == 0);
        CHECK(!rd::ParseKey("not a key!", back));
    }

    // Good handshake + data both directions.
    rd::ClientHandshake ch;
    uint8_t hs1[rd::kHs1Len], hs2[rd::kHs2Len];
    CHECK(ch.Begin(hs1));
    rd::Session host, client;
    CHECK(rd::HostRespond(psk, hs1, hs2, host));
    CHECK(ch.Finish(psk, hs2, client));

    const uint8_t msg[] = "hello screen";
    uint8_t wire[256], plain[256];
    size_t pl = 0;
    size_t wn = host.Seal(msg, sizeof(msg), wire);
    CHECK(wn == sizeof(rd::WireHeader) + sizeof(msg) + rd::kTagLen);
    CHECK(client.Open(wire, wn, plain, pl) && pl == sizeof(msg) && memcmp(plain, msg, pl) == 0);
    CHECK(!client.Open(wire, wn, plain, pl));  // replay rejected

    uint8_t w2[256];
    size_t n2 = client.Seal(msg, sizeof(msg), w2);
    CHECK(host.Open(w2, n2, plain, pl) && memcmp(plain, msg, pl) == 0);

    // Tampering (ciphertext, header) rejected.
    wn = host.Seal(msg, sizeof(msg), wire);
    wire[sizeof(rd::WireHeader) + 2] ^= 0x40;
    CHECK(!client.Open(wire, wn, plain, pl));
    wn = host.Seal(msg, sizeof(msg), wire);
    wire[8] ^= 1;  // seq (authenticated via nonce)
    CHECK(!client.Open(wire, wn, plain, pl));

    // Direction separation: a packet sealed by the host is not valid for the host.
    wn = host.Seal(msg, sizeof(msg), wire);
    CHECK(!host.Open(wire, wn, plain, pl));

    // Out-of-order within window is accepted once.
    uint8_t a[256], b[256];
    size_t na = host.Seal(msg, sizeof(msg), a), nb = host.Seal(msg, sizeof(msg), b);
    CHECK(client.Open(b, nb, plain, pl));
    CHECK(client.Open(a, na, plain, pl));
    CHECK(!client.Open(a, na, plain, pl));

    // Wrong pairing key: client must reject the host's Hs2.
    {
        rd::ClientHandshake c2;
        uint8_t h1[rd::kHs1Len], h2[rd::kHs2Len];
        rd::Session hs, cs;
        CHECK(c2.Begin(h1));
        CHECK(rd::HostRespond(wrong, h1, h2, hs));
        CHECK(!c2.Finish(psk, h2, cs));
    }
    // Tampered Hs2 (swapped host public key) rejected.
    {
        rd::ClientHandshake c2;
        uint8_t h1[rd::kHs1Len], h2[rd::kHs2Len];
        rd::Session hs, cs;
        c2.Begin(h1);
        rd::HostRespond(psk, h1, h2, hs);
        h2[rd::kNonceLen + 5] ^= 1;
        CHECK(!c2.Finish(psk, h2, cs));
    }
    // Sessions from different handshakes can't read each other.
    {
        rd::ClientHandshake c2;
        uint8_t h1[rd::kHs1Len], h2[rd::kHs2Len];
        rd::Session hs2s, cs2;
        c2.Begin(h1);
        rd::HostRespond(psk, h1, h2, hs2s);
        c2.Finish(psk, h2, cs2);
        wn = host.Seal(msg, sizeof(msg), wire);
        CHECK(!cs2.Open(wire, wn, plain, pl));
    }

    printf(g_fail ? "%d FAILED\n" : "all session tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
