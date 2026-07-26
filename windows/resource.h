#ifndef NWC_RESOURCE_H
#define NWC_RESOURCE_H
/* RCDATA resource IDs for the binaries embedded into nwc-connector.exe.
 * The referenced files are produced/downloaded by the release workflow and sit
 * next to nwc-connector.rc at compile time. */
#define IDR_PROBE          101   /* nwcusb_probe.exe */
#define IDR_WINTUN         102   /* wintun.dll       */
#define IDR_LIBUSB         103   /* libusb-1.0.dll   */
#define IDR_WINDIVERT_DLL  104   /* WinDivert.dll    */
#define IDR_WINDIVERT_SYS  105   /* WinDivert64.sys  */
#endif
