@echo off
setlocal
cd /d "%~dp0"

rem --- locate the Visual Studio 2022 x64 toolchain -----------------------------
if not defined VCToolsInstallDir (
    if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    ) else (
        echo [ERROR] Visual Studio 2022 x64 environment not found.
        echo   Run build.bat from an "x64 Native Tools Command Prompt for VS 2022",
        echo   or edit build.bat to point at your vcvars64.bat.
        exit /b 1
    )
)

rem --- output directory --------------------------------------------------------
set "OUTDIR=%~dp0out"
set "OBJDIR=%~dp0build\obj"
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

cl /nologo /O2 /arch:AVX2 /std:c++20 /EHsc /W3 /fp:fast /D_CRT_SECURE_NO_WARNINGS ^
   /I "src" /I "Interception\library" ^
   src\config.cpp src\native.cpp src\capture.cpp src\gpu_capture.cpp src\aim_bot.cpp src\main.cpp ^
   /Fo"%OBJDIR%\\" ^
   /Fe:"%OUTDIR%\Discord.exe" ^
   /link "Interception\library\x64\interception.lib" user32.lib gdi32.lib dxgi.lib d3d11.lib d3dcompiler.lib winmm.lib /OPT:REF /OPT:ICF /SUBSYSTEM:CONSOLE
if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)

copy /y "Interception\library\x64\interception.dll" "%OUTDIR%\" >nul
copy /y "..\owc.cfg" "%OUTDIR%\" >nul

echo [OK] Build output: %OUTDIR%\Discord.exe
exit /b 0