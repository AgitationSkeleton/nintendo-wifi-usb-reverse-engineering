Nintendo Wi-Fi USB Connector - Windows (all-in-one)
===================================================

WHAT THIS IS
  nwc-connector.exe turns a Ralink RT2570 USB Wi-Fi dongle (USB ID 0411:008B,
  the original "Nintendo Wi-Fi USB Connector" hardware, or a compatible RT2570)
  into a working access point that brings a Nintendo DS / DS Lite online on
  Wiimmfi - no Windows XP and no original driver required.

  It is a single self-contained executable. It carries the probe engine, the
  Wintun adapter DLL and libusb inside itself and unpacks them on first run.

ONE-TIME SETUP: bind the dongle to WinUSB/libusbK
  The dongle must use the WinUSB (or libusbK) driver so the connector can talk
  to it. Run install-driver.cmd (fetches Zadig) OR use Zadig manually:
      Options -> List All Devices, select USB ID 0411:008B,
      choose "WinUSB", click Replace/Install Driver.
  You only do this once per dongle.

RUN IT
  Double-click nwc-connector.exe.
    - It asks for administrator rights (Wintun + NAT need them).
    - A console window opens and streams live logging.
    - Wait for "[nwc] up. Scan for the connector from the DS."
  On the DS:
    Nintendo Wi-Fi Connection Setup -> Options ->
      "Connect to your Nintendo Wi-Fi USB Connector" -> test connection,
    then start an online game.

START AUTOMATICALLY AT LOGON
    nwc-connector.exe --install-autostart      (registers an elevated logon task)
    nwc-connector.exe --uninstall-autostart    (removes it)

NOTES
  - The dongle's link to the DS is the weak spot; keep it within a couple of feet
    with a clear line of sight (a short USB extension cable helps).
  - The connector provides internet by NAT-ing the DS onto whatever adapter this
    PC uses to reach the internet.
  - To stop: close the console window (Ctrl+C). NAT is torn down on exit.
