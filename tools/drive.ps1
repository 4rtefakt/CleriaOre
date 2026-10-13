# Drives the game through CleriaOre's test drive (cleriaore/drive/keys.txt, cmd.txt).
#   -start            install the built DLL and start the game with the drive on
#   -seq "D0:150,wait:500,1C:150"   keys (hex DIK codes, '+' joins) held for ms, or waits
#   -cmd "name args"  a cleria.command
#   -shot file.png    the game's window
#   -log N            the last N lines of cleriaore.log
#   -stop             close the game
param([switch]$start, [string]$seq = "", [string]$cmd = "", [string]$shot = "", [int]$log = 0, [switch]$stop, [int]$wait = 0)
$g = "D:\SteamLibrary\steamapps\common\Ys Origin"; $d = "$g\cleriaore\drive"
New-Item -ItemType Directory -Force $d | Out-Null
if ($start) {
    if (Get-Process yso_win -ErrorAction SilentlyContinue) { "already running" } else {
        Copy-Item "C:\Users\kores\repos\CleriaOre\build\Release\dinput8.dll" "$g\dinput8.dll" -Force
        Set-Content "$d\keys.txt" "" -NoNewline
        $env:CLERIAORE_DRIVE = "1"
        Start-Process -FilePath "$g\yso_win.exe" -WorkingDirectory $g | Out-Null
        Start-Sleep -Seconds 14
    }
}
foreach ($step in $seq.Split(",", [StringSplitOptions]::RemoveEmptyEntries)) {
    $k, $ms = $step.Split(":")
    if ($k -eq "wait") { Start-Sleep -Milliseconds ([int]$ms); continue }
    Set-Content "$d\keys.txt" ($k -replace "\+", " ") -NoNewline
    Start-Sleep -Milliseconds ([int]$ms)
    Set-Content "$d\keys.txt" "" -NoNewline
    Start-Sleep -Milliseconds 120
}
if ($cmd) { Set-Content "$d\cmd.tmp" $cmd; Move-Item "$d\cmd.tmp" "$d\cmd.txt" -Force }
if ($wait) { Start-Sleep -Milliseconds $wait }
if ($shot) {
    Add-Type -AssemblyName System.Drawing
    Add-Type @"
using System; using System.Runtime.InteropServices;
public class WD { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 public struct RECT { public int L, T, R, B; } }
"@
    $p = Get-Process yso_win -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($p) {
        $r = New-Object WD+RECT; [WD]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
        $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
        [System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
        $small = New-Object System.Drawing.Bitmap $bmp, 1280, ([int](1280 * $bmp.Height / $bmp.Width))
        $small.Save($shot)
    } else { "game not running" }
}
if ($log) { Get-Content "$g\cleriaore\cleriaore.log" -Tail $log }
if ($stop) { $p = Get-Process yso_win -ErrorAction SilentlyContinue; if ($p) { $p.CloseMainWindow() | Out-Null; Start-Sleep 3; Get-Process yso_win -ErrorAction SilentlyContinue | Stop-Process -Force } }
if (-not (Get-Process yso_win -ErrorAction SilentlyContinue) -and -not $stop) { "GAME NOT RUNNING" }
