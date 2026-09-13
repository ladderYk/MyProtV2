@echo off
REM ============================================================
REM MyProt V2 — 依赖下载与解压脚本
REM 运行环境: Windows + PowerShell (Win10/11 内置)
REM 用法: scripts\setup.bat
REM ============================================================

setlocal enabledelayedexpansion

set ROOT=%~dp0..
set TP=%ROOT%\third_party

echo ============================================================
echo  MyProt V2 — 第三方依赖安装
echo  目标: VS2015 (v140) / C++11 兼容版本
echo ============================================================
echo.

REM ── asio 1.20.0 (standalone) ──
echo [1/4] asio 1.20.0 ...
if not exist "%TP%\asio\include\asio.hpp" (
    powershell -Command "& { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri 'https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-20-0.zip' -OutFile '%TP%\asio.zip' }"
    powershell -Command "& { Expand-Archive -Path '%TP%\asio.zip' -DestinationPath '%TP%\asio_tmp' -Force }"
    if not exist "%TP%\asio\include" mkdir "%TP%\asio\include"
    xcopy /E /Y /Q "%TP%\asio_tmp\asio-asio-1-20-0\asio\include\*" "%TP%\asio\include\" >nul
    rmdir /S /Q "%TP%\asio_tmp" 2>nul
    del "%TP%\asio.zip" 2>nul
    echo   OK
) else (
    echo   Already exists, skipping.
)

REM ── nlohmann/json 3.7.3 ──
echo [2/4] nlohmann/json 3.7.3 ...
if not exist "%TP%\nlohmann_json\include\nlohmann\json.hpp" (
    powershell -Command "& { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri 'https://github.com/nlohmann/json/releases/download/v3.7.3/include.zip' -OutFile '%TP%\json.zip' }"
    powershell -Command "& { Expand-Archive -Path '%TP%\json.zip' -DestinationPath '%TP%\nlohmann_json_tmp' -Force }"
    if not exist "%TP%\nlohmann_json\include" mkdir "%TP%\nlohmann_json\include"
    xcopy /E /Y /Q "%TP%\nlohmann_json_tmp\include\*" "%TP%\nlohmann_json\include\" >nul
    rmdir /S /Q "%TP%\nlohmann_json_tmp" 2>nul
    del "%TP%\json.zip" 2>nul
    echo   OK
) else (
    echo   Already exists, skipping.
)

REM ── GoogleTest 1.8.1 ──
echo [3/4] GoogleTest 1.8.1 ...
echo   NOTE: GoogleTest 需要预编译为静态库。
echo   请手动编译 gtest 并放置:
echo     %TP%\gtest\include\  (gtest 头文件)
echo     %TP%\gtest\lib\Win32\gtest.lib, gtest_main.lib
echo     %TP%\gtest\lib\x64\gtest.lib, gtest_main.lib

REM ── OpenSSL 1.1.1 ──
echo [4/4] OpenSSL 1.1.1 ...
echo   NOTE: v1 TlsChannel 仍为 stub, OpenSSL 仅作 v1.5+ 启用准备。
echo   推荐从 https://slproweb.com/products/Win32-OpenSSL.html 下载
echo   并放置到:
echo     %TP%\openssl\include\  (openssl 头文件)
echo     %TP%\openssl\lib\Win32\  (32位 lib)
echo     %TP%\openssl\lib\x64\    (64位 lib)

echo.
echo ============================================================
echo  安装完成!
echo  注意: GoogleTest 和 OpenSSL 需要手动放置预编译二进制。
echo  详见 third_party\README.md
echo ============================================================

endlocal
