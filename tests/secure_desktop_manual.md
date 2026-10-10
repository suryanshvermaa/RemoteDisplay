# Secure desktop: manual test (needs an administrator and a real lock)

The automated tests cover the shared picture, the real helper's capture on your desktop, the
viewer path, and the input gate (`test-secure`, `secure_input_gate_e2e.ps1`). They cannot enter the
UAC prompt, the sign-in screen or the lock screen, so run these steps yourself, once.

You need: a second PC or phone-free second instance to view from. Using the same PC as the viewer also
works, but the viewer window is on the desktop that gets locked, so the second PC is the clearer test.

## Setup (one time, administrator)

1. Open RemoteDisplay. In **Run in background**, click **Install service** and accept UAC.
2. Tick **Allow viewing UAC and sign-in screens (admin)** and accept UAC.
3. Click **Share your screen** and note the address and pairing key.
4. On the other PC, open RemoteDisplay and connect with that address and key. Accept the prompt.

## Test A: lock screen

5. On the sharing PC press **Win + L**.
6. Expected within about 2 seconds: the viewer shows the lock screen (not a frozen desktop).
7. Try typing a password into the viewer. Expected: nothing reaches the lock screen (view only).
8. Sign in on the sharing PC. Expected: the viewer returns to the normal desktop.

## Test B: UAC prompt

9. On the sharing PC, open a program that asks for administrator rights (for example, *Run as
   administrator* on Task Manager). The UAC prompt appears.
10. Expected: the viewer shows the UAC prompt. Click **Yes** on the viewer: nothing happens there.
11. Click **No** on the sharing PC's UAC prompt. Expected: the viewer returns to the normal desktop.

## Test C: consent off

12. Untick the consent checkbox and accept UAC.
13. Lock the PC again (**Win + L**). Expected: the viewer does **not** show the lock screen; it stays
    on the last picture of the normal desktop.

## Logs

- `%ProgramData%\RemoteDisplay\audit.log` should show `secure-desktop-entered` and
  `secure-desktop-left` for Tests A and B, and `secure-desktop-policy allowed=0` for Test C's change.
- `%ProgramData%\RemoteDisplay\secure-helper.log` shows when the helper starts and which desktop is
  the input desktop.

Please send back: which of A, B and C passed, and the last 20 lines of both logs if anything did not.
