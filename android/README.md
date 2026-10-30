# RemoteDisplay for Android (viewer)

View and control a Windows PC that is sharing with RemoteDisplay, from an Android phone or tablet
(Android 8.0 or later). It uses the same encrypted protocol, pairing key and LAN discovery as the Windows apps.

## Using it

1. On the PC, open RemoteDisplay and start sharing.
2. On the phone, open RemoteDisplay. Pick the PC under **PCs on this network**, or type its address.
3. Enter the pairing key shown on the PC, then tap **Connect**.

Touch works as the mouse:

| Gesture | Action |
|---|---|
| Tap | Click |
| Long press | Right click |
| Drag | Hold the left button and move (select, drag windows) |
| Two-finger drag | Scroll |

The toolbar at the bottom opens the keyboard and sends Esc, Tab and Win. The app follows the phone's light or
dark mode.

## Code

- `app/src/main/java/.../core/`: the protocol in plain Kotlin, with no Android dependencies, so it is tested on
  a PC.
  - Handshake and AES-GCM session.
  - Device identity.
  - Connection loop.
  - LAN discovery.
  - Keyboard scancodes.
- `MainActivity`: the connect screen.
- `ViewerActivity`: the full-screen viewer, with touch input and the keyboard.
- `VideoDecoder`: hardware H.264 decoding with MediaCodec.

Two details differ from Windows CNG and are converted in `Crypto.kt`:
- the ECDH shared secret is little-endian on Windows;
- ECDSA signatures are raw r|s on Windows and DER in Java.

## Build and test (Windows)

The toolchain used to build it is in `%LOCALAPPDATA%\RemoteDisplayAndroid`: JDK 17, the Android SDK and
Gradle 8.9.

```bash
set JAVA_HOME=%LOCALAPPDATA%\RemoteDisplayAndroid\jdk\jdk-17.0.20.1+1
```

```bash
gradlew.bat assembleDebug testDebugUnitTest
```

The APK is written to `app/build/outputs/apk/debug/app-debug.apk`.

To test against a running Windows host (for example `rd-host --port 47820 --key <key>`):

```bash
gradlew.bat testDebugUnitTest --tests "*HostInteropTest*" -Drd.host=127.0.0.1:47820 -Drd.key=<key>
```

You can also open this folder in Android Studio.

## Before publishing

The APK is signed with the debug key, which is fine for sideloading and testing. For a public release, create a
release keystore and build a signed release APK or AAB.
