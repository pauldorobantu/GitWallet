@echo off
rem GitWallet - configure + build (MSVC x64 + Ninja + Qt 6.10.3 + vcpkg).
rem Reuses the shared vcpkg at C:\app-vcpkg and its installed tree so botan/zlib
rem (already built for FlowSuite) are not rebuilt; only secp256k1 compiles.
setlocal
cd /d "%~dp0"

if not exist "C:\app-vcpkg\vcpkg.exe" (
    echo [build] vcpkg not found at C:\app-vcpkg
    exit /b 1
)
set "VCPKG_TOOLCHAIN=C:\app-vcpkg\scripts\buildsystems\vcpkg.cmake"
set "REPO=%CD%"

set "VCVARS="
for /f "usebackq delims=" %%i in (`"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if defined VS set "VCVARS=%VS%\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS (
    echo [build] vcvars64.bat not found - install VS 2022 "Desktop development with C++"
    exit /b 1
)
call "%VCVARS%" >nul || exit /b 1

if not defined QT_DIR set "QT_DIR=C:\Qt\6.10.3\msvc2022_64"
if not exist "%QT_DIR%\bin\windeployqt.exe" (
    echo [build] Qt not found at %QT_DIR% - set QT_DIR to the msvc2022_64 folder
    exit /b 1
)

cmake -S . -B build -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_PREFIX_PATH=%QT_DIR% ^
    -DCMAKE_TOOLCHAIN_FILE="%VCPKG_TOOLCHAIN%" ^
    -DVCPKG_TARGET_TRIPLET=x64-windows-release-only ^
    -DVCPKG_OVERLAY_TRIPLETS="%REPO%\cmake\triplets" ^
    -DVCPKG_INSTALLED_DIR=C:/app-vcpkg-installed || exit /b 1
cmake --build build || exit /b 1

echo [build] OK: build\GitWallet.exe
