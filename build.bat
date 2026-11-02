@echo off
rem GitWallet - configure + build (MSVC x64 + Ninja + Qt 6.10.3 + vcpkg).
rem Reuses the shared vcpkg at C:\app-vcpkg and its installed tree so what is already
rem built there is not built again; a clean machine builds every library in vcpkg.json.
setlocal
cd /d "%~dp0"

rem GW_VCPKG / GW_VCPKG_INSTALLED point elsewhere on the build machine (CI);
rem GW_BUILD_DIR keeps an existing build folder as it is.
if not defined GW_BUILD_DIR set "GW_BUILD_DIR=build"
if not defined GW_VCPKG set "GW_VCPKG=C:\app-vcpkg"
if not defined GW_VCPKG_INSTALLED set "GW_VCPKG_INSTALLED=C:/app-vcpkg-installed"
if not exist "%GW_VCPKG%\vcpkg.exe" (
    echo [build] vcpkg not found at %GW_VCPKG%
    exit /b 1
)
set "VCPKG_TOOLCHAIN=%GW_VCPKG%\scripts\buildsystems\vcpkg.cmake"
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

cmake -S . -B "%GW_BUILD_DIR%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    "-DCMAKE_PREFIX_PATH=%QT_DIR%" ^
    -DCMAKE_TOOLCHAIN_FILE="%VCPKG_TOOLCHAIN%" ^
    -DVCPKG_TARGET_TRIPLET=x64-windows-release-only ^
    -DVCPKG_OVERLAY_TRIPLETS="%REPO%\cmake\triplets" ^
    "-DVCPKG_INSTALLED_DIR=%GW_VCPKG_INSTALLED%" || exit /b 1
cmake --build "%GW_BUILD_DIR%" || exit /b 1

rem dist\ = the folder to copy elsewhere: the exe, its DLLs and the Qt plugins in
rem lib\plugins, where the embedded qt.conf (Prefix = lib) looks for them.
rem A running exe cannot be opened for writing, wherever it was started from: that is
rem asked first, as rmdir would delete what it can and leave a broken folder. Renamed
rem before it is deleted, as a window with its working folder inside refuses that.
set "DIST=%REPO%\dist"
if exist "%DIST%.old" rmdir /s /q "%DIST%.old"
if exist "%DIST%\GitWallet.exe" 2>nul (>>"%DIST%\GitWallet.exe" (call )) || (echo [build] GitWallet is running from dist\ - close it & exit /b 1)
if exist "%DIST%" ren "%DIST%" dist.old || (echo [build] another program or window is using dist\ - close it & exit /b 1)
if exist "%DIST%.old" rmdir /s /q "%DIST%.old"
mkdir "%DIST%" || exit /b 1
copy /y "%GW_BUILD_DIR%\GitWallet.exe" "%DIST%\" >nul || exit /b 1
copy /y "%GW_BUILD_DIR%\*.dll" "%DIST%\" >nul || exit /b 1
"%QT_DIR%\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler ^
    --no-system-dxc-compiler --no-opengl-sw --no-compiler-runtime ^
    --plugindir "%DIST%\lib\plugins" "%DIST%\GitWallet.exe" >nul || exit /b 1
rem windeployqt also copies Windows' own ICU, which every supported Windows has.
if exist "%DIST%\icuuc.dll" del "%DIST%\icuuc.dll"
rem The Visual C++ runtime next to the exe, so the folder also runs where it is
rem not installed.
copy /y "%VCToolsRedistDir%x64\Microsoft.VC143.CRT\*.dll" "%DIST%\" >nul || exit /b 1

echo [build] OK: start dist\GitWallet.exe
