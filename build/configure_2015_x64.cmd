@echo off
set OUT=..\out\x64
mkdir %OUT% 2>NUL
set TARGET="Visual Studio 14 2015 Win64" 
set CFGS=Debug;RelWithDebInfo
cmd /C "pushd %OUT% & cmake ../../build -G %TARGET% -DCMAKE_CONFIGURATION_TYPES=%CFGS%"
