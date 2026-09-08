# Build and run the ARB fragment program checker.
#
# It extracts the program literal straight out of src/render.cpp into
# program.inc, so the thing being checked is the thing that ships -- a copied
# shader would drift and the check would start lying.

param([string]$Toolchain = "C:\Users\Renan Macena\tools\msvc", [switch]$NoRun)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$root = Split-Path (Split-Path $here -Parent) -Parent
$outDir = Join-Path $root "out"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# --- extract the literal from render.cpp ------------------------------------
$render = Get-Content (Join-Path $root "src\render.cpp") -Raw
$match = [regex]::Match($render, 'const char kPostProgram\[\] =\s*(?<body>.*?);\r?\n', 'Singleline')
if (-not $match.Success) { Write-Host "ERROR: kPostProgram not found in src/render.cpp" -ForegroundColor Red; exit 1 }
$body = $match.Groups['body'].Value
# Keep only the quoted string pieces; drop the // comments between them.
$pieces = [regex]::Matches($body, '"(?:[^"\\]|\\.)*"') | ForEach-Object { $_.Value }
$inc = "static const char kPostProgram[] =`n" + ($pieces -join "`n") + ";`n"
Set-Content -Path (Join-Path $here "program.inc") -Value $inc -Encoding ascii
Write-Host "program.inc: $($pieces.Count) string pieces extracted from src/render.cpp"

# --- toolchain ---------------------------------------------------------------
$msvcRoot = Join-Path $Toolchain "VC\Tools\MSVC"
$ver = (Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$msvc = Join-Path $msvcRoot $ver
$binx86 = Join-Path $msvc "bin\HostX64\x86"
$kits = Join-Path $Toolchain "Windows Kits\10"
$sdkVer = (Get-ChildItem (Join-Path $kits "Include") -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$sdkInc = Join-Path $kits "Include\$sdkVer"
$sdkLib = Join-Path $kits "Lib\$sdkVer"

$env:PATH = "$binx86;" + (Join-Path $msvc "bin\HostX64\x64") + ";" + $env:PATH
$env:INCLUDE = @((Join-Path $msvc "include"), (Join-Path $sdkInc "ucrt"),
                 (Join-Path $sdkInc "um"), (Join-Path $sdkInc "shared")) -join ";"
$env:LIB = @((Join-Path $msvc "lib\x86"), (Join-Path $sdkLib "ucrt\x86"),
             (Join-Path $sdkLib "um\x86")) -join ";"

$exe = Join-Path $outDir "arbcheck.exe"
& (Join-Path $binx86 "cl.exe") /nologo /EHsc /O2 /I $here `
    (Join-Path $here "arbcheck.cpp") `
    /Fe:$exe /Fo:"$outDir\arbcheck.obj" `
    /link opengl32.lib gdi32.lib user32.lib
if ($LASTEXITCODE -ne 0) { Write-Host "ERROR: compile failed" -ForegroundColor Red; exit 1 }
Write-Host "built $exe"

if (-not $NoRun) {
    & $exe
    exit $LASTEXITCODE
}
