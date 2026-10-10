# End-to-end test of trusted devices ("Always allow this device"), using two app instances:
# A shares, B is the viewer. Both belong to the same Windows user, so they share one device identity,
# which is exactly what we need to test remembering. Your config.ini is backed up and restored.
#   powershell -ExecutionPolicy Bypass -File tests\trust_e2e.ps1
param([string]$Bin = "$PSScriptRoot\..\build\Release")
$Bin = (Resolve-Path $Bin).Path
Add-Type -AssemblyName UIAutomationClient
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text; using System.Collections.Generic;
public class R {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, int w, StringBuilder l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr a, string c, string t);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  public static List<IntPtr> Windows(uint pid) {
    var l = new List<IntPtr>();
    EnumWindows((h, p) => { uint x; GetWindowThreadProcessId(h, out x); if (x == pid && IsWindowVisible(h)) l.Add(h); return true; }, IntPtr.Zero);
    return l;
  }
  public static string Class(IntPtr h) { var s = new StringBuilder(128); GetClassName(h, s, 128); return s.ToString(); }
  public static string Title(IntPtr h) { var s = new StringBuilder(256); GetWindowText(h, s, 256); return s.ToString(); }
}
'@
$fails = 0
Add-Type -AssemblyName System.Drawing
[R]::SetProcessDPIAware() | Out-Null
function Shot($h, $path) {
  $r = New-Object R+RECT; [R]::GetWindowRect($h, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
  $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc(); [R]::PrintWindow($h, $hdc, 2) | Out-Null; $g.ReleaseHdc($hdc)
  $bmp.Save($path)
}
function Check($ok, $what) { if ($ok) { "ok   $what" } else { "FAIL $what"; $script:fails++ } }
function Text($h) { $sb = New-Object System.Text.StringBuilder 512; [R]::SendMessage($h, 0x000D, 512, $sb) | Out-Null; $sb.ToString() }
function Wait($s) { Start-Sleep -Seconds $s }
function Win($procId, $cls) { foreach ($w in [R]::Windows([uint32]$procId)) { if ([R]::Class($w) -eq $cls) { return $w } } return [IntPtr]::Zero }
function Dialog { [R]::FindWindow("#32770", "RemoteDisplay - connection request") }
function WaitDialog($sec) { for ($i = 0; $i -lt ($sec * 4); $i++) { $d = Dialog; if ($d -ne [IntPtr]::Zero) { return $d }; Start-Sleep -Milliseconds 250 }; return [IntPtr]::Zero }
# Task Dialog buttons are DirectUI (not real windows): read their labels with UI Automation and press them
# with the dialog's own TDM_CLICK_BUTTON message (WM_USER+102) using the button ids set in the app.
$BTN = @{ "Allow once" = 1002; "Always allow this device" = 1003; "Deny" = 1001 }
function ButtonLabels($dlg) {
  $el = [System.Windows.Automation.AutomationElement]::FromHandle($dlg)
  $cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Button)
  @($el.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond) | ForEach-Object { $_.Current.Name })
}
function Press($dlg, $label) { [R]::PostMessage($dlg, 0x0466, [IntPtr]$BTN[$label], [IntPtr]::Zero) | Out-Null; return $true }
function Click($h) { [R]::PostMessage($h, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null }   # BM_CLICK, async
function Status($hw) { Text ([R]::GetDlgItem($hw, 107)) }
function TrustedCount { $t = Get-Content $cfg -Raw -ErrorAction SilentlyContinue; if ($t -match "(?m)^\[trusted\]") { if ($t -match "(?m)^count=(\d+)") { 0 }; $m = [regex]::Match($t, "(?ms)^\[trusted\]\s*(.*?)(^\[|\z)"); if ($m.Success -and $m.Groups[1].Value -match "(?m)^count=(\d+)") { [int]$Matches[1] } else { 0 } } else { 0 } }

$exe = "$Bin\RemoteDisplay.exe"
$cfg = Join-Path $env:APPDATA "RemoteDisplay\config.ini"
$backup = "$env:TEMP\rd-config-backup.ini"
$hadConfig = Test-Path $cfg
if ($hadConfig) { Copy-Item $cfg $backup -Force }
Get-Process RemoteDisplay, rd-client -ErrorAction SilentlyContinue | Stop-Process -Force
Wait 1
$IDC_SHARE = 106; $IDC_REMOTE = 109; $IDC_RKEY = 110; $IDC_CONNECT = 111; $IDC_KEY = 102; $IDC_TRUSTED = 127
try {
  $a = Start-Process $exe -ArgumentList "--allow-multiple" -PassThru
  $b = Start-Process $exe -ArgumentList "--allow-multiple" -PassThru
  Wait 3
  $ha = Win $a.Id "RemoteDisplayApp"; $hb = Win $b.Id "RemoteDisplayApp"
  $key = Text ([R]::GetDlgItem($ha, $IDC_KEY))
  [R]::SendMessage([R]::GetDlgItem($hb, $IDC_REMOTE), 0x000C, [IntPtr]::Zero, "127.0.0.1") | Out-Null
  [R]::SendMessage([R]::GetDlgItem($hb, $IDC_RKEY), 0x000C, [IntPtr]::Zero, $key) | Out-Null
  Click ([R]::GetDlgItem($ha, $IDC_SHARE)); Wait 2
  $before = TrustedCount
  Check ($before -eq 0) "starting with no trusted devices"

  function ViewerOf($proc) { foreach ($w in [R]::Windows([uint32]$proc.Id)) { if ([R]::Class($w) -eq "rdviewer") { return $w } } return [IntPtr]::Zero }
  function CloseViewer { $v = ViewerOf $b; if ($v -ne [IntPtr]::Zero) { [R]::PostMessage($v, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null } }
  function Connect { Click ([R]::GetDlgItem($hb, $IDC_CONNECT)) }

  # 1) first connection: the prompt offers three choices; "Always allow this device"
  Connect
  $d = WaitDialog 8
  Check ($d -ne [IntPtr]::Zero) "first connection from an unknown device asks for approval"
  "     prompt: " + [R]::Title($d)
  # The three choices are drawn by Windows (not readable as controls), so keep a picture to look at.
  Shot $d (Join-Path $PSScriptRoot "..\docs\ui-approval-trust.png")
  Check (Test-Path (Join-Path $PSScriptRoot "..\docs\ui-approval-trust.png")) "saved a screenshot of the prompt"
  Check (Press $d "Always allow this device") "choosing Always allow this device"
  Wait 5
  Check ((Status $ha) -match "viewing|controlling") "streaming after approval: $(Status $ha)"
  Check ((TrustedCount) -eq 1) "the device is now in the trusted list"
  Check ((Get-Content $cfg -Raw) -match "name0=$([regex]::Escape($env:COMPUTERNAME))") "the trusted entry carries this PC's name"

  # 2) later connection: no prompt (wait out the 20 s same-IP grace first so only the trust can explain it)
  CloseViewer
  Write-Host "     (waiting 26 s so the short same-IP grace expires)"; Wait 26
  Check ((Status $ha) -notmatch "viewing|controlling") "viewer gone, host back to waiting: $(Status $ha)"
  Connect
  Wait 5
  Check ((Dialog) -eq [IntPtr]::Zero) "trusted device reconnects with NO prompt"
  Check ((Status $ha) -match "viewing|controlling") "and streams straight away: $(Status $ha)"

  # 3) trusted devices dialog lists it and can remove it
  Click ([R]::GetDlgItem($ha, $IDC_TRUSTED)); Wait 2
  $td = [R]::FindWindow("RDTrusted", "Trusted devices")
  Check ($td -ne [IntPtr]::Zero) "Trusted devices dialog opens"
  $lb = [R]::GetDlgItem($td, 301)
  Check (([int][R]::SendMessage($lb, 0x018B, [IntPtr]0, [IntPtr]0)) -eq 1) "dialog lists exactly one device"
  [R]::SendMessage($lb, 0x0186, [IntPtr]0, [IntPtr]0) | Out-Null                       # LB_SETCURSEL 0
  [R]::PostMessage($td, 0x0111, [IntPtr](301 -bor (1 -shl 16)), $lb) | Out-Null        # LBN_SELCHANGE enables Remove
  Wait 1
  Click ([R]::GetDlgItem($td, 302)); Wait 1
  Check ((TrustedCount) -eq 0) "Remove selected deletes the trust"
  Click ([R]::GetDlgItem($td, 304)); Wait 1

  # 4) after removal the same device is asked again; "Allow once" is NOT remembered
  CloseViewer
  Write-Host "     (waiting 26 s)"; Wait 26
  Connect
  $d = WaitDialog 8
  Check ($d -ne [IntPtr]::Zero) "after removal the device is asked again"
  Check (Press $d "Allow once") "choosing Allow once"
  Wait 5
  Check ((Status $ha) -match "viewing|controlling") "streams: $(Status $ha)"
  Check ((TrustedCount) -eq 0) "Allow once does not remember the device"
  CloseViewer
  Write-Host "     (waiting 26 s)"; Wait 26
  Connect
  $d = WaitDialog 8
  Check ($d -ne [IntPtr]::Zero) "next time it is asked again"
  Check (Press $d "Deny") "choosing Deny"
  Wait 5
  Check ((Status $ha) -notmatch "viewing|controlling") "denied: nothing streams: $(Status $ha)"
  Check ((TrustedCount) -eq 0) "Deny remembers nothing"
}
finally {
  Get-Process RemoteDisplay, rd-client -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep -Milliseconds 500
  if ($hadConfig) { Copy-Item $backup $cfg -Force; "config.ini restored" } else { Remove-Item $cfg -ErrorAction SilentlyContinue }
}
if ($fails -eq 0) { "ALL TRUST CHECKS PASSED" } else { "$fails CHECK(S) FAILED" }
exit $fails
