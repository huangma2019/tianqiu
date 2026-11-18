@echo off
setlocal
REM Build TQ (TianQiu / Celestial Sphere) : Win32 + GDI+ + SQLite + WinHTTP
REM Requires VS2022 with "Desktop development with C++".

set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSPATH=%%i"
if "%VSPATH%"=="" (
    echo [ERROR] No Visual Studio with "Desktop development with C++" found.
    echo         Install it with VS Installer, or run install_vs.bat as Administrator.
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [ERROR] vcvars64.bat failed & exit /b 1 )

cd /d "%~dp0"

REM 图标资源：tq.ico -> tq.res，链接进 exe 后才会有文件图标
rc /nologo /fo tq.res tq.rc
if errorlevel 1 ( echo [ERROR] rc.exe failed. & exit /b 1 )

cl /nologo /EHsc /std:c++17 /utf-8 /O2 /W3 ^
   /DUNICODE /D_UNICODE /DNOMINMAX /DSQLITE_THREADSAFE=0 ^
   main.cpp celestial_sphere.cpp star_catalog.cpp builtin_data.cpp constellation_data.cpp catalog_download.cpp city_db.cpp sqlite3.c ^
   tq.res ^
   /link /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib gdiplus.lib comctl32.lib winhttp.lib ole32.lib shell32.lib comdlg32.lib ^
   /OUT:TQ.exe

if errorlevel 1 ( echo [ERROR] Build failed. & exit /b 1 )
echo [OK] TQ.exe built.
exit /b 0
