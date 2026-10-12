@echo off
rem Configures and builds the C99 port with clang-cl through NMake. Usage: c\build.cmd [Debug|Release] [cmake --build arguments]
setlocal
set "ROOT=%~dp0"
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Debug"
if not "%~1"=="" shift
for %%e in (Enterprise Professional Community BuildTools) do (
    if exist "C:\Program Files\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\%%e"
)
if not defined VSDIR (
    echo Visual Studio 2022 was not found.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
set "CLANG=C:/Program Files/LLVM/bin/clang-cl.exe"
if not exist "%CLANG%" set "CLANG=%VSDIR:\=/%/VC/Tools/Llvm/x64/bin/clang-cl.exe"
cmake -S "%ROOT%." -B "%ROOT%build-%CONFIG%" -G "NMake Makefiles" -DCMAKE_C_COMPILER="%CLANG%" -DCMAKE_BUILD_TYPE=%CONFIG% || exit /b 1
cmake --build "%ROOT%build-%CONFIG%" %1 %2 %3 %4 || exit /b 1
