# End-to-end test of "Show tray icon" off: no window AND no tray icon, still shares, still safe.
#   powershell -ExecutionPolicy Bypass -File tests\tray_hidden_e2e.ps1
param([string]$Bin = "$PSScriptRoot\..\build\Release")
$Bin = (Resolve-Path $Bin).Path
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text; using System.Collections.Generic;
public class T {
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
  [DllImport("shell32.dll")] public static extern int Shell_NotifyIconGetRect(ref NID id, out RECT r);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern bool WritePrivateProfileString(string sec, string key, string val, string file);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [StructLayout(LayoutKind.Sequential)] public struct NID { public uint cbSize; public IntPtr hWnd; public uint uID; public Guid guid; }
  public static List<IntPtr> Windows(uint pid, bool visibleOnly) {
    var l = new List<IntPtr>();
    EnumWindows((h, p) => { uint x; GetWindowThreadProcessId(h, out x); if (x == pid && (!visibleOnly || IsWindowVisible(h))) l.Add(h); return true; }, IntPtr.Zero);
    return l;
  }
  public static string Class(IntPtr h) { var s = new StringBuilder(128); GetClassName(h, s, 128); return s.ToString(); }
  // 0 (S_OK) if the shell currently shows a tray icon (id 1) owned by this window
  public static int TrayIconStatus(IntPtr hwnd) {
    NID id = new NID(); id.cbSize = (uint)Marshal.SizeOf(typeof(NID)); id.hWnd = hwnd; id.uID = 1; RECT r;
    return Shell_NotifyIconGetRect(ref id, out r);
  }
}
'@
$fails = 0
function Check($ok, $what) { if ($ok) { "ok   $what" } else { "FAIL $what"; $script:fails++ } }
function Text($h) { $sb = New-Object System.Text.StringBuilder 512; [T]::SendMessage($h, 0x000D, 512, $sb) | Out-Null; $sb.ToString() }
function AppWindow($procId) { foreach ($w in [T]::Windows([uint32]$procId, $false)) { if ([T]::Class($w) -eq "RemoteDisplayApp") { return $w } } return [IntPtr]::Zero }
function Visible($procId) { @([T]::Windows([uint32]$procId, $true) | Where-Object { [T]::Class($_) -eq "RemoteDisplayApp" }).Count -gt 0 }
function Wait($s) { Start-Sleep -Seconds $s }
function ClickDialog($title, $button) {
  for ($i = 0; $i -lt 20; $i++) {
    $d = [T]::FindWindow("#32770", $title)
    if ($d -ne [IntPtr]::Zero) { [T]::SendMessage([T]::FindWindowEx($d, [IntPtr]::Zero, "Button", $button), 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null; return $true }
    Start-Sleep -Milliseconds 250
  }
  return $false
}
function Port47800Owner { $l = netstat -ano -p UDP | Select-String ":47800\s"; if ($l) { ($l.ToString() -split "\s+")[-1] } else { $null } }

$exe = "$Bin\RemoteDisplay.exe"
$cfg = Join-Path $env:APPDATA "RemoteDisplay\config.ini"
$IDC_KEY = 102; $IDC_SHOWTRAY = 126
Get-Process RemoteDisplay, rd-client -ErrorAction SilentlyContinue | Stop-Process -Force
Wait 1
try {
  # 0) baseline: with the default setting the shell DOES have a tray icon (proves the detector works)
  $bg0 = Start-Process $exe -ArgumentList "--background" -PassThru
  Wait 4
  $st = [T]::TrayIconStatus((AppWindow $bg0.Id))
  Check ($st -eq 0) "default: the shell has a tray icon for the app (detector sanity check, status $st)"
  Stop-Process -Id $bg0.Id -Force
  Wait 1

  # 1) untick "Show tray icon" in the real UI: a confirmation explains what happens
  $gui = Start-Process $exe -ArgumentList "--allow-multiple" -PassThru
  Wait 3
  $gw = AppWindow $gui.Id
  $key = Text ([T]::GetDlgItem($gw, $IDC_KEY))
  [T]::PostMessage([T]::GetDlgItem($gw, $IDC_SHOWTRAY), 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # BM_CLICK (async: a modal box opens)
  Check (ClickDialog "RemoteDisplay" "&No") "unticking asks for confirmation; answering No keeps the icon"
  Wait 1
  [T]::PostMessage([T]::GetDlgItem($gw, $IDC_SHOWTRAY), 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  Check (ClickDialog "RemoteDisplay" "&Yes") "answering Yes hides the icon"
  Wait 1
  Stop-Process -Id $gui.Id -Force
  Wait 1
  Check ((Get-Content $cfg -Raw) -match "(?m)^tray=0") "the choice is saved"

  # 2) background start with the icon hidden
  $bg = Start-Process $exe -ArgumentList "--background" -PassThru
  Wait 4
  $hw = AppWindow $bg.Id
  Check (-not $bg.HasExited) "background instance runs"
  Check (-not (Visible $bg.Id)) "no window on screen"
  $st = [T]::TrayIconStatus($hw)
  Check ($st -ne 0) "NO tray icon in the shell (status $st)"
  Check ((Port47800Owner) -eq "$($bg.Id)") "still sharing: it listens on UDP 47800"

  $cli = Start-Process "$Bin\rd-client.exe" -ArgumentList "--key", $key, "--host", "127.0.0.1" -PassThru
  Check (ClickDialog "RemoteDisplay - connection request" "&Yes") "connection approval prompt still appears (nothing else is visible)"
  Wait 4
  $vw = [T]::Windows([uint32]$cli.Id, $true) | Select -First 1
  Check ((Text $vw) -match "\d+ fps") "viewer gets video from the fully hidden instance"
  Stop-Process -Id $cli.Id -Force

  # 3) getting back in: launching the app again brings the window forward
  $second = Start-Process $exe -PassThru
  Wait 2
  Check $second.HasExited "relaunching does not start a duplicate"
  Check (Visible $bg.Id) "relaunching brings the hidden app's window forward"

  # 4) closing the window while sharing with no icon must ask, never silently keep running
  [T]::PostMessage($hw, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null   # WM_CLOSE
  Check (ClickDialog "RemoteDisplay" "Cancel") "closing asks what to do (Cancel = stay)"
  Wait 1
  Check ((Visible $bg.Id) -and (-not $bg.HasExited)) "Cancel keeps the window open"
  [T]::PostMessage($hw, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
  Check (ClickDialog "RemoteDisplay" "&Yes") "closing again, choosing Yes (keep running)"
  Wait 1
  Check ((-not (Visible $bg.Id)) -and (-not $bg.HasExited) -and ((Port47800Owner) -eq "$($bg.Id)")) "Yes: window hidden, still sharing, still no icon"
  $second = Start-Process $exe -PassThru
  Wait 2
  Check (Visible $bg.Id) "relaunch shows it again"
  [T]::PostMessage($hw, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
  Check (ClickDialog "RemoteDisplay" "&No") "closing once more, choosing No (stop and exit)"
  Wait 2
  Check $bg.HasExited "No: the process exits"
  Check ($null -eq (Port47800Owner)) "UDP 47800 is free again"
}
finally {
  # restore the user's setting: the key did not exist before, so remove it (= icon shown)
  Get-Process RemoteDisplay, rd-client -ErrorAction SilentlyContinue | Stop-Process -Force
  [T]::WritePrivateProfileString("app", "tray", $null, $cfg) | Out-Null
  "setting restored: tray icon shown = " + (-not ((Get-Content $cfg -Raw) -match "(?m)^tray=0"))
}
if ($fails -eq 0) { "ALL HIDDEN-TRAY CHECKS PASSED" } else { "$fails CHECK(S) FAILED" }
exit $fails
