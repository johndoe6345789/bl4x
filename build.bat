@echo off
rem Builds bl4x with the VS 2022 Build Tools (MSVC 14.44) and Ninja.
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 && (
  if not exist "%~dp0build\build.ninja" cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=Release
) && cmake --build "%~dp0build"
