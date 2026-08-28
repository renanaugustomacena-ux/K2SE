# Build K2SE by driving cl.exe/link.exe directly, with no CMake and no
# Visual Studio installation.
#
# Why this exists alongside CMakeLists.txt: CMake's MSVC path embeds a manifest,
# which needs rc.exe and mt.exe from the Windows SDK's bin package. A DLL does
# not need a manifest at all, and this project is six translation units, so
# calling the compiler directly is simpler than teaching CMake to skip it.
#
# CMakeLists.txt stays the supported path for anyone with a real VS install.

param(
    [string]$Toolchain = "C:\Users\Renan Macena\tools\msvc",
    [switch]$Debug
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$outDir = Join-Path $root "out"

function Fail($msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }

# --- toolchain discovery ----------------------------------------------------
$msvcRoot = Join-Path $Toolchain "VC\Tools\MSVC"
if (-not (Test-Path $msvcRoot)) { Fail "no MSVC toolset under $msvcRoot" }
$ver = (Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$msvc = Join-Path $msvcRoot $ver
$binx86 = Join-Path $msvc "bin\HostX64\x86"
$cl = Join-Path $binx86 "cl.exe"
$link = Join-Path $binx86 "link.exe"
if (-not (Test-Path $cl)) { Fail "cl.exe not found at $cl" }

$kits = Join-Path $Toolchain "Windows Kits\10"
$sdkVer = (Get-ChildItem (Join-Path $kits "Include") -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$sdkInc = Join-Path $kits "Include\$sdkVer"
$sdkLib = Join-Path $kits "Lib\$sdkVer"

$env:PATH = "$binx86;" + (Join-Path $msvc "bin\HostX64\x64") + ";" + $env:PATH
$env:INCLUDE = @(
    (Join-Path $msvc "include"),
    (Join-Path $sdkInc "ucrt"),
    (Join-Path $sdkInc "um"),
    (Join-Path $sdkInc "shared")
) -join ";"
$env:LIB = @(
    (Join-Path $msvc "lib\x86"),
    (Join-Path $sdkLib "ucrt\x86"),
    (Join-Path $sdkLib "um\x86")
) -join ";"

Write-Host "MSVC $ver / SDK $sdkVer"

# --- generate the proxy -----------------------------------------------------
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $python) { $python = "C:\Users\Renan Macena\AppData\Local\Programs\Python\Python312\python.exe" }
& $python (Join-Path $root "build\generate_proxy.py") | Out-Null
if ($LASTEXITCODE -ne 0) { Fail "generate_proxy.py failed" }
Write-Host "proxy sources generated"

# --- compile ----------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
Push-Location $outDir

$sources = @(
    "dllmain.cpp", "log.cpp", "fingerprint.cpp", "vm.cpp", "routines.cpp",
    "vmstack.cpp", "exostring.cpp", "gameobj.cpp", "glhook.cpp",
    "proxy_version_generated.cpp"
) | ForEach-Object { Join-Path $root "src\$_" }

$opt = if ($Debug) { "/Od", "/Zi" } else { "/O2" }
$cflags = @(
    "/nologo", "/c", "/W4", "/MT", "/GS-", "/std:c++17", "/permissive-", "/EHsc"
) + $opt + @(
    "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS",
    "/DK2SE_VERSION_MAJOR=0", "/DK2SE_VERSION_MINOR=1", "/DK2SE_VERSION_PATCH=0",
    "/I", (Join-Path $root "src")
)

Write-Host "`n--- compiling ---"
& $cl @cflags @sources
if ($LASTEXITCODE -ne 0) { Pop-Location; Fail "compilation failed" }

# --- link -------------------------------------------------------------------
Write-Host "`n--- linking ---"
$objs = Get-ChildItem $outDir -Filter *.obj | ForEach-Object { $_.FullName }
& $link /nologo /DLL /MACHINE:X86 /MANIFEST:NO /OPT:REF /OPT:ICF `
    "/DEF:$(Join-Path $root 'src\version.def')" `
    "/OUT:$(Join-Path $outDir 'version.dll')" `
    @objs kernel32.lib
$linkRc = $LASTEXITCODE
Pop-Location
if ($linkRc -ne 0) { Fail "link failed" }

$dll = Join-Path $outDir "version.dll"
if (-not (Test-Path $dll)) { Fail "link reported success but version.dll is missing" }

Write-Host "`nBUILT: $dll" -ForegroundColor Green
Write-Host ("size: {0:N0} bytes" -f (Get-Item $dll).Length)
