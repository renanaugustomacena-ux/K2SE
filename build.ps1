# Build K2SE with a portable MSVC toolchain (no Visual Studio installation).
#
# The toolchain is extracted from an official vs_BuildTools --layout download,
# which needs no administrator rights -- see tools/README-toolchain.md.
#
#   .\build.ps1                 configure + build Release
#   .\build.ps1 -Clean          wipe the build dir first
#   .\build.ps1 -Config Debug

param(
    [string]$Toolchain = "C:\Users\Renan Macena\tools\msvc",
    [string]$CMake     = "C:\Users\Renan Macena\tools\cmake-4.4.2-windows-x86_64\bin\cmake.exe",
    [string]$Config    = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

function Fail($msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }

if (-not (Test-Path $CMake)) { Fail "cmake not found at $CMake" }

# --- locate the MSVC toolset ------------------------------------------------
$msvcRoot = Join-Path $Toolchain "VC\Tools\MSVC"
if (-not (Test-Path $msvcRoot)) { Fail "MSVC toolset not found under $msvcRoot" }
$ver = (Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$msvc = Join-Path $msvcRoot $ver

$binHostX64 = Join-Path $msvc "bin\HostX64"
$clx86 = Join-Path $binHostX64 "x86\cl.exe"
if (-not (Test-Path $clx86)) { Fail "cl.exe (x86 target) not found at $clx86" }

# --- locate the Windows SDK -------------------------------------------------
$sdkInclude = $null
$sdkLib = $null
foreach ($cand in @("Windows Kits\10", "Program Files\Windows Kits\10", "Program Files (x86)\Windows Kits\10")) {
    $p = Join-Path $Toolchain $cand
    if (Test-Path (Join-Path $p "Include")) {
        $sdkVer = (Get-ChildItem (Join-Path $p "Include") -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
        $sdkInclude = Join-Path $p "Include\$sdkVer"
        $sdkLib = Join-Path $p "Lib\$sdkVer"
        break
    }
}
if (-not $sdkInclude) { Fail "Windows SDK not found under $Toolchain -- did the MSI extraction finish?" }

Write-Host "MSVC toolset : $ver"
Write-Host "SDK          : $sdkInclude"

# --- compose the environment ------------------------------------------------
$env:PATH = "$binHostX64\x86;$binHostX64\x64;" + $env:PATH

$incs = @(
    (Join-Path $msvc "include"),
    (Join-Path $sdkInclude "ucrt"),
    (Join-Path $sdkInclude "um"),
    (Join-Path $sdkInclude "shared"),
    (Join-Path $sdkInclude "winrt")
) | Where-Object { Test-Path $_ }
$env:INCLUDE = ($incs -join ";")

$libs = @(
    (Join-Path $msvc "lib\x86"),
    (Join-Path $sdkLib "ucrt\x86"),
    (Join-Path $sdkLib "um\x86")
) | Where-Object { Test-Path $_ }
$env:LIB = ($libs -join ";")

Write-Host "INCLUDE dirs : $($incs.Count)"
Write-Host "LIB dirs     : $($libs.Count)"

foreach ($required in @("windows.h", "stdio.h")) {
    $hit = $incs | ForEach-Object { Join-Path $_ $required } | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $hit) { Fail "$required not found in INCLUDE -- the SDK/UCRT extraction is incomplete" }
}
foreach ($required in @("kernel32.lib", "libcmt.lib")) {
    $hit = $libs | ForEach-Object { Join-Path $_ $required } | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $hit) { Fail "$required not found in LIB -- the SDK/CRT extraction is incomplete" }
}

# --- generate the proxy sources --------------------------------------------
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $python) { $python = "C:\Users\Renan Macena\AppData\Local\Programs\Python\Python312\python.exe" }
Write-Host "`n--- generating version.dll proxy ---"
& $python (Join-Path $root "build\generate_proxy.py")
if ($LASTEXITCODE -ne 0) { Fail "generate_proxy.py failed" }

# --- manifest tooling, or the lack of it ------------------------------------
# CMake links MSVC targets through `cmake -E vs_link_exe`, which embeds a
# manifest by running rc.exe. A vs_BuildTools --layout extraction carries the
# SDK headers and libraries but NOT rc.exe/mt.exe -- those live in a separate
# component -- and when they are missing the link dies in CMake's own compiler
# test, before a single line of K2SE is ever compiled:
#
#   RC Pass 1: command "rc /fo ...manifest.res ...manifest.rc" failed
#   ... is not able to compile a simple test program
#
# which reads like a broken compiler and is nothing of the kind. So: use the
# resource compiler when the toolchain has one, and turn manifest embedding off
# when it does not. K2SE does not want an embedded manifest either way -- it is
# a proxy DLL that imports only KERNEL32, with nothing to declare.
$rc = Get-ChildItem (Join-Path $Toolchain "Windows Kits\10\bin") -Recurse -Filter "rc.exe" `
        -ErrorAction SilentlyContinue |
      Where-Object { $_.DirectoryName -match "\\x86$" } |
      Select-Object -First 1

$linkerFlags = @()
if ($rc) {
    $env:PATH = "$($rc.DirectoryName);" + $env:PATH
    Write-Host "resource cc  : $($rc.FullName)"
} else {
    $linkerFlags += "/MANIFEST:NO"
    Write-Host "resource cc  : not in this toolchain -- linking with /MANIFEST:NO"
}

# --- configure + build ------------------------------------------------------
$buildDir = Join-Path $root "out"
if ($Clean -and (Test-Path $buildDir)) { Remove-Item $buildDir -Recurse -Force }

Write-Host "`n--- configuring ---"
# EXE flags matter as much as SHARED ones: CMake's try-compile probe builds an
# executable, so without them configure fails before reaching this project.
$flags = $linkerFlags -join " "
& $CMake -S $root -B $buildDir -G "NMake Makefiles" `
    "-DCMAKE_BUILD_TYPE=$Config" `
    "-DCMAKE_C_COMPILER=$clx86" `
    "-DCMAKE_CXX_COMPILER=$clx86" `
    "-DCMAKE_EXE_LINKER_FLAGS=$flags" `
    "-DCMAKE_SHARED_LINKER_FLAGS=$flags"
if ($LASTEXITCODE -ne 0) { Fail "CMake configure failed" }

Write-Host "`n--- building ---"
& $CMake --build $buildDir
if ($LASTEXITCODE -ne 0) { Fail "build failed" }

$dll = Join-Path $buildDir "version.dll"
if (Test-Path $dll) {
    Write-Host "`nBUILT: $dll" -ForegroundColor Green
    Write-Host ("size: {0:N0} bytes" -f (Get-Item $dll).Length)
} else {
    Fail "build reported success but version.dll is missing"
}
