@echo off
setlocal EnableExtensions
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
  echo vcvars64 failed
  exit /b 1
)
set "BUILD=C:\Users\victor morrow\Git-projects\liboffs\cmake-build-msvc"
cmake --build "%BUILD%" --target offs-ca -j 4
exit /b %ERRORLEVEL%
