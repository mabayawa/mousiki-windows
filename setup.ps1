#Requires -Version 5.1
<#
.SYNOPSIS
    Installs mousiki's dependencies and builds it, on a clean Windows machine.

.DESCRIPTION
    The Windows counterpart of upstream's setup.sh. Re-runnable: everything it
    does is idempotent, and anything already present is left alone.

    It installs CMake, FFmpeg, yt-dlp and Python through winget, makes sure the
    MSVC C++ toolset is available, seeds the config file if there isn't one,
    writes the yt-dlp extractor config that setup.sh also writes, builds, and
    then verifies every dependency is actually callable before telling you it
    worked.

.PARAMETER SkipDeps
    Build only. Use when the tools are already installed.

.PARAMETER SkipBuild
    Install dependencies but do not build.

.PARAMETER Generator
    Override the CMake generator. Detected from the installed Visual Studio by
    default; pass e.g. "Ninja" to use something else.

.EXAMPLE
    .\setup.ps1

.EXAMPLE
    # If PowerShell refuses to run the script at all:
    Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass; .\setup.ps1
#>
[CmdletBinding()]
param(
    [switch] $SkipDeps,
    [switch] $SkipBuild,
    [string] $Generator
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Definition

# ---------------------------------------------------------------------------
# Output helpers
# ---------------------------------------------------------------------------
function Write-Head($text) {
    Write-Host ''
    Write-Host ('  ' + $text) -ForegroundColor Cyan
    Write-Host ('  ' + ('-' * $text.Length)) -ForegroundColor DarkCyan
}
function Write-Step($text) { Write-Host "  ... $text" -ForegroundColor DarkGray }
function Write-Ok($text)   { Write-Host "  [ok] $text" -ForegroundColor Green }
function Write-Warn($text) { Write-Host "  [!]  $text" -ForegroundColor Yellow }
function Write-Bad($text)  { Write-Host "  [x]  $text" -ForegroundColor Red }

# ---------------------------------------------------------------------------
# winget-installed tools do not appear on the PATH of an already-running shell.
# Without this refresh the very first run fails with a confusing "not found"
# immediately after a successful install.
# ---------------------------------------------------------------------------
function Update-PathFromRegistry {
    $machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $user    = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:PATH = (@($machine, $user) | Where-Object { $_ }) -join ';'
}

function Test-Tool($name) {
    $null -ne (Get-Command $name -ErrorAction SilentlyContinue)
}

function Install-WingetPackage($id, $friendly) {
    Write-Step "installing $friendly ($id)"
    # --disable-interactivity keeps winget from prompting in a non-interactive
    # session; the agreement flags stop it blocking on a first-run EULA.
    winget install --id $id --exact --silent --disable-interactivity `
        --accept-source-agreements --accept-package-agreements | Out-Null
    # winget returns non-zero for "already installed", which is not an error
    # here -- the verification pass below is what actually decides.
    Update-PathFromRegistry
}

# ---------------------------------------------------------------------------
# Visual Studio / MSVC
# ---------------------------------------------------------------------------
function Get-VsInstance {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    $json = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                       -latest -format json 2>$null
    if (-not $json) { return $null }
    $parsed = $json | ConvertFrom-Json
    if ($parsed -is [array]) { return $parsed[0] }
    return $parsed
}

function Get-GeneratorForInstance($instance) {
    if (-not $instance) { return $null }
    $major = [int](($instance.installationVersion -split '\.')[0])
    switch ($major) {
        18      { 'Visual Studio 18 2026' }
        17      { 'Visual Studio 17 2022' }
        16      { 'Visual Studio 16 2019' }
        default { $null }
    }
}

function Install-MsvcToolset {
    # The C++ workload is NOT selected by a plain `winget install` of either
    # Build Tools or Community -- it installs the shell with no compiler, which
    # then fails at configure time with a baffling "could not find any instance
    # of Visual Studio". The workload has to be requested explicitly through
    # --override, and --passive --wait is what makes winget actually block
    # until the multi-GB install has finished.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $existing = if (Test-Path $vswhere) {
        & $vswhere -products * -latest -format json 2>$null | ConvertFrom-Json
    } else { $null }

    if ($existing) {
        # A Visual Studio is already here and merely lacks the C++ components.
        # Modifying it in place is far smaller than installing a second
        # toolchain beside it.
        $installPath = if ($existing -is [array]) { $existing[0].installationPath } else { $existing.installationPath }
        $setup = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
        Write-Step "adding the C++ workload to the existing install at $installPath"
        Write-Warn 'this is a multi-gigabyte download and will ask for administrator rights'
        $argString = 'modify --installPath "{0}" --add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended --passive --norestart' -f $installPath
        $p = Start-Process -FilePath $setup -ArgumentList $argString -PassThru -Verb RunAs
        $p.WaitForExit()
    } else {
        Write-Step 'installing Visual Studio 2022 Build Tools with the C++ workload'
        Write-Warn 'this is a multi-gigabyte download'
        winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --force `
            --accept-source-agreements --accept-package-agreements `
            --override '--passive --wait --add Microsoft.VisualStudio.Workload.VCTools;includeRecommended' | Out-Null
    }
    Update-PathFromRegistry
}

# ===========================================================================
Write-Host ''
Write-Host '  mousiki (Windows)' -ForegroundColor White
Write-Host '  a terminal music player' -ForegroundColor DarkGray

# ---------------------------------------------------------------------------
Write-Head 'Dependencies'

if ($SkipDeps) {
    Write-Step 'skipped (-SkipDeps)'
} else {
    if (-not (Test-Tool 'winget')) {
        Write-Bad 'winget is not available.'
        Write-Host '       Install "App Installer" from the Microsoft Store, or install'
        Write-Host '       CMake, FFmpeg, yt-dlp and Python 3 yourself and re-run with -SkipDeps.'
        exit 1
    }

    Update-PathFromRegistry

    $wanted = @(
        @{ Cmd = 'cmake';  Id = 'Kitware.CMake';      Name = 'CMake' },
        @{ Cmd = 'ffmpeg'; Id = 'Gyan.FFmpeg';        Name = 'FFmpeg' },
        @{ Cmd = 'yt-dlp'; Id = 'yt-dlp.yt-dlp';      Name = 'yt-dlp' },
        @{ Cmd = 'python'; Id = 'Python.Python.3.12'; Name = 'Python 3.12' }
    )
    foreach ($tool in $wanted) {
        if (Test-Tool $tool.Cmd) {
            Write-Ok "$($tool.Name) already installed"
        } else {
            Install-WingetPackage $tool.Id $tool.Name
        }
    }

    if (-not (Get-VsInstance)) {
        Install-MsvcToolset
    } else {
        Write-Ok 'MSVC C++ toolset already installed'
    }
}

# ---------------------------------------------------------------------------
Write-Head 'Python requests (soft dependency)'

# Only the lyrics panel needs this. Upstream treats it the same way: warn and
# carry on, because playback and streaming work perfectly well without it.
$python = if (Test-Tool 'py') { 'py' } elseif (Test-Tool 'python') { 'python' } else { $null }
if (-not $python) {
    Write-Warn 'no Python interpreter found -- synced lyrics will be unavailable'
} else {
    $pyArgs = if ($python -eq 'py') { @('-3', '-m', 'pip') } else { @('-m', 'pip') }
    & $python @pyArgs install --quiet --upgrade requests 2>$null
    if ($LASTEXITCODE -eq 0) {
        Write-Ok "requests installed for $python"
    } else {
        Write-Warn 'could not install requests -- synced lyrics will be unavailable'
    }
}

# ---------------------------------------------------------------------------
Write-Head 'Configuration'

# %USERPROFILE% rather than %APPDATA%: the app resolves its paths from HOME,
# which win_compat points at the user profile so the layout matches the other
# platforms exactly ($HOME/.config/mousiki, $HOME/.cache/mousiki).
$configDir  = Join-Path $env:USERPROFILE '.config\mousiki'
$configFile = Join-Path $configDir 'config.txt'
if (Test-Path $configFile) {
    Write-Ok "keeping your existing config at $configFile"
} else {
    New-Item -ItemType Directory -Force -Path $configDir | Out-Null
    Copy-Item (Join-Path $root 'config.txt') $configFile
    Write-Ok "seeded $configFile"
}

# The same extractor-args line setup.sh writes. Without it yt-dlp falls back to
# a client that throws 403s on a good proportion of tracks.
$ytdlpDir  = Join-Path $env:APPDATA 'yt-dlp'
$ytdlpConf = Join-Path $ytdlpDir 'config'
$ytdlpLine = '--extractor-args "youtube:player_client=android"'
New-Item -ItemType Directory -Force -Path $ytdlpDir | Out-Null
if ((Test-Path $ytdlpConf) -and (Select-String -Path $ytdlpConf -Pattern 'player_client' -Quiet)) {
    Write-Ok 'yt-dlp config already sets player_client'
} else {
    Add-Content -Path $ytdlpConf -Value $ytdlpLine -Encoding utf8
    Write-Ok "wrote $ytdlpConf"
}

# ---------------------------------------------------------------------------
Write-Head 'Build'

$exePath = $null
if ($SkipBuild) {
    Write-Step 'skipped (-SkipBuild)'
} else {
    if (-not $Generator) {
        $instance = Get-VsInstance
        $Generator = Get-GeneratorForInstance $instance
        if (-not $Generator) {
            Write-Bad 'no Visual Studio with the C++ workload was found after installation.'
            Write-Host '       Open the Visual Studio Installer, choose Modify, and tick'
            Write-Host '       "Desktop development with C++", then re-run this script.'
            exit 1
        }
        Write-Step "using generator: $Generator"
    }

    $buildDir = Join-Path $root 'build'
    & cmake -S $root -B $buildDir -G $Generator -A x64
    if ($LASTEXITCODE -ne 0) { Write-Bad 'cmake configure failed'; exit 1 }

    & cmake --build $buildDir --config Release --parallel
    if ($LASTEXITCODE -ne 0) { Write-Bad 'build failed'; exit 1 }

    $exePath = Join-Path $buildDir 'Release\mousiki.exe'
    if (-not (Test-Path $exePath)) {
        # Single-config generators (Ninja, Makefiles) put it straight in build/.
        $exePath = Join-Path $buildDir 'mousiki.exe'
    }
    if (Test-Path $exePath) { Write-Ok "built $exePath" }
    else { Write-Bad 'build reported success but no mousiki.exe was produced'; exit 1 }
}

# ---------------------------------------------------------------------------
Write-Head 'Verification'

$checks = @(
    @{ Name = 'cmake';   Required = $true;  Present = (Test-Tool 'cmake') },
    @{ Name = 'ffmpeg';  Required = $true;  Present = (Test-Tool 'ffmpeg') },
    @{ Name = 'ffprobe'; Required = $true;  Present = (Test-Tool 'ffprobe') },
    @{ Name = 'yt-dlp';  Required = $false; Present = (Test-Tool 'yt-dlp') },
    @{ Name = 'python';  Required = $false; Present = ($null -ne $python) }
)
$missingRequired = $false
foreach ($c in $checks) {
    if ($c.Present)       { Write-Ok $c.Name }
    elseif ($c.Required)  { Write-Bad "$($c.Name) (required)"; $missingRequired = $true }
    else                  { Write-Warn "$($c.Name) (optional -- online search and lyrics need it)" }
}
if ($missingRequired) {
    Write-Host ''
    Write-Bad 'some required tools are missing. Open a new terminal and re-run this script.'
    exit 1
}

# ---------------------------------------------------------------------------
Write-Head 'Done'
if ($exePath) {
    Write-Host "  Run it with:  $exePath"
    Write-Host ''
    Write-Host '  mousiki draws with ANSI escape sequences, so run it in Windows Terminal.'
    Write-Host '  To launch it by name, add this to your PowerShell profile:'
    Write-Host ''
    Write-Host "      function mousiki { & '$exePath' }" -ForegroundColor DarkGray
    Write-Host ''
    Write-Host "  Config:  $configFile"
    Write-Host "  Cache:   $(Join-Path $env:USERPROFILE '.cache\mousiki')"
}
Write-Host ''
