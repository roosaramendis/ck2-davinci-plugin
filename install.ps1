<#
CorridorKey 2.0 - DaVinci Resolve plugin installer for Windows.
Usually run for you by the ck2-gui app (Setup -> DCC plugins). By hand:

  powershell -ExecutionPolicy Bypass -File install.ps1 -Engine C:\Users\you\ck2\CorridorKey2.0
  powershell -ExecutionPolicy Bypass -File install.ps1 -Uninstall

Options: -Engine DIR  -Ffmpeg PATH  -NoSam  -ConnectOnly  -Uninstall
Copying into "C:\Program Files\Common Files\OFX\Plugins" asks once for permission (UAC).
#>
param(
    [string]$Engine = "",
    [string]$Ffmpeg = "",
    [switch]$NoSam,
    [switch]$ConnectOnly,
    [switch]$Uninstall,
    [switch]$Gui            # accepted for symmetry with install.sh (UAC is always a dialog)
)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$Here      = Split-Path -Parent $MyInvocation.MyCommand.Path
$Bundle    = "CorridorKey2.ofx.bundle"
$OfxDir    = Join-Path $env:CommonProgramFiles "OFX\Plugins"
$ConfDir   = Join-Path $env:APPDATA "ck2-davinci-plugin"
$Conf      = Join-Path $ConfDir "ofx.conf"
$CacheDir  = Join-Path $env:LOCALAPPDATA "corridorkey-ofx"
$TempDir   = Join-Path $env:LOCALAPPDATA "corridorkey-tmp"
$MacroDir  = Join-Path $env:APPDATA "Blackmagic Design\DaVinci Resolve\Support\Fusion\Macros"
$SamModel  = "facebook/sam2.1-hiera-large"
$Prebuilt  = Join-Path $Here "prebuilt\CorridorKey2-ofx-windows.zip"

function Say($m)  { Write-Host "==> $m" }
function Ok($m)   { Write-Host " [ok] $m" }
function Die($m)  { Write-Host " [error] $m"; exit 1 }

# Run a PowerShell snippet as administrator (one UAC prompt), wait, return its exit code.
function Invoke-Admin([string]$Script) {
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
               ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if ($isAdmin) { & ([scriptblock]::Create($Script)); return 0 }
    $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes(
        "`$ErrorActionPreference='Stop'; try { $Script; exit 0 } catch { exit 1 }"))
    try {
        $p = Start-Process powershell -Verb RunAs -Wait -PassThru -WindowStyle Hidden `
             -ArgumentList "-NoProfile", "-EncodedCommand", $enc
    } catch {
        Die "Permission was not given (UAC cancelled)."
    }
    return $p.ExitCode
}

# Native programs: Windows PowerShell 5 turns their stderr into errors when
# $ErrorActionPreference is Stop, so relax it while they run. Returns the exit code.
function Run-Native([scriptblock]$Block) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $Block | Out-Host; return $LASTEXITCODE } finally { $ErrorActionPreference = $old }
}

function Write-Utf8NoBom([string]$Path, [string[]]$Lines) {
    [IO.File]::WriteAllLines($Path, $Lines, (New-Object Text.UTF8Encoding($false)))
}

# ---------------------------------------------------------------- uninstall
if ($Uninstall) {
    $target = Join-Path $OfxDir $Bundle
    if (Test-Path $target) {
        Say "Removing $target (asks for permission)"
        $code = Invoke-Admin "Remove-Item -LiteralPath '$target' -Recurse -Force"
        if ($code -ne 0 -or (Test-Path $target)) { Die "Couldn't remove $target." }
        Remove-Item -LiteralPath (Join-Path $MacroDir "CorridorKey Outputs.setting") -ErrorAction SilentlyContinue
        Ok "Plugin removed. Cached keys are still in $CacheDir (delete by hand if you want)."
    } else {
        Ok "Plugin isn't installed."
    }
    exit 0
}

# ---------------------------------------------------------------- engine
function Find-Python([string]$dir) {
    foreach ($c in @("venv\Scripts\python.exe", ".venv\Scripts\python.exe")) {
        $p = Join-Path $dir $c
        if (Test-Path $p) { return $p }
    }
    return $null
}
if (-not $Engine -and (Test-Path $Conf)) {
    $line = Select-String -Path $Conf -Pattern '^engine=(.*)$' | Select-Object -First 1
    if ($line) { $Engine = $line.Matches[0].Groups[1].Value }
}
if (-not $Engine) { $Engine = Join-Path $HOME "ck2\CorridorKey2.0" }
if (-not (Test-Path (Join-Path $Engine "infer.py")) -or -not (Find-Python $Engine)) {
    Die "No CorridorKey 2.0 engine at $Engine (infer.py + venv\). Install the engine first (ck2-gui Setup tab)."
}
$Engine = (Resolve-Path $Engine).Path
$Py = Find-Python $Engine
Ok "Engine: $Engine"

function Connect {
    New-Item -ItemType Directory -Force -Path $ConfDir, $CacheDir, $TempDir | Out-Null
    Copy-Item (Join-Path $Here "support\roto_daemon.py") $ConfDir -Force
    Copy-Item (Join-Path $Here "support\roto_picker.py") $ConfDir -Force
    $ff = $Ffmpeg
    if (-not $ff -or -not (Test-Path $ff)) {
        $cmd = Get-Command ffmpeg -ErrorAction SilentlyContinue
        $ff = if ($cmd) { $cmd.Source } else { "" }
    }
    if (-not $ff) { Write-Host "   warning: ffmpeg not found - the engine refuses to start without it" }
    $lines = @(
        "# Written by ck2-davinci-plugin/install.ps1 - which engine the Resolve plugin runs.",
        "engine=$Engine",
        "python=$Py",
        "backend=cuda",
        "cache_dir=$CacheDir",
        "temp_dir=$TempDir",
        "roto_daemon=$(Join-Path $ConfDir 'roto_daemon.py')",
        "roto_picker=$(Join-Path $ConfDir 'roto_picker.py')",
        "sam_model=$SamModel"
    )
    if ($ff) { $lines += "ffmpeg=$ff" }
    Write-Utf8NoBom $Conf $lines
    Ok "Connected: wrote $Conf"
}

if ($ConnectOnly) { Connect; exit 0 }

# ---------------------------------------------------------------- plugin files
if (-not (Test-Path $Prebuilt)) {
    Die "The ready-made Windows plugin ($Prebuilt) is missing. Download a release of ck2-davinci-plugin."
}
$stage = Join-Path $env:TEMP "ck2-ofx-stage"
Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
Expand-Archive -Path $Prebuilt -DestinationPath $stage -Force
$src = Join-Path $stage $Bundle
if (-not (Test-Path (Join-Path $src "Contents\Win64\CorridorKey2.ofx"))) { Die "The Windows plugin zip looks broken." }
Ok "Plugin ready"

Say "Copying into $OfxDir (asks for permission)"
$target = Join-Path $OfxDir $Bundle
$code = Invoke-Admin ("New-Item -ItemType Directory -Force -Path '$OfxDir' | Out-Null; " +
                      "Copy-Item -LiteralPath '$src' -Destination '$OfxDir' -Recurse -Force")
if ($code -ne 0 -or -not (Test-Path (Join-Path $target "Contents\Win64\CorridorKey2.ofx"))) {
    Die "Copying into $OfxDir didn't work (permission cancelled, or DaVinci Resolve is open and using the plugin - close it and try again)."
}
Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
Ok "Installed $target"

Connect

# ---------------------------------------------------------------- Fusion macro
New-Item -ItemType Directory -Force -Path $MacroDir | Out-Null
Copy-Item (Join-Path $Here "fusion\CorridorKey Outputs.setting") $MacroDir -Force
Ok "Fusion macro: $MacroDir"

# ---------------------------------------------------------------- PySide6 for the Roto picker
if ((Run-Native { & $Py -c "import PySide6" *> $null }) -ne 0) {
    Say "Adding PySide6 to the engine's Python (for the Roto picker window)"
    $uv = Get-Command uv -ErrorAction SilentlyContinue
    $code = if ($uv) { Run-Native { & $uv.Source pip install --python $Py "PySide6>=6.7" } }
            else     { Run-Native { & $Py -m pip install "PySide6>=6.7" } }
    if ($code -ne 0) { Write-Host "   warning: PySide6 install failed - the Roto picker won't open" }
}

# ---------------------------------------------------------------- SAM 2 model
if (-not $NoSam) {
    if ((Run-Native { & $Py -c "import transformers; transformers.Sam2VideoModel" *> $null }) -eq 0) {
        Say "Downloading $SamModel for the Roto node (once, ~0.9 GB)"
        $samLog = Join-Path $ConfDir "sam_download.log"
        $code = Run-Native { & $Py -c "from huggingface_hub import snapshot_download as d; d('$SamModel')" 2> $samLog }
        if ($code -eq 0) { Ok "SAM 2 model ready" }
        else { Write-Host "   warning: couldn't download $SamModel now; the Roto node will try on first use" }
    } else {
        Write-Host "   warning: this engine's transformers has no SAM 2 (the Roto node needs transformers>=4.56)"
    }
}

Write-Host ""
Write-Host "Done. Next:"
Write-Host "  1. Restart DaVinci Resolve."
Write-Host "  2. Effects -> OpenFX -> Keying: CorridorKey 2.0, CorridorKey Pass, CorridorKey Roto."
Write-Host "  3. Press 'Key clip' in the Inspector, then play the clip once."
exit 0
