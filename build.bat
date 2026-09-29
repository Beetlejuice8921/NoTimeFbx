@echo off
rem Usage: build.bat [debug]
setlocal
cd /d "%~dp0"

where cl >nul 2>nul && goto :have_env
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:have_env

if not exist build mkdir build

set CFG=release
set OPT=/MT /O2 /GL /DNDEBUG
set LFLAGS=/LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS
if /i "%1"=="debug" (
    set CFG=debug
    set OPT=/MTd /Od /Zi /D_DEBUG
    set LFLAGS=/DEBUG /SUBSYSTEM:WINDOWS
)

rem ufbx: only the parser and triangulation (no subdivision, caches, animation baking, OBJ).
rem Single-precision reals: the mesh ends up as float anyway, and big scenes need ~27% less memory.
set UFBX_DEFS=/DUFBX_MINIMAL /DUFBX_ENABLE_TRIANGULATION /DUFBX_REAL_IS_FLOAT

fxc /nologo /T vs_5_0 /E vs_main /O3 /Vn g_vs_main /Fh build\shaders_vs.h src\shaders.hlsl || exit /b 1
fxc /nologo /T ps_5_0 /E ps_main /O3 /Vn g_ps_main /Fh build\shaders_ps.h src\shaders.hlsl || exit /b 1

rc /nologo /fo build\NoTimeFbx.res res\NoTimeFbx.rc || exit /b 1

rem ufbx.c is ~1 MB of C and rarely changes: recompile it only when the source or its flags differ
rem from what the cached object was built with (copies of both are kept next to the object).
set UFBX_OBJ=build\ufbx_%CFG%.obj
> build\ufbx_%CFG%.flags.new echo %OPT% %UFBX_DEFS%
if not exist %UFBX_OBJ% goto :build_ufbx
fc /b third_party\ufbx\ufbx.c build\ufbx_%CFG%.src >nul 2>nul || goto :build_ufbx
fc /b build\ufbx_%CFG%.flags.new build\ufbx_%CFG%.flags >nul 2>nul || goto :build_ufbx
goto :ufbx_done
:build_ufbx
cl /nologo /W0 %OPT% %UFBX_DEFS% /c third_party\ufbx\ufbx.c /Fo%UFBX_OBJ% /Fdbuild\ || exit /b 1
copy /y third_party\ufbx\ufbx.c build\ufbx_%CFG%.src >nul
copy /y build\ufbx_%CFG%.flags.new build\ufbx_%CFG%.flags >nul
:ufbx_done

cl /nologo /utf-8 /std:c++20 /W4 /EHsc /DUNICODE /D_UNICODE %OPT% %UFBX_DEFS% /Ibuild /Ithird_party\ufbx ^
    src\main.cpp src\assoc.cpp src\loader.cpp src\texture.cpp src\cache.cpp src\install.cpp ^
    build\ufbx_%CFG%.obj build\NoTimeFbx.res ^
    /Fobuild\ /Fdbuild\ /Febuild\NoTimeFbx.exe /link %LFLAGS% || exit /b 1
rem The installer is the same program under another name (see src\install.cpp).
copy /y build\NoTimeFbx.exe build\NoTimeFbx-setup.exe >nul || exit /b 1
echo Built build\NoTimeFbx.exe and build\NoTimeFbx-setup.exe
