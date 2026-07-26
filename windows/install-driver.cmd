@echo off
setlocal
rem ---------------------------------------------------------------------------
rem One-time WinUSB/libusbK driver bind for the RT2570 dongle (VID_0411 PID_008B).
rem libusb (and therefore nwc-connector.exe) can only open the dongle after it is
rem bound to WinUSB or libusbK instead of the default Ralink/rt2570 driver.
rem
rem This fetches Zadig (a tiny, well-known driver-install tool) and launches it.
rem In Zadig:
rem    1. Options -> "List All Devices"
rem    2. Select the RT2570 dongle (USB ID  0411:008B)
rem    3. Choose target driver "WinUSB" (or "libusbK")
rem    4. Click "Replace Driver" / "Install Driver"
rem    5. Close Zadig and run nwc-connector.exe
rem ---------------------------------------------------------------------------

set "ZADIG=%TEMP%\zadig.exe"
set "URL=https://github.com/pbatard/libwdi/releases/download/v1.5.1/zadig-2.9.exe"

echo Fetching Zadig...
where curl >nul 2>&1 && (
    curl -L -o "%ZADIG%" "%URL%"
) || (
    powershell -NoProfile -Command "Invoke-WebRequest -Uri '%URL%' -OutFile '%ZADIG%'"
)

if not exist "%ZADIG%" (
    echo.
    echo Could not download Zadig automatically. Get it from https://zadig.akeo.ie/
    echo then bind the 0411:008B dongle to WinUSB or libusbK manually.
    pause
    exit /b 1
)

echo.
echo Launching Zadig. Bind USB ID 0411:008B to WinUSB (or libusbK), then close it.
echo (Options -^> List All Devices, pick the RT2570, Replace Driver.)
echo.
start "" "%ZADIG%"
echo When done, run nwc-connector.exe
pause
