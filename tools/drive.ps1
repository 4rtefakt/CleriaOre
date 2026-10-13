# Drives the game through CleriaOre's test drive (cleriaore/drive/keys.txt, cmd.txt).
#   -start            install the built DLL and start the game with the drive on
#   -seq "D0:150,wait:500,1C:150"   keys (hex DIK codes, '+' joins) held for ms, or waits
#   -cmd "name args"  a cleria.command
#   -shot file.png    the game's frame
#   -log N            the last N lines of cleriaore.log
#   -load N           load save slot N (0-based) from the title, through the probe mod's `lua` command
#   -stop             close the game
param([int]$load = -1, [switch]$start, [string]$seq = "", [string]$cmd = "", [string]$shot = "", [int]$log = 0, [switch]$stop, [int]$wait = 0)
$g = "D:\SteamLibrary\steamapps\common\Ys Origin"; $d = "$g\cleriaore\drive"
New-Item -ItemType Directory -Force $d | Out-Null
if ($start) {
    if (Get-Process yso_win -ErrorAction SilentlyContinue) { "already running" } else {
        Copy-Item "C:\Users\kores\repos\CleriaOre\build\Release\dinput8.dll" "$g\dinput8.dll" -Force
        Set-Content "$d\keys.txt" "" -NoNewline
        $env:CLERIAORE_DRIVE = "1"
        foreach ($try in 1..3) {   # the game sometimes stays on a black window at its start: its log then never says "[ui] ready"
            Start-Process -FilePath "$g\yso_win.exe" -WorkingDirectory $g | Out-Null
            Start-Sleep -Seconds 14
            if (Select-String -Path "$g\cleriaore\cleriaore.log" -Pattern "\[ui\] ready" -Quiet) { break }
            "start $try did not come up, again"
            Get-Process yso_win -ErrorAction SilentlyContinue | Stop-Process -Force
            Start-Sleep -Seconds 25
        }
    }
}
if ($load -ge 0) { if ($start) { Start-Sleep -Seconds 5 }; Set-Content "$d\cmd.tmp" "lua cleria.game.load($load)"; Move-Item "$d\cmd.tmp" "$d\cmd.txt" -Force; Start-Sleep -Seconds 9 }
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
if ($shot) {   # the frame as the game's device holds it (the loader's "shot" command): the desktop is never captured
    $bmp = "$d\shot.bmp"
    Set-Content "$d\cmd.tmp" "shot shot"; Move-Item "$d\cmd.tmp" "$d\cmd.txt" -Force
    Start-Sleep -Milliseconds 1200
    if (Test-Path $bmp) {
        Add-Type -AssemblyName System.Drawing
        $src = [System.Drawing.Image]::FromFile($bmp)
        $small = New-Object System.Drawing.Bitmap $src, 1280, ([int](1280 * $src.Height / $src.Width))
        $small.Save($shot); $small.Dispose(); $src.Dispose()
    } else { "no frame" }
}
if ($log) { Get-Content "$g\cleriaore\cleriaore.log" -Tail $log }
if ($stop) { $p = Get-Process yso_win -ErrorAction SilentlyContinue; if ($p) { $p.CloseMainWindow() | Out-Null; Start-Sleep 3; Get-Process yso_win -ErrorAction SilentlyContinue | Stop-Process -Force } }
if (-not (Get-Process yso_win -ErrorAction SilentlyContinue) -and -not $stop) { "GAME NOT RUNNING" }
