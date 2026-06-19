@echo off
setlocal EnableExtensions
if not defined VCVARS_ALL call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
  echo vcvars64 failed
  exit /b 1
)
if defined SRC (
  set "SRC_OVERRIDE=%SRC%"
) else (
  set "SRC_OVERRIDE=%~dp0.."
)
if defined BUILD_DIR (
  set "BUILD=%BUILD_DIR%"
) else (
  set "BUILD=%SRC_OVERRIDE%\cmake-build-msvc"
)
if not defined VCPKG_ROOT set "VCPKG_ROOT=%USERPROFILE%\vcpkg"
if not defined CMAKE_TOOLCHAIN_FILE set "CMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake"
if not defined VCPKG_TARGET_TRIPLET set "VCPKG_TARGET_TRIPLET=x64-windows-static-md"
if not defined VCPKG_INSTALLED_DIR set "VCPKG_INSTALLED_DIR=%VCPKG_ROOT%\installed"
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=%CMAKE_TOOLCHAIN_FILE%" "-DVCPKG_TARGET_TRIPLET=%VCPKG_TARGET_TRIPLET%" "-S" "%SRC_OVERRIDE%" "-B" "%BUILD%"
exit /b %ERRORLEVEL%
