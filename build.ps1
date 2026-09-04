# Builds copper_mc.exe with cl.exe (MSVC) directly -- no CMake, no vcpkg,
# no external dependencies. Locates vcvars64.bat automatically.
$ErrorActionPreference = "Stop"

$vcvarsCandidates = @(
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
)
$vcvars = $vcvarsCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vcvars) {
    throw "Could not find vcvars64.bat. Install the MSVC C++ build tools (Visual Studio)."
}

$root = $PSScriptRoot
$binDir = Join-Path $root "bin"
New-Item -ItemType Directory -Force -Path $binDir | Out-Null

$config = if ($args -contains "-Debug") { "Debug" } else { "Release" }
$optFlags = if ($config -eq "Debug") { "/Od /Zi /MDd" } else { "/O2 /DNDEBUG /MD" }
# -Tests also builds and runs the two test suites in tests/, so the no-CMake
# path can verify the engine too instead of only producing the pricer binary.
$withTests = $args -contains "-Tests"

Write-Host "Building copper_mc.exe ($config)..."

# Run cl.exe inside a cmd.exe that has sourced vcvars64.bat, since MSVC's
# environment (INCLUDE/LIB/PATH) is only set for that process tree.
$cmd = "call `"$vcvars`" >nul && cl.exe /std:c++20 /EHsc /W4 /nologo $optFlags " +
       "/I `"$root\include`" `"$root\src\main.cpp`" " +
       "/Fe:`"$binDir\copper_mc.exe`" /Fo:`"$binDir\\`""

cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) {
    throw "Build failed (exit code $LASTEXITCODE)"
}

Write-Host "Build succeeded: $binDir\copper_mc.exe"

if ($withTests) {
    foreach ($suite in @("test_pricing_math", "test_engine_properties")) {
        Write-Host "Building $suite..."
        $testCmd = "call `"$vcvars`" >nul && cl.exe /std:c++20 /EHsc /W4 /nologo $optFlags " +
                   "/I `"$root\include`" /I `"$root\tests`" `"$root\tests\$suite.cpp`" " +
                   "/Fe:`"$binDir\$suite.exe`" /Fo:`"$binDir\${suite}_`""
        cmd.exe /c $testCmd
        if ($LASTEXITCODE -ne 0) { throw "Build failed for $suite (exit code $LASTEXITCODE)" }
    }

    $failed = 0
    foreach ($suite in @("test_pricing_math", "test_engine_properties")) {
        Write-Host ""
        & "$binDir\$suite.exe"
        if ($LASTEXITCODE -ne 0) { $failed = 1 }
    }
    if ($failed -ne 0) { throw "Test suite failed" }
    Write-Host ""
    Write-Host "All test suites passed."
}
