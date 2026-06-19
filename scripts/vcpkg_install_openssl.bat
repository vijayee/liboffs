@echo off
setlocal
cd /d C:\Users\victor morrow\vcpkg
set VCPKG_ROOT=C:\Users\victor morrow\vcpkg
.\vcpkg.exe install openssl:x64-windows-static-md
exit /b %ERRORLEVEL%
