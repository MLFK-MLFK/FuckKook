#requires -version 5
<#
  KookClean / FuckKookObs - one-shot installer.

  KOOK (Squirrel) keeps every build in its own folder:
      E:\KOOK\KOOK\app-0.112.0\KOOK.exe
  Because KOOK.exe statically imports VERSION.dll, a "version.dll" sitting in that
  folder is loaded into every KOOK process automatically - but only if the file is
  there.

  This script only copies the files into the app-* folders that exist right now.
  It does NOT register any scheduled task, does not start any watchdog and leaves
  nothing resident - it is a one-shot copy. AFTER A KOOK UPDATE a brand new
  app-<version> folder appears without the DLL, so run the launcher again:

    dist\\install.cmd "<KOOK root>"      or      dist\\StartKOOK.exe

  The launcher (tools\\launcher\\launcher.cpp) does exactly this: enumerate the
  app-* folders that already contain KOOK.exe, copy KookClean\\version.dll +
  kook-adblock.txt into each of them, then start the real KOOK.exe.

  Usage:
    powershell -NoProfile -ExecutionPolicy Bypass -File install-kook-dll.ps1
    powershell -NoProfile -ExecutionPolicy Bypass -File install-kook-dll.ps1 -KookRoot "D:\KOOK\KOOK"
    powershell -NoProfile -ExecutionPolicy Bypass -File install-kook-dll.ps1 -Uninstall

  Options:
    -KookRoot    KOOK install root, default E:\KOOK\KOOK
    -DllPath     version.dll to install (default: auto-detect in the project)
    -ConfigPath  selector config (kook-adblock.txt), default tools\kook-adblock.txt
    -Uninstall   remove version.dll + kook-adblock.txt from every app-* folder
    -Quiet       no console output, log file only

  Log: %TEMP%\kookclean-install.log
#>
[CmdletBinding()]
param(
    [string]$KookRoot = 'E:\KOOK\KOOK',
    [string]$DllPath = '',
    [string]$ConfigPath = '',
    [switch]$Uninstall,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$LogPath = Join-Path $env:TEMP 'kookclean-install.log'

function Write-Log {
    param([string]$Message)
    $line = ('[{0}] {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Message)
    if (-not $Quiet) { Write-Host $line }
    try { Add-Content -LiteralPath $LogPath -Value $line -Encoding UTF8 } catch { }
}

function Resolve-DllPath {
    if ($DllPath -ne '') {
        if (Test-Path -LiteralPath $DllPath) { return (Resolve-Path -LiteralPath $DllPath).Path }
        throw "DllPath not found: $DllPath"
    }
    $root = Split-Path -Parent $PSScriptRoot
    $candidates = @(
        (Join-Path $root 'x64\Release\version.dll'),
        (Join-Path $root 'Release\version.dll'),
        (Join-Path $root 'dist\KookClean\version.dll'),
        (Join-Path $root 'version.dll')
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    throw 'version.dll not found. Build Release|x64 first, or pass -DllPath.'
}

function Resolve-ConfigPath {
    if ($ConfigPath -ne '') {
        if (Test-Path -LiteralPath $ConfigPath) { return (Resolve-Path -LiteralPath $ConfigPath).Path }
        throw "ConfigPath not found: $ConfigPath"
    }
    $candidate = Join-Path $PSScriptRoot 'kook-adblock.txt'
    if (Test-Path -LiteralPath $candidate) { return (Resolve-Path -LiteralPath $candidate).Path }
    return ''
}

# Every folder that looks like a KOOK build folder: app-0.112.0, app-0.113.0, ...
function Get-KookAppDirs {
    if (-not (Test-Path -LiteralPath $KookRoot)) { throw "KookRoot not found: $KookRoot" }
    @(Get-ChildItem -LiteralPath $KookRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^app-\d' } |
        Sort-Object -Property Name -Descending)
}

function Test-FileLocked {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    try {
        # A file carried over from a KOOK release is read-only; clear that first, otherwise
        # the read-write open below would report a false "in use".
        $attributes = [System.IO.File]::GetAttributes($Path)
        if (($attributes -band [System.IO.FileAttributes]::ReadOnly) -ne 0) {
            [System.IO.File]::SetAttributes($Path, ($attributes -band (-bnot [System.IO.FileAttributes]::ReadOnly)))
        }
        $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
        $stream.Close()
        return $false
    } catch {
        return $true
    }
}

function Install-KookClean {
    param([string]$Dll, [string]$Config)
    $appDirs = Get-KookAppDirs
    if ($appDirs.Count -eq 0) {
        Write-Log ("no app-* folder under {0}" -f $KookRoot)
        return 0
    }

    $sourceHash = (Get-FileHash -LiteralPath $Dll -Algorithm SHA256).Hash
    $updated = 0
    foreach ($dir in $appDirs) {
        $exe = Join-Path $dir.FullName 'KOOK.exe'
        if (-not (Test-Path -LiteralPath $exe)) {
            Write-Log ("skip {0}: no KOOK.exe (not a usable build folder)" -f $dir.Name)
            continue
        }

        $target = Join-Path $dir.FullName 'version.dll'
        $needsCopy = $true
        if (Test-Path -LiteralPath $target) {
            try {
                if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -eq $sourceHash) { $needsCopy = $false }
            } catch {
                $needsCopy = $true
            }
        }
        if ($needsCopy) {
            if (Test-FileLocked $target) {
                Write-Log ("skip {0}: file is in use, close KOOK and run again" -f $target)
            } else {
                try {
                    Copy-Item -LiteralPath $Dll -Destination $target -Force
                    Write-Log ("installed version.dll -> {0}" -f $target)
                    $updated++
                } catch {
                    Write-Log ("failed to install {0}: {1}" -f $target, $_.Exception.Message)
                }
            }
        }

        if ($Config -ne '') {
            $configTarget = Join-Path $dir.FullName 'kook-adblock.txt'
            try {
                $same = (Test-Path -LiteralPath $configTarget) -and
                        ((Get-FileHash -LiteralPath $Config -Algorithm SHA256).Hash -eq
                         (Get-FileHash -LiteralPath $configTarget -Algorithm SHA256).Hash)
                if (-not $same) {
                    if (Test-FileLocked $configTarget) {
                        Write-Log ("skip {0}: file is in use" -f $configTarget)
                    } else {
                        Copy-Item -LiteralPath $Config -Destination $configTarget -Force
                        Write-Log ("installed kook-adblock.txt -> {0}" -f $configTarget)
                        $updated++
                    }
                }
            } catch {
                Write-Log ("failed to install selector config to {0}: {1}" -f $configTarget, $_.Exception.Message)
            }
        }
    }
    return $updated
}

function Uninstall-KookClean {
    $appDirs = Get-KookAppDirs
    $removed = 0
    foreach ($dir in $appDirs) {
        foreach ($name in @('version.dll', 'kook-adblock.txt')) {
            $path = Join-Path $dir.FullName $name
            if (-not (Test-Path -LiteralPath $path)) { continue }
            if ($name -eq 'version.dll') {
                # only remove our own build, never the file shipped by KOOK itself
                $marker = 'FKO_KOOKCLEAN'
                $hit = $false
                try {
                    $text = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::ASCII)
                    $hit = $text.Contains($marker)
                } catch { }
                if (-not $hit) {
                    Write-Log ("keep {0}: not our build" -f $path)
                    continue
                }
            }
            if (Test-FileLocked $path) {
                Write-Log ("skip {0}: file is in use, close KOOK and run again" -f $path)
                continue
            }
            try {
                Remove-Item -LiteralPath $path -Force
                Write-Log ("removed {0}" -f $path)
                $removed++
            } catch {
                Write-Log ("failed to remove {0}: {1}" -f $path, $_.Exception.Message)
            }
        }
    }
    return $removed
}

if ($Uninstall) {
    $count = Uninstall-KookClean
    Write-Log ("uninstall finished, {0} file(s) removed" -f $count)
    return
}

$dll = Resolve-DllPath
$config = Resolve-ConfigPath
Write-Log ("install: dll={0} config={1} root={2}" -f $dll, $config, $KookRoot)
$count = Install-KookClean -Dll $dll -Config $config
Write-Log ("install finished, {0} file(s) updated" -f $count)
