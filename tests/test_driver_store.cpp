// Tests for the driver-store check that stops a repeated install from adding the same driver again.
// Pure parsing only: no driver is installed or removed.
#include <cstdio>
#include <fstream>
#include <string>

#include "virtual_monitor.h"

static int fails = 0;
#define CHECK(cond, what)                                                  \
    do {                                                                   \
        if (cond) printf("  ok   %s\n", what);                             \
        else { printf("  FAIL %s (line %d)\n", what, __LINE__); ++fails; } \
    } while (0)

// The shape of `pnputil /enum-drivers` output (one block per package).
static const char* kListing =
    "Microsoft PnP Utility\n"
    "\n"
    "Published Name:     oem32.inf\n"
    "Original Name:      sampledisplay.inf\n"
    "Provider Name:      TODO-Set-Provider\n"
    "Class Name:         Display\n"
    "Class GUID:         {4d36e968-e325-11ce-bfc1-08002be10318}\n"
    "Driver Version:     03/15/2011 0.3.15.11\n"
    "Signer Name:        RemoteDisplay Test\n"
    "\n"
    "Published Name:     oem40.inf\n"
    "Original Name:      RemoteDisplayVirtual.inf\n"
    "Provider Name:      RemoteDisplay\n"
    "Class Name:         Display\n"
    "Class GUID:         {4d36e968-e325-11ce-bfc1-08002be10318}\n"
    "Driver Version:     03/15/2026 1.0.0.0\n"
    "Signer Name:        RemoteDisplay Test\n"
    "\n"
    "Published Name:     oem14.inf\n"
    "Original Name:      u0387389.inf\n"
    "Provider Name:      Advanced Micro Devices, Inc.\n"
    "Driver Version:     10/25/2022 31.0.12044.3\n";

int main() {
    using namespace rdapp;

    printf("version spelling\n");
    CHECK(NormalizeVersion("0.03.15.0011") == "0.3.15.11", "leading zeros are dropped");
    CHECK(NormalizeVersion("0.3.15.11") == NormalizeVersion("0.03.15.0011"), "INF and pnputil spellings compare equal");
    CHECK(NormalizeVersion("10.0.26100.1") == "10.0.26100.1", "a plain version is unchanged");
    CHECK(NormalizeVersion("1.0.0") != NormalizeVersion("1.0.0.0"), "a shorter version is a different version");

    printf("driver store listing\n");
    CHECK(FindDriverInListing(kListing, "remotedisplayvirtual.inf", "1.0.0.0"),
          "same name and version is found (name match ignores case)");
    CHECK(!FindDriverInListing(kListing, "remotedisplayvirtual.inf", "1.0.0.1"),
          "a different version is not found, so the newer driver is added");
    CHECK(!FindDriverInListing(kListing, "remotedisplayvirtual.inf", "1.0.0"),
          "a different spelling of a different version is not found");
    CHECK(FindDriverInListing(kListing, "sampledisplay.inf", "0.03.15.0011"),
          "the same version written differently is found");
    CHECK(!FindDriverInListing(kListing, "oem40.inf", "1.0.0.0"),
          "a published name (oemNN.inf) is not an original name");
    CHECK(!FindDriverInListing(kListing, "notthere.inf", "1.0.0.0"), "a package not in the store is not found");
    CHECK(!FindDriverInListing("", "remotedisplayvirtual.inf", "1.0.0.0"), "an empty listing finds nothing");

    printf("INF version\n");
    {
        wchar_t dir[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, dir);
        const std::wstring path = std::wstring(dir) + L"rd-test-driverver-" + std::to_wstring(GetTickCount()) + L".inf";
        {
            std::ofstream f(path);
            f << "[Version]\r\nSignature=\"$Windows NT$\"\r\nDriverVer = 03/15/2026,1.0.0.0\r\n";
        }
        CHECK(InfDriverVersion(path) == "1.0.0.0", "the DriverVer field is read (spaces around = are allowed)");
        DeleteFileW(path.c_str());
        CHECK(InfDriverVersion(path).empty(), "a missing file gives no version");
    }

    printf(fails ? "%d FAILED\n" : "all driver store tests passed\n", fails);
    return fails ? 1 : 0;
}
