# Drives two RemoteDisplay instances through the real UI: A shares, B connects, A approves.
param([string]$Exe = "A:\projects\RemoteDisplay\build\Release\RemoteDisplay.exe", [string]$OutDir = "A:\projects\RemoteDisplay\docs")
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text; using System.Collections.Generic;
public class U {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, int w, StringBuilder l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr a, string c, string t);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  public static List<IntPtr> Windows(uint pid) {
    var l = new List<IntPtr>();
    EnumWindows((h, p) => { uint x; GetWindowThreadProcessId(h, out x); if (x == pid && IsWindowVisible(h)) l.Add(h); return true; }, IntPtr.Zero);
    return l;
  }
}
'@
[U]::SetProcessDPIAware() | Out-Null
function Text($h) { $sb = New-Object System.Text.StringBuilder 512; [U]::SendMessage($h, 0x000D, 512, $sb) | Out-Null; $sb.ToString() }   # WM_GETTEXT
function Shot($h, $name) {
  $r = New-Object U+RECT; [U]::GetWindowRect($h, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
  $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc(); [U]::PrintWindow($h, $hdc, 2) | Out-Null; $g.ReleaseHdc($hdc)
  $bmp.Save("$OutDir\$name")
}
function Click($h) { [U]::SendMessage($h, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null }   # BM_CLICK
$IDC = @{ KEY=102; APPROVE=105; SHARE=106; STATUS=107; DISCONNECT=108; REMOTE=109; RKEY=110; CONNECT=111; MSG=112 }

$a = Start-Process $Exe -ArgumentList "--allow-multiple" -PassThru; $b = Start-Process $Exe -ArgumentList "--allow-multiple" -PassThru
Start-Sleep -Seconds 3
$ha = [U]::Windows([uint32]$a.Id)[0]; $hb = [U]::Windows([uint32]$b.Id)[0]
"A=$($a.Id) B=$($b.Id)"

Click ([U]::GetDlgItem($ha, $IDC.SHARE))           # A: Start sharing
Start-Sleep -Seconds 2
"A status after start : " + (Text ([U]::GetDlgItem($ha, $IDC.STATUS)))

$key = Text ([U]::GetDlgItem($ha, $IDC.KEY))
"A pairing key        : $key"
[U]::SendMessage([U]::GetDlgItem($hb, $IDC.REMOTE), 0x000C, [IntPtr]::Zero, "127.0.0.1") | Out-Null   # WM_SETTEXT
[U]::SendMessage([U]::GetDlgItem($hb, $IDC.RKEY),   0x000C, [IntPtr]::Zero, $key) | Out-Null
Click ([U]::GetDlgItem($hb, $IDC.CONNECT))          # B: Connect
Start-Sleep -Seconds 3
"A status on request  : " + (Text ([U]::GetDlgItem($ha, $IDC.STATUS)))
"B message            : " + (Text ([U]::GetDlgItem($hb, $IDC.MSG)))

$dlg = [U]::FindWindow("#32770", "RemoteDisplay - connection request")
"approval dialog found: " + ($dlg -ne [IntPtr]::Zero)
if ($dlg -ne [IntPtr]::Zero) {
  Shot $dlg "ui-approval.png"
  [U]::PostMessage($dlg, 0x0466, [IntPtr]1002, [IntPtr]::Zero) | Out-Null   # TDM_CLICK_BUTTON: "Allow once"
  Start-Sleep -Seconds 1
  "A status 1s after Yes: " + (Text ([U]::GetDlgItem($ha, $IDC.STATUS)))
  "dialog still open    : " + ([U]::FindWindow("#32770", "RemoteDisplay - connection request") -ne [IntPtr]::Zero)
}
Start-Sleep -Seconds 5
"A status streaming   : " + (Text ([U]::GetDlgItem($ha, $IDC.STATUS)))
$views = [U]::Windows([uint32]$b.Id)
foreach ($v in $views) { "B window: " + (Text $v) }
Shot $ha "ui-sharing.png"
Shot $hb "ui-connected.png"
$viewer = $views | Where-Object { (Text $_) -like "RemoteDisplay - 127*" } | Select -First 1
if ($viewer) { Shot $viewer "ui-viewer.png" }

# Mouse input through the viewer: the sharing side should report "controlling"
if ($viewer) {
  [U]::PostMessage($viewer, 0x0200, [IntPtr]0, [IntPtr](300 -bor (200 -shl 16))) | Out-Null
  Start-Sleep -Milliseconds 900
  "A status after input  : " + (Text ([U]::GetDlgItem($ha, $IDC.STATUS)))
  Shot $ha "ui-controlling.png"
}

# Disconnect from A
Click ([U]::GetDlgItem($ha, $IDC.DISCONNECT))
Start-Sleep -Seconds 2
"A status after kick  : " + (Text ([U]::GetDlgItem($ha, $IDC.STATUS)))
Stop-Process -Id $a.Id, $b.Id -Force
