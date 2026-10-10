param([string]$Bin = "A:\projects\RemoteDisplay\build\Release")
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text; using System.Collections.Generic;
public class V {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vk);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint c, uint t);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  public static IntPtr FirstWindow(uint pid) { IntPtr f = IntPtr.Zero;
    EnumWindows((h, p) => { uint x; GetWindowThreadProcessId(h, out x); if (x == pid && IsWindowVisible(h) && f == IntPtr.Zero) f = h; return true; }, IntPtr.Zero); return f; }
}
'@
[V]::SetProcessDPIAware() | Out-Null
$global:fails = 0
function Check($ok, $what) { if ($ok) { "ok   $what" } else { "FAIL $what"; $global:fails++ } }
function Title($h) { $sb = New-Object System.Text.StringBuilder 256; [V]::GetWindowText($h, $sb, 256) | Out-Null; $sb.ToString() }
function Wait($ms) { $end = [DateTime]::Now.AddMilliseconds($ms); while ([DateTime]::Now -lt $end) { [System.Windows.Forms.Application]::DoEvents(); Start-Sleep -Milliseconds 10 } }
function MakeLParam($x, $y) { [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF)) }

$key = "ABCDE-FGHIJ-KLMNO-PQRST-UV234-4"
$host_ = Start-Process "$Bin\rd-host.exe" -ArgumentList "--key", $key -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2

# a topmost target window with a text box, in a known place on the primary screen
$form = New-Object System.Windows.Forms.Form
$form.FormBorderStyle = 'None'; $form.StartPosition = 'Manual'; $form.Location = New-Object System.Drawing.Point(300, 300)
$form.Size = New-Object System.Drawing.Size(420, 220); $form.TopMost = $true
$tb = New-Object System.Windows.Forms.TextBox; $tb.Multiline = $true; $tb.Dock = 'Fill'; $form.Controls.Add($tb)
$form.Show(); Wait 500
$scr = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$tx = 510; $ty = 410    # a point inside the target window

$cli = Start-Process "$Bin\rd-client.exe" -ArgumentList "--key", $key, "--host", "127.0.0.1" -PassThru
Wait 4000
$vh = [V]::FirstWindow([uint32]$cli.Id)
"viewer title: " + (Title $vh)
Check ((Title $vh) -like "*remote control on*" -or (Title $vh) -like "*controlling*") "viewer learns that control is allowed"

# map a desktop pixel to a viewer client pixel (same letterbox maths as the viewer)
$rc = New-Object V+RECT; [V]::GetClientRect($vh, [ref]$rc) | Out-Null
$ww = $rc.R; $wh = $rc.B
$k = [Math]::Min($ww / $scr.Width, $wh / $scr.Height)
$dw = $scr.Width * $k; $dh = $scr.Height * $k; $left = ($ww - $dw) / 2; $top = ($wh - $dh) / 2
function ToViewer($px, $py) { @([int]($left + $px / $scr.Width * $dw), [int]($top + $py / $scr.Height * $dh)) }

# 1) mouse move -> real cursor on the remote (here: the same) screen
$p = ToViewer $tx $ty
[V]::PostMessage($vh, 0x0200, [IntPtr]::Zero, (MakeLParam $p[0] $p[1])) | Out-Null   # WM_MOUSEMOVE
Wait 600
$pt = New-Object V+POINT; [V]::GetCursorPos([ref]$pt) | Out-Null
"cursor at $($pt.X),$($pt.Y), wanted about $tx,$ty"
Check ([Math]::Abs($pt.X - $tx) -le 4 -and [Math]::Abs($pt.Y - $ty) -le 4) "mouse move maps to the right pixel"

# 2) click through the viewer activates the target window
[V]::PostMessage($vh, 0x0201, [IntPtr]1, (MakeLParam $p[0] $p[1])) | Out-Null    # WM_LBUTTONDOWN
[V]::PostMessage($vh, 0x0202, [IntPtr]0, (MakeLParam $p[0] $p[1])) | Out-Null    # WM_LBUTTONUP
Wait 600
Check ([V]::GetForegroundWindow() -eq $form.Handle) "click via the viewer focused the target window"
"debug: textbox focused=$($tb.Focused) activeControl=$($form.ActiveControl) formActive=$([System.Windows.Forms.Form]::ActiveForm -ne $null)"

# 3) typing: WM_KEYDOWN/UP exactly as Windows would deliver them (scancode in lParam bits 16-23)
if ([V]::GetForegroundWindow() -eq $form.Handle) {
  foreach ($c in "hello 123".ToCharArray()) {
    $vk = if ($c -eq ' ') { 0x20 } elseif ($c -match '\d') { [int][char]$c } else { [int][char]::ToUpper($c) }
    $scan = [V]::MapVirtualKey($vk, 0)
    $down = [IntPtr](1 -bor ($scan -shl 16))
    $up   = [IntPtr](1 -bor ($scan -shl 16) -bor 0xC0000000)
    [V]::PostMessage($vh, 0x0100, [IntPtr]$vk, $down) | Out-Null          # WM_KEYDOWN
    [V]::PostMessage($vh, 0x0101, [IntPtr]$vk, $up) | Out-Null            # WM_KEYUP
  }
  Wait 1000
  "text box: '$($tb.Text)'"
  Check ($tb.Text -eq "hello 123") "typed text arrives in the remote window"
} else { "SKIP typing (target not foreground, refusing to type elsewhere)" }

# 4) viewer loses focus while Ctrl is held: Ctrl must be released remotely
$ctrlScan = [V]::MapVirtualKey(0xA2, 0)
[V]::PostMessage($vh, 0x0100, [IntPtr]0x11, [IntPtr](1 -bor ($ctrlScan -shl 16))) | Out-Null
Wait 500
Check (([V]::GetAsyncKeyState(0xA2) -band 0x8000) -ne 0) "Ctrl is held remotely while the viewer holds it"
[V]::PostMessage($vh, 0x0008, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_KILLFOCUS
Wait 700
Check (([V]::GetAsyncKeyState(0xA2) -band 0x8000) -eq 0) "focus loss releases the held key on the remote PC"

$form.Close()
Stop-Process -Id $cli.Id, $host_.Id -Force -ErrorAction SilentlyContinue
if ($global:fails -eq 0) { "ALL VIEWER INPUT CHECKS PASSED" } else { "$($global:fails) CHECK(S) FAILED" }
