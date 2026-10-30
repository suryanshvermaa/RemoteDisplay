# Prebuilt virtual monitor driver package

A copy of the package built by `driver\build.ps1 -Package` (`driver\out\pkg`), kept here so the CI release build
can put it in the installer: CI has no WDK and no signing certificate, so it cannot build the driver itself.

**It is signed with the project's TEST certificate** (`CN=RemoteDisplay Test`). Windows only loads it on a PC
that is in test-signing mode and trusts that certificate, so for ordinary users the driver step fails (it is
logged and does not stop the install). Public use needs Microsoft attestation signing.

To update it after changing the driver:

```bash
powershell -ExecutionPolicy Bypass -File driver\build.ps1 -Package
```

```bash
copy driver\out\pkg\* driver\prebuilt\
```

`installer\Build-Installer.ps1` uses `driver\out\pkg` when it exists and this folder otherwise.
