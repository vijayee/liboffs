@echo off
setlocal EnableExtensions
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
  echo vcvars64 failed
  exit /b 1
)
set "SRC=C:\Users\victor morrow\Git-projects\liboffs"
set "BUILD=%SRC%\cmake-build-msvc"
set "TEST_EXE=%BUILD%\test\testliboffs.exe"

REM Configure (picks up poll-dancer / src changes via ExternalProject_Add).
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  "-DCMAKE_TOOLCHAIN_FILE=C:\Users\victor morrow\vcpkg\scripts\buildsystems\vcpkg.cmake" ^
  "-DVCPKG_TARGET_TRIPLET=x64-windows-static-md" ^
  -S "%SRC%" -B "%BUILD%"
if errorlevel 1 exit /b %ERRORLEVEL%

REM Force-rebuild poll-dancer if its source files have been edited
REM (ExternalProject's CONFIGURE only runs when liboffs' CMakeLists changes;
REM source edits inside deps/poll-dancer aren't always picked up otherwise).
pushd "%BUILD%\deps\poll-dancer" || exit /b 1
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cmake --build . --target poll_dancer --config Release
set "PD_RC=%ERRORLEVEL%"
popd
if not "%PD_RC%"=="0" exit /b %PD_RC%

REM Build the test executable against the freshly-rebuilt poll-dancer.lib.
cmake --build "%BUILD%" --target testliboffs -j 4
if errorlevel 1 exit /b %ERRORLEVEL%

if not exist "%TEST_EXE%" (
  echo testliboffs.exe not found at %TEST_EXE%
  exit /b 1
)

REM Run the TestPlatformLocal + TestUnixTransport suite twice:
REM  1) with OFFS_FORCE_NAMED_PIPE=1 (the named-pipe backend, which is
REM     also the default);
REM  2) with OFFS_USE_AF_UNIX=1 (opts into the AF_UNIX backend on
REM     Windows 10 1803+; falls through to named pipes on older SKUs or
REM     if AF_UNIX bind/listen fails).
echo.
echo === testliboffs run 1: OFFS_FORCE_NAMED_PIPE=1 (named-pipe) ===
set "OFFS_FORCE_NAMED_PIPE=1"
"%TEST_EXE%" --gtest_filter=TestPlatformLocal*:TestUnixTransport.*
set "RC1=%ERRORLEVEL%"
set "OFFS_FORCE_NAMED_PIPE="

echo.
echo === testliboffs run 2: OFFS_USE_AF_UNIX=1 (AF_UNIX) ===
set "OFFS_USE_AF_UNIX=1"
"%TEST_EXE%" --gtest_filter=TestPlatformLocal*:TestUnixTransport.*
set "RC2=%ERRORLEVEL%"
set "OFFS_USE_AF_UNIX="

if not "%RC1%"=="0" (
  echo.
  echo *** run 1 ^(named-pipe branch^) FAILED with code %RC1%
  exit /b %RC1%
)
if not "%RC2%"=="0" (
  echo.
  echo *** run 2 ^(AF_UNIX branch^) FAILED with code %RC2%
  exit /b %RC2%
)

echo.
echo === testliboffs: both branches PASSED ===
exit /b 0
