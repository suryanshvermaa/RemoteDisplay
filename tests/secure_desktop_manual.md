# Secure desktop: manual test (needs an administrator and a real lock)

The UAC prompt and the sign-in / lock screen are shown and **controlled** from the viewer, like other remote
desktop tools. This is on by default. The automated tests cover the shared picture, the helper's capture, the
input queue and its access rules (`test-secure`, `secure_input_gate_e2e.ps1`). They cannot enter the UAC
prompt, the sign-in screen or the lock screen, so run these steps yourself, once.

How it works: a SYSTEM helper sits on the secure desktop. It captures the picture and, because the host
passes it the viewer's mouse and keyboard through a queue, it clicks and types there. The queue accepts input
only from SYSTEM and Administrators, so a program running with a normal user's rights cannot click "Yes" on a
UAC prompt. In practice: **the host must run as administrator** (Run as administrator, or the service's admin
mode). Without that, the secure screens are shown but are view-only.

You need: a second PC (or the Android app) to view from. Using the same PC as the viewer also works, but the
viewer window is on the desktop that gets locked, so a second device is the clearer test.

## Setup (one time, administrator)

1. Open RemoteDisplay. In **Run in background**, click **Install** (the service) and accept UAC.
2. Tick **Run as administrator** and accept UAC. Leave **Control UAC and sign-in screens** ticked (default).
3. Note the address and pairing key. On the other device, connect and accept the prompt.

## Test A: lock screen

4. On the sharing PC press **Win + L**.
5. Expected within about 2 seconds: the viewer shows the lock screen.
6. Type the Windows password or PIN in the viewer, then press Enter. Expected: the PC unlocks.
7. Expected: the viewer returns to the normal desktop.

## Test B: UAC prompt

8. On the sharing PC, open a program that asks for administrator rights (for example, *Run as
   administrator* on Task Manager).
9. Expected: the viewer shows the UAC prompt. Click **Yes** in the viewer: the program starts.
10. Repeat and click **No** in the viewer: the prompt closes, and the viewer returns to the normal desktop.

## Test C: switched off

11. Untick **Control UAC and sign-in screens** and accept UAC.
12. Lock the PC again (**Win + L**). Expected: the viewer does **not** show the lock screen; it stays on the
    last picture of the normal desktop.

## Test D: not running as administrator

13. Untick **Run as administrator**, restart the app or service, and repeat Test B.
14. Expected: the UAC prompt is shown, but clicks and keys from the viewer do nothing there (view-only).
    `secure-helper.log` says the input was not delivered because the host is not running as administrator.

## Logs

Three files in `%ProgramData%\RemoteDisplay\`:

- `audit.log`: `secure-desktop-entered` and `secure-desktop-left` for each lock or UAC prompt,
  `secure-desktop-input` once per prompt when the viewer first sends input there, and `secure-desktop-policy`
  when the switch is changed.
- `secure-helper.log`: `input desktop is now 'Winlogon'` when the lock screen or UAC prompt appears, then
  `first picture from 'Winlogon': WxH`, then `input is being applied on 'Winlogon'` when you type or click,
  then `input desktop is now 'Default'` afterwards. Any `OpenInputDesktop failed` or `SetThreadDesktop ...
  failed` line means the helper could not follow the secure desktop; the error number is the important part.
- `service.log`: `secure picture: N new frames in 10 s` while the secure screen is up, and
  `screen duplication unavailable` / `screen duplication restored` around it.

Please send back: which of A, B, C and D passed, and the last 40 lines of all three logs if anything did not.
Include the time you did each test, so the lines can be matched up.
