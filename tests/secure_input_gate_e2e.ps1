Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text; using System.Collections.Generic;
public class IG {
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  public static IntPtr Find(uint pid, string cls) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, p) => { uint x; GetWindowThreadProcessId(h, out x); var sb = new StringBuilder(64); GetClassName(h, sb, 64);
      if (x == pid && sb.ToString() == cls) { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
  public static string Title(IntPtr h) { var s = new StringBuilder(256); GetWindowText(h, s, 256); return s.ToString(); }
}
'@
[IG]::SetProcessDPIAware() | Out-Null
$bin = Join-Path $PSScriptRoot "..\build\Release"
$key = "32VLQ-FLJ2A-Q25MN-NW5GE-JHNOZ-A"

function Run($label, $secure) {
  Get-Process rd-host, rd-client, RemoteDisplay -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep -Milliseconds 800
  $helper = $null
  if ($secure) { $helper = Start-Process "$bin\RemoteDisplay.exe" -ArgumentList "--secure-helper", "--force" -PassThru -WindowStyle Hidden; Start-Sleep -Seconds 3 }
  if ($secure) { $env:RD_SECURE_TEST = "1" }
  $h = Start-Process "$bin\rd-host.exe" -ArgumentList "--key", $key -PassThru -RedirectStandardOutput "$env:TEMP\rdhost_$label.out"
  if ($secure) { Remove-Item Env:RD_SECURE_TEST }
  Start-Sleep -Seconds 2
  $c = Start-Process "$bin\rd-client.exe" -ArgumentList "--key", $key, "--host", "127.0.0.1" -PassThru
  Start-Sleep -Seconds 6
  $w = [IG]::Find([uint32]$c.Id, "rdviewer")
  [IG]::SetCursorPos(900, 600) | Out-Null
  Start-Sleep -Milliseconds 300
  $p0 = New-Object IG+POINT; [IG]::GetCursorPos([ref]$p0) | Out-Null
  $cr = New-Object IG+RECT; [IG]::GetClientRect($w, [ref]$cr) | Out-Null
  # move the pointer inside the viewer, away from where it is now, several times
  for ($i = 0; $i -lt 40; $i++) {
    $x = 60 + ($i % 2) * 300; $y = 80 + ($i % 2) * 120
    [IG]::PostMessage($w, 0x0200, [IntPtr]0, [IntPtr](($y -shl 16) -bor $x)) | Out-Null   # WM_MOUSEMOVE
    Start-Sleep -Milliseconds 50
  }
  Start-Sleep -Seconds 1
  $p1 = New-Object IG+POINT; [IG]::GetCursorPos([ref]$p1) | Out-Null
  $moved = ($p0.X -ne $p1.X) -or ($p0.Y -ne $p1.Y)
  "{0}: cursor {1},{2} -> {3},{4}  moved={5}  viewer: {6}" -f $label, $p0.X, $p0.Y, $p1.X, $p1.Y, $moved, ([IG]::Title($w))
  Stop-Process -Id $c.Id -Force; Stop-Process -Id $h.Id -Force
  if ($helper) { Stop-Process -Id $helper.Id -Force -ErrorAction SilentlyContinue }
}
Run "secure-path" $true
Run "normal-path" $false
