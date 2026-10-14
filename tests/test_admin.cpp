// Tests for the administrator-mode plumbing that can run WITHOUT elevation.
// (Anything that really needs UAC - installing the service, creating the elevated task - cannot be
// exercised here; this checks the parts it is built from.)
#include <cstdio>
#include <string>

#include "admin.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); ++g_fail; } else { printf("ok   %s\n", #c); } } while (0)

static std::string Capture(const char* cmd) {
    std::string out;
    if (FILE* p = _popen(cmd, "r")) {
        char buf[512];
        while (size_t n = fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
        _pclose(p);
    }
    return out;
}

static std::wstring Sddl(HANDLE h) {
    DWORD need = 0;
    const SECURITY_INFORMATION si = DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION;
    GetKernelObjectSecurity(h, si, nullptr, 0, &need);
    std::vector<uint8_t> sd(need);
    if (!GetKernelObjectSecurity(h, si, sd.data(), need, &need)) return L"";
    LPWSTR s = nullptr;
    if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(sd.data(), SDDL_REVISION_1, si, &s, nullptr)) return L"";
    std::wstring r = s;
    LocalFree(s);
    return r;
}

int main() {
    using namespace rdapp;
    printf("running elevated: %s\n", IsElevated() ? "yes" : "no");

    // ---- shared objects get an explicit ACL so a normal window can use an elevated copy's objects
    SECURITY_ATTRIBUTES* sa = IpcSecurity();
    CHECK(sa != nullptr, "IpcSecurity builds");
    if (sa) {
        HANDLE ev = CreateEventW(sa, FALSE, FALSE, L"Local\\RemoteDisplayTestAcl");
        CHECK(ev != nullptr, "an event can be created with it");
        const std::wstring d = Sddl(ev);
        wprintf(L"     %ls\n", d.c_str());
        CHECK(d.find(L"IU") != std::wstring::npos, "interactive user (the same person's normal-rights process) is allowed");
        CHECK(d.find(L"BA") != std::wstring::npos && d.find(L"SY") != std::wstring::npos, "Administrators and SYSTEM are allowed");
        CHECK(d.find(L"ML;;NW;;;ME") != std::wstring::npos, "Medium integrity label, so Medium processes may write");
        // and it is usable: a second handle opens it by name
        HANDLE again = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\RemoteDisplayTestAcl");
        CHECK(again != nullptr, "re-opening the named object works");
        if (again) CloseHandle(again);
        if (ev) CloseHandle(ev);
    }

    // ---- the machine-wide switch cannot be flipped without admin rights
    // The flag may already be on: the app's own "Run as administrator" setting writes it. The test does not
    // change it; it checks that normal rights cannot change it.
    if (!IsElevated()) {
        const bool before = GetElevateWorker();
        CHECK(!SetElevateWorker(true), "normal rights cannot set the machine-wide elevation flag");
        CHECK(GetElevateWorker() == before, "and the flag is left as it was");
    } else {
        printf("SKIP flag test: running elevated, not touching HKLM\n");
    }

    // ---- scheduled-task XML
    {
        const std::wstring xml = BuildTaskXml(L"A&B\\user", L"C:\\Program Files\\x y\\a<b>.exe");
        CHECK(xml.find(L"A&amp;B\\user") != std::wstring::npos, "user name is XML-escaped");
        CHECK(xml.find(L"a&lt;b&gt;.exe") != std::wstring::npos, "program path is XML-escaped");
        CHECK(xml.find(L"<RunLevel>HighestAvailable</RunLevel>") != std::wstring::npos, "runs with highest privileges");
        CHECK(xml.find(L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>") != std::wstring::npos, "never stopped after 72 hours");
        CHECK(xml.find(L"<DisallowStartIfOnBatteries>false") != std::wstring::npos &&
              xml.find(L"<StopIfGoingOnBatteries>false") != std::wstring::npos, "keeps running on battery");
        CHECK(xml.find(L"<Arguments>--background</Arguments>") != std::wstring::npos, "starts hidden in the background");
    }

    // ---- Windows itself accepts and stores that XML (throwaway task, normal rights, removed right away)
    {
        const wchar_t* name = L"RemoteDisplayXmlTest";
        DeleteStartupTask(name);
        const std::wstring user = CurrentUserName();
        CHECK(!user.empty(), "current user name is known: %ls", user.c_str());
        if (CreateStartupTask(user, SelfPath(), L"LeastPrivilege", name)) {
            CHECK(TaskExists(name), "Windows accepted the task definition");
            const std::string stored = Capture("schtasks.exe /Query /TN RemoteDisplayXmlTest /XML");
            CHECK(stored.find("PT0S") != std::string::npos, "stored task has no execution time limit");
            CHECK(stored.find("<DisallowStartIfOnBatteries>false") != std::string::npos, "stored task ignores battery state");
            CHECK(stored.find("--background") != std::string::npos, "stored task launches with --background");
            CHECK(DeleteStartupTask(name) && !TaskExists(name), "task deleted again");
        } else {
            printf("SKIP importing the task: Windows needs administrator rights even for a logon-triggered task here\n");
        }
        CHECK(!TaskExists(), "the real 'RemoteDisplay' task was not touched");
    }

    // ---- secure desktop: detection, opt-in guard, audit trail ----
    {
        const std::wstring desk = InputDesktopName();
        wprintf(L"     input desktop: %ls\n", desk.c_str());
        CHECK(!desk.empty(), "the input desktop name can be read");
        if (!IsElevated() && !SecureDesktopAllowed()) {
            CHECK(!SetSecureDesktopAllowed(true), "normal rights cannot turn secure-desktop consent on");
            CHECK(!SecureDesktopAllowed(), "and consent is still off");
        }
        const std::wstring logPath = AuditLogPath();
        DeleteFileW(logPath.c_str());
        Audit("test-event", "marker=unit-test");
        std::string text;
        HANDLE f = CreateFileW(logPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            char buf[2048];
            DWORD n = 0;
            while (ReadFile(f, buf, sizeof(buf), &n, nullptr) && n) text.append(buf, n);
            CloseHandle(f);
        }
        CHECK(text.find("AUDIT test-event") != std::string::npos && text.find("marker=unit-test") != std::string::npos,
              "an audit line is written with the event and detail");
        CHECK(text.find("user=") != std::string::npos && text.find("pid=") != std::string::npos,
              "audit lines record the user and process");
        DeleteFileW(logPath.c_str());
    }

    printf(g_fail ? "%d FAILED\n" : "all admin-plumbing tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
