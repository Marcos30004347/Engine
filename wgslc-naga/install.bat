@echo off
setlocal

set BIN_NAME=wgslc-naga
set TARGET=release

echo Building %BIN_NAME%...
cargo build --release

set SOURCE_EXE=target\%TARGET%\%BIN_NAME%.exe

if not exist "%SOURCE_EXE%" (
    echo ERROR: Build failed. %SOURCE_EXE% not found.
    exit /b 1
)

set INSTALL_DIR=%LOCALAPPDATA%\Programs\wgslc-naga\bin

echo Installing to %INSTALL_DIR%
if not exist "%INSTALL_DIR%" mkdir "%INSTALL_DIR%"

copy /Y "%SOURCE_EXE%" "%INSTALL_DIR%\%BIN_NAME%.exe" >nul

echo Adding to PATH (user scope)
setx PATH "%PATH%;%INSTALL_DIR%" >nul

echo.
echo Installed %BIN_NAME%
echo Restart your terminal to use it.
