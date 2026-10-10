# End-to-end test of the "runs hidden in the background" mode (tray app).
#   powershell -ExecutionPolicy Bypass -File tests\background_e2e.ps1
param([string]$Bin = "$PSScriptRoot\..\build\Release")
$Bin = (Resolve-Path $Bin).Path
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text; using System.Collections.Generic;
public class B {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, int w, StringBuilder l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr a, string c, string t);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  // all top-level windows of a process, visible or not
  public static List<IntPtr> Windows(uint pid, bool visibleOnly) {
    var l = new List<IntPtr>();
    EnumWindows((h, p) => { uint x; GetWindowThreadProcessId(h, out x); if (x == pid && (!visibleOnly || IsWindowVisible(h))) l.Add(h); return true; }, IntPtr.Zero);
    return l;
  }
  public static string Class(IntPtr h) { var s = new StringBuilder(128); GetClassName(h, s, 128); return s.ToString(); }
}
'@
$fails = 0
function Check($ok, $what) { if ($ok) { "ok   $what" } else { "FAIL $what"; $script:fails++ } }
function Text($h) { $sb = New-Object System.Text.StringBuilder 512; [B]::SendMessage($h, 0x000D, 512, $sb) | Out-Null; $sb.ToString() }
function AppWindow($procId) { foreach ($w in [B]::Windows([uint32]$procId, $false)) { if ([B]::Class($w) -eq "RemoteDisplayApp") { return $w } } return [IntPtr]::Zero }
function Visible($procId) { @([B]::Windows([uint32]$procId, $true) | Where-Object { [B]::Class($_) -eq "RemoteDisplayApp" }).Count -gt 0 }
function Wait($s) { Start-Sleep -Seconds $s }
function Pids($name) { @(Get-Process $name -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }) }

Get-Process RemoteDisplay, rd-client, rd-host -ErrorAction SilentlyContinue | Stop-Process -Force
Wait 1
$exe = "$Bin\RemoteDisplay.exe"
$IDC_KEY = 102; $IDC_AUTOSTART = 125; $IDM_EXIT = 2004
$runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
$startupBefore = (Get-ItemProperty $runKey -Name RemoteDisplay -ErrorAction SilentlyContinue).RemoteDisplay

# a normal GUI instance, only to read the saved pairing key
$gui = Start-Process $exe -ArgumentList "--allow-multiple" -PassThru
Wait 3
$key = Text ([B]::GetDlgItem((AppWindow $gui.Id), $IDC_KEY))
"pairing key: $key"
Check (Visible $gui.Id) "a normal launch shows its window"

# ---- startup entry (per-user, no admin): checkbox writes and removes it
$cb = [B]::GetDlgItem((AppWindow $gui.Id), $IDC_AUTOSTART)
[B]::SendMessage($cb, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null      # BM_CLICK
Wait 1
$v = (Get-ItemProperty $runKey -Name RemoteDisplay -ErrorAction SilentlyContinue).RemoteDisplay
"startup value: $v"
$toggledOn = [bool]$v
Check ($toggledOn -ne [bool]$startupBefore) "checkbox toggles the per-user startup entry"
if ($v) { Check ($v -like '*RemoteDisplay.exe" --background') "startup entry launches RemoteDisplay.exe --background" }
[B]::SendMessage($cb, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null      # click again: back to how it was
Wait 1
$v2 = (Get-ItemProperty $runKey -Name RemoteDisplay -ErrorAction SilentlyContinue).RemoteDisplay
Check ([bool]$v2 -eq [bool]$startupBefore) "toggling again restores the original startup state"
# make doubly sure we leave the registry exactly as found
if ($startupBefore) { Set-ItemProperty $runKey -Name RemoteDisplay -Value $startupBefore } else { Remove-ItemProperty $runKey -Name RemoteDisplay -ErrorAction SilentlyContinue }
Stop-Process -Id $gui.Id -Force
Wait 1

# ---- hidden background mode
$bg = Start-Process $exe -ArgumentList "--background" -PassThru
Wait 4
Check (-not $bg.HasExited) "background instance is running"
Check (-not (Visible $bg.Id)) "background instance shows NO window on screen"
$listening = (netstat -ano -p UDP | Select-String ":47800\s").ToString() -match "\s$($bg.Id)\s*$"
Check $listening "background instance is listening on UDP 47800 (sharing started by itself)"

# connect a real viewer; the approval prompt must still appear even though the app is hidden
$cli = Start-Process "$Bin\rd-client.exe" -ArgumentList "--key", $key, "--host", "127.0.0.1" -PassThru
Wait 3
$dlg = [B]::FindWindow("#32770", "RemoteDisplay - connection request")
Check ($dlg -ne [IntPtr]::Zero) "approval prompt appears with the main window hidden"
if ($dlg -ne [IntPtr]::Zero) {
  [B]::PostMessage($dlg, 0x0466, [IntPtr]1002, [IntPtr]::Zero) | Out-Null   # TDM_CLICK_BUTTON: "Allow once"
}
Wait 4
$viewer = [B]::Windows([uint32]$cli.Id, $true) | Select -First 1
$title = Text $viewer
"viewer: $title"
Check ($title -match "\d+ fps") "viewer receives video from the hidden background instance"
Check (-not (Visible $bg.Id)) "still no window while someone is connected"
Stop-Process -Id $cli.Id -Force

# ---- tray behaviour (simulated tray clicks / close / exit)
$hw = AppWindow $bg.Id
[B]::PostMessage($hw, 0x8014, [IntPtr]0, [IntPtr]0x0202) | Out-Null           # kMsgTray, WM_LBUTTONUP
Wait 1
Check (Visible $bg.Id) "left-click on the tray icon opens the window"
[B]::PostMessage($hw, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null                # WM_CLOSE
Wait 1
Check ((-not $bg.HasExited) -and (-not (Visible $bg.Id))) "closing the window while sharing hides it to the tray and keeps running"

# ---- single instance: a second normal launch brings the first one forward and exits
$second = Start-Process $exe -PassThru
Wait 2
Check $second.HasExited "a second launch exits instead of starting a duplicate"
Check (Visible $bg.Id) "the second launch brings the running instance's window forward"

# ---- Exit from the tray menu really ends it and frees the port
[B]::PostMessage($hw, 0x0111, [IntPtr]$IDM_EXIT, [IntPtr]::Zero) | Out-Null    # WM_COMMAND IDM_EXIT
Wait 2
Check $bg.HasExited "Exit from the tray menu ends the process"
Check (-not ((netstat -ano -p UDP | Select-String ":47800\s") -ne $null)) "UDP 47800 is free again after Exit"

Get-Process RemoteDisplay, rd-client -ErrorAction SilentlyContinue | Stop-Process -Force
if ($fails -eq 0) { "ALL BACKGROUND-MODE CHECKS PASSED" } else { "$fails CHECK(S) FAILED" }
exit $fails
