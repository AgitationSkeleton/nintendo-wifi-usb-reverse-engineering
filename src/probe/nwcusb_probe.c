/*
 * Early native Windows probe for the Nintendo Wi-Fi USB Connector.
 *
 * This is intentionally limited to enumeration and safe read-style operations.
 * The dongle must be bound to WinUSB/libusb before libusb can open it.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN   /* keep windows.h from pulling in winsock.h v1 (wintun.h needs winsock2.h) */
#include <windows.h>
#include <winsock2.h>         /* struct timeval, used by the TX-pacing RX pump in send_80211_frame */
#endif
#include "libusb.h"
#ifndef _WIN32
#include "linux_compat.h"
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if_tun.h>
static int g_tap_fd = -1;            /* TAP fd for the DS<->internet data bridge (NWC_DATAPATH) */
static void tiny_sleep(void);
static int mgmt_ie_offset(const uint8_t *frame, int frame_len);
#endif


#ifdef NWC_BACKEND_KMDF
/*
 * KMDF driver backend. When built with -DNWC_BACKEND_KMDF the four transport
 * leaves below (vendor_read, vendor_write, send_80211_frame's bulk-OUT, and
 * poll_rx's bulk-IN) talk to the in-kernel driver nwcusbap.sys via IOCTLs on
 * \\.\NWCUSBAP instead of libusb/WinUSB. Everything else — the entire proven
 * init / RF / beacon / auth / assoc / WEP SoftAP logic — is byte-for-byte
 * identical. libusb.h is still included for its types and constants (the opaque
 * handle stays a libusb_device_handle*), but no libusb symbol is linked in a
 * KMDF build; the kernel driver owns the dongle, so libusb cannot open it.
 */
#include "kmdf/nwcusbap_ioctl.h"
/* Cosmetic-only libusb symbol still referenced from a few KMDF-compiled error
 * prints (e.g. read_eeprom). The kernel driver owns the dongle so libusb is not
 * linked; shim it to a static string so no libusb import remains. */
static const char *nwc_libusb_err(int e) { (void)e; return "(kmdf: no libusb)"; }
#define libusb_error_name nwc_libusb_err
static HANDLE g_kmdf = INVALID_HANDLE_VALUE;
static void kmdf_postinit(void)
{
    DWORD ret = 0;
    if (DeviceIoControl(g_kmdf, IOCTL_NWC_POSTINIT, NULL, 0, NULL, 0, &ret, NULL))
        printf("[kmdf] post-init pipe kick OK (reader restarted against live RX engine)\n");
    else
        printf("[kmdf] post-init pipe kick failed gle=%lu\n", GetLastError());
}
static void kmdf_print_stats(const char *tag)
{
    NWC_STATS s;
    DWORD ret = 0;
    if (DeviceIoControl(g_kmdf, IOCTL_NWC_STATS, NULL, 0, &s, sizeof(s), &ret, NULL) &&
        ret >= sizeof(s)) {
        printf("[stats%s] rx_complete=%u rx_bytes=%u rx_drop=%u rx_failed=%u tx=%u tx_fail=%u "
               "reader_cfg=0x%08lx last_reader=0x%08lx usbd=0x%08x last_tx=0x%08lx pipes=0x%x\n",
               tag, s.rx_complete, s.rx_bytes, s.rx_dropped, s.rx_failed, s.tx_count, s.tx_fail,
               (unsigned long)s.reader_config_status, (unsigned long)s.last_reader_status,
               s.last_usbd_status, (unsigned long)s.last_tx_status, s.pipe_flags);
        printf("[reset%s] usb_reset=0x%08lx  CSR12-17 before=%04x %04x %04x %04x %04x %04x  after=%04x %04x %04x %04x %04x %04x\n",
               tag, (unsigned long)s.reset_status,
               s.csr12_17_before[0], s.csr12_17_before[1], s.csr12_17_before[2],
               s.csr12_17_before[3], s.csr12_17_before[4], s.csr12_17_before[5],
               s.csr12_17_after[0], s.csr12_17_after[1], s.csr12_17_after[2],
               s.csr12_17_after[3], s.csr12_17_after[4], s.csr12_17_after[5]);
    } else {
        printf("[stats%s] IOCTL_NWC_STATS failed gle=%lu ret=%lu\n", tag, GetLastError(), ret);
    }
}
#endif

#define NWCUSB_VID 0x0411
#define NWCUSB_PID 0x008b

#define USB_VENDOR_REQUEST_IN  (LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE)
#define USB_VENDOR_REQUEST_OUT (LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE)
#define USB_DEVICE_MODE 1
#define USB_SINGLE_WRITE 2
#define USB_EEPROM_READ 9
#define USB_MULTI_READ 7
#define USB_MULTI_WRITE 6

#define USB_MODE_TEST 4

#define MAC_CSR0  0x0400
#define MAC_CSR1  0x0402
#define MAC_CSR8  0x0410
#define MAC_CSR9  0x0412
#define MAC_CSR11 0x0416
#define MAC_CSR13 0x041a
#define MAC_CSR14 0x041c
#define MAC_CSR15 0x041e
#define MAC_CSR16 0x0420
#define MAC_CSR17 0x0422
#define MAC_CSR18 0x0424
#define MAC_CSR20 0x0428
#define MAC_CSR21 0x042a
#define MAC_CSR22 0x042c
#define TXRX_CSR0 0x0440
#define TXRX_CSR1 0x0442
#define TXRX_CSR2 0x0444
#define TXRX_CSR3 0x0446
#define TXRX_CSR4 0x0448
#define TXRX_CSR5 0x044a
#define TXRX_CSR6 0x044c
#define TXRX_CSR7 0x044e
#define TXRX_CSR8 0x0450
#define TXRX_CSR10 0x0454
#define TXRX_CSR11 0x0456
#define TXRX_CSR18 0x0464
#define TXRX_CSR19 0x0466
#define TXRX_CSR20 0x0468
#define TXRX_CSR21 0x046a
#define SEC_CSR0   0x0480
#define PHY_CSR2  0x04c4
#define PHY_CSR4  0x04c8
#define PHY_CSR5  0x04ca
#define PHY_CSR6  0x04cc
#define PHY_CSR7  0x04ce
#define PHY_CSR8  0x04d0
#define PHY_CSR9  0x04d2
#define PHY_CSR10 0x04d4
#define STA_CSR5  0x04ea   /* hardware "beacon sent" counter — proxy for TSF/beacon engine alive */

#define BBP_DESIRE_STATE_AWAKE 0x0006
#define RF_DESIRE_STATE_AWAKE  0x0018
#define SET_STATE_TRIGGER      0x0001

#define REGISTER_USB_BUSY_COUNT 20
#define BULK_OUT_EP 0x01
#define BULK_IN_EP  0x81
#define VERBOSE_USB_REG 0

static void hexdump(const uint8_t *data, int len)
{
    for (int i = 0; i < len; i++) {
        if ((i % 16) == 0)
            printf("%04x: ", i);
        printf("%02x ", data[i]);
        if ((i % 16) == 15 || i == len - 1)
            printf("\n");
    }
}

static void copy_ssid(char *ssid, size_t ssid_cap, const uint8_t *frame, int frame_len);

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xff);
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xff);
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool mac_equal(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

static bool mac_broadcast(const uint8_t a[6])
{
    static const uint8_t bc[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    return mac_equal(a, bc);
}

/* Is this MAC's OUI registered to Nintendo? Covers the DS / DSi / 2DS / 3DS handheld
 * families and the Wii / Wii U (which use the same connector/WFC path). Used as an
 * optional client allow-list so the AP only handshakes with genuine Nintendo systems.
 * NOTE: an OUI check is defense-in-depth only -- MAC addresses are spoofable, so this
 * stops casual/accidental non-Nintendo clients, not a determined attacker. The real
 * gate is the proprietary connector registration + WEP-shared-key handshake, which
 * ordinary Wi-Fi clients do not speak. This list is not exhaustive (Nintendo holds
 * 100+ OUIs); NWC_ANY_CLIENT=1 disables the check if a legitimate unit is rejected. */
static bool is_nintendo_mac(const uint8_t m[6])
{
    static const uint32_t oui[] = {
        0x0009BF,0x001656,0x0017AB,0x00191D,0x0019FD,0x001AE9,0x001B7A,0x001BEA,
        0x001CBE,0x001DBC,0x001E35,0x001F32,0x001FC5,0x002147,0x0021BD,0x00224C,
        0x0022AA,0x0022D7,0x002331,0x0023CC,0x00241E,0x002444,0x0024F3,0x0025A0,
        0x002659,0x002709,0x0403D6,0x182A7B,0x2C10C1,0x34AF2C,0x40D28A,0x40F407,
        0x58BDA3,0x5C521E,0x606BFF,0x64B5C6,0x78A2A0,0x7CBB8A,0x8C56C5,0x98415C,
        0x98B6E9,0x9CE635,0xA438CC,0xA45C27,0xA4C0E1,0xB88AEC,0xB8AE6E,0xCC9E00,
        0xCCFB65,0xD86BF7,0xDC68EB,0xE00C7F,0xE0E751,0xE84ECE,0xE8DA20,0xECC40D
    };
    uint32_t o = ((uint32_t)m[0] << 16) | ((uint32_t)m[1] << 8) | m[2];
    for (unsigned i = 0; i < sizeof(oui)/sizeof(oui[0]); i++)
        if (oui[i] == o) return true;
    return false;
}

/* Enforce the Nintendo-only client allow-list? On by default; NWC_ANY_CLIENT=1 turns
 * it off (accept any client that speaks the connector protocol, like the original). */
static bool nintendo_only(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("NWC_ANY_CLIENT"); v = (e && *e && *e != '0') ? 0 : 1; }
    return v;
}

static bool contains_ascii_nocase(const uint8_t *data, int len, const char *needle)
{
    size_t n = strlen(needle);
    if (n == 0 || len < (int)n)
        return false;
    for (int i = 0; i <= len - (int)n; i++) {
        size_t j;
        for (j = 0; j < n; j++) {
            uint8_t a = data[i + (int)j];
            uint8_t b = (uint8_t)needle[j];
            if (a >= 'A' && a <= 'Z')
                a = (uint8_t)(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z')
                b = (uint8_t)(b + ('a' - 'A'));
            if (a != b)
                break;
        }
        if (j == n)
            return true;
    }
    return false;
}

static bool contains_utf16le_ascii_nocase(const uint8_t *data, int len, const char *needle)
{
    size_t n = strlen(needle);
    if (n == 0 || len < (int)(n * 2))
        return false;
    for (int i = 0; i <= len - (int)(n * 2); i++) {
        size_t j;
        for (j = 0; j < n; j++) {
            uint8_t a = data[i + (int)j * 2];
            uint8_t z = data[i + (int)j * 2 + 1];
            uint8_t b = (uint8_t)needle[j];
            if (z != 0)
                break;
            if (a >= 'A' && a <= 'Z')
                a = (uint8_t)(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z')
                b = (uint8_t)(b + ('a' - 'A'));
            if (a != b)
                break;
        }
        if (j == n)
            return true;
    }
    return false;
}

static bool contains_registration_hint(const uint8_t *data, int len)
{
    return contains_ascii_nocase(data, len, "Nintendo") ||
           contains_utf16le_ascii_nocase(data, len, "Nintendo") ||
           contains_ascii_nocase(data, len, "NWCUSB") ||
           contains_utf16le_ascii_nocase(data, len, "NWCUSB");
}

static void sleep_ms(unsigned int ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    (void)ms;
    tiny_sleep();
#endif
}

/* USB transfer timeout (ms). libusbK occasionally hangs a transfer for the FULL timeout under load,
 * blocking the single-threaded main loop for that long (Linux usbfs never does this). The old 2000ms
 * meant one hung register read/write stalled the loop ~2s -> beacon dark -> DS drops -> auth storm.
 * Cap it (NWC_TX_TIMEOUT, default 250ms) so a hang costs a dropped frame, not the connection. */
static unsigned nwc_usb_timeout(void)
{
    static int t = -1;
    if (t < 0) { const char *e = getenv("NWC_TX_TIMEOUT"); t = (e && *e) ? atoi(e) : 250;
                 if (t < 50) t = 50; if (t > 2000) t = 2000; }
    return (unsigned)t;
}

static int vendor_read(libusb_device_handle *handle, uint8_t request,
                       uint16_t index, void *buffer, uint16_t length,
                       const char *label)
{
#ifdef NWC_BACKEND_KMDF
    (void)handle;
    NWC_CONTROL c;
    DWORD ret = 0;
    c.request = request; c.dir_in = 1; c.value = 0; c.index = index; c.length = length;
    if (VERBOSE_USB_REG)
        printf("[usb] IN request=%u index=0x%04x length=%u label=%s\n",
               request, index, length, label);
    if (!DeviceIoControl(g_kmdf, IOCTL_NWC_CONTROL, &c, sizeof(c),
                         buffer, length, &ret, NULL)) {
        printf("[usb] ERROR %s: DeviceIoControl gle=%lu\n", label, GetLastError());
        return LIBUSB_ERROR_IO;
    }
    if (VERBOSE_USB_REG)
        printf("[usb] OK %s: %lu bytes\n", label, ret);
    return (int)ret;
#else
    if (VERBOSE_USB_REG)
        printf("[usb] IN request=%u index=0x%04x length=%u label=%s\n",
               request, index, length, label);
    int rc = libusb_control_transfer(handle, USB_VENDOR_REQUEST_IN, request,
                                     0, index, (unsigned char *)buffer,
                                     length, nwc_usb_timeout());
    if (rc < 0) {
        printf("[usb] ERROR %s: %s\n", label, libusb_error_name(rc));
        return rc;
    }
    if (VERBOSE_USB_REG)
        printf("[usb] OK %s: %d bytes\n", label, rc);
    return rc;
#endif
}

static int vendor_write(libusb_device_handle *handle, uint8_t request,
                        uint16_t index, uint16_t value,
                        const void *buffer, uint16_t length,
                        const char *label)
{
#ifdef NWC_BACKEND_KMDF
    (void)handle;
    uint8_t io[sizeof(NWC_CONTROL) + 512];
    NWC_CONTROL *c = (NWC_CONTROL *)io;
    DWORD ret = 0;
    if (length > 512)
        return LIBUSB_ERROR_INVALID_PARAM;
    c->request = request; c->dir_in = 0; c->value = value; c->index = index; c->length = length;
    if (length && buffer)
        memcpy(io + sizeof(NWC_CONTROL), buffer, length);
    if (VERBOSE_USB_REG)
        printf("[usb] OUT request=%u index=0x%04x value=0x%04x length=%u label=%s\n",
               request, index, value, length, label);
    if (!DeviceIoControl(g_kmdf, IOCTL_NWC_CONTROL, io,
                         (DWORD)(sizeof(NWC_CONTROL) + length), NULL, 0, &ret, NULL)) {
        printf("[usb] ERROR %s: DeviceIoControl gle=%lu\n", label, GetLastError());
        return LIBUSB_ERROR_IO;
    }
    /* libusb_control_transfer returns the data-stage byte count on success. */
    return (int)length;
#else
    if (VERBOSE_USB_REG)
        printf("[usb] OUT request=%u index=0x%04x value=0x%04x length=%u label=%s\n",
               request, index, value, length, label);
    int rc = libusb_control_transfer(handle, USB_VENDOR_REQUEST_OUT, request,
                                     value, index, (unsigned char *)buffer,
                                     length, nwc_usb_timeout());
    if (rc < 0) {
        printf("[usb] ERROR %s: %s\n", label, libusb_error_name(rc));
        return rc;
    }
    if (VERBOSE_USB_REG)
        printf("[usb] OK %s: %d bytes\n", label, rc);
    return rc;
#endif
}

static int read16(libusb_device_handle *handle, uint16_t offset, uint16_t *value)
{
    uint8_t buf[2] = {0};
    int rc = vendor_read(handle, USB_MULTI_READ, offset, buf, sizeof(buf), "read16");
    if (rc == 2) {
        *value = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
        if (VERBOSE_USB_REG)
            printf("[reg] read  0x%04x = 0x%04x\n", offset, *value);
        return 0;
    }
    return rc < 0 ? rc : LIBUSB_ERROR_IO;
}

static int write16(libusb_device_handle *handle, uint16_t offset, uint16_t value)
{
    uint8_t buf[2] = { (uint8_t)(value & 0xff), (uint8_t)(value >> 8) };
    int rc = vendor_write(handle, USB_MULTI_WRITE, offset, 0, buf, sizeof(buf), "write16");
    if (rc == 2 || rc == 0) {
        if (VERBOSE_USB_REG)
            printf("[reg] write 0x%04x = 0x%04x\n", offset, value);
        return 0;
    }
    return rc < 0 ? rc : LIBUSB_ERROR_IO;
}

static int write_single(libusb_device_handle *handle, uint8_t request,
                        uint16_t offset, uint16_t value, const char *label)
{
    int rc = vendor_write(handle, request, offset, value, NULL, 0, label);
    return rc < 0 ? rc : 0;
}

static void tiny_sleep(void)
{
#ifdef _WIN32
    Sleep(1);
#else
    usleep(1000);
#endif
}

static int wait_reg_clear(libusb_device_handle *handle, uint16_t offset,
                          uint16_t busy_mask, const char *label,
                          uint16_t *last_value)
{
    uint16_t reg = 0xffff;
    for (int i = 0; i < REGISTER_USB_BUSY_COUNT; i++) {
        int rc = read16(handle, offset, &reg);
        if (rc != 0)
            return rc;
        if ((reg & busy_mask) == 0) {
            if (last_value)
                *last_value = reg;
            return 0;
        }
        tiny_sleep();
    }
    printf("[wait] timeout %s offset=0x%04x last=0x%04x mask=0x%04x\n",
           label, offset, reg, busy_mask);
    if (last_value)
        *last_value = reg;
    return LIBUSB_ERROR_TIMEOUT;
}

static int bbp_write(libusb_device_handle *handle, uint8_t reg_id, uint8_t value)
{
    uint16_t ignored;
    int rc = wait_reg_clear(handle, PHY_CSR8, 0x0001, "bbp", &ignored);
    if (rc != 0)
        return rc;
    uint16_t word = (uint16_t)value | ((uint16_t)(reg_id & 0x7f) << 8);
    if (VERBOSE_USB_REG)
        printf("[bbp] write r%u = 0x%02x\n", reg_id, value);
    return write16(handle, PHY_CSR7, word);
}

static int bbp_read(libusb_device_handle *handle, uint8_t reg_id, uint8_t *value)
{
    uint16_t reg = 0;
    int rc = wait_reg_clear(handle, PHY_CSR8, 0x0001, "bbp", &reg);
    if (rc != 0)
        return rc;
    rc = write16(handle, PHY_CSR7, (uint16_t)0x8000 | ((uint16_t)(reg_id & 0x7f) << 8));
    if (rc != 0)
        return rc;
    rc = wait_reg_clear(handle, PHY_CSR8, 0x0001, "bbp-read", &reg);
    if (rc != 0)
        return rc;
    rc = read16(handle, PHY_CSR7, &reg);
    if (rc != 0)
        return rc;
    *value = (uint8_t)(reg & 0xff);
    if (VERBOSE_USB_REG)
        printf("[bbp] read r%u = 0x%02x\n", reg_id, *value);
    return 0;
}

static uint16_t eeprom_word(const uint8_t *eeprom, uint16_t word_index)
{
    uint16_t offset = (uint16_t)(word_index * 2);
    return (uint16_t)eeprom[offset] | ((uint16_t)eeprom[offset + 1] << 8);
}

static int read_eeprom(libusb_device_handle *handle, uint8_t *eeprom, uint16_t length)
{
    memset(eeprom, 0, length);
    for (int attempt = 1; attempt <= 3; attempt++) {
        int rc = vendor_read(handle, USB_EEPROM_READ, 0, eeprom, length, "eeprom");
        if (rc == length)
            return 0;

        printf("[eeprom] read attempt %d failed: %s\n", attempt,
               rc < 0 ? libusb_error_name(rc) : "short read");
        sleep_ms(50);
    }

    if (length >= 10) {
        /* MAC-AGNOSTIC fallback (no hardcoded device MAC): the RT2570's USB iSerialNumber string
         * descriptor IS this dongle's MAC (e.g. "001601787421" / "00160177A3EB"), so read it and use
         * whatever unit is plugged in. Works for any 0411:008b dongle; nothing device-specific baked in. */
        uint8_t mac[6]; int got = 0;
        libusb_device *dev = libusb_get_device(handle);
        struct libusb_device_descriptor dd;
        if (dev && libusb_get_device_descriptor(dev, &dd) == 0 && dd.iSerialNumber) {
            unsigned char ser[64] = { 0 };
            int sl = libusb_get_string_descriptor_ascii(handle, dd.iSerialNumber, ser, (int)sizeof ser - 1);
            if (sl >= 12) {
                int ok = 1;
                for (int i = 0; i < 6; i++) {
                    unsigned v;
                    if (sscanf((const char *)ser + i * 2, "%2x", &v) == 1) mac[i] = (uint8_t)v;
                    else { ok = 0; break; }
                }
                got = ok;
            }
        }
        if (!got) {
            /* last resort only if the serial is unreadable: a locally-administered address (bit1 of
             * byte0 set) that is NOT any real device -- keeps the AP up without impersonating a unit. */
            static const uint8_t la[6] = { 0x02, 0x16, 0x01, 0x00, 0x00, 0x01 };
            memcpy(mac, la, 6);
        }
        memcpy(eeprom + 4, mac, 6);
        printf("[eeprom] read failed; using %s MAC %02x:%02x:%02x:%02x:%02x:%02x; BBP EEPROM overrides disabled\n",
               got ? "USB-serial" : "locally-administered",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return 0;
    }

    return LIBUSB_ERROR_IO;
}

static int init_bbp(libusb_device_handle *handle, const uint8_t *eeprom)
{
    uint8_t value = 0xff;
    int ready = LIBUSB_ERROR_TIMEOUT;
    for (int i = 0; i < REGISTER_USB_BUSY_COUNT; i++) {
        int rc = bbp_read(handle, 0, &value);
        if (rc == 0 && value != 0xff && value != 0x00) {
            ready = 0;
            break;
        }
        tiny_sleep();
    }
    if (ready != 0) {
        printf("[bbp] BBP did not become ready, last r0=0x%02x\n", value);
        return ready;
    }

    static const uint8_t defaults[][2] = {
        {3, 0x02}, {4, 0x19}, {14, 0x1c}, {15, 0x30},
        {16, 0xac}, {18, 0x18}, {19, 0xff}, {20, 0x1e},
        {21, 0x08}, {22, 0x08}, {23, 0x08}, {24, 0x80},
        {25, 0x50}, {26, 0x08}, {27, 0x23}, {30, 0x10},
        {31, 0x2b}, {32, 0xb9}, {34, 0x12}, {35, 0x50},
        {39, 0xc4}, {40, 0x02}, {41, 0x60}, {53, 0x10},
        {54, 0x18}, {56, 0x08}, {57, 0x10}, {58, 0x08},
        {61, 0x60}, {62, 0x10}, {75, 0xff},
    };

    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        int rc = bbp_write(handle, defaults[i][0], defaults[i][1]);
        if (rc != 0)
            return rc;
    }

    for (uint16_t i = 0; i < 16; i++) {
        uint16_t word = eeprom_word(eeprom, (uint16_t)(0x000e + i));
        if (word != 0xffff && word != 0x0000) {
            uint8_t bbp_value = (uint8_t)(word & 0xff);
            uint8_t reg_id = (uint8_t)(word >> 8);
            int rc = bbp_write(handle, reg_id, bbp_value);
            if (rc != 0)
                return rc;
        }
    }

    /* BBP R17 = CCA / RX-sensitivity threshold. The reset default is very sensitive, so ambient
     * WiFi keeps the RT2570's carrier-sense asserted (CCA jam) and the beacon/TX engine backs off
     * until it stalls. rt2500usb tunes R17 dynamically against the false-CCA count; we don't, so set
     * a higher (less twitchy) fixed threshold. The DS is a strong, close signal so reduced RX
     * sensitivity is harmless. NWC_BBP_R17 overrides (e.g. 0x28..0x40); 0 keeps the reset default. */
    { const char *e = getenv("NWC_BBP_R17");
      long r17 = (e && *e) ? strtol(e, NULL, 0) : 0x38;
      if (r17 > 0) { bbp_write(handle, 17, (uint8_t)r17);
          printf("[bbp] R17 CCA/RX-sensitivity threshold = 0x%02x\n", (unsigned)(r17 & 0xff)); } }

    printf("[bbp] init complete\n");
    return 0;
}

static int rf_write(libusb_device_handle *handle, uint8_t rf_id, uint32_t value)
{
    uint16_t ignored;
    int rc = wait_reg_clear(handle, PHY_CSR10, 0x8000, "rf", &ignored);
    if (rc != 0)
        return rc;

    if (VERBOSE_USB_REG)
        printf("[rf] write r%u = 0x%08x\n", rf_id, value);
    rc = write16(handle, PHY_CSR9, (uint16_t)(value & 0xffff));
    if (rc != 0)
        return rc;
    uint16_t high = (uint16_t)((value >> 16) & 0x00ff);
    high |= (uint16_t)(20 << 8);
    high |= 0x8000;
    return write16(handle, PHY_CSR10, high);
}

struct rf_channel {
    uint8_t channel;
    uint32_t rf1;
    uint32_t rf2;
    uint32_t rf3;
    uint32_t rf4;
};

static const struct rf_channel rf2525e_channels[] = {
    { 1,  0x00022010, 0x0000089a, 0x00060111, 0x00000e1b },
    { 2,  0x00022010, 0x0000089e, 0x00060111, 0x00000e07 },
    { 3,  0x00022010, 0x0000089e, 0x00060111, 0x00000e1b },
    { 4,  0x00022010, 0x000008a2, 0x00060111, 0x00000e07 },
    { 5,  0x00022010, 0x000008a2, 0x00060111, 0x00000e1b },
    { 6,  0x00022010, 0x000008a6, 0x00060111, 0x00000e07 },
    { 7,  0x00022010, 0x000008a6, 0x00060111, 0x00000e1b },
    { 8,  0x00022010, 0x000008aa, 0x00060111, 0x00000e07 },
    { 9,  0x00022010, 0x000008aa, 0x00060111, 0x00000e1b },
    { 10, 0x00022010, 0x000008ae, 0x00060111, 0x00000e07 },
    { 11, 0x00022010, 0x000008ae, 0x00060111, 0x00000e1b },
    { 12, 0x00022010, 0x000008b2, 0x00060111, 0x00000e07 },
    { 13, 0x00022010, 0x000008b2, 0x00060111, 0x00000e1b },
    { 14, 0x00022010, 0x000008b6, 0x00060111, 0x00000e23 },
};

static const struct rf_channel *find_rf2525e_channel(uint8_t channel)
{
    for (size_t i = 0; i < sizeof(rf2525e_channels) / sizeof(rf2525e_channels[0]); i++) {
        if (rf2525e_channels[i].channel == channel)
            return &rf2525e_channels[i];
    }
    return &rf2525e_channels[0];
}

static int config_channel_rf2525e(libusb_device_handle *handle, uint8_t channel, uint8_t txpower)
{
    const struct rf_channel *base = find_rf2525e_channel(channel);
    uint32_t rf1 = base->rf1;
    uint32_t rf2 = base->rf2;
    uint32_t rf3 = base->rf3;
    uint32_t rf4 = base->rf4;

    if (txpower > 31)
        txpower = 24;
    rf3 &= ~0x00003e00u;
    rf3 |= ((uint32_t)txpower << 9) & 0x00003e00u;

    static const uint32_t rf2525e_pretune[] = {
        0x000008aa, 0x000008ae, 0x000008ae, 0x000008b2,
        0x000008b2, 0x000008b6, 0x000008b6, 0x000008ba,
        0x000008ba, 0x000008be, 0x000008b7, 0x00000902,
        0x00000902, 0x00000906
    };

    uint16_t rx = 0;
    read16(handle, TXRX_CSR2, &rx);
    write16(handle, TXRX_CSR2, (uint16_t)(rx | 0x0001));
    write16(handle, TXRX_CSR19, 0x0000);

    int rc = rf_write(handle, 2, rf2525e_pretune[base->channel - 1]);
    if (rc != 0)
        return rc;
    rc = rf_write(handle, 4, rf4);
    if (rc != 0)
        return rc;
    rc = rf_write(handle, 1, rf1);
    if (rc != 0)
        return rc;
    rc = rf_write(handle, 2, rf2);
    if (rc != 0)
        return rc;
    rc = rf_write(handle, 3, rf3);
    if (rc != 0)
        return rc;
    rc = rf_write(handle, 4, rf4);
    if (rc == 0) {
        write16(handle, TXRX_CSR2, rx);
        printf("[rf] channel %u RF2525E programmed\n", base->channel);
    }
    return rc;
}

/*
 * Configure the TX/RX antenna path. This was entirely missing from the native
 * probe, which is almost certainly why the radio never actually emitted RF: the
 * RF2525E transmitter requires a TX I/Q flip (BBP R2 + PHY_CSR5/CSR6) and an
 * explicit TX/RX antenna selection. Without it the PHY drives the wrong/uncal-
 * ibrated path and nothing radiates. Ported from rt2500usb_config_ant() using
 * the per-device antenna defaults stored in EEPROM_ANTENNA (word 0x0b).
 *
 * rt2x00 antenna enum: 0 = SW diversity, 1 = antenna A, 2 = antenna B,
 * 3 = HW diversity.
 */
static int config_ant_rf2525e(libusb_device_handle *handle, const uint8_t *eeprom)
{
    uint16_t antword = eeprom_word(eeprom, 0x000b);
    unsigned tx = (antword >> 2) & 0x3;   /* EEPROM_ANTENNA_TX_DEFAULT (0x000c) */
    unsigned rx = (antword >> 4) & 0x3;   /* EEPROM_ANTENNA_RX_DEFAULT (0x0030) */
    /* rt2x00lib resolves SW-diversity defaults to a concrete antenna; use B. */
    if (tx == 0) tx = 2;
    if (rx == 0) rx = 2;

    uint8_t r2 = 0, r14 = 0;
    uint16_t csr5 = 0, csr6 = 0;
    bbp_read(handle, 2, &r2);
    bbp_read(handle, 14, &r14);
    read16(handle, PHY_CSR5, &csr5);
    read16(handle, PHY_CSR6, &csr6);

    /* TX antenna -> BBP_R2_TX_ANTENNA (0x03) + PHY_CSR5_CCK/PHY_CSR6_OFDM (0x03). */
    uint8_t tx_sel = (tx == 3) ? 1 : (tx == 1) ? 0 : 2;   /* HWdiv=1, A=0, B=2 */
    r2 = (uint8_t)((r2 & ~0x03) | tx_sel);
    csr5 = (uint16_t)((csr5 & ~0x0003u) | tx_sel);
    csr6 = (uint16_t)((csr6 & ~0x0003u) | tx_sel);

    /* RX antenna -> BBP_R14_RX_ANTENNA (0x03). */
    uint8_t rx_sel = (rx == 3) ? 1 : (rx == 1) ? 0 : 2;
    r14 = (uint8_t)((r14 & ~0x03) | rx_sel);

    /* RF2525E requires TX I/Q flip; it does NOT need RX I/Q flip. */
    r2 |= 0x04;                        /* BBP_R2_TX_IQ_FLIP */
    csr5 |= 0x0004;                    /* PHY_CSR5_CCK_FLIP */
    csr6 |= 0x0004;                    /* PHY_CSR6_OFDM_FLIP */
    r14 = (uint8_t)(r14 & ~0x04);      /* BBP_R14_RX_IQ_FLIP = 0 */

    int rc;
    if ((rc = bbp_write(handle, 2, r2)) != 0) return rc;
    if ((rc = bbp_write(handle, 14, r14)) != 0) return rc;
    if ((rc = write16(handle, PHY_CSR5, csr5)) != 0) return rc;
    if ((rc = write16(handle, PHY_CSR6, csr6)) != 0) return rc;
    printf("[ant] RF2525E antenna configured tx=%u rx=%u r2=0x%02x r14=0x%02x csr5=0x%04x csr6=0x%04x\n",
           tx, rx, r2, r14, csr5, csr6);
    return 0;
}

static int write_mac_words(libusb_device_handle *handle, uint16_t base, const uint8_t mac[6])
{
    int rc = write16(handle, base + 0, (uint16_t)mac[0] | ((uint16_t)mac[1] << 8));
    if (rc != 0)
        return rc;
    rc = write16(handle, base + 2, (uint16_t)mac[2] | ((uint16_t)mac[3] << 8));
    if (rc != 0)
        return rc;
    return write16(handle, base + 4, (uint16_t)mac[4] | ((uint16_t)mac[5] << 8));
}

static uint32_t msvcrt_rand_step(uint32_t *seed)
{
    *seed = (*seed * 0x343fdu) + 0x269ec3u;
    return (*seed >> 16) & 0x7fffu;
}

static void make_original_ssid(char ssid[33], const uint8_t mac[6])
{
    (void)mac;
    static bool seeded = false;
    static uint32_t values[4];

    if (!seeded) {
        uint32_t seed = (uint32_t)time(NULL);
#ifdef _WIN32
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        seed ^= ft.dwLowDateTime ^ ft.dwHighDateTime;
#endif
        seed %= 10000000u;
        for (int i = 0; i < 4; i++)
            values[i] = msvcrt_rand_step(&seed);
        seeded = true;
    }

    snprintf(ssid, 33, "NWCUSBAP    %05u%05u%05u%05u",
             values[0], values[1], values[2], values[3]);
    ssid[32] = '\0';
}

static void derive_original_wep_key(const char ssid[33], uint8_t key[13])
{
    static const uint8_t subst[16] = {
        0x0a, 0x0d, 0x0e, 0x08, 0x09, 0x03, 0x06, 0x00,
        0x0c, 0x05, 0x02, 0x07, 0x0b, 0x01, 0x0f, 0x04
    };
    static const uint8_t perm[13] = {
        0x05, 0x01, 0x0c, 0x04, 0x02, 0x03, 0x0a,
        0x00, 0x0b, 0x07, 0x09, 0x08, 0x06
    };
    static const uint8_t mask_a[13] = {
        'g', 'w', 'i', '\'', '6', '&', 'f', 's', '=', '0', 'N', 'f', '~'
    };
    static const uint8_t mask_b[13] = {
        '%', '(', 'e', 'g', 'E', 'r', ')', 'a', 'g', '(', 's', '&', 'm'
    };
    uint8_t tmp[13];

    for (int i = 0; i < 13; i++)
        key[i] = ((const uint8_t *)ssid)[i] ^ ((const uint8_t *)ssid)[13 + (i % 7)];
    for (int i = 0; i < 7; i++)
        key[3 + i] ^= ((const uint8_t *)ssid)[13 + i];
    for (int i = 0; i < 13; i++)
        key[i] ^= mask_a[i];

    memcpy(tmp, key, sizeof(tmp));
    for (int i = 0; i < 13; i++)
        key[perm[i]] = tmp[i];

    for (int i = 0; i < 13; i++) {
        key[i] ^= mask_b[i];
        key[i] = (uint8_t)((subst[key[i] >> 4] << 4) | subst[key[i] & 0x0f]);
    }

    key[0] ^= key[6];
    key[3] ^= key[9];
    key[6] = key[3] ^ key[6];
    key[9] ^= key[0];
    key[12] ^= key[0];

    key[1] ^= key[7];
    key[4] ^= key[10];
    key[7] = key[4] ^ key[7];
    key[10] ^= key[1];
    key[12] ^= key[1];

    key[2] ^= key[8];
    key[5] ^= key[11];
    key[8] = key[5] ^ key[8];
    key[11] ^= key[2];
    key[12] ^= key[2];
}

static void print_key_hex(const uint8_t key[13])
{
    for (int i = 0; i < 13; i++)
        printf("%02x", key[i]);
}

static uint32_t crc32_ieee(const uint8_t *data, int len)
{
    uint32_t crc = 0xffffffffu;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void rc4_crypt(const uint8_t *key, int key_len, const uint8_t *in, uint8_t *out, int len)
{
    uint8_t s[256];
    uint8_t i = 0;
    uint8_t j = 0;

    for (int n = 0; n < 256; n++)
        s[n] = (uint8_t)n;
    for (int n = 0; n < 256; n++) {
        j = (uint8_t)(j + s[n] + key[n % key_len]);
        uint8_t tmp = s[n];
        s[n] = s[j];
        s[j] = tmp;
    }

    i = 0;
    j = 0;
    for (int n = 0; n < len; n++) {
        i = (uint8_t)(i + 1);
        j = (uint8_t)(j + s[i]);
        uint8_t tmp = s[i];
        s[i] = s[j];
        s[j] = tmp;
        out[n] = (uint8_t)(in[n] ^ s[(uint8_t)(s[i] + s[j])]);
    }
}

static bool wep_decrypt_body(const uint8_t *body, int body_len, const uint8_t key[13],
                             uint8_t *plain, int plain_cap, int *plain_len)
{
    uint8_t rc4_key[16];
    uint8_t decrypted[256];
    int crypt_len;
    int data_len;
    uint32_t got_icv;
    uint32_t calc_icv;

    *plain_len = 0;
    if (body_len < 8)
        return false;
    crypt_len = body_len - 4;
    if (crypt_len > (int)sizeof(decrypted) || crypt_len < 4)
        return false;

    rc4_key[0] = body[0];
    rc4_key[1] = body[1];
    rc4_key[2] = body[2];
    memcpy(rc4_key + 3, key, 13);
    rc4_crypt(rc4_key, sizeof(rc4_key), body + 4, decrypted, crypt_len);

    data_len = crypt_len - 4;
    got_icv = get32(decrypted + data_len);
    calc_icv = crc32_ieee(decrypted, data_len);
    if (got_icv != calc_icv) {
        printf("[wep] ICV mismatch got=%08x calc=%08x iv=%02x%02x%02x keyid=%u\n",
               got_icv, calc_icv, body[0], body[1], body[2], body[3] >> 6);
        return false;
    }
    if (data_len > plain_cap)
        return false;
    memcpy(plain, decrypted, data_len);
    *plain_len = data_len;
    return true;
}

static int config_wep128_key0(libusb_device_handle *handle, const uint8_t key[13])
{
    uint8_t material[16] = {0};
    memcpy(material, key, 13);
    printf("[wep] programming hardware WEP128 key slot 0: ");
    print_key_hex(key);
    printf("\n");

    for (int i = 0; i < 8; i++) {
        uint16_t word = (uint16_t)material[i * 2] | ((uint16_t)material[i * 2 + 1] << 8);
        int rc = write16(handle, (uint16_t)(SEC_CSR0 + i * 2), word);
        if (rc != 0)
            return rc;
    }

    uint16_t reg = 0;
    int rc = read16(handle, TXRX_CSR0, &reg);
    if (rc != 0)
        return rc;
    reg &= 0xe000u;
    reg |= 0x0002;                 /* CIPHER_WEP128 */
    reg |= (uint16_t)(24u << 3);   /* IV starts after 24-byte 802.11 header. */
    reg |= (uint16_t)(1u << 9);    /* key slot 0 valid. */
    return write16(handle, TXRX_CSR0, reg);
}

static size_t build_beacon(uint8_t *out, size_t out_cap, const uint8_t mac[6], const char *ssid,
                           uint8_t channel, bool privacy)
{
    uint8_t *p = out;
    size_t ssid_len = strlen(ssid);
    if (ssid_len > 32)
        ssid_len = 32;
    if (out_cap < 128)
        return 0;

    put16(p, 0x0080); p += 2;          /* beacon */
    put16(p, 0x0000); p += 2;          /* duration */
    memset(p, 0xff, 6); p += 6;        /* DA */
    memcpy(p, mac, 6); p += 6;         /* SA */
    memcpy(p, mac, 6); p += 6;         /* BSSID */
    put16(p, 0x0000); p += 2;          /* seq */
    memset(p, 0x00, 8); p += 8;        /* timestamp */
    put16(p, 100); p += 2;             /* beacon interval */
    put16(p, privacy ? 0x0011 : 0x0001); p += 2; /* ESS, optionally privacy */

    *p++ = 0; *p++ = (uint8_t)ssid_len;
    memcpy(p, ssid, ssid_len); p += ssid_len;

    *p++ = 1; *p++ = 4;                /* supported rates: 1/2 basic, 5.5/11 optional */
    *p++ = 0x82; *p++ = 0x84; *p++ = 0x0b; *p++ = 0x16;

    *p++ = 3; *p++ = 1; *p++ = channel;

    *p++ = 5; *p++ = 4;                /* TIM */
    *p++ = 0; *p++ = 1; *p++ = 0; *p++ = 0;

    return (size_t)(p - out);
}

static size_t build_probe_response(uint8_t *out, size_t out_cap, const uint8_t mac[6],
                                   const uint8_t dst[6], const char *ssid, uint8_t channel,
                                   bool privacy)
{
    uint8_t *p = out;
    size_t ssid_len = strlen(ssid);
    if (ssid_len > 32)
        ssid_len = 32;
    if (out_cap < 128)
        return 0;

    put16(p, 0x0050); p += 2;          /* probe response */
    put16(p, 0x0000); p += 2;
    memcpy(p, dst, 6); p += 6;
    memcpy(p, mac, 6); p += 6;
    memcpy(p, mac, 6); p += 6;
    put16(p, 0x0000); p += 2;
    memset(p, 0x00, 8); p += 8;
    put16(p, 100); p += 2;
    put16(p, privacy ? 0x0011 : 0x0001); p += 2;

    *p++ = 0; *p++ = (uint8_t)ssid_len;
    memcpy(p, ssid, ssid_len); p += ssid_len;

    *p++ = 1; *p++ = 4;
    *p++ = 0x82; *p++ = 0x84; *p++ = 0x0b; *p++ = 0x16;

    *p++ = 3; *p++ = 1; *p++ = channel;

    return (size_t)(p - out);
}

static size_t build_auth_response(uint8_t *out, size_t out_cap, const uint8_t mac[6],
                                  const uint8_t dst[6], uint16_t auth_alg,
                                  uint16_t auth_seq, uint16_t status,
                                  bool include_challenge)
{
    uint8_t *p = out;
    if (out_cap < 160)
        return 0;

    put16(p, 0x00b0); p += 2;          /* authentication */
    put16(p, 0x0000); p += 2;
    memcpy(p, dst, 6); p += 6;
    memcpy(p, mac, 6); p += 6;
    memcpy(p, mac, 6); p += 6;
    put16(p, 0x0000); p += 2;
    put16(p, auth_alg); p += 2;        /* open-system or shared-key */
    put16(p, auth_seq); p += 2;
    put16(p, status); p += 2;
    if (include_challenge) {
        *p++ = 16;                     /* Challenge Text */
        *p++ = 128;
        for (uint8_t i = 0; i < 128; i++)
            *p++ = (uint8_t)(0xa5u ^ i);
    }
    return (size_t)(p - out);
}

static size_t build_assoc_response(uint8_t *out, size_t out_cap, const uint8_t mac[6],
                                   const uint8_t dst[6], uint16_t aid, bool reassoc,
                                   bool privacy)
{
    uint8_t *p = out;
    if (out_cap < 48)
        return 0;

    put16(p, reassoc ? 0x0030 : 0x0010); p += 2;
    put16(p, 0x0000); p += 2;
    memcpy(p, dst, 6); p += 6;
    memcpy(p, mac, 6); p += 6;
    memcpy(p, mac, 6); p += 6;
    put16(p, 0x0000); p += 2;
    put16(p, privacy ? 0x0011 : 0x0001); p += 2;
    put16(p, 0x0000); p += 2;          /* successful */
    put16(p, (uint16_t)(0xc000 | (aid & 0x3fff))); p += 2;

    *p++ = 1; *p++ = 4;                /* supported rates: 1/2 basic, 5.5/11 optional */
    *p++ = 0x82; *p++ = 0x84; *p++ = 0x0b; *p++ = 0x16;

    return (size_t)(p - out);
}

/* forward ref: the async-RX libusb context (defined near ap_loop), so the TX pacing gap can pump
 * RX completions instead of starving them while it waits. */
static libusb_context *g_ctx;

/* A boolean env flag is ON only if set to a non-empty value that isn't "0"/"false"/"no".
 * (Plain presence-checks treated NWC_FOO="0" as ON, which silently enabled the self-test flood.) */
static int env_on(const char *name) {
    const char *v = getenv(name);
    return v && *v && strcmp(v, "0") != 0 && strcmp(v, "false") != 0 && strcmp(v, "no") != 0;
}
static int nwc_quiet(void);   /* fwd decl: defined below, used by the TX/heartbeat log gates above it */

/* ---------------- Async TX pool (non-blocking bulk-OUT) ----------------------------------------
 * The blocking libusb_bulk_transfer serialises the main loop during a DS data burst on libusbK,
 * which stalls the beacon + RX servicing and makes the dongle "go dark" (proven by the NUC/libusb
 * comparison: same idle CCA, but Windows' loop stretches under load). Submit bulk-OUT transfers
 * asynchronously from a recycled pool so the loop NEVER blocks on TX -- exactly how the Linux
 * kernel path behaves. Completions are driven by the main loop's libusb_handle_events (+ the RX-
 * drain pump). NWC_SYNC_TX forces the old blocking path for A/B. */
#ifndef NWC_BACKEND_KMDF
#define TX_POOL_SLOTS 256
struct nwc_tx_slot { struct libusb_transfer *xfer; uint8_t buf[20 + 1600 + 8]; volatile int busy; };
static struct nwc_tx_slot g_txpool[TX_POOL_SLOTS];
static int g_txpool_ready = 0;
static unsigned long g_tx_dropped = 0;
static void LIBUSB_CALL nwc_tx_cb(struct libusb_transfer *t) { ((struct nwc_tx_slot*)t->user_data)->busy = 0; }
static int nwc_txpool_init(void) {
    for (int i = 0; i < TX_POOL_SLOTS; i++) {
        g_txpool[i].xfer = libusb_alloc_transfer(0);
        if (!g_txpool[i].xfer) return -1;
        g_txpool[i].busy = 0;
    }
    g_txpool_ready = 1; return 0;
}
/* Queue `len` bytes on BULK_OUT_EP without blocking. 0=submitted, <0=libusb error, 1=pool full. */
static int nwc_tx_async(libusb_device_handle *h, const uint8_t *data, int len) {
    int i; struct nwc_tx_slot *s = NULL;
    /* Reap completed/timed-out transfers FIRST (non-blocking). On libusbK the
     * submit latency climbs sharply once many URBs are outstanding in the driver
     * queue: during the DS re-assoc storm the pool saturates (queued~129), the
     * loop gets stuck IN libusb_submit_transfer (200-800ms each) so it never
     * reaches the main loop's handle_events to drain them -> the server->DS GPCM
     * challenge is never delivered -> DS times out -> 61010 + more storm. Draining
     * here keeps both our slot pool and the driver queue free so submit stays ~1ms.
     * RX callbacks only enqueue to g_rxq (same thread) so this is reentrancy-safe. */
    unsigned long _t0 = GetTickCount();
    if (g_ctx) { struct timeval z = {0,0}; libusb_handle_events_timeout_completed(g_ctx, &z, NULL); }
    unsigned long _t1 = GetTickCount();
    if (_t1 - _t0 > 80) printf("[stall] tx-reap %lums\n", _t1 - _t0);
    for (i = 0; i < TX_POOL_SLOTS; i++) if (!g_txpool[i].busy) { s = &g_txpool[i]; break; }
    if (!s) return 1;
    if (len > (int)sizeof s->buf) len = (int)sizeof s->buf;
    s->busy = 1; memcpy(s->buf, data, (size_t)len);
    /* Async TX timeout capped to nwc_usb_timeout() (default 250ms), NOT 2000ms.
     * On libusbK the driver holds a bounded number of outstanding bulk-OUT URBs;
     * when the DS stops ACKing (link dropping mid-connection) a transfer hangs to
     * its timeout, saturating that queue, so libusb_submit_transfer() itself blocks
     * ~240ms waiting for a free slot -> each mgmt-response submit stalls -> 24 of
     * them stack into a ~5.8s rxdrain stall -> beacon starved -> DS Data-Abort.
     * A 250ms cap frees a hung slot 8x faster; a healthy bulk-OUT completes in ~1ms
     * so this never clips a good transfer. Software retransmit covers the rest. */
    libusb_fill_bulk_transfer(s->xfer, h, BULK_OUT_EP, s->buf, len, nwc_tx_cb, s,
                              nwc_usb_timeout());
    unsigned long _t2 = GetTickCount();
    int rc = libusb_submit_transfer(s->xfer);
    { unsigned long _t3 = GetTickCount(); if (_t3 - _t2 > 80) printf("[stall] tx-submit %lums\n", _t3 - _t2); }
    if (rc != 0) { s->busy = 0; return rc; }
    return 0;
}
#endif

static int send_80211_frame(libusb_device_handle *handle, const uint8_t *frame,
                            size_t frame_len, bool request_timestamp,
                            bool request_ack, const char *label)
{
    /* Was 512 — too small for bridged DS<->internet DATA frames: a full TCP segment (e.g. the
     * conntest HTTP response, ~536B payload) + SNAP + WEP + 802.11 header exceeds 512 and was
     * silently rejected, so the DS never received downstream data (52203 at the last step). The
     * RT2570 TXD DATABYTE_COUNT is 12 bits (max 4095); 1600 covers a standard 1500-MTU frame. */
    uint8_t packet[20 + 1600 + 4];
    if (frame_len > 1600)
        return LIBUSB_ERROR_INVALID_PARAM;
    memset(packet, 0, sizeof(packet));

    uint32_t w0 = 0;
    if (request_ack) {
        /* RETRY_LIMIT = 0 (was 7). The gold XP capture shows EVERY mgmt response uses
         * retry_limit=0. With 7, a single unacked response hogs the RT2570's SINGLE TX
         * engine through up to 7 retransmit + ACK-timeout cycles (~ms each) — precisely
         * blocking the ~10us SIFS hardware auto-ACK for the DS's next auth seq1, so the
         * DS never sees seq1 acknowledged and loops (fc=0x08b0) -> 51303. NWC_RETRY7
         * restores the old value for A/B testing. */
        /* RETRY_LIMIT stays 0. Tried retry_limit=7 for DATA frames (normal 802.11 reliability):
         * it hangs the RT2570's single TX engine -- the bulk-OUT completion is withheld through
         * the retransmit/ACK-timeout cycles and libusb bulk_transfer returns LIBUSB_ERROR_TIMEOUT,
         * so DATA TX fails outright. Software-level retransmit (below) is the only viable path. */
        if (getenv("NWC_RETRY7") && *getenv("NWC_RETRY7"))
            w0 |= 0x00000070u;
        w0 |= 0x00000200u; /* ACK required */
    }
    if (request_timestamp)
        w0 |= 0x00000400u;
    w0 |= 0x00001000u; /* new seq */
    w0 |= ((uint32_t)frame_len & 0x0fff) << 16;
    uint32_t w1 = 0;
    w1 |= (2u << 6);   /* AIFS */
    w1 |= (4u << 8);   /* CWMIN */
    w1 |= (10u << 12); /* CWMAX */
    uint32_t data_len = (uint32_t)frame_len + 4u; /* 802.11 FCS is transmitted by hardware. */
    uint32_t plcp_duration = data_len * 8u;       /* 1 Mbps CCK, expressed in usec. */
    uint32_t w2 = 0;
    w2 |= 0x00;        /* 1 Mbps CCK */
    w2 |= 0x04u << 8;
    w2 |= (plcp_duration & 0xffu) << 16;
    w2 |= ((plcp_duration >> 8) & 0xffu) << 24;

    put32(packet + 0, w0);
    put32(packet + 4, w1);
    put32(packet + 8, w2);
    memcpy(packet + 20, frame, frame_len);

    int transfer_len = (int)(20 + frame_len);
    if (transfer_len & 1)
        transfer_len++;
    if ((transfer_len % 512) == 0)
        transfer_len += 2;

    /*
     * The RT2570 bulk-OUT TX path over user-mode libusb REQUIRES the two-part
     * transfer: a 1-byte "guardian" transfer first, then the descriptor+frame.
     * Empirically, dropping the guardian for normal frames makes the frame bulk
     * transfer time out (LIBUSB_ERROR_TIMEOUT) and nothing is transmitted. So we
     * send the guardian for EVERY outgoing frame, not just beacons. (In the
     * kernel rt2500usb driver this guardian is documented only for beacons, but
     * the synchronous user-mode path needs it universally to kick the endpoint.)
     */
    bool quiet = (strstr(label, "beacon") != NULL);        /* any beacon: don't log every ~100ms */
    uint8_t guardian = 0;
    int transferred = 0;
    if (!quiet || VERBOSE_USB_REG)
        printf("[tx] %s frame=%zu transfer=%d ack=%u timestamp=%u\n",
               label, frame_len, transfer_len, request_ack ? 1 : 0, request_timestamp ? 1 : 0);
#ifdef NWC_BACKEND_KMDF
    (void)handle; (void)transferred;
    {
        DWORD ret = 0;
        /* Replicate the guardian-then-frame OUT sequence verbatim through the
         * driver's raw-TX IOCTL (the driver just bulk-writes what it is given). */
        if (!DeviceIoControl(g_kmdf, IOCTL_NWC_TX_FRAME, &guardian, 1, NULL, 0, &ret, NULL)) {
            printf("[tx] %s guardian failed: gle=%lu\n", label, GetLastError());
            return LIBUSB_ERROR_IO;
        }
        if (!DeviceIoControl(g_kmdf, IOCTL_NWC_TX_FRAME, packet, (DWORD)transfer_len,
                             NULL, 0, &ret, NULL)) {
            printf("[tx] %s frame failed: gle=%lu\n", label, GetLastError());
            return LIBUSB_ERROR_IO;
        }
        if (strcmp(label, "beacon") != 0 || VERBOSE_USB_REG)
            printf("[tx] %s sent: %d bytes\n", label, transfer_len);
        return 0;
    }
#else
    /* NWC_NOGUARDIAN: skip the 1-byte guardian kick. If TX still succeeds, our
     * OUT path is a proper autonomous-TX path (like the original) rather than a
     * software-kick-only path — the precondition for the hardware auto-ACK
     * (which has no software kick) to be able to transmit at all. */
    int noguard = (env_on("NWC_NOGUARDIAN"));
    /* usbmon capture of the WORKING original XP driver: the guardian byte precedes ONLY the
     * beacon; data/mgmt frames (incl. the seq2 auth-response, frame 15447) are a bare bulk-OUT
     * with NO guardian. We had been prepending a guardian to every frame — a candidate for why
     * our unicast seq2 never radiates. Match the original: guardian beacons only.
     * NWC_GUARD_ALL restores the old "guardian every frame" behaviour for A/B testing. */
    int guard_all = (env_on("NWC_GUARD_ALL"));
    /* Over-the-air capture (AR9271) proved the Windows/libusbK failure mode: a DATA frame handed to
     * the dongle over USB is accepted (TX "succeeds") but NEVER keys the radio without the 1-byte
     * guardian kick -- beacons (which get the kick) radiated fine while 559 data frames produced
     * ZERO on-air frames. So the data path needs the guardian too; auth/mgmt stay guardian-less to
     * keep the seq2 auth-response radiation fix intact. NWC_NOGUARD_DATA disables it for A/B. */
    /* Default = guardian-beacons-only, exactly like the proven Linux path. NWC_GUARD_DATA adds it
     * to data frames for A/B testing (an earlier "data needs the guardian" theory came from captures
     * later found to be contaminated by a second dongle's rogue beacon on the same channel). */
    int guard_data = (env_on("NWC_GUARD_DATA"));
    int want_guardian = guard_all || (strstr(label, "beacon") != NULL)
                        || (guard_data && strstr(label, "data") != NULL);
    int rc = 0;

    /* HYBRID TX: only LARGE frames (bulk DS<->internet data, >=256B transfer) go async, so a data
     * burst never blocks the main loop. Everything SMALL -- beacons (need a steady ~100ms cadence;
     * async made them jittery -> 51303 "can't see it"), and time-critical control frames (DNS
     * answers, ARP, auth/mgmt) -- stays SYNCHRONOUS so it's delivered promptly. A DNS reply stuck
     * behind the async bulk backlog during the QR2 burst times out -> Wiimmfi 64030 "DNS failure".
     * NWC_SYNC_TX forces everything sync; NWC_ASYNC_MIN overrides the size threshold. */
    const char *am = getenv("NWC_ASYNC_MIN"); int async_min = (am && *am) ? atoi(am) : 256;
    /* Force DATA frames (internet forwarding + WinDivert gamespy injections) async REGARDLESS of size.
     * PROVEN: a libusbK SYNC bulk transfer blocks the main loop ~10ms each (Linux usbfs = microseconds),
     * so the small (<256B) gamespy result/ACK injections stall the loop 80ms+ during the login burst ->
     * beacon starves + the cleared-on-read CCA counter inflates into a fake spike -> dropped GPCM. Auth
     * frames stay sync (their SIFS auto-ACK timing needs it). NWC_DATA_SYNC forces the old behaviour. */
    /* Async also for probe-response + assoc-response: during a DS re-scan/auth STORM the RX drain
     * processes many frames per loop pass and EACH triggered a SYNC response -> dozens of blocking
     * USB TX in one iteration = multi-second loop stall -> beacon starves -> more storm. Only the
     * auth seq2/seq4 (SIFS auto-ACK timing) must stay sync. NWC_DATA_SYNC forces the old behaviour. */
    int force_async = (strstr(label, "data") != NULL || strstr(label, "probe") != NULL
                       || strstr(label, "assoc") != NULL) && !env_on("NWC_DATA_SYNC");
    if (!env_on("NWC_SYNC_TX") && strstr(label, "beacon") == NULL && (force_async || transfer_len >= async_min)) {
        if (!g_txpool_ready && nwc_txpool_init() != 0) {
            printf("[tx] async pool alloc failed\n"); return LIBUSB_ERROR_NO_MEM; }
        /* CRITICAL RELIABILITY (Linux-parity fix): server->DS DATA frames must NEVER be
         * silently dropped. On a full async pool the old code did g_tx_dropped++/return 0,
         * reporting success while the frame vanished -> a lost GPCM segment -> the DS never
         * ACKs -> server retransmit STORM -> that very flood keeps the pool full -> more
         * drops. Linux's kernel NAT never drops, so its GPCM completes and no storm forms.
         * Here: if the pool is full for a "data" frame, fall through to the reliable SYNC
         * path below instead of dropping. Non-data frames (probe/assoc responses) may still
         * drop -- a later one re-arms. NWC_NO_DATASYNC_FALLBACK disables for A/B. */
        bool is_data = (strstr(label, "data") != NULL) && !env_on("NWC_NO_DATASYNC_FALLBACK");
        bool pool_full = false;
        if (!noguard && want_guardian) {
            static const uint8_t gzero = 0;
            rc = nwc_tx_async(handle, &gzero, 1);
            if (rc == 1) pool_full = true;
            else if (rc < 0) { printf("[tx] %s guardian submit: %s\n", label, libusb_error_name(rc)); return rc; }
        }
        if (!pool_full) {
            rc = nwc_tx_async(handle, packet, transfer_len);
            if (rc == 1) pool_full = true;
            else if (rc < 0) { printf("[tx] %s frame submit: %s\n", label, libusb_error_name(rc)); return rc; }
            else {
                if ((strcmp(label, "beacon") != 0 && strcmp(label, "beacon-sw") != 0) || VERBOSE_USB_REG)
                    printf("[tx] %s queued: %d bytes (async)\n", label, transfer_len);
                return 0;
            }
        }
        if (pool_full) {
            if (!is_data) { g_tx_dropped++; return 0; }   /* non-data: drop, a later frame re-arms */
            /* data frame + full pool -> fall through to reliable SYNC send below */
            static unsigned long g_data_syncfb = 0;
            if ((++g_data_syncfb & 0x3f) == 1)
                printf("[tx] data pool-full -> reliable sync fallback (count=%lu)\n", g_data_syncfb);
        }
    }

    /* Legacy blocking path (NWC_SYNC_TX) + the beacon/auth sync frames. THE last loop-stall fix:
     * the timeout was 2000ms, so an occasional libusbK hang under load blocked the whole main loop
     * for ~2s -> beacon dark -> DS drops -> auth storm. Cap it (NWC_TX_TIMEOUT, default 250ms): a hang
     * now costs one dropped beacon, the loop continues, the next beacon retries. Linux usbfs never
     * hangs so this is Windows-specific. */
    static int txto = -1;
    if (txto < 0) { const char *e=getenv("NWC_TX_TIMEOUT"); txto=(e&&*e)?atoi(e):250; if(txto<50)txto=50; if(txto>2000)txto=2000; }
    if (!noguard && want_guardian) {
        rc = libusb_bulk_transfer(handle, BULK_OUT_EP, &guardian, 1, &transferred, (unsigned)txto);
        if (rc != 0) { printf("[tx] %s guardian failed: %s\n", label, libusb_error_name(rc)); return rc; }
    }
    rc = libusb_bulk_transfer(handle, BULK_OUT_EP, packet, transfer_len, &transferred, (unsigned)txto);
    if (rc != 0) { printf("[tx] %s frame failed (noguard=%d): %s\n", label, noguard, libusb_error_name(rc)); return rc; }
    if (VERBOSE_USB_REG || (strcmp(label, "beacon") != 0 && !(nwc_quiet() && strstr(label, "beacon") != NULL)))
        printf("[tx] %s sent: %d bytes (noguard=%d)\n", label, transferred, noguard);
    return 0;
#endif
}

static int send_beacon(libusb_device_handle *handle, const uint8_t mac[6], const char *ssid,
                       uint8_t channel, bool privacy)
{
    uint8_t frame[256];
    size_t frame_len = build_beacon(frame, sizeof(frame), mac, ssid, channel, privacy);
    if (!frame_len)
        return LIBUSB_ERROR_INVALID_PARAM;
    if (VERBOSE_USB_REG)
        printf("[beacon] ssid=\"%s\" channel=%u\n", ssid, channel);
    return send_80211_frame(handle, frame, frame_len, true, false, "beacon");
}

/* Raw bulk-OUT of len bytes to the TX endpoint (no guardian/descriptor logic of its own) —
 * used for the beacon guardian byte and the beacon frame, matching rt2500usb's URB submits. */
static int raw_bulk_out(libusb_device_handle *handle, const uint8_t *buf, int len, const char *label)
{
#ifdef NWC_BACKEND_KMDF
    (void)handle; DWORD ret = 0;
    if (!DeviceIoControl(g_kmdf, IOCTL_NWC_TX_FRAME, (LPVOID)buf, (DWORD)len, NULL, 0, &ret, NULL)) {
        printf("[hwbcn] %s bulk failed: gle=%lu\n", label, GetLastError());
        return LIBUSB_ERROR_IO;
    }
    return 0;
#else
    int tr = 0;
    int rc = libusb_bulk_transfer(handle, BULK_OUT_EP, (unsigned char *)buf, len, &tr, nwc_usb_timeout());
    if (rc != 0) { printf("[hwbcn] %s bulk failed: %s\n", label, libusb_error_name(rc)); return rc; }
    return 0;
#endif
}

/*
 * Arm the RT2570 HARDWARE beacon generator so the chip auto-transmits the beacon at every TBTT
 * (proper AP / original-driver behavior) — instead of us bulk-sending a software beacon every
 * 100ms. This is the precondition for a live TSF and the hardware auto-responder.
 *
 * VERIFIED against rt2500usb_write_beacon (linux rt2x00): there is NO beacon register buffer —
 * the beacon is a plain bulk transfer to the (single) OUT endpoint 0x01, with this exact dance:
 *   1) clear BEACON_GEN in TXRX_CSR19
 *   2) submit a 1-byte guardian on the beacon pipe
 *   3) toggle TXRX_CSR19 BEACON_GEN on/off/on/off/on (source: "Beacon generation will fail
 *      initially ... change TXRX_CSR19 several times"), with TSF_COUNT|TBCN set
 *   4) send the [20B TXdesc][beacon] frame (rt2500usb does this async on guardian completion)
 */
static int hw_load_beacon(libusb_device_handle *handle, const uint8_t mac[6], const char *ssid,
                          uint8_t channel, bool privacy, uint16_t csr19_on, uint16_t csr19_off)
{
    uint8_t frame[256];
    size_t frame_len = build_beacon(frame, sizeof(frame), mac, ssid, channel, privacy);
    if (!frame_len) return LIBUSB_ERROR_INVALID_PARAM;

    /* [20-byte TX descriptor][beacon frame]; descriptor = beacon flavour (ts=1, ack=0). */
    uint8_t pkt[20 + 256]; memset(pkt, 0, sizeof(pkt));
    uint32_t w0 = 0x00000400u | 0x00001000u | (((uint32_t)frame_len & 0x0fff) << 16); /* TS|NEWSEQ|len */
    uint32_t w1 = (2u << 6) | (4u << 8) | (10u << 12);
    uint32_t dl = (uint32_t)frame_len + 4u, plcp = dl * 8u;
    uint32_t w2 = (0x04u << 8) | ((plcp & 0xffu) << 16) | (((plcp >> 8) & 0xffu) << 24);
    put32(pkt + 0, w0); put32(pkt + 4, w1); put32(pkt + 8, w2);
    memcpy(pkt + 20, frame, frame_len);
    int total = 20 + (int)frame_len;
    int xfer = total; if (xfer & 1) xfer++; if ((xfer % 512) == 0) xfer += 2; /* rt2500usb get_tx_data_len */

    write16(handle, TXRX_CSR19, csr19_off);                 /* 1) BEACON_GEN off */
    uint8_t guardian = 0;
    int rc = raw_bulk_out(handle, &guardian, 1, "guardian"); /* 2) guardian byte */
    if (rc) return rc;
    write16(handle, TXRX_CSR19, csr19_on);                  /* 3) on/off/on/off/on toggle */
    write16(handle, TXRX_CSR19, csr19_off);
    write16(handle, TXRX_CSR19, csr19_on);
    write16(handle, TXRX_CSR19, csr19_off);
    write16(handle, TXRX_CSR19, csr19_on);
    rc = raw_bulk_out(handle, pkt, xfer, "beacon-frame");    /* 4) the beacon */
    if (rc) return rc;
    printf("[hwbcn] bulk beacon armed: guardian + %d-byte frame on EP; CSR19 off=0x%04x on=0x%04x\n",
           xfer, csr19_off, csr19_on);
    return 0;
}

/* ---- DS<->internet data bridge (NWC_DATAPATH): software WEP + 802.11<->Ethernet ----
 * The SNAP header, WEP-encrypt, and the Ethernet->802.11 TX helper are cross-platform. The Linux
 * TAP path (kernel does ARP/DHCP/NAT) and the Windows Wintun path (probe does ARP/DHCP, Windows
 * NAT) share them. */
static const uint8_t SNAP_HDR[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };
static uint8_t g_sta_mac[6] = {0};   /* associated DS station MAC, learned from RX data (both OSes) */
static int g_sta_known = 0;
static unsigned long g_last_ds_rx = 0;  /* GetTickCount() of the last frame heard from the DS */
volatile unsigned long g_loop_max = 0;  /* worst single main-loop iteration (ms) since last stacsr */
static unsigned long g_last_auth_rx = 0; /* GetTickCount() of the last AUTH frame from the DS.
                                          * Used to tell DISCOVERY (DS scanning, wants probe-
                                          * responses) apart from the AUTH window (must stay off
                                          * the air so the DS's seq1 gets its SIFS auto-ACK). */
/* net->DS TCP re-send cache. THE gamespy fix: pktmon proved the server retransmits each control/data
 * segment many times (SYN-ACK ~36x, the \lc\ login response ~42x), but Windows' New-NetNat only
 * delivers ~2 of them onto Wintun and then stops -- so with retry_limit=0 (lossy OTA) the DS never
 * receives the segment, never ACKs, and gpcm login stalls (-> 61010/61070). Linux's kernel NAT
 * delivers every retransmit. We replicate that: cache the latest server->DS SYN-ACK *or PSH data*
 * segment and re-send it to the DS every ~150ms until the DS replies to that flow (or a bound). The
 * buffer holds a full small gamespy segment (challenge / \lc\2\ login result are ~100-200 bytes). */
static struct {
    uint8_t  eth[700]; int len;
    uint32_t srv_ip; uint16_t srv_port, ds_port;
    uint32_t end_seq;                 /* server seq just past this segment; clear when DS ACKs >= it */
    unsigned long last_send; int resends; int active;
} g_synack = {0};
static void rawret_note_flow(uint32_t srv_ip, uint16_t srv_port, uint16_t ds_port); /* raw-return fwd */
static int  g_wd_active = 0;                 /* 1 = full userspace NAT via WinDivert (WinNAT bypassed) */
static void wd_nat_outbound(const uint8_t *ip, int len);  /* SNAT DS->server + WinDivertSend (fwd) */

/* WEP-encrypt a plaintext body -> [IV(3)][keyid(1)][RC4(plain||ICV)]. Returns out length. */
static int wep_encrypt_body(const uint8_t *plain, int plain_len, const uint8_t key[13],
                            uint8_t *out, int out_cap)
{
    if (plain_len < 0 || plain_len > 2304 || plain_len + 8 > out_cap) return -1;
    static uint32_t iv_ctr = 1;
    iv_ctr = (iv_ctr + 1) & 0x00ffffffu;
    out[0] = (uint8_t)(iv_ctr & 0xff);
    out[1] = (uint8_t)((iv_ctr >> 8) & 0xff);
    out[2] = (uint8_t)((iv_ctr >> 16) & 0xff);
    out[3] = 0;                                   /* key id 0 */
    uint8_t buf[2320];
    memcpy(buf, plain, plain_len);
    uint32_t icv = crc32_ieee(plain, plain_len);
    buf[plain_len + 0] = (uint8_t)(icv & 0xff);
    buf[plain_len + 1] = (uint8_t)((icv >> 8) & 0xff);
    buf[plain_len + 2] = (uint8_t)((icv >> 16) & 0xff);
    buf[plain_len + 3] = (uint8_t)((icv >> 24) & 0xff);
    uint8_t rc4_key[16];
    rc4_key[0] = out[0]; rc4_key[1] = out[1]; rc4_key[2] = out[2];
    memcpy(rc4_key + 3, key, 13);
    rc4_crypt(rc4_key, sizeof rc4_key, buf, out + 4, plain_len + 4);
    return 4 + plain_len + 4;
}

/* network-order (big-endian) helpers for IP/UDP/ARP/DHCP packet building */
static uint16_t rdbe16(const uint8_t *p){ return (uint16_t)(((uint16_t)p[0]<<8)|p[1]); }
static uint32_t rdbe32(const uint8_t *p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static void wrbe16(uint8_t *p, uint16_t v){ p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }
static void wrbe32(uint8_t *p, uint32_t v){ p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
static uint16_t inet_cksum(const uint8_t *d, int n){ uint32_t s=0; int i; for(i=0;i+1<n;i+=2) s+=rdbe16(d+i); if(n&1) s+=(uint32_t)d[n-1]<<8; while(s>>16) s=(s&0xffff)+(s>>16); return (uint16_t)~s; }

static int send_80211_frame(libusb_device_handle *handle, const uint8_t *frame,
                            size_t frame_len, bool request_timestamp,
                            bool request_ack, const char *label);

/* Build an 802.11 FromDS data frame carrying an Ethernet payload (SNAP-encapsulated), WEP-encrypt
 * if privacy, and TX to the DS. da=dest(DS), sa=src(gateway). Shared by TAP + Wintun paths. */
static void send_eth_to_ds(libusb_device_handle *handle, const uint8_t mac[6], const char *ssid,
                           bool privacy, const uint8_t da[6], const uint8_t sa[6],
                           uint16_t ethertype, const uint8_t *payload, int paylen)
{
    if (paylen < 0 || paylen > 1600) return;
    uint8_t plainbody[2320];
    memcpy(plainbody, SNAP_HDR, 6);
    plainbody[6] = (uint8_t)(ethertype >> 8); plainbody[7] = (uint8_t)(ethertype & 0xff);
    memcpy(plainbody + 8, payload, (size_t)paylen);
    int bp_len = 8 + paylen;
    uint8_t out[2400];
    uint16_t fc = 0x0208;                          /* data, FromDS=1 */
    put16(out + 0, fc); put16(out + 2, 0);
    memcpy(out + 4, da, 6); memcpy(out + 10, mac, 6); memcpy(out + 16, sa, 6);
    put16(out + 22, 0);
    int body_len;
    /* DIAGNOSTIC: NWC_PLAINTEXT_DATA forces plaintext data frames (no WEP, no Protected bit) to
     * test whether the Protected/WEP path is what stops data frames radiating on Windows. */
    int plaintext = (env_on("NWC_PLAINTEXT_DATA"));
    if (privacy && !plaintext) {
        uint8_t wk[13]; derive_original_wep_key(ssid + 12, wk);
        body_len = wep_encrypt_body(plainbody, bp_len, wk, out + 24, (int)sizeof out - 24);
        if (body_len < 0) return;
        put16(out + 0, (uint16_t)(fc | 0x4000));   /* Protected */
    } else {
        if (24 + bp_len > (int)sizeof out) return;
        memcpy(out + 24, plainbody, (size_t)bp_len); body_len = bp_len;
    }
    send_80211_frame(handle, out, (size_t)(24 + body_len), false, true, "data");
}

#ifndef _WIN32
static int tap_open(const char *name)
{
    int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) { printf("[tap] open /dev/net/tun failed: %s\n", strerror(errno)); return -1; }
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        printf("[tap] TUNSETIFF %s failed: %s (create it: ip tuntap add %s mode tap)\n",
               name, strerror(errno), name);
        close(fd); return -1;
    }
    int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    printf("[tap] bridge attached to %s (fd=%d)\n", name, fd);
    return fd;
}

/* DS data frame (from RX) -> decrypt -> Ethernet -> TAP (into the Linux net stack -> NAT). */
static void tap_rx_dsdata(const uint8_t *frame, int frame_len, const char *ssid)
{
    if (g_tap_fd < 0 || frame_len < 24) return;
    uint16_t fc = get16(frame);
    if (((fc >> 2) & 0x3) != 2) return;            /* type 2 = data */
    uint8_t subtype = (uint8_t)((fc >> 4) & 0xf);
    if (subtype & 0x4) return;                     /* Null / CF / QoS-Null: no data payload */
    int hdrlen = 24;
    if (subtype & 0x8) hdrlen += 2;                /* QoS data -> +2 QoS control */
    bool prot = (fc & 0x4000) != 0;
    if (frame_len < hdrlen + 8) return;

    /* The RT2570 HW-decrypts WEP in place: it leaves the 4-byte IV/keyid and puts the decrypted
     * SNAP+payload at frame+hdrlen+4, with the ICV(4) trailing. Prefer that (matches the auth
     * path's frame+28). Fall back to software decrypt only if the HW plaintext isn't a SNAP hdr. */
    uint8_t swbuf[2320];
    const uint8_t *plain = NULL; int plain_len = 0;
    if (prot) {
        int off = hdrlen + 4;
        int hw_len = frame_len - off - 4;          /* minus trailing ICV */
        if (hw_len >= 8 && memcmp(frame + off, SNAP_HDR, 6) == 0) {
            plain = frame + off; plain_len = hw_len;
        } else {
            uint8_t wk[13]; derive_original_wep_key(ssid + 12, wk);
            int pl = 0;
            if (wep_decrypt_body(frame + hdrlen, frame_len - hdrlen, wk, swbuf, sizeof swbuf, &pl)
                && pl >= 8 && memcmp(swbuf, SNAP_HDR, 6) == 0) { plain = swbuf; plain_len = pl; }
        }
    } else {
        int pl = frame_len - hdrlen;
        if (pl >= 8 && memcmp(frame + hdrlen, SNAP_HDR, 6) == 0) { plain = frame + hdrlen; plain_len = pl; }
    }
    if (!plain || plain_len < 8) return;
    const uint8_t *sa = frame + 10, *da = frame + 16;   /* ToDS: addr2=SA(DS), addr3=DA */
    int paylen = plain_len - 8;
    uint8_t eth[1600];
    if (14 + paylen > (int)sizeof eth) return;
    memcpy(eth + 0, da, 6);
    memcpy(eth + 6, sa, 6);
    eth[12] = plain[6]; eth[13] = plain[7];        /* ethertype from SNAP */
    memcpy(eth + 14, plain + 8, paylen);
    if (!g_sta_known) { memcpy(g_sta_mac, sa, 6); g_sta_known = 1;
        printf("[bridge] learned DS station %02x:%02x:%02x:%02x:%02x:%02x\n",
               sa[0],sa[1],sa[2],sa[3],sa[4],sa[5]); }
    if (write(g_tap_fd, eth, 14 + paylen) < 0 && errno != EAGAIN) { /* ignore */ }
}

/* Ethernet from TAP (NAT replies) -> 802.11 data -> WEP-encrypt -> TX to the DS. */
static int send_80211_frame(libusb_device_handle *handle, const uint8_t *frame,
                            size_t frame_len, bool request_timestamp,
                            bool request_ack, const char *label);
static void tap_poll_to_ds(libusb_device_handle *handle, const uint8_t mac[6],
                           const char *ssid, bool privacy)
{
    if (g_tap_fd < 0) return;
    uint8_t eth[1600];
    for (int iter = 0; iter < 12; iter++) {
        ssize_t n = read(g_tap_fd, eth, sizeof eth);
        if (n <= 14) return;                       /* EAGAIN / runt */
        const uint8_t *da = eth + 0;               /* dest = DS */
        const uint8_t *sa = eth + 6;               /* src = gateway (nwc0) */
        int paylen = (int)n - 14;
        uint8_t plainbody[2320];
        memcpy(plainbody, SNAP_HDR, 6);
        plainbody[6] = eth[12]; plainbody[7] = eth[13];
        if (8 + paylen > (int)sizeof plainbody) continue;
        memcpy(plainbody + 8, eth + 14, paylen);
        int bp_len = 8 + paylen;

        uint8_t out[2400];
        uint16_t fc = 0x0208;                      /* data, FromDS=1 */
        put16(out + 0, fc);
        put16(out + 2, 0x0000);                    /* duration */
        memcpy(out + 4, da, 6);                    /* addr1 = DA (DS) */
        memcpy(out + 10, mac, 6);                  /* addr2 = BSSID (us) */
        memcpy(out + 16, sa, 6);                   /* addr3 = SA (gateway) */
        put16(out + 22, 0x0000);                   /* seq */
        int body_len;
        if (privacy) {
            uint8_t wk[13]; derive_original_wep_key(ssid + 12, wk);
            body_len = wep_encrypt_body(plainbody, bp_len, wk, out + 24, (int)sizeof out - 24);
            if (body_len < 0) continue;
            put16(out + 0, (uint16_t)(fc | 0x4000));   /* Protected */
        } else {
            if (24 + bp_len > (int)sizeof out) continue;
            memcpy(out + 24, plainbody, bp_len); body_len = bp_len;
        }
        send_80211_frame(handle, out, (size_t)(24 + body_len), false, true, "data");
    }
}
#endif /* !_WIN32 */

#ifdef _WIN32
/* ---- Windows Wintun data path (L3) ----
 * Wintun is IP-only, so the probe answers the DS's ARP + DHCP itself and injects the DS's IP
 * packets into a Wintun adapter; Windows New-NetNat + IP forwarding carry them to the internet.
 * DNS is also answered in-probe (Windows' NAT/ICS DNS proxy otherwise replies from the gateway IP,
 * which the DS rejects): we forward the query to the real Wiimmfi DNS and reply with its source. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include "wintun.h"
#include "windivert.h"
static SOCKET g_dns_sock = INVALID_SOCKET;
static uint32_t g_wiimmfi_ip = 0xBC22B1D9u;   /* 188.34.177.217 (nas.wiimmfi.de); refreshed at startup */
static WINTUN_CREATE_ADAPTER_FUNC         *pWtCreate;
static WINTUN_CLOSE_ADAPTER_FUNC          *pWtClose;
static WINTUN_START_SESSION_FUNC          *pWtStart;
static WINTUN_END_SESSION_FUNC            *pWtEnd;
static WINTUN_ALLOCATE_SEND_PACKET_FUNC   *pWtAlloc;
static WINTUN_SEND_PACKET_FUNC            *pWtSend;
static WINTUN_RECEIVE_PACKET_FUNC         *pWtRecv;
static WINTUN_RELEASE_RECEIVE_PACKET_FUNC *pWtRelease;
static WINTUN_OPEN_ADAPTER_FUNC           *pWtOpen;
static WINTUN_ADAPTER_HANDLE  g_wt_adapter;
static WINTUN_SESSION_HANDLE  g_wt_session;
static int g_win_bridge;

#define WNET_GW   0xC0A82C01u   /* 192.168.44.1  (gateway = us)          */
#define WNET_DS   0xC0A82C14u   /* 192.168.44.20 (assigned to the DS)    */
#define WNET_MASK 0xFFFFFF00u   /* 255.255.255.0                         */
#define WNET_DNS  0xA4842C6Au   /* 164.132.44.106 (public Wiimmfi DNS)   */
static const uint8_t GW_MAC[6] = { 0x02, 0x4e, 0x57, 0x43, 0x44, 0x01 }; /* locally-administered */
static void win_send_eth(libusb_device_handle *h, const uint8_t mac[6], const char *ssid, bool privacy,
                         const uint8_t *eth, int ethlen);   /* fwd decl (used by win_handle_dns) */

static int win_datapath_init(void)
{
    HMODULE h = LoadLibraryExW(L"wintun.dll", NULL,
                   LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h) { printf("[wintun] cannot load wintun.dll (place it next to the exe): gle=%lu\n", GetLastError()); return -1; }
    pWtCreate =(WINTUN_CREATE_ADAPTER_FUNC*)        GetProcAddress(h,"WintunCreateAdapter");
    pWtClose  =(WINTUN_CLOSE_ADAPTER_FUNC*)         GetProcAddress(h,"WintunCloseAdapter");
    pWtStart  =(WINTUN_START_SESSION_FUNC*)         GetProcAddress(h,"WintunStartSession");
    pWtEnd    =(WINTUN_END_SESSION_FUNC*)           GetProcAddress(h,"WintunEndSession");
    pWtAlloc  =(WINTUN_ALLOCATE_SEND_PACKET_FUNC*)  GetProcAddress(h,"WintunAllocateSendPacket");
    pWtSend   =(WINTUN_SEND_PACKET_FUNC*)           GetProcAddress(h,"WintunSendPacket");
    pWtRecv   =(WINTUN_RECEIVE_PACKET_FUNC*)        GetProcAddress(h,"WintunReceivePacket");
    pWtRelease=(WINTUN_RELEASE_RECEIVE_PACKET_FUNC*)GetProcAddress(h,"WintunReleaseReceivePacket");
    pWtOpen   =(WINTUN_OPEN_ADAPTER_FUNC*)          GetProcAddress(h,"WintunOpenAdapter");
    if (!pWtCreate||!pWtStart||!pWtAlloc||!pWtSend||!pWtRecv||!pWtRelease){ printf("[wintun] missing exports\n"); return -1; }
    GUID g = { 0x4e574344, 0x0001, 0x4001, { 0x80,0x00,0x00,0x11,0x22,0x33,0x44,0x55 } };
    /* A killed predecessor's adapter tears down asynchronously (and Wintun may
     * uninstall/reinstall its driver on last-adapter-removal), so create can
     * transiently fail with ERROR_NOT_FOUND(1168)/ERROR_DEVICE_REINITIALIZATION_NEEDED.
     * Retry, trying open-existing first each round so we adopt a stale adapter if present. */
    for (int attempt = 0; attempt < 12 && !g_wt_adapter; attempt++) {
        if (pWtOpen) {
            g_wt_adapter = pWtOpen(L"NWC-DS");
            if (g_wt_adapter) { printf("[wintun] reusing existing 'NWC-DS' adapter\n"); break; }
        }
        g_wt_adapter = pWtCreate(L"NWC-DS", L"Wintun", &g);
        if (g_wt_adapter) break;
        DWORD gle = GetLastError();
        printf("[wintun] create attempt %d failed gle=%lu; retrying...\n", attempt+1, gle);
        Sleep(600);
    }
    if (!g_wt_adapter){ printf("[wintun] Create/Open adapter failed after retries: gle=%lu\n", GetLastError()); return -1; }
    g_wt_session = pWtStart(g_wt_adapter, 0x400000);
    if (!g_wt_session){ printf("[wintun] StartSession failed: gle=%lu\n", GetLastError()); pWtClose(g_wt_adapter); return -1; }
    { WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
      g_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      if (g_dns_sock != INVALID_SOCKET) { DWORD tmo = 400; /* ms */
          setsockopt(g_dns_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo); }
      /* Wiimmfi's own DNS (164.132.44.106) is dead; resolve the live server IP via the
       * host resolver so we can answer the DS's *.nintendowifi.net queries authoritatively.
       * Override with NWC_WIIMMFI_IP=a.b.c.d. */
      const char *ovr = getenv("NWC_WIIMMFI_IP");
      struct addrinfo hints, *res = NULL; memset(&hints,0,sizeof hints); hints.ai_family = AF_INET;
      if (ovr && ovr[0]) { g_wiimmfi_ip = ntohl(inet_addr(ovr)); }
      else if (getaddrinfo("nas.wiimmfi.de", NULL, &hints, &res) == 0 && res) {
          g_wiimmfi_ip = ntohl(((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr);
      }
      if (res) freeaddrinfo(res);
      printf("[wintun] Wiimmfi server IP for DS DNS answers = %u.%u.%u.%u\n",
             (g_wiimmfi_ip>>24)&0xff,(g_wiimmfi_ip>>16)&0xff,(g_wiimmfi_ip>>8)&0xff,g_wiimmfi_ip&0xff); }
    g_win_bridge = 1;
    printf("[wintun] adapter 'NWC-DS' created; now run nwc-datapath-setup.ps1 for IP + NAT\n");
    return 0;
}

/* UDP checksum over the pseudo-header + UDP segment. The DS's DNS resolver (unlike its DHCP
 * client) validates this and silently drops a checksum=0 datagram, so it must be correct. */
static uint16_t udp_cksum(uint32_t src, uint32_t dst, const uint8_t *udp, int udplen)
{
    uint32_t sum = 0;
    sum += (src >> 16) & 0xffff; sum += src & 0xffff;
    sum += (dst >> 16) & 0xffff; sum += dst & 0xffff;
    sum += 17; sum += (uint32_t)udplen;
    int i; for (i = 0; i + 1 < udplen; i += 2) sum += (uint32_t)((udp[i] << 8) | udp[i+1]);
    if (udplen & 1) sum += (uint32_t)(udp[udplen-1] << 8);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    uint16_t c = (uint16_t)~sum;
    return c ? c : 0xffff;
}

/* Map a DS DNS query (X.nintendowifi.net) to the CORRECT Wiimmfi server IP. Wiimmfi splits its
 * services across hosts -- NAS/conntest/web on one IP (188.34.177.217), the gamespy stack
 * (gpcm/gpsp/master/natneg on 29900/29901/28910) on another (95.217.77.151) -- so a single-IP
 * answer breaks online matchmaking (game logs into conntest/NAS fine, then its gpcm connection
 * hits the NAS host where 29900 is closed -> 52103). We resolve the equivalent X.wiimmfi.de via
 * the host resolver (cached) to get whichever host Wiimmfi actually serves that name from. */
static struct { char name[80]; uint32_t ip; } g_dnsc[48];
static int g_dnsc_n = 0;

/* ===== ASYNC DNS (Linux-parity fix for the Windows loop stall) ==========================
 * The Windows path answers the DS's DNS queries in-process (Wiimmfi's own DNS 164.132.44.106
 * is dead). The OLD wiimmfi_ip_for() resolved each new name with a BLOCKING getaddrinfo() on
 * the RX/main-loop thread -> ap_loop stalled up to ~5s per new name (the DS issues ~8 lookups
 * during a gamespy login) -> wd_drain couldn't run -> the server->DS ring backed up -> the
 * late 45-byte GPCM segment was stranded -> DS never ACKed -> null-deref Data-Abort crash.
 * Linux never does this: tap_rx_dsdata just write()s the query to the TAP and the kernel/
 * dnsmasq resolves it asynchronously. We mirror that: a dedicated resolver thread does all
 * getaddrinfo work OFF the packet thread, plus a SYNCHRONOUS startup pre-warm so the cache is
 * populated before the DS ever connects. The packet thread (wiimmfi_ip_for) ONLY reads the
 * cache and NEVER blocks. ===============================================================*/
static CRITICAL_SECTION g_dns_cs;
static int g_dns_cs_init = 0;
static char g_dns_q[64][80]; static int g_dns_qh = 0, g_dns_qt = 0;

static void dns_store(const char *host, uint32_t ip)
{
    if (!g_dns_cs_init) return;
    EnterCriticalSection(&g_dns_cs);
    int i; for (i = 0; i < g_dnsc_n; i++) if (_stricmp(g_dnsc[i].name, host)==0) { g_dnsc[i].ip = ip; break; }
    if (i == g_dnsc_n && g_dnsc_n < (int)(sizeof(g_dnsc)/sizeof(g_dnsc[0]))) {
        strncpy(g_dnsc[g_dnsc_n].name, host, sizeof(g_dnsc[0].name)-1);
        g_dnsc[g_dnsc_n].name[sizeof(g_dnsc[0].name)-1]=0; g_dnsc[g_dnsc_n].ip = ip; g_dnsc_n++;
    }
    LeaveCriticalSection(&g_dns_cs);
}

/* BLOCKING resolve -- call ONLY off the packet thread (startup prewarm or resolver thread). */
static uint32_t dns_resolve_now(const char *host)
{
    char target[96];                                      /* rewrite *.nintendowifi.net -> *.wiimmfi.de */
    const char *suf = ".nintendowifi.net"; size_t hl = strlen(host), sl = strlen(suf);
    if (hl > sl && _stricmp(host + hl - sl, suf)==0) {
        memcpy(target, host, hl - sl); strcpy(target + (hl - sl), ".wiimmfi.de");
    } else { strncpy(target, host, sizeof(target)-1); target[sizeof(target)-1]=0; }
    uint32_t ip = g_wiimmfi_ip;                           /* fallback if resolution fails */
    struct addrinfo hints, *res = NULL; memset(&hints,0,sizeof hints); hints.ai_family = AF_INET;
    if (getaddrinfo(target, NULL, &hints, &res)==0 && res)
        ip = ntohl(((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr);
    if (res) freeaddrinfo(res);
    dns_store(host, ip);
    printf("[dns-resolve] %s -> %s -> %u.%u.%u.%u\n", host, target,
           (ip>>24)&0xff,(ip>>16)&0xff,(ip>>8)&0xff,ip&0xff);
    return ip;
}

static DWORD WINAPI dns_resolver_thread(LPVOID a)
{
    (void)a;
    for (;;) {
        char host[80]; host[0] = 0;
        EnterCriticalSection(&g_dns_cs);
        if (g_dns_qt != g_dns_qh) { strncpy(host, g_dns_q[g_dns_qt], 79); host[79]=0; g_dns_qt=(g_dns_qt+1)%64; }
        LeaveCriticalSection(&g_dns_cs);
        if (!host[0]) { Sleep(15); continue; }
        dns_resolve_now(host);
    }
    return 0;
}

static void dns_enqueue(const char *host)               /* non-blocking: hand a miss to the resolver thread */
{
    if (!g_dns_cs_init) return;
    EnterCriticalSection(&g_dns_cs);
    int q = 0; for (int i = g_dns_qt; i != g_dns_qh; i=(i+1)%64) if (_stricmp(g_dns_q[i], host)==0) { q=1; break; }
    if (!q && ((g_dns_qh+1)%64) != g_dns_qt) { strncpy(g_dns_q[g_dns_qh], host, 79); g_dns_q[g_dns_qh][79]=0; g_dns_qh=(g_dns_qh+1)%64; }
    LeaveCriticalSection(&g_dns_cs);
}

/* Startup: init the lock, launch the resolver thread, and SYNCHRONOUSLY pre-warm the cache
 * with every name a DS gamespy session queries (off the hot path -- the DS isn't connected
 * yet). After this returns the packet thread finds every login name already cached. */
static void dns_init_prewarm(void)
{
    if (!g_dns_cs_init) { InitializeCriticalSection(&g_dns_cs); g_dns_cs_init = 1; }
    CreateThread(NULL, 0, dns_resolver_thread, NULL, 0, NULL);
    /* Wiimmfi PATCHES the game's DNS to query *.wiimmfi.de DIRECTLY (observed live: the DS asks
     * for conntest.wiimmfi.de / gpcm.gs.wiimmfi.de, NOT *.nintendowifi.net). So pre-warm BOTH
     * forms -- the .wiimmfi.de keys are the ones that actually get hit. A miss on a gamespy name
     * used to fall back to the NAS IP (29900 closed there) -> 61020/52203/crash. */
    static const char *pre[] = {
        /* the forms the DS actually queries (Wiimmfi-patched) */
        "nas.wiimmfi.de","naswii.wiimmfi.de","conntest.wiimmfi.de",
        "gpcm.gs.wiimmfi.de","gpsp.gs.wiimmfi.de","sake.gs.wiimmfi.de",
        "master.gs.wiimmfi.de","natneg1.gs.wiimmfi.de","natneg2.gs.wiimmfi.de","natneg3.gs.wiimmfi.de",
        "gamestats.gs.wiimmfi.de","gamestats2.gs.wiimmfi.de",
        "mariokartds.master.gs.wiimmfi.de","mariokartds.available.gs.wiimmfi.de",
        "metroidprimehunters.master.gs.wiimmfi.de","metroidprimehunters.available.gs.wiimmfi.de",
        /* legacy .nintendowifi.net forms (unpatched games), kept for safety */
        "nas.nintendowifi.net","conntest.nintendowifi.net",
        "gpcm.gs.nintendowifi.net","gpsp.gs.nintendowifi.net","master.gs.nintendowifi.net",
        "mariokartds.master.gs.nintendowifi.net","mariokartds.available.gs.nintendowifi.net"
    };
    printf("[dns] pre-warming %d Wiimmfi names (off the packet thread)...\n",
           (int)(sizeof pre/sizeof pre[0]));
    for (unsigned i = 0; i < sizeof pre/sizeof pre[0]; i++) dns_resolve_now(pre[i]);
    printf("[dns] pre-warm complete (%d cached).\n", g_dnsc_n);
}

/* Packet-thread lookup: CACHE-ONLY, never blocks. Miss -> queue for async resolve + return
 * the fallback for this one answer (the next query for the name hits the cache). */
static uint32_t wiimmfi_ip_for(const char *qname)
{
    char host[80]; size_t n = 0;
    for (const char *p = qname; *p && n < sizeof(host)-1; p++) host[n++] = *p;
    if (n && host[n-1]=='.') n--;                         /* strip trailing dot from parser */
    host[n] = 0;
    uint32_t ip = 0; int hit = 0;
    if (g_dns_cs_init) EnterCriticalSection(&g_dns_cs);
    for (int i = 0; i < g_dnsc_n; i++) if (_stricmp(g_dnsc[i].name, host)==0) { ip = g_dnsc[i].ip; hit = 1; break; }
    if (g_dns_cs_init) LeaveCriticalSection(&g_dns_cs);
    if (hit) return ip;
    dns_enqueue(host);                                    /* resolve in the background, no stall */
    /* Gamespy-aware fallback: *.gs.* names live on the gamespy host (95.217.77.151), NOT the NAS
     * host -- returning the NAS IP for a gpcm/gpsp/master miss sends the DS to a closed 29900
     * (61020/52203/crash). Use the gamespy IP for gs names; the general fallback otherwise. */
    uint32_t fb = (strstr(host, ".gs.") != NULL) ? 0x5FD94D97u /*95.217.77.151*/ : g_wiimmfi_ip;
    printf("[dns-miss] %s -> async queued; fallback %u.%u.%u.%u this answer\n", host,
           (fb>>24)&0xff,(fb>>16)&0xff,(fb>>8)&0xff,fb&0xff);
    return fb;
}

/* Answer the DS's DNS query authoritatively with Wiimmfi's live server IP. Wiimmfi's own DNS
 * (164.132.44.106) is dead, so we synthesise the A record here, resolving each name to the correct
 * Wiimmfi host (see wiimmfi_ip_for). We reply from the exact server
 * IP the DS queried (src preserved) so the DS accepts it (Windows' ICS proxy would answer from
 * the gateway IP, which the DS rejects). */
static void win_handle_dns(libusb_device_handle *h, const uint8_t mac[6], const char *ssid, bool privacy,
                           const uint8_t *ip, int iplen, const uint8_t ds_mac[6])
{
    int ihl = (ip[0]&0x0f)*4;
    const uint8_t *udp = ip + ihl;
    int udplen = rdbe16(udp+4);
    if (udplen < 20 || ihl + udplen > iplen) { printf("[dns] ERR: bad udplen %d\n", udplen); return; }
    const uint8_t *q = udp + 8; int qlen = udplen - 8;
    uint16_t ds_port = rdbe16(udp+0);
    uint32_t server = rdbe32(ip+16);                      /* the DNS server the DS queried (preserve as reply src) */
    if (qlen < 12 || rdbe16(q+4) < 1) { printf("[dns] ERR: no question\n"); return; }
    /* walk the QNAME (labels terminated by a zero length byte), then qtype(2)+qclass(2) */
    int p = 12; char qname[256]; int qn = 0;
    while (p < qlen && q[p] != 0) { int l = q[p++]; if (l > 63 || p + l > qlen) { printf("[dns] ERR: bad qname\n"); return; }
        for (int i = 0; i < l && qn < 254; i++) qname[qn++] = (char)q[p+i]; if (qn < 254) qname[qn++]='.'; p += l; }
    if (p >= qlen) { printf("[dns] ERR: qname overrun\n"); return; }
    qname[qn] = 0; int qend = p + 1;                       /* past the root label */
    if (qend + 4 > qlen) { printf("[dns] ERR: no qtype\n"); return; }
    uint16_t qtype = rdbe16(q + qend); qend += 4;
    uint8_t ans[512]; if (qend > (int)sizeof(ans) - 16) { printf("[dns] ERR: query too long\n"); return; }
    memcpy(ans, q, (size_t)qend);                         /* header + single question */
    ans[2] = 0x85; ans[3] = 0x80;                         /* QR=1 AA=1 RD=1 / RA=1 rcode=0 */
    wrbe16(ans+6, (qtype==1)?1:0);                        /* ANCOUNT: 1 for A, else 0 */
    wrbe16(ans+8, 0); wrbe16(ans+10, 0);                  /* NSCOUNT/ARCOUNT: drop any EDNS OPT */
    uint32_t ans_ip = wiimmfi_ip_for(qname);              /* correct Wiimmfi host for THIS name */
    int anslen = qend;
    if (qtype == 1) {                                     /* A record -> Wiimmfi IP */
        ans[anslen++]=0xC0; ans[anslen++]=0x0C;           /* name ptr -> question */
        wrbe16(ans+anslen,1); anslen+=2; wrbe16(ans+anslen,1); anslen+=2;   /* TYPE=A CLASS=IN */
        wrbe32(ans+anslen,60); anslen+=4;                 /* TTL=60 */
        wrbe16(ans+anslen,4); anslen+=2;                  /* RDLENGTH=4 */
        wrbe32(ans+anslen, ans_ip); anslen+=4;
    }
    uint8_t pkt[600]; int totlen = 20 + 8 + anslen;
    memset(pkt,0,20); pkt[0]=0x45; wrbe16(pkt+2,(uint16_t)totlen); pkt[8]=64; pkt[9]=17;
    wrbe32(pkt+12, server); wrbe32(pkt+16, WNET_DS);      /* src = queried DNS server (DS accepts it) */
    wrbe16(pkt+10, inet_cksum(pkt,20));
    wrbe16(pkt+20, 53); wrbe16(pkt+22, ds_port);
    wrbe16(pkt+24, (uint16_t)(8+anslen)); wrbe16(pkt+26, 0);
    memcpy(pkt+28, ans, (size_t)anslen);
    wrbe16(pkt+26, udp_cksum(server, WNET_DS, pkt+20, 8+anslen));   /* DS validates this on DNS */
    uint8_t eth[600]; memcpy(eth,ds_mac,6); memcpy(eth+6,GW_MAC,6); wrbe16(eth+12,0x0800);
    memcpy(eth+14, pkt, (size_t)totlen);
    static int dns_dumped = 0;
    if (!dns_dumped) { dns_dumped = 1;
        printf("[dns-hex] query %d bytes / reply IP+UDP+DNS %d bytes:\n  QUERY:", qlen, totlen);
        for (int i=0;i<qlen && i<80;i++) printf(" %02x", q[i]);
        printf("\n  REPLY:");
        for (int i=0;i<totlen && i<120;i++) printf(" %02x", pkt[i]);
        printf("\n"); }
    /* DNS answers are the control plane (the DS connects nowhere until it resolves the server) and
     * are small, so send a few copies -- a single lost gamespy reply otherwise leaves the DS
     * re-querying forever and reporting "no servers" (20998). retry_limit=0 = no hardware retry. */
    { static int dns_dup = -1;
      if (dns_dup < 0) { const char *e = getenv("NWC_DNS_DUP"); dns_dup = (e&&*e)?atoi(e):3;
                         if (dns_dup < 1) dns_dup = 1; if (dns_dup > 5) dns_dup = 5; }
      for (int d = 0; d < dns_dup; d++) win_send_eth(h, mac, ssid, privacy, eth, 14+totlen); }
    printf("[dns] '%s' type=%u -> %u.%u.%u.%u (authoritative)\n", qname, qtype,
           (ans_ip>>24)&0xff,(ans_ip>>16)&0xff,(ans_ip>>8)&0xff,ans_ip&0xff);
}

static void win_send_eth(libusb_device_handle *h, const uint8_t mac[6], const char *ssid, bool privacy,
                         const uint8_t *eth, int ethlen)
{
    if (ethlen < 14) return;
    send_eth_to_ds(h, mac, ssid, privacy, eth+0, eth+6, (uint16_t)((eth[12]<<8)|eth[13]), eth+14, ethlen-14);
}

/* Answer the DS's ARP "who has the gateway" so it can send us IP traffic. */
static void win_handle_arp(libusb_device_handle *h, const uint8_t mac[6], const char *ssid, bool privacy,
                           const uint8_t *arp, int arplen, const uint8_t ds_mac[6])
{
    if (arplen < 28 || rdbe16(arp+6) != 1) return;      /* opcode 1 = request */
    if (rdbe32(arp+24) != WNET_GW) return;              /* only answer for our gateway IP */
    uint8_t out[42];
    memcpy(out+0, ds_mac, 6); memcpy(out+6, GW_MAC, 6); wrbe16(out+12, 0x0806);
    wrbe16(out+14, 1); wrbe16(out+16, 0x0800); out[18]=6; out[19]=4; wrbe16(out+20, 2);
    memcpy(out+22, GW_MAC, 6); wrbe32(out+28, WNET_GW);
    memcpy(out+32, ds_mac, 6); wrbe32(out+38, WNET_DS);
    win_send_eth(h, mac, ssid, privacy, out, 42);
}

/* Answer the DS's DHCP DISCOVER/REQUEST from the in-probe server (hands out Wiimmfi DNS). */
static void win_handle_dhcp(libusb_device_handle *h, const uint8_t mac[6], const char *ssid, bool privacy,
                            const uint8_t *ip, int iplen, const uint8_t ds_mac[6])
{
    int ihl = (ip[0] & 0x0f) * 4; if (ihl < 20 || iplen < ihl + 8) return;
    const uint8_t *udp = ip + ihl; const uint8_t *bootp = udp + 8;
    int bootp_len = iplen - ihl - 8; if (bootp_len < 240) return;
    uint32_t xid = rdbe32(bootp+4);
    int msgtype = 0; const uint8_t *opt = bootp + 240, *end = bootp + bootp_len;
    while (opt + 2 <= end && *opt != 0xff) {
        if (*opt == 0) { opt++; continue; }
        if (opt[0] == 53 && opt[1] >= 1) msgtype = opt[2];
        opt += 2 + opt[1];
    }
    if (msgtype != 1 && msgtype != 3) return;           /* DISCOVER or REQUEST */
    int reply = (msgtype == 1) ? 2 : 5;                 /* OFFER : ACK */
    printf("[dhcp] %s from DS -> %s (192.168.44.20)\n", msgtype==1?"DISCOVER":"REQUEST", reply==2?"OFFER":"ACK");

    uint8_t bp[300]; memset(bp, 0, sizeof bp);
    bp[0]=2; bp[1]=1; bp[2]=6; wrbe32(bp+4, xid);
    wrbe32(bp+16, WNET_DS); wrbe32(bp+20, WNET_GW);
    memcpy(bp+28, ds_mac, 6);
    bp[236]=0x63; bp[237]=0x82; bp[238]=0x53; bp[239]=0x63;
    int o = 240;
    bp[o++]=53; bp[o++]=1; bp[o++]=(uint8_t)reply;
    bp[o++]=54; bp[o++]=4; wrbe32(bp+o, WNET_GW);   o+=4;
    bp[o++]=51; bp[o++]=4; wrbe32(bp+o, 86400);     o+=4;
    bp[o++]=1;  bp[o++]=4; wrbe32(bp+o, WNET_MASK); o+=4;
    bp[o++]=3;  bp[o++]=4; wrbe32(bp+o, WNET_GW);   o+=4;
    bp[o++]=6;  bp[o++]=4; wrbe32(bp+o, WNET_DNS);  o+=4;
    bp[o++]=0xff;
    int bplen = o;

    uint8_t pkt[400];
    int udplen = 8 + bplen, totlen = 20 + udplen;
    memset(pkt, 0, 20);
    pkt[0]=0x45; wrbe16(pkt+2, (uint16_t)totlen); pkt[8]=64; pkt[9]=17;
    wrbe32(pkt+12, WNET_GW); wrbe32(pkt+16, WNET_DS);
    wrbe16(pkt+10, inet_cksum(pkt, 20));
    wrbe16(pkt+20, 67); wrbe16(pkt+22, 68);
    wrbe16(pkt+24, (uint16_t)udplen); wrbe16(pkt+26, 0);
    memcpy(pkt+28, bp, bplen);
    uint8_t eth[420];
    memcpy(eth+0, ds_mac, 6); memcpy(eth+6, GW_MAC, 6); wrbe16(eth+12, 0x0800);
    memcpy(eth+14, pkt, totlen);
    win_send_eth(h, mac, ssid, privacy, eth, 14 + totlen);
}

/* DS data frame -> decrypt -> Ethernet -> dispatch (ARP/DHCP in-probe; normal IP -> Wintun). */
static void win_rx_dsdata(libusb_device_handle *h, const uint8_t *frame, int frame_len,
                          const uint8_t mac[6], const char *ssid, bool privacy)
{
    if (!g_win_bridge || frame_len < 24) return;
    uint16_t fc = get16(frame);
    if (((fc >> 2) & 0x3) != 2) return;
    uint8_t subtype = (uint8_t)((fc >> 4) & 0xf);
    if (subtype & 0x4) return;
    int hdrlen = 24; if (subtype & 0x8) hdrlen += 2;
    bool prot = (fc & 0x4000) != 0;
    if (frame_len < hdrlen + 8) return;
    uint8_t swbuf[2320]; const uint8_t *plain=NULL; int plain_len=0;
    if (prot) {
        int off = hdrlen + 4, hw_len = frame_len - off - 4;
        if (hw_len >= 8 && memcmp(frame+off, SNAP_HDR, 6)==0) { plain=frame+off; plain_len=hw_len; }
        else { uint8_t wk[13]; derive_original_wep_key(ssid+12, wk); int pl=0;
            if (wep_decrypt_body(frame+hdrlen, frame_len-hdrlen, wk, swbuf, (int)sizeof swbuf, &pl)
                && pl>=8 && memcmp(swbuf, SNAP_HDR,6)==0){ plain=swbuf; plain_len=pl; } }
    } else { int pl=frame_len-hdrlen;
        if (pl>=8 && memcmp(frame+hdrlen, SNAP_HDR,6)==0){ plain=frame+hdrlen; plain_len=pl; } }
    if (!plain || plain_len < 8) return;
    const uint8_t *ds_mac = frame + 10;                 /* addr2 = SA (DS) */
    uint16_t et = (uint16_t)((plain[6]<<8)|plain[7]);
    const uint8_t *pay = plain + 8; int paylen = plain_len - 8;
    g_last_ds_rx = GetTickCount();                      /* the DS is alive; keep the bridge open */
    if (nintendo_only() && !is_nintendo_mac(ds_mac)) return;   /* only bridge for a genuine Nintendo client */
    if (!g_sta_known){ memcpy(g_sta_mac, ds_mac, 6); g_sta_known=1;
        printf("[bridge] learned DS station %02x:%02x:%02x:%02x:%02x:%02x\n",
               ds_mac[0],ds_mac[1],ds_mac[2],ds_mac[3],ds_mac[4],ds_mac[5]);
        /* The DS is now ASSOCIATED and sending data -- auth (whose precise auto-ACK timing forbids
         * above-calibrated TX power) is finished. Our RX of the DS is clean but TX *to* it is
         * marginal on Windows, so raise TX power to max for the data phase to lift the link margin
         * (DHCP/DNS/conntest replies). Reverts to calibrated power only on a fresh AP restart.
         * NWC_DATA_TXPOWER=n overrides (0 disables the boost). */
        const char *bp = getenv("NWC_DATA_TXPOWER");
        int boost = (bp && *bp) ? atoi(bp) : 31;
        if (boost > 0) {
            if (boost > 31) boost = 31;
            config_channel_rf2525e(h, 1, (uint8_t)boost);
            printf("[rf] data-phase TX power boosted to %d (post-association)\n", boost);
        }
    }
    if (et == 0x0806) { win_handle_arp(h, mac, ssid, privacy, pay, paylen, ds_mac); return; }
    if (et == 0x0800 && paylen >= 28 && (pay[0]&0xf0)==0x40) {
        int ihl=(pay[0]&0x0f)*4;
        printf("[rxip] proto=%u dst=%u.%u.%u.%u dport=%d len=%d\n", pay[9],
               pay[16],pay[17],pay[18],pay[19],
               ((pay[9]==17||pay[9]==6)&&paylen>=ihl+4)?rdbe16(pay+ihl+2):-1, paylen);
        if (pay[9]==17 && paylen>=ihl+8 && rdbe16(pay+ihl+2)==67) {
            win_handle_dhcp(h, mac, ssid, privacy, pay, paylen, ds_mac); return;
        }
        if (pay[9]==17 && paylen>=ihl+8 && rdbe16(pay+ihl+2)==53) {   /* DNS -> handle in-probe */
            win_handle_dns(h, mac, ssid, privacy, pay, paylen, ds_mac); return;
        }
        /* Clear the re-send cache only when the DS's TCP ACK number reaches past the cached segment
         * (it truly received it) -- NOT on any stray reply. Signed diff handles seq wraparound. */
        if (g_synack.active && pay[9]==6 && rdbe32(pay+16)==g_synack.srv_ip
            && paylen>=ihl+20 && rdbe16(pay+ihl+2)==g_synack.srv_port && (pay[ihl+13] & 0x10)) {
            uint32_t ackno = rdbe32(pay+ihl+8);
            if ((int32_t)(ackno - g_synack.end_seq) >= 0) {
                g_synack.active = 0;
                printf("[synack] DS ACKed %u.%u.%u.%u:%u (ack=%u >= %u) -> cache cleared\n",
                       pay[16],pay[17],pay[18],pay[19], g_synack.srv_port, ackno, g_synack.end_seq);
            }
        }
        /* Record the DS<->server flow so the return path can map the server's reply back. */
        if ((pay[9]==6 || pay[9]==17) && paylen >= ihl+4)
            rawret_note_flow(rdbe32(pay+16), rdbe16(pay+ihl+2), rdbe16(pay+ihl));
        if (g_wd_active) {
            /* Full userspace NAT: SNAT + WinDivertSend straight to the WAN, bypassing WinNAT (which
             * unreliably swallows gamespy returns). The reply is captured by the WinDivert thread. */
            wd_nat_outbound(pay, paylen);
            printf("[bridge] DS->net (windivert) proto=%u dst=%u.%u.%u.%u len=%d\n",
                   pay[9], pay[16],pay[17],pay[18],pay[19], paylen);
        } else { BYTE *wp = pWtAlloc(g_wt_session, (DWORD)paylen);
          if (wp){ memcpy(wp, pay, (size_t)paylen); pWtSend(g_wt_session, wp);
              printf("[bridge] DS->net proto=%u dst=%u.%u.%u.%u len=%d\n",
                     pay[9], pay[16],pay[17],pay[18],pay[19], paylen); }
          else printf("[bridge] DS->net DROPPED (Wintun ring full) len=%d\n", paylen); }
    }
}

/* Wintun -> IP packets (NAT return) -> Ethernet to the DS -> 802.11 + WEP. */
static void win_poll_wintun(libusb_device_handle *h, const uint8_t mac[6], const char *ssid, bool privacy)
{
    if (!g_win_bridge || !g_sta_known) return;
    /* If the DS has gone quiet (e.g. it left after an earlier attempt), stop forwarding: Windows
     * keeps NAT-returning stale TCP retransmits to the old DS IP, and draining that flood -- with
     * the TX pacing gap -- starves the beacon, so the DS can no longer even see the AP (51303). */
    if ((GetTickCount() - g_last_ds_rx) > 6000u) {
        g_sta_known = 0;
        printf("[bridge] DS silent >6s -> closing bridge (stop forwarding stale traffic)\n");
        return;
    }
    /* Drain the Wintun ring per call. The old cap of 4 was a Windows-only throttle (Linux used
     * kernel NAT + TAP with no such limit) that could leave server->DS bursts backed up in the
     * ring: during the GPCM login the server's challenge/response PLUS Windows NAT retransmits
     * exceed 4/iteration, the ring overflows, a login segment is lost, and the DS reports 61010
     * "communication error while logging in". Drain deeper (NWC_WT_DRAIN, default 16) so the
     * ring stays empty; the beacon already went out at the top of the loop, so this can't delay
     * the CURRENT beacon, and the per-frame TX is small. Count leftover backlog for diagnosis. */
    static int wt_drain = -1;
    if (wt_drain < 0) { const char *e = getenv("NWC_WT_DRAIN"); wt_drain = (e&&*e)?atoi(e):16;
                        if (wt_drain < 4) wt_drain = 4; if (wt_drain > 64) wt_drain = 64; }
    int i; for (i=0;i<wt_drain;i++){
        DWORD sz=0; BYTE *p = pWtRecv(g_wt_session, &sz);
        if (!p) break;
        /* DIAG: log EVERY packet arriving on Wintun (pre-filter) from a gamespy/NAS server, so we can
         * see whether New-NetNat delivered the 171-byte login result here at all (vs swallowed it). */
        if (sz >= 20 && (p[0]&0xf0)==0x40) { uint32_t s=rdbe32(p+12), d=rdbe32(p+16);
            if (s==0x5FD94D97u || s==0xBC22B1D9u || s==0x4E2EE79Bu) {   /* 95.217.77.151 / 188.34.177.217 / 78.46.231.155 */
                static unsigned long ll=0; unsigned long nn=GetTickCount();
                if (nn-ll>200u){ ll=nn; int ih=(p[0]&0xf)*4;
                    printf("[wtrx] from %u.%u.%u.%u:%u -> %u.%u.%u.%u proto=%u len=%lu\n",
                       (s>>24)&0xff,(s>>16)&0xff,(s>>8)&0xff,s&0xff, (sz>=(DWORD)(ih+2))?rdbe16(p+ih):0,
                       (d>>24)&0xff,(d>>16)&0xff,(d>>8)&0xff,d&0xff, p[9], (unsigned long)sz); } } }
        /* Only forward NAT-return traffic actually destined for the DS. Windows' own local
         * services (ICS DNS proxy, NetBIOS/LLMNR/SSDP) spray UDP from the gateway IP and to
         * broadcast/multicast onto this interface; forwarding that junk floods the DS and
         * starves the RT2570's marginal TX path (it was crowding out our DHCP ACK). */
        if (sz >= 20 && (p[0]&0xf0)==0x40 && 14 + (int)sz <= 1600
            && rdbe32(p+16) == WNET_DS && rdbe32(p+12) != WNET_GW) {
            uint8_t eth[1600];
            memcpy(eth+0, g_sta_mac, 6); memcpy(eth+6, GW_MAC, 6); wrbe16(eth+12, 0x0800);
            memcpy(eth+14, p, sz);
            /* The downstream (net->DS) link is marginally lossy on Windows and retry_limit=0 means
             * the hardware never retransmits, so larger NAS/HTTP responses are dropped and the DS
             * stalls (52103) even though USB TX succeeds. Send each frame a couple of times; TCP
             * silently discards the duplicates. NWC_TX_DUP overrides (1 = off). */
            static int tx_dup = -1;
            if (tx_dup < 0) { const char *e = getenv("NWC_TX_DUP"); tx_dup = (e&&*e)?atoi(e):2;
                              if (tx_dup < 1) tx_dup = 1; if (tx_dup > 4) tx_dup = 4; }
            for (int d = 0; d < tx_dup; d++) win_send_eth(h, mac, ssid, privacy, eth, 14 + (int)sz);
            printf("[bridge] net->DS proto=%u src=%u.%u.%u.%u len=%lu x%d\n",
                   p[9], p[12],p[13],p[14],p[15], (unsigned long)sz, tx_dup);
            /* Cache a server->DS TCP SYN-ACK or PSH-data segment so we keep re-delivering it (Windows
             * NAT won't). SYN-ACK completes the handshake; PSH data carries the gpcm challenge and the
             * \lc\2\ login result -- both were being retransmitted by the server ~40x and lost. */
            { int ihl2 = (p[0]&0x0f)*4; int fl = (sz >= (DWORD)(ihl2+14)) ? p[ihl2+13] : 0;
              if (p[9]==6 && sz >= (DWORD)(ihl2+14) && ((fl & 0x12)==0x12 || (fl & 0x08))
                  && (14 + (int)sz) <= (int)sizeof(g_synack.eth)) {
                  int tcphl = ((p[ihl2+12]>>4)&0xf)*4;
                  int plen = (int)sz - ihl2 - tcphl; if (plen < 0) plen = 0;
                  uint32_t seq = rdbe32(p+ihl2+4);
                  memcpy(g_synack.eth, eth, 14 + (int)sz); g_synack.len = 14 + (int)sz;
                  g_synack.srv_ip = rdbe32(p+12); g_synack.srv_port = rdbe16(p+ihl2);
                  g_synack.ds_port = rdbe16(p+ihl2+2);
                  /* SYN and FIN each consume 1 seq; data consumes its length. Clear only once the DS
                   * ACKs past this -> we keep re-delivering THIS exact segment (challenge, \lc\2\
                   * result) until the DS truly has it, not merely until it sends some other ACK. */
                  g_synack.end_seq = seq + (uint32_t)plen + (((fl & 0x02) || (fl & 0x01)) ? 1u : 0u);
                  g_synack.last_send = GetTickCount(); g_synack.resends = 20; g_synack.active = 1;
                  printf("[synack] cached %s from %u.%u.%u.%u:%u seq=%u+%u -> re-deliver until DS ACKs it\n",
                         (fl & 0x08) ? "PSH-data" : "SYN-ACK", p[12],p[13],p[14],p[15],
                         g_synack.srv_port, seq, (unsigned)plen); (void)plen;
              } }
        }
        pWtRelease(g_wt_session, p);
    }
    /* Pump the cached SYN-ACK: re-send to the DS every ~150ms until it ACKs (cleared in
     * win_rx_dsdata) or the bound is hit -- gives the DS the many delivery chances that Linux's
     * kernel NAT provides but Windows' New-NetNat does not. Small frame, doesn't disturb beacon. */
    if (g_synack.active && g_synack.resends > 0) {
        unsigned long now = GetTickCount();
        if (now - g_synack.last_send >= 150u) {
            win_send_eth(h, mac, ssid, privacy, g_synack.eth, g_synack.len);
            g_synack.last_send = now;
            if (--g_synack.resends == 0) { g_synack.active = 0;
                printf("[synack] re-delivery bound reached for :%u (DS never ACKed)\n", g_synack.srv_port); }
        }
    }
    if (i >= wt_drain) {
        static unsigned long last_backlog = 0; unsigned long now = GetTickCount();
        if (now - last_backlog > 500u) { last_backlog = now;
            printf("[bridge] net->DS drain hit cap %d (ring may be backing up)\n", wt_drain); }
    }
}

/* ===================== RAW-SOCKET RETURN PATH (bypass New-NetNat) =====================
 * pktmon proved New-NetNat systematically drops some server->DS segments (e.g. the 171-byte gpcm
 * \lc\2\ login result) even though they arrive at the WAN NIC -- so the DS never gets them and login
 * fails (61010). Instead of trusting New-NetNat's reverse delivery to Wintun, we sniff inbound
 * packets straight off the WAN via a SIO_RCVALL raw socket, match them to a DS<->server flow, rewrite
 * the destination back to the DS, and inject them ourselves. New-NetNat still handles the OUTBOUND
 * SNAT (which works). Duplicates for segments New-NetNat also delivers are harmless (TCP dedups). */
#ifndef SIO_RCVALL
#define SIO_RCVALL 0x98000001
#endif
static SOCKET g_rawret = INVALID_SOCKET;
static uint32_t g_wan_ip = 0;
static struct { uint32_t srv_ip; uint16_t srv_port, ds_port; unsigned long tick; int used; } g_flows[64];

/* Record a DS->server flow so the raw return path knows which DS port a server reply maps back to. */
static void rawret_note_flow(uint32_t srv_ip, uint16_t srv_port, uint16_t ds_port)
{
    unsigned long now = GetTickCount();
    for (int i = 0; i < 64; i++)
        if (g_flows[i].used && g_flows[i].srv_ip==srv_ip && g_flows[i].srv_port==srv_port) {
            g_flows[i].ds_port = ds_port; g_flows[i].tick = now; return;   /* newest DS port wins */
        }
    for (int i = 0; i < 64; i++)
        if (!g_flows[i].used || now - g_flows[i].tick > 60000u) {
            g_flows[i].srv_ip=srv_ip; g_flows[i].srv_port=srv_port; g_flows[i].ds_port=ds_port;
            g_flows[i].tick=now; g_flows[i].used=1; return;
        }
}

/* Full L4 checksum (TCP proto 6 / UDP proto 17) over the pseudo-header + segment. */
static uint16_t l4_cksum(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t *l4, int l4len)
{
    uint32_t sum = 0;
    sum += (src>>16)&0xffff; sum += src&0xffff; sum += (dst>>16)&0xffff; sum += dst&0xffff;
    sum += proto; sum += (uint32_t)l4len;
    int i; for (i=0;i+1<l4len;i+=2) sum += (uint32_t)((l4[i]<<8)|l4[i+1]);
    if (l4len&1) sum += (uint32_t)(l4[l4len-1]<<8);
    while (sum>>16) sum=(sum&0xffff)+(sum>>16);
    uint16_t c=(uint16_t)~sum; return (proto==17 && c==0) ? 0xffff : c;
}

static void rawret_open(void)
{
    if (env_on("NWC_NO_RAWRETURN")) { printf("[rawret] disabled (NWC_NO_RAWRETURN)\n"); return; }
    /* Determine our WAN IP (the local address the default route uses). NWC_WAN_IP overrides. */
    const char *we = getenv("NWC_WAN_IP");
    if (we && *we) g_wan_ip = ntohl(inet_addr(we));
    else {
        SOCKET t = socket(AF_INET, SOCK_DGRAM, 0);
        if (t != INVALID_SOCKET) {
            struct sockaddr_in d; memset(&d,0,sizeof d); d.sin_family=AF_INET;
            d.sin_port=htons(53); d.sin_addr.s_addr=htonl(0x08080808u);   /* 8.8.8.8 (no packet sent) */
            if (connect(t,(struct sockaddr*)&d,sizeof d)==0) {
                struct sockaddr_in l; int ll=sizeof l;
                if (getsockname(t,(struct sockaddr*)&l,&ll)==0) g_wan_ip=ntohl(l.sin_addr.s_addr);
            }
            closesocket(t);
        }
    }
    if (!g_wan_ip) { printf("[rawret] could not determine WAN IP; raw return OFF\n"); return; }
    g_rawret = socket(AF_INET, SOCK_RAW, IPPROTO_IP);
    if (g_rawret == INVALID_SOCKET) { printf("[rawret] socket() failed %d (need admin); OFF\n", WSAGetLastError()); return; }
    struct sockaddr_in sa; memset(&sa,0,sizeof sa); sa.sin_family=AF_INET; sa.sin_addr.s_addr=htonl(g_wan_ip);
    if (bind(g_rawret,(struct sockaddr*)&sa,sizeof sa)!=0) { printf("[rawret] bind failed %d; OFF\n", WSAGetLastError()); closesocket(g_rawret); g_rawret=INVALID_SOCKET; return; }
    DWORD inv=1, outv=0, br=0;
    if (WSAIoctl(g_rawret, SIO_RCVALL, &inv,sizeof inv,&outv,sizeof outv,&br,NULL,NULL)!=0)
        printf("[rawret] SIO_RCVALL failed %d (may still work)\n", WSAGetLastError());
    u_long nb=1; ioctlsocket(g_rawret, FIONBIO, &nb);
    printf("[rawret] raw return path ARMED on WAN %u.%u.%u.%u -- bypassing New-NetNat reverse delivery\n",
           (g_wan_ip>>24)&0xff,(g_wan_ip>>16)&0xff,(g_wan_ip>>8)&0xff,g_wan_ip&0xff);
}

/* Poll the raw socket: for each inbound server->WAN packet matching a known DS flow, rewrite the
 * destination to the DS and inject it. Called each main-loop iteration. */
static void rawret_poll(libusb_device_handle *h, const uint8_t mac[6], const char *ssid, bool privacy)
{
    if (g_rawret==INVALID_SOCKET) return;
    static uint8_t buf[65536];        /* full IP datagram; small buffer -> WSAEMSGSIZE drops packets */
    static unsigned long g_rawrx_total = 0, g_rawrx_tous = 0, last_diag = 0;
    { unsigned long nn=GetTickCount();
      if (nn-last_diag > 5000u) { last_diag=nn;
          printf("[rawret] diag: rawrx_total=%lu to_us=%lu (WAN=%u.%u.%u.%u)\n", g_rawrx_total, g_rawrx_tous,
                 (g_wan_ip>>24)&0xff,(g_wan_ip>>16)&0xff,(g_wan_ip>>8)&0xff,g_wan_ip&0xff); } }
    for (int n=0; n<128; n++) {
        int r = recv(g_rawret, (char*)buf, sizeof buf, 0);
        if (r <= 0) {
            int e = WSAGetLastError();
            if (e == WSAEMSGSIZE) continue;   /* oversized datagram truncated+removed -> keep draining */
            break;                            /* WSAEWOULDBLOCK / other -> no more data this pass */
        }
        g_rawrx_total++;
        if (r < 28 || (buf[0]&0xf0)!=0x40) continue;
        int ihl=(buf[0]&0x0f)*4; if (ihl<20 || ihl+4>r) continue;
        uint8_t proto=buf[9]; if (proto!=6 && proto!=17) continue;
        uint32_t src=rdbe32(buf+12), dst=rdbe32(buf+16);
        if (dst == g_wan_ip) g_rawrx_tous++;
        if (dst != g_wan_ip) continue;                     /* only inbound destined to us */
        uint16_t sport=rdbe16(buf+ihl), dport=rdbe16(buf+ihl+2);
        /* DIAG: sample to_us TCP sources so we can see whether the gamespy/NAS server replies are
         * even visible to the raw socket (vs consumed by New-NetNat's WFP layer first). */
        if (proto==6) { static unsigned long ls=0; unsigned long nn=GetTickCount();
            if (nn-ls > 700u) { ls=nn;
                printf("[rawret] seen-tcp src=%u.%u.%u.%u:%u -> :%u (flows note gamespy replies here)\n",
                       (src>>24)&0xff,(src>>16)&0xff,(src>>8)&0xff,src&0xff, sport, dport); } }
        (void)dport;
        /* Match a DS flow by (server ip, server port). */
        uint16_t ds_port=0; int hit=-1;
        for (int i=0;i<64;i++) if (g_flows[i].used && g_flows[i].srv_ip==src && g_flows[i].srv_port==sport) { ds_port=g_flows[i].ds_port; hit=i; break; }
        if (hit<0) continue;
        int iplen = rdbe16(buf+2); if (iplen>r) iplen=r;
        if (14+iplen > 1600) continue;
        /* Rewrite dst IP -> DS, dst port -> the DS's original port, fix checksums. */
        wrbe32(buf+16, WNET_DS);
        wrbe16(buf+ihl+2, ds_port);
        wrbe16(buf+10, 0); wrbe16(buf+10, inet_cksum(buf,ihl));           /* IP header cksum */
        int l4len = iplen - ihl;
        if (proto==6 && l4len>=18) { wrbe16(buf+ihl+16,0); wrbe16(buf+ihl+16, l4_cksum(src,WNET_DS,6,buf+ihl,l4len)); }
        else if (proto==17 && l4len>=8) { wrbe16(buf+ihl+6,0); wrbe16(buf+ihl+6, l4_cksum(src,WNET_DS,17,buf+ihl,l4len)); }
        uint8_t eth[1600];
        memcpy(eth+0, g_sta_mac, 6); memcpy(eth+6, GW_MAC, 6); wrbe16(eth+12,0x0800);
        memcpy(eth+14, buf, (size_t)iplen);
        win_send_eth(h, mac, ssid, privacy, eth, 14+iplen);
        static unsigned long last_log=0; unsigned long now=GetTickCount();
        if (now-last_log > 200u) { last_log=now;
            printf("[rawret] inject srv %u.%u.%u.%u:%u -> DS:%u proto=%u len=%d\n",
                   (src>>24)&0xff,(src>>16)&0xff,(src>>8)&0xff,src&0xff, sport, ds_port, proto, iplen); }
    }
}

/* ===================== WINDIVERT RETURN PATH (the real New-NetNat bypass) =====================
 * [wtrx] proof: New-NetNat delivers short-lived NAS/HTTP fully but SILENTLY SWALLOWS later packets on
 * the persistent gamespy connection (delivered the 38-byte gpcm challenge, dropped the 171-byte \lc\2\
 * login result) -- so the DS never completes login (61010). A SIO_RCVALL raw socket can't help because
 * New-NetNat's WFP callout rewrites/consumes the return before the socket sees it. WinDivert installs
 * its OWN WFP callout at the INBOUND IP-packet layer at high priority, so it intercepts the gamespy
 * returns BEFORE New-NetNat, and we forward them to the DS ourselves -- exactly what Linux's kernel NAT
 * does reliably. A capture thread does WinDivertRecv (blocking) + un-NAT and queues finished ethernet
 * frames; the main loop drains the queue and does the (single-threaded) USB TX. NWC_NO_WINDIVERT off. */
static HANDLE g_wd = NULL;                      /* INVALID_HANDLE_VALUE checked below */
static CRITICAL_SECTION g_wd_cs;
static struct { uint8_t f[1600]; int len; } g_wd_ring[256];
static volatile int g_wd_head = 0, g_wd_tail = 0;
static volatile unsigned long g_wd_seen = 0, g_wd_queued = 0, g_wd_sent = 0;

/* SNAT the DS's outbound IP packet (src -> our WAN IP, DS port preserved) and inject it to the WAN via
 * WinDivertSend. WinDivert injection bypasses Windows' raw-TCP send restriction, so this carries the
 * DS's real TCP/UDP to the servers -- WinNAT is entirely out of the loop (it's the thing that dropped
 * the gamespy returns). The reply comes back to WAN:ds_port and is captured by wd_thread. */
static void wd_nat_outbound(const uint8_t *ip, int len)
{
    if (!g_wd || g_wd==INVALID_HANDLE_VALUE || len < 20 || len > 1500) return;
    uint8_t pkt[1600]; memcpy(pkt, ip, (size_t)len);
    /* MSS CLAMP on outbound SYNs: the DS advertises mss 536, so the gpcm server sends its 171-byte
     * \lc\2\ login result as ONE ~245-byte 802.11 frame whose long air-time loses to gamespy-phase
     * CCA spikes (the 38-byte challenge on the same link gets through). Clamping the advertised MSS
     * makes the server split large responses into small frames (like the challenge) that survive.
     * NWC_MSS overrides (0 disables). */
    { int ih=(pkt[0]&0x0f)*4;
      if (pkt[9]==6 && len>=ih+20 && (pkt[ih+13]&0x02)) {           /* TCP SYN */
        static int clamp=-1; if(clamp<0){ const char *e=getenv("NWC_MSS"); clamp=(e&&*e)?atoi(e):120; }
        if (clamp>0) { int thl=((pkt[ih+12]>>4)&0xf)*4, o=ih+20, endo=ih+thl;
          while (o+1<endo && o+1<len) { uint8_t k=pkt[o];
            if(k==0)break; if(k==1){o++;continue;}
            uint8_t ol=pkt[o+1]; if(ol<2)break;
            if(k==2 && ol==4 && o+3<len){ int m=(pkt[o+2]<<8)|pkt[o+3];
              if(m>clamp){ pkt[o+2]=(uint8_t)(clamp>>8); pkt[o+3]=(uint8_t)clamp; } }
            o+=ol; } } } }
    wrbe32(pkt+12, g_wan_ip);                     /* SNAT source -> WAN IP (port unchanged) */
    WINDIVERT_ADDRESS addr; memset(&addr, 0, sizeof addr);
    addr.Layer = WINDIVERT_LAYER_NETWORK; addr.Event = WINDIVERT_EVENT_NETWORK_PACKET;
    addr.Outbound = 1;
    WinDivertHelperCalcChecksums(pkt, (UINT)len, &addr, 0);
    UINT sent=0;
    if (WinDivertSend(g_wd, pkt, (UINT)len, &sent, &addr)) g_wd_sent++;
    else { static unsigned long le=0; unsigned long nn=GetTickCount();
        if (nn-le>1000u){ le=nn; printf("[windivert] send err gle=%lu\n", GetLastError()); } }
}

static DWORD WINAPI wd_thread(LPVOID arg)
{
    (void)arg;
    static uint8_t buf[65536];
    WINDIVERT_ADDRESS addr; UINT rlen;
    while (g_wd && g_wd != INVALID_HANDLE_VALUE) {
        if (!WinDivertRecv(g_wd, buf, sizeof buf, &rlen, &addr)) { Sleep(1); continue; }
        g_wd_seen++;
        if (rlen < 28 || (buf[0]&0xf0)!=0x40) { WinDivertSend(g_wd, buf, rlen, NULL, &addr); continue; }
        int ihl=(buf[0]&0x0f)*4; if (ihl<20) { WinDivertSend(g_wd, buf, rlen, NULL, &addr); continue; }
        uint8_t proto=buf[9]; uint32_t src=rdbe32(buf+12), dst=rdbe32(buf+16);
        uint16_t sport=rdbe16(buf+ihl);
        int iplen = rdbe16(buf+2); if (iplen>(int)rlen) iplen=(int)rlen;
        /* DIAG: log EVERY captured 95.217 packet (pre-match) so we can see if the 171 result is
         * captured-but-unmatched vs never captured (WinDivert queue drop / outrun). */
        if (src==0x5FD94D97u) { int pl=iplen-ihl-((proto==6)?(((buf[ihl+12]>>4)&0xf)*4):8);
            printf("[wdcap] src=95.217:%u -> dst=%u.%u.%u.%u:%u proto=%u iplen=%d payload=%d flags=0x%02x\n",
                   sport, (dst>>24)&0xff,(dst>>16)&0xff,(dst>>8)&0xff,dst&0xff, rdbe16(buf+ihl+2),
                   proto, iplen, pl, (proto==6 && iplen>=ihl+14)?buf[ihl+13]:0); }
        /* Reply from a server we NAT for, arriving at our WAN IP -> un-NAT to the DS. Match the flow
         * by (server ip, server port); reinject anything that isn't a DS flow so the PC is untouched. */
        uint16_t ds_port=0; int hit=0;
        if (dst==g_wan_ip)
            for (int i=0;i<64;i++) if (g_flows[i].used && g_flows[i].srv_ip==src && g_flows[i].srv_port==sport) { ds_port=g_flows[i].ds_port; hit=1; break; }
        if (!hit || !g_sta_known || 14+iplen > 1600) { WinDivertSend(g_wd, buf, rlen, NULL, &addr); continue; }
        wrbe32(buf+16, WNET_DS); wrbe16(buf+ihl+2, ds_port);   /* dst -> DS (port already preserved) */
        /* Use WinDivert's own checksum helper (definitively correct) instead of a hand-rolled one --
         * rules out any edge case on larger/odd segments like the 171-byte gpcm login result. */
        { WINDIVERT_ADDRESS ca; memset(&ca,0,sizeof ca); ca.Layer=WINDIVERT_LAYER_NETWORK;
          ca.Event=WINDIVERT_EVENT_NETWORK_PACKET;
          WinDivertHelperCalcChecksums(buf, (UINT)iplen, &ca, 0); }
        /* BARE-ACK storm-breaker: for a server->DS TCP DATA segment (PSH), also queue a headers-only
         * copy (~40 bytes, no data, PSH cleared). That tiny frame carries the server's ACK of the DS's
         * login and survives the OTA far better than the full data frame -- so the DS stops its own
         * login-retransmit storm even while the data is still in flight, dropping CCA so the data can
         * then land. NWC_NO_BAREACK disables. */
        if (proto==6 && (buf[ihl+13]&0x08) && (buf[ihl+13]&0x10) && !env_on("NWC_NO_BAREACK")) {
            int tcphl=((buf[ihl+12]>>4)&0xf)*4; int bl=ihl+tcphl;
            if (bl>=40 && bl<=80) {
                uint8_t bare[80]; memcpy(bare, buf, (size_t)bl);
                wrbe16(bare+2, (uint16_t)bl);          /* IP total length = headers only */
                bare[ihl+13] &= ~0x08;                 /* clear PSH (pure ACK) */
                WINDIVERT_ADDRESS ca2; memset(&ca2,0,sizeof ca2); ca2.Layer=WINDIVERT_LAYER_NETWORK;
                ca2.Event=WINDIVERT_EVENT_NETWORK_PACKET;
                WinDivertHelperCalcChecksums(bare, (UINT)bl, &ca2, 0);
                EnterCriticalSection(&g_wd_cs);
                int nhb=(g_wd_head+1)%256;
                if (nhb != g_wd_tail) { uint8_t *fb=g_wd_ring[g_wd_head].f;
                    memcpy(fb, g_sta_mac,6); memcpy(fb+6, GW_MAC,6); wrbe16(fb+12,0x0800);
                    memcpy(fb+14, bare, (size_t)bl); g_wd_ring[g_wd_head].len=14+bl; g_wd_head=nhb; g_wd_queued++; }
                LeaveCriticalSection(&g_wd_cs);
            }
        }
        { static unsigned long ll=0; unsigned long nn=GetTickCount();
          if (nn-ll>120u){ ll=nn;
            printf("[windivert] recv srv=%u.%u.%u.%u:%u -> DS:%u proto=%u len=%d (payload=%d)\n",
                   (src>>24)&0xff,(src>>16)&0xff,(src>>8)&0xff,src&0xff, sport, ds_port, proto, iplen, iplen-ihl-((proto==6)?((buf[ihl+12]>>4)*4):8)); } }
        EnterCriticalSection(&g_wd_cs);
        int nh=(g_wd_head+1)%256;
        if (nh != g_wd_tail) {
            uint8_t *f=g_wd_ring[g_wd_head].f;
            memcpy(f, g_sta_mac,6); memcpy(f+6, GW_MAC,6); wrbe16(f+12,0x0800);
            memcpy(f+14, buf, (size_t)iplen); g_wd_ring[g_wd_head].len=14+iplen; g_wd_head=nh; g_wd_queued++;
            /* Cache TCP PSH-data (gpcm challenge / \lc\2\ result) so the main-loop pump re-delivers it
             * every ~120ms until the DS ACKs it -- the 211-byte result is lossy OTA under gamespy-phase
             * CCA spikes and the server's own retransmit is a slow ~1s RTO, which spirals into a storm.
             * Fast local re-delivery lets the DS ACK before the storm builds. Clears in win_rx_dsdata. */
            if (env_on("NWC_WD_RESEND") && proto==6 && (buf[ihl+13]&0x08) && (14+iplen)<=(int)sizeof(g_synack.eth)) {
                int tcphl=((buf[ihl+12]>>4)&0xf)*4; int plen=iplen-ihl-tcphl; if(plen<0)plen=0;
                memcpy(g_synack.eth, f, 14+iplen); g_synack.len=14+iplen;
                g_synack.srv_ip=src; g_synack.srv_port=sport; g_synack.ds_port=ds_port;
                g_synack.end_seq=rdbe32(buf+ihl+4)+(uint32_t)plen;
                g_synack.last_send=GetTickCount(); g_synack.resends=16; g_synack.active=1;
            }
        }
        LeaveCriticalSection(&g_wd_cs);
        /* DS-bound: consumed (not reinjected) -> we deliver it to the DS over USB */
    }
    return 0;
}

static void wd_open(void)
{
    if (env_on("NWC_NO_WINDIVERT")) { printf("[windivert] disabled (NWC_NO_WINDIVERT) -> Wintun+WinNAT path\n"); return; }
    if (!g_wan_ip) {   /* determine WAN IP independently of rawret */
        const char *we=getenv("NWC_WAN_IP");
        if (we&&*we) g_wan_ip=ntohl(inet_addr(we));
        else { SOCKET t=socket(AF_INET,SOCK_DGRAM,0);
            if (t!=INVALID_SOCKET){ struct sockaddr_in d; memset(&d,0,sizeof d); d.sin_family=AF_INET;
                d.sin_port=htons(53); d.sin_addr.s_addr=htonl(0x08080808u);
                if (connect(t,(struct sockaddr*)&d,sizeof d)==0){ struct sockaddr_in l; int ll=sizeof l;
                    if (getsockname(t,(struct sockaddr*)&l,&ll)==0) g_wan_ip=ntohl(l.sin_addr.s_addr); }
                closesocket(t); } }
    }
    if (!g_wan_ip) { printf("[windivert] no WAN IP; full NAT OFF\n"); return; }
    /* FULL userspace NAT at the NETWORK layer: we inject the DS's SNAT'd traffic outbound and capture
     * the replies inbound HERE -- WinNAT is not in the path at all, so it can't swallow gamespy returns.
     * Filter TIGHTLY to inbound from the Wiimmfi server IPs only (resolved now) -- a broad dst==WAN
     * filter grabbed the PC's ENTIRE inbound (40k+ pkts), whose reinjection jittered USB/beacon timing
     * and cost the DS visibility (51303). High priority so we see replies before the local stack RSTs
     * them. NWC_WIIMMFI_IPS can add extra IPs (comma list). */
    uint32_t wips[24]; int nw=0;
    /* Seed with the known Wiimmfi server IPs so a momentary resolve miss can't drop a flow (Wiimmfi
     * rotates NAS/conntest between 78.46.231.155 and 188.34.177.217; gamespy = 95.217.77.151). */
    { uint32_t seed[]={0x4E2EE79Bu,0xBC22B1D9u,0x5FD94D97u}; for (unsigned k=0;k<3;k++) wips[nw++]=seed[k]; }
    static const char *hn[] = { "nas.wiimmfi.de","gpcm.gs.wiimmfi.de","gpsp.gs.wiimmfi.de",
        "conntest.wiimmfi.de","master.gs.wiimmfi.de","natneg1.gs.wiimmfi.de","sake.gs.wiimmfi.de",
        "mariokartds.available.gs.wiimmfi.de" };
    for (int i=0;i<(int)(sizeof hn/sizeof hn[0]) && nw<16;i++) {
        struct addrinfo h2, *res=NULL; memset(&h2,0,sizeof h2); h2.ai_family=AF_INET;
        if (getaddrinfo(hn[i], NULL, &h2, &res)==0 && res) {
            uint32_t ip=ntohl(((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr);
            int dup=0; for (int j=0;j<nw;j++) if (wips[j]==ip) dup=1;
            if (!dup && ip) wips[nw++]=ip;
        }
        if (res) freeaddrinfo(res);
    }
    if (nw==0) { printf("[windivert] could not resolve any Wiimmfi IP; full NAT OFF (New-NetNat fallback)\n"); return; }
    /* PEER-NAT WIDENING (Linux-parity fix). The DS's matchmaking hole-punches to ARBITRARY
     * peer IPs over UDP (natneg, then P2P game traffic e.g. 190.6.7.157:13213) -- NOT just the
     * fixed Wiimmfi server IPs. Linux's kernel MASQUERADE (conntrack) returns any peer reply
     * transparently; the old fixed-SrcAddr filter here would DROP peer replies, so a race could
     * never connect even after GPCM works. Fix: keep the tight TCP filter (server IPs only, so
     * we don't grab the PC's TCP browsing) but ALSO capture all inbound UDP to our WAN IP
     * (gamespy QR2/natneg + peer P2P). The g_flows table -- populated for EVERY DS outbound
     * flow incl. peers -- lets wd_thread separate DS replies from the PC's own UDP (non-matching
     * UDP is re-injected to the local stack). QUIC (srcport 443) is excluded so the PC's own web
     * UDP adds no load. NWC_NO_WD_PEERS reverts to the old server-IPs-only behavior. */
    int wd_peers = !env_on("NWC_NO_WD_PEERS");
    char filter[640]; int fo=0;
    fo += _snprintf(filter+fo, sizeof(filter)-fo, "inbound and ((tcp and (");
    for (int i=0;i<nw;i++)
        fo += _snprintf(filter+fo, sizeof(filter)-fo, "%sip.SrcAddr==%u.%u.%u.%u",
                        i?" or ":"", (wips[i]>>24)&0xff,(wips[i]>>16)&0xff,(wips[i]>>8)&0xff,wips[i]&0xff);
    fo += _snprintf(filter+fo, sizeof(filter)-fo, "))");
    if (wd_peers) {
        fo += _snprintf(filter+fo, sizeof(filter)-fo,
                        " or (udp and ip.DstAddr==%u.%u.%u.%u and udp.SrcPort!=443)",
                        (g_wan_ip>>24)&0xff,(g_wan_ip>>16)&0xff,(g_wan_ip>>8)&0xff,g_wan_ip&0xff);
    } else {
        fo += _snprintf(filter+fo, sizeof(filter)-fo, " or (udp and (");
        for (int i=0;i<nw;i++)
            fo += _snprintf(filter+fo, sizeof(filter)-fo, "%sip.SrcAddr==%u.%u.%u.%u",
                            i?" or ":"", (wips[i]>>24)&0xff,(wips[i]>>16)&0xff,(wips[i]>>8)&0xff,wips[i]&0xff);
        fo += _snprintf(filter+fo, sizeof(filter)-fo, "))");
    }
    _snprintf(filter+fo, sizeof(filter)-fo, ")");
    g_wd = WinDivertOpen(filter, WINDIVERT_LAYER_NETWORK, 1000, 0);
    if (g_wd == INVALID_HANDLE_VALUE) {
        g_wd = NULL;
        printf("[windivert] open FAILED gle=%lu (driver load blocked? need admin / AV allow)\n", GetLastError());
        return;
    }
    WinDivertSetParam(g_wd, WINDIVERT_PARAM_QUEUE_LENGTH, WINDIVERT_PARAM_QUEUE_LENGTH_MAX);
    WinDivertSetParam(g_wd, WINDIVERT_PARAM_QUEUE_TIME, WINDIVERT_PARAM_QUEUE_TIME_MAX);
    InitializeCriticalSection(&g_wd_cs);
    CreateThread(NULL, 0, wd_thread, NULL, 0, NULL);
    g_wd_active = 1;
    printf("[windivert] FULL NAT ARMED (WinNAT bypassed): DS<->WAN via WinDivert, filter='%s'\n", filter);
}

static void wd_drain(libusb_device_handle *h, const uint8_t mac[6], const char *ssid, bool privacy)
{
    if (!g_wd || g_wd == INVALID_HANDLE_VALUE) return;
    { static unsigned long ld=0; unsigned long nn=GetTickCount();
      if (nn-ld>3000u){ ld=nn; printf("[windivert] diag: sent=%lu seen=%lu queued=%lu\n", g_wd_sent, g_wd_seen, g_wd_queued); } }
    /* Cap injections PER main-loop iteration so a gamespy burst can't monopolise the single USB TX
     * engine and starve the ~75ms beacon (a beacon stall drops the DS mid-session). Leftover frames
     * stay queued (max WinDivert queue) and drain over the next iterations. NWC_WD_DRAIN overrides. */
    static int wdd = -1;
    if (wdd < 0) { const char *e=getenv("NWC_WD_DRAIN"); wdd=(e&&*e)?atoi(e):8; if (wdd<2) wdd=2; if (wdd>64) wdd=64; }
    for (int k=0;k<wdd;k++) {
        uint8_t f[1600]; int len=0;
        EnterCriticalSection(&g_wd_cs);
        if (g_wd_tail != g_wd_head) { len=g_wd_ring[g_wd_tail].len; memcpy(f,g_wd_ring[g_wd_tail].f,(size_t)len); g_wd_tail=(g_wd_tail+1)%256; }
        LeaveCriticalSection(&g_wd_cs);
        if (!len) break;
        /* Send each server->DS frame twice: the dongle->DS OTA hop is lossy under the gamespy-phase
         * CCA spikes, and a lost gpcm result forces the server's slow ~1s retransmit -> retransmit
         * storm -> CCA spike -> beacon stall -> dropped connection. Two immediate copies let the DS
         * ACK on the first exchange so GPCM completes before the storm builds. NWC_WD_DUP overrides. */
        static int wddup=-1; if (wddup<0){ const char *e=getenv("NWC_WD_DUP"); wddup=(e&&*e)?atoi(e):2; if(wddup<1)wddup=1; if(wddup>3)wddup=3; }
        for (int d=0; d<wddup; d++) win_send_eth(h, mac, ssid, privacy, f, len);
        static unsigned long ll=0; unsigned long nn=GetTickCount();
        if (nn-ll>200u){ ll=nn; uint8_t *ip=f+14; int ih=(ip[0]&0xf)*4;
            printf("[windivert] inject -> DS proto=%u sport=%u len=%d x%d\n", ip[9], rdbe16(ip+ih), len-14, wddup); }
    }
}
#endif /* _WIN32 */

static int send_probe_response(libusb_device_handle *handle, const uint8_t mac[6],
                               const uint8_t dst[6], const char *ssid, uint8_t channel,
                               bool privacy)
{
    uint8_t frame[256];
    size_t frame_len = build_probe_response(frame, sizeof(frame), mac, dst, ssid, channel, privacy);
    if (!frame_len)
        return LIBUSB_ERROR_INVALID_PARAM;
    printf("[probe] responding to %02x:%02x:%02x:%02x:%02x:%02x ssid=\"%s\" channel=%u\n",
           dst[0], dst[1], dst[2], dst[3], dst[4], dst[5], ssid, channel);
    return send_80211_frame(handle, frame, frame_len, true, true, "probe-response");
}

/*
 * USB-Connector registration probe-response with an EXPLICIT 32-byte SSID (may
 * contain embedded NULs, so it can't use the strlen path). Mirrors the original
 * rt25usbap.sys @0x23b20:
 *   accepted=false (PENDING)  -> ssid[9] &= 0x0e (clear accept bit) + zero the
 *                                20 digits; puts the DS into "awaiting permission".
 *   accepted=true  (ACCEPTED) -> ssid[9] |= 1 + keep the real 20 digits, so the
 *                                DS self-derives the WEP key and proceeds.
 */
static int send_connector_response(libusb_device_handle *handle, const uint8_t mac[6],
                                   const uint8_t dst[6], const char *base_ssid,
                                   bool accepted, uint8_t channel)
{
    uint8_t s[32];
    size_t n = strlen(base_ssid); if (n > 32) n = 32;
    memset(s, 0x20, sizeof(s));
    memcpy(s, base_ssid, n);
    if (accepted) {
        s[9] = (uint8_t)(s[9] | 0x01);
    } else {
        s[9] = (uint8_t)(s[9] & 0x0e);
        memset(s + 12, 0x00, 20);
    }

    uint8_t frame[256];
    uint8_t *p = frame;
    put16(p, 0x0050); p += 2;              /* probe response */
    put16(p, 0x0000); p += 2;
    memcpy(p, dst, 6); p += 6;             /* addr1 = DS (unicast) */
    memcpy(p, mac, 6); p += 6;             /* addr2 = us */
    memcpy(p, mac, 6); p += 6;             /* addr3 = BSSID */
    put16(p, 0x0000); p += 2;
    memset(p, 0x00, 8); p += 8;            /* timestamp */
    put16(p, 100); p += 2;                 /* beacon interval */
    put16(p, 0x0011); p += 2;              /* capability: ESS + Privacy */
    *p++ = 0; *p++ = 32; memcpy(p, s, 32); p += 32;
    *p++ = 1; *p++ = 4; *p++ = 0x82; *p++ = 0x84; *p++ = 0x0b; *p++ = 0x16;
    *p++ = 3; *p++ = 1; *p++ = channel;

    printf("[connector] -> %s reply to %02x:%02x:%02x:%02x:%02x:%02x ssid[9]=0x%02x\n",
           accepted ? "ACCEPTED" : "PENDING", dst[0], dst[1], dst[2], dst[3], dst[4], dst[5], s[9]);
    return send_80211_frame(handle, frame, (size_t)(p - frame), true, true, "connector-resp");
}

/* Per-DS registration timing: reply PENDING for the first ~2s (so the DS enters
 * "awaiting permission"), then auto-ACCEPT — automating the original GUI approve. */
static struct { uint8_t mac[6]; unsigned long first_tick; int used; } g_reg[16];

static bool reg_is_accepted_now(const uint8_t mac[6])
{
    unsigned long now = GetTickCount();
    for (int i = 0; i < 16; i++)
        if (g_reg[i].used && mac_equal(g_reg[i].mac, mac))
            return (now - g_reg[i].first_tick) >= 2000u;
    for (int i = 0; i < 16; i++)
        if (!g_reg[i].used) {
            memcpy(g_reg[i].mac, mac, 6);
            g_reg[i].first_tick = now;
            g_reg[i].used = 1;
            return false;
        }
    return true;
}

static int send_auth_response(libusb_device_handle *handle, const uint8_t mac[6],
                              const uint8_t dst[6], uint16_t auth_alg,
                              uint16_t auth_seq, uint16_t status,
                              bool include_challenge)
{
    uint8_t frame[192];
    size_t frame_len = build_auth_response(frame, sizeof(frame), mac, dst, auth_alg,
                                           auth_seq, status, include_challenge);
    if (!frame_len)
        return LIBUSB_ERROR_INVALID_PARAM;
    printf("[auth] response to %02x:%02x:%02x:%02x:%02x:%02x alg=%u seq=%u status=%u challenge=%u\n",
           dst[0], dst[1], dst[2], dst[3], dst[4], dst[5], auth_alg, auth_seq, status,
           include_challenge ? 1 : 0);
    /* OTA sniff (AR9271) proved auth-responses submit+succeed over USB but NEVER radiate,
     * while probe-responses (same TX fn) DO. The one descriptor bit that differs is 0x400
     * ("timestamp"). NWC_AUTH_TS=1 sets it on auth to test whether 0x400 is really a
     * TX-enable/autonomous bit (radio only inserts TSF for beacon/proberesp subtypes) rather
     * than a corrupting timestamp-insert. NWC_AUTH_NOACK=1 makes seq2 fire-and-forget so the
     * single TX engine doesn't stall waiting on the DS ACK. */
    bool auth_ts   =  (env_on("NWC_AUTH_TS"));
    bool auth_ack  = !(env_on("NWC_AUTH_NOACK"));
    return send_80211_frame(handle, frame, frame_len, auth_ts, auth_ack, "auth-response");
}

static int send_assoc_response(libusb_device_handle *handle, const uint8_t mac[6],
                               const uint8_t dst[6], uint16_t aid, bool reassoc,
                               bool privacy)
{
    uint8_t frame[64];
    size_t frame_len = build_assoc_response(frame, sizeof(frame), mac, dst, aid, reassoc, privacy);
    if (!frame_len)
        return LIBUSB_ERROR_INVALID_PARAM;
    printf("[%s] accepting %02x:%02x:%02x:%02x:%02x:%02x aid=%u\n",
           reassoc ? "reassoc" : "assoc",
           dst[0], dst[1], dst[2], dst[3], dst[4], dst[5], aid);
    return send_80211_frame(handle, frame, frame_len, false, true,
                            reassoc ? "reassoc-response" : "assoc-response");
}

static bool is_probe_request(const uint8_t *frame, int frame_len)
{
    if (frame_len < 24)
        return false;
    uint16_t fc = (uint16_t)frame[0] | ((uint16_t)frame[1] << 8);
    return ((fc >> 2) & 0x3) == 0 && ((fc >> 4) & 0xf) == 4;
}

static bool is_mgmt_subtype(const uint8_t *frame, int frame_len, uint8_t subtype)
{
    if (frame_len < 24)
        return false;
    uint16_t fc = get16(frame);
    return ((fc >> 2) & 0x3) == 0 && ((fc >> 4) & 0xf) == subtype;
}

/* Per-station probe-response throttle. OTA sniff showed the DS's directed NWCUSBAP probe
 * storm (2022 answered / 291 radiated) saturates the RT2570's SINGLE TX engine — starving
 * the seq2 auth-response of a TX slot. NWC_PROBE_MINGAP_MS>0 answers each station at most
 * once per that many ms, freeing the engine during the auth window. 0 = answer every probe. */
static bool probe_throttled(const uint8_t src[6])
{
    const char *e = getenv("NWC_PROBE_MINGAP_MS");
    unsigned long gap = (e && *e) ? strtoul(e, NULL, 10) : 0;
    if (!gap) return false;
    static struct { uint8_t mac[6]; unsigned long tick; int used; } tbl[16];
    unsigned long now = GetTickCount();
    for (int i = 0; i < 16; i++)
        if (tbl[i].used && memcmp(tbl[i].mac, src, 6) == 0) {
            if (now - tbl[i].tick < gap) return true;
            tbl[i].tick = now; return false;
        }
    for (int i = 0; i < 16; i++)
        if (!tbl[i].used || now - tbl[i].tick > 8000u) {
            memcpy(tbl[i].mac, src, 6); tbl[i].tick = now; tbl[i].used = 1; return false;
        }
    return false;
}

static bool should_answer_probe(const uint8_t *frame, int frame_len, const char *ssid)
{
    (void)ssid;
    if (!is_probe_request(frame, frame_len))
        return false;
    if (probe_throttled(frame + 10))
        return false;
    /* P1: only answer probes aimed at OUR connector — the SSID IE must begin "NWCUSBAP"
     * (the DS's registration probe, or a directed probe for our SSID). Skip the
     * broadcast/wildcard/foreign probe STORM from unrelated nearby stations: the gold XP
     * capture shows ~0 probe-responses during the auth window, and answering them all
     * (tx=9673 in our logs) keeps the single TX engine busy and blocks the DS's auth
     * SIFS auto-ACK. Passive discovery still works via the beacon. NWC_ALLPROBE = answer all. */
    if (env_on("NWC_ALLPROBE"))
        return true;
    int ie = mgmt_ie_offset(frame, frame_len);
    if (ie >= 0 && ie + 2 + 8 <= frame_len && frame[ie] == 0 && frame[ie + 1] >= 8 &&
        memcmp(frame + ie + 2, "NWCUSBAP", 8) == 0)
        return true;
    /* DISCOVERY-phase answer: the DS's active scan sends BROADCAST/wildcard probe-requests
     * (SSID IE len 0) BEFORE it has found us, and if it also misses the beacon window it
     * reports 51303 "can't see it" — exactly the dark-recovered-dongle failure. So while we
     * are NOT in the auth window (no auth frame from any station in the last 2s), answer
     * broadcast probes too. probe_throttled() (NWC_PROBE_MINGAP_MS, default 300ms) already
     * caps this to ~3/s per station, so it can't storm the single TX engine; and once auth
     * begins g_last_auth_rx gates it right back off so the seq1 SIFS auto-ACK is protected.
     * NWC_NO_DISCPROBE disables. */
    if (!env_on("NWC_NO_DISCPROBE") && (GetTickCount() - g_last_auth_rx) > 2000u) {
        int wlen = (ie >= 0 && ie + 1 < frame_len) ? frame[ie + 1] : -1;
        if (wlen == 0)   /* wildcard/broadcast probe */
            return true;
    }
    return false;
}

static int mgmt_ie_offset(const uint8_t *frame, int frame_len)
{
    if (frame_len < 24)
        return -1;
    uint16_t fc = get16(frame);
    uint8_t type = (uint8_t)((fc >> 2) & 0x3);
    uint8_t subtype = (uint8_t)((fc >> 4) & 0xf);
    if (type != 0)
        return -1;
    if (subtype == 4 || subtype == 0 || subtype == 2)
        return 24;
    if (subtype == 5 || subtype == 8)
        return 36;
    return -1;
}

static void describe_probe_ies(const uint8_t *frame, int frame_len)
{
    int pos = mgmt_ie_offset(frame, frame_len);
    if (pos < 0)
        return;

    char ssid[34];
    copy_ssid(ssid, sizeof(ssid), frame, frame_len);
    printf("[probe-ie] from=%02x:%02x:%02x:%02x:%02x:%02x requested_ssid=\"%s\"",
           frame[10], frame[11], frame[12], frame[13], frame[14], frame[15],
           ssid[0] ? ssid : "<broadcast>");

    while (pos + 2 <= frame_len) {
        uint8_t id = frame[pos++];
        uint8_t len = frame[pos++];
        if (pos + len > frame_len)
            break;
        if (id == 1 || id == 50) {
            printf(" %s=", id == 1 ? "rates" : "ext_rates");
            for (uint8_t i = 0; i < len; i++) {
                uint8_t r = frame[pos + i];
                printf("%s%u%s", i ? "," : "", (unsigned int)(r & 0x7f),
                       (r & 0x80) ? "b" : "");
            }
        } else if (id == 3 && len >= 1) {
            printf(" ds_channel=%u", frame[pos]);
        } else if (id == 221 && len >= 3) {
            printf(" vendor=%02x:%02x:%02x/%u", frame[pos], frame[pos + 1], frame[pos + 2], len);
        }
        pos += len;
    }
    printf("\n");
}

static bool is_for_ap(const uint8_t *frame, int frame_len, const uint8_t mac[6])
{
    if (frame_len < 24)
        return false;
    return mac_equal(frame + 4, mac) || mac_equal(frame + 16, mac) || mac_broadcast(frame + 4);
}

/* De-dup auth responses so we answer each (station, seq) ONCE. The real driver sends one
 * seq2 for the DS's ~7 seq1 retransmits and otherwise keeps the single TX engine idle, so
 * each of the DS's seq1 frames gets its ~10us SIFS hardware auto-ACK. Answering every
 * retransmit saturates the engine and blocks the ACK -> DS loops -> 51303. NWC_NODEDUP off. */
static bool auth_answered_recently(const uint8_t src[6], uint16_t seq)
{
    static struct { uint8_t mac[6]; uint16_t seq; unsigned long tick; int used; } tbl[16];
    unsigned long now = GetTickCount();
    if (env_on("NWC_NODEDUP")) return false;
    for (int i = 0; i < 16; i++)
        if (tbl[i].used && tbl[i].seq == seq && memcmp(tbl[i].mac, src, 6) == 0) {
            if (now - tbl[i].tick < 800u) return true;   /* answered this (src,seq) < 800ms ago */
            tbl[i].tick = now; return false;
        }
    for (int i = 0; i < 16; i++)
        if (!tbl[i].used || now - tbl[i].tick > 4000u) {
            memcpy(tbl[i].mac, src, 6); tbl[i].seq = seq; tbl[i].tick = now; tbl[i].used = 1;
            return false;
        }
    return false;
}

static void maybe_answer_management(libusb_device_handle *handle, const uint8_t *frame, int frame_len,
                                    const uint8_t mac[6], const char *ssid, uint8_t channel,
                                    bool privacy)
{
    if (frame_len < 24)
        return;

    const uint8_t *src = frame + 10;

    /* NINTENDO-ONLY CLIENT ALLOW-LIST (default on; NWC_ANY_CLIENT=1 disables).
     * Refuse to answer probe/auth/assoc from a MAC whose OUI is not Nintendo, so the
     * AP only completes a handshake with a genuine DS/DSi/2DS/3DS or Wii/Wii U. This is
     * defense-in-depth (MACs are spoofable); the real gate is the proprietary connector
     * registration + WEP handshake that ordinary Wi-Fi clients cannot perform. */
    if (nintendo_only() && !mac_broadcast(src) && !is_nintendo_mac(src)) {
        static unsigned long g_rej = 0;
        if ((++g_rej & 0x3f) == 1)
            printf("[security] ignoring non-Nintendo client %02x:%02x:%02x:%02x:%02x:%02x "
                   "(OUI not Nintendo; NWC_ANY_CLIENT=1 to allow) [x%lu]\n",
                   src[0],src[1],src[2],src[3],src[4],src[5], g_rej);
        return;
    }

    /* ACTIVE-CONNECTION GUARD (Windows crash fix): once a DS is associated and
     * we're bridging its data (g_sta_known), IGNORE management frames from any
     * OTHER station. A neighbour's active-scan probe flood (subtype 4) would
     * otherwise each cost a synchronous libusbK probe-response TX of hundreds
     * of ms, stalling the main loop up to ~2s (observed: rxdrain 1985ms). That
     * stall starves the DS's uplink: its GPCM ACKs never reach Wiimmfi, the
     * server retransmits everything (42 SYN-ACK / 1638 keepalive-ACK / data x6+),
     * and the DS's IP stack eventually crashes deep in the handshake. Linux
     * absorbs the flood (usbfs = microseconds); Windows sync-TX cannot. Our own
     * DS's frames (re-auth / re-assoc / re-probe) still pass. The guard lifts the
     * instant the DS drops (win_poll_wintun's 6s-silence clears g_sta_known). */
    if (g_sta_known && !env_on("NWC_ANSWER_FOREIGN_MGMT") &&
        memcmp(src, g_sta_mac, 6) != 0)
        return;

    if (should_answer_probe(frame, frame_len, ssid)) {
        /* Plain probe-response with our connector SSID (byte9=space, real 20
         * digits). This is exactly what commit 82ba5e8 used to get the DS's
         * Connector mode to "awaiting permission". Log a registration probe for
         * visibility but DO NOT rewrite ssid[9]/digits — that broke recognition. */
        int ie = mgmt_ie_offset(frame, frame_len);
        if (ie >= 0 && ie + 2 + 32 <= frame_len &&
            frame[ie] == 0 && frame[ie + 1] == 32 &&
            memcmp(frame + ie + 2, "NWCUSBAP", 8) == 0) {
            const uint8_t *rssid = frame + ie + 2;
            char name[16]; int ni = 0;
            for (int k = 0; k < 20 && ni < 15; k += 2) {
                uint8_t c = rssid[12 + k];
                if (c == 0) break;
                name[ni++] = (c >= 0x20 && c <= 0x7e) ? (char)c : '.';
            }
            name[ni] = '\0';
            /*
             * Two-phase grant (the correct order this time):
             *  phase 1 (first ~2s): PLAIN response (ssid[9]=space) so the DS
             *    discovers us and enters "awaiting permission" (proven by 82ba5e8).
             *  phase 2 (after ~2s): GRANT by setting ssid[9] |= 1 (space 0x20 ->
             *    0x21) while KEEPING the 20 digits, mirroring rt25usbap.sys
             *    @0x23d33 accepted-form. This is the automated "Accept".
             */
            bool grant = reg_is_accepted_now(src);
            char rssid_buf[33];
            size_t sl = strlen(ssid); if (sl > 32) sl = 32;
            memcpy(rssid_buf, ssid, sl); rssid_buf[sl] = '\0';
            if (grant && sl >= 10)
                rssid_buf[9] = (char)((uint8_t)rssid_buf[9] | 0x01);
            printf("[connector] registration probe from %02x:%02x:%02x:%02x:%02x:%02x "
                   "name=\"%s\" ctrl=0x%02x -> %s (reply ssid[9]=0x%02x)\n",
                   src[0], src[1], src[2], src[3], src[4], src[5], name, rssid[9],
                   grant ? "GRANT" : "awaiting-permission", (uint8_t)rssid_buf[9]);
            send_probe_response(handle, mac, src, rssid_buf, channel, privacy);
            return;
        }
        send_probe_response(handle, mac, src, ssid, channel, privacy);
        return;
    }

    if (!is_for_ap(frame, frame_len, mac))
        return;

    if (is_mgmt_subtype(frame, frame_len, 11) && frame_len >= 30) {
        uint16_t fc = get16(frame);
        uint8_t auth_plain[192];
        const uint8_t *auth = frame + 24;
        int auth_len = frame_len - 24;
        bool decrypted = false;
        if ((fc & 0x4000) != 0) {
            uint8_t wep_key[13];
            int plain_len = 0;
            bool sw_ok = false;
            if (privacy && strlen(ssid) == 32) {
                derive_original_wep_key(ssid + 12, wep_key);
                if (wep_decrypt_body(frame + 24, frame_len - 24, wep_key,
                                     auth_plain, sizeof(auth_plain), &plain_len)) {
                    auth = auth_plain;
                    auth_len = plain_len;
                    decrypted = true;
                    sw_ok = true;
                    printf("[wep] decrypted protected auth body len=%d\n", auth_len);
                }
            }
            if (!sw_ok) {
                /* Gold-capture truth (xp_dongle_DS_full_interaction.pcapng seq3):
                 * the RT2570 HARDWARE WEP-decrypts the frame in place — the Protected
                 * bit stays set and the 4-byte IV remains at frame+24, with the
                 * plaintext auth body ('01 00 03 00 ..' = alg1/seq3/status0) at
                 * frame+28. Point at it so alg/seq/status parse correctly. */
                auth = frame + 28;
                auth_len = frame_len - 28;
                decrypted = true;
                printf("[wep] using HW-decrypted auth body at frame+28 (len=%d)\n", auth_len);
            }
        }
        if (auth_len < 6) {
            printf("[auth] short auth body from %02x:%02x:%02x:%02x:%02x:%02x len=%d protected=%u\n",
                   src[0], src[1], src[2], src[3], src[4], src[5], auth_len,
                   (fc & 0x4000) ? 1 : 0);
            return;
        }
        uint16_t alg = get16(auth);
        uint16_t seq = get16(auth + 2);
        uint16_t status = get16(auth + 4);
        g_last_auth_rx = GetTickCount();   /* now in the auth window -> broadcast probes go silent */
        printf("[auth] request from %02x:%02x:%02x:%02x:%02x:%02x alg=%u seq=%u status=%u\n",
               src[0], src[1], src[2], src[3], src[4], src[5], alg, seq, status);
        if (auth_answered_recently(src, seq)) {
            /* Already answered this (station, seq): stay OFF the air so the DS's
             * retransmitted seq1 gets its SIFS hardware auto-ACK (XP: 1 seq2 per ~7 seq1). */
            printf("[auth] dup seq=%u (already answered) -> silent, freeing TX for auto-ACK\n", seq);
            return;
        }
        if (alg == 0 && seq == 1) {
            send_auth_response(handle, mac, src, alg, 2, 0, false);
        } else if (alg == 1 && seq == 1) {
            /* OTA sniff: the 160B challenge-bearing seq2 physically never radiates over our
             * libusb TX path (a short seq2 does). NWC_AUTH_NOCHAL sends a challenge-less seq2
             * that DOES radiate — the DS self-derives the WEP key per the connector flow, so
             * it may accept it and advance to seq3. */
            bool nochal = (env_on("NWC_AUTH_NOCHAL"));
            send_auth_response(handle, mac, src, alg, 2, 0, !nochal);
        } else if (alg == 1 && seq == 3) {
            /* Send seq4=success UNCONDITIONALLY on seq3 — the gold XP capture shows
             * the connector immediately accepts (seq4 status=0) and never re-verifies
             * the echoed challenge. Gating this on software-decrypt was the code bug
             * that (once the auto-ACK lets the DS reach seq3) would have stalled auth. */
            send_auth_response(handle, mac, src, alg, 4, 0, false);
        } else {
            printf("[auth] unsupported auth exchange alg=%u seq=%u\n", alg, seq);
        }
        return;
    }

    if (is_mgmt_subtype(frame, frame_len, 0) || is_mgmt_subtype(frame, frame_len, 2)) {
        bool reassoc = is_mgmt_subtype(frame, frame_len, 2);
        printf("[%s] request from %02x:%02x:%02x:%02x:%02x:%02x\n",
               reassoc ? "reassoc" : "assoc",
               src[0], src[1], src[2], src[3], src[4], src[5]);
        send_assoc_response(handle, mac, src, 1, reassoc, privacy);
        return;
    }
}

static int send_beacon_and_enable(libusb_device_handle *handle, const uint8_t mac[6],
                                  const char *ssid, uint8_t channel,
                                  uint16_t on, uint16_t off, bool privacy)
{
    /* rt2500usb_write_beacon: DISABLE BEACON_GEN before reloading the ring, so the
     * generator never reads half-written beacon data (the reason our HW beacon may
     * emit garbage / not radiate). Then upload, then the alternating enable dance. */
    write16(handle, TXRX_CSR19, off);
    int rc = send_beacon(handle, mac, ssid, channel, privacy);
    if (rc != 0)
        return rc;
    write16(handle, TXRX_CSR19, on);
    write16(handle, TXRX_CSR19, off);
    write16(handle, TXRX_CSR19, on);
    write16(handle, TXRX_CSR19, off);
    write16(handle, TXRX_CSR19, on);
    return 0;
}

static const char *frame_type_name(uint16_t fc)
{
    uint8_t type = (uint8_t)((fc >> 2) & 0x3);
    uint8_t subtype = (uint8_t)((fc >> 4) & 0xf);
    if (type == 0) {
        switch (subtype) {
        case 4: return "probe-request";
        case 5: return "probe-response";
        case 8: return "beacon";
        case 11: return "authentication";
        case 0: return "assoc-request";
        case 2: return "reassoc-request";
        default: return "management";
        }
    }
    if (type == 1)
        return "control";
    if (type == 2)
        return "data";
    return "extension";
}

static void copy_ssid(char *ssid, size_t ssid_cap, const uint8_t *frame, int frame_len)
{
    ssid[0] = '\0';
    int pos = 24;
    uint16_t fc = (uint16_t)frame[0] | ((uint16_t)frame[1] << 8);
    uint8_t subtype = (uint8_t)((fc >> 4) & 0xf);
    if (subtype == 8 || subtype == 5)
        pos = 36;
    while (pos + 2 <= frame_len) {
        uint8_t id = frame[pos++];
        uint8_t len = frame[pos++];
        if (pos + len > frame_len)
            return;
        if (id == 0) {
            size_t copy = len;
            if (copy >= ssid_cap)
                copy = ssid_cap - 1;
            memcpy(ssid, frame + pos, copy);
            ssid[copy] = '\0';
            for (size_t i = 0; i < copy; i++) {
                if ((unsigned char)ssid[i] < 0x20 || (unsigned char)ssid[i] > 0x7e)
                    ssid[i] = '.';
            }
            return;
        }
        pos += len;
    }
}

static bool is_interesting_frame(const uint8_t *frame, int frame_len,
                                 const uint8_t *ap_mac, const char *ap_ssid)
{
    if (frame_len < 24)
        return true;
    uint16_t fc = get16(frame);
    uint8_t type = (uint8_t)((fc >> 2) & 0x3);
    uint8_t subtype = (uint8_t)((fc >> 4) & 0xf);
    if (type == 2)
        return true;
    if (type == 0 && (subtype == 0 || subtype == 2 || subtype == 10 || subtype == 11 || subtype == 12))
        return true;
    if (ap_mac && (mac_equal(frame + 4, ap_mac) || mac_equal(frame + 16, ap_mac)))
        return true;
    if (ap_ssid && subtype == 4) {
        char requested[34];
        copy_ssid(requested, sizeof(requested), frame, frame_len);
        if (requested[0] == '\0' || strcmp(requested, ap_ssid) == 0 ||
            strncmp(requested, "NWCUSBAP", 8) == 0)
            return true;
    }
    return false;
}

static int nwc_quiet(void)
{
    static int q = -1;
    if (q < 0) { const char *e = getenv("NWC_QUIET"); q = (e && *e) ? 1 : 0; }
    return q;
}

static void log_80211_frame(const uint8_t *frame, int frame_len, const char *source,
                            const uint8_t *ap_mac, const char *ap_ssid)
{
    /* NWC_QUIET: suppress the verbose per-frame logger (hexdumps + field decode).
     * Under load this emits many KB/frame; if the buffered write flushes mid-loop it
     * can block the main loop on disk I/O -- a candidate for the residual multi-second
     * stall. Keeping [stall]/diag/delivery markers lets us tell logging-stall from real. */
    if (nwc_quiet()) return;
    if (frame_len < 24) {
        printf("[rx] %s short raw frame len=%d\n", source, frame_len);
        hexdump(frame, frame_len < 96 ? frame_len : 96);
        return;
    }

    bool registration_hint = contains_registration_hint(frame, frame_len);
    if (!registration_hint && !is_interesting_frame(frame, frame_len, ap_mac, ap_ssid))
        return;

    uint16_t fc = get16(frame);
    char ssid[34];
    copy_ssid(ssid, sizeof(ssid), frame, frame_len);
    printf("[rx] %s len=%d type=%s fc=0x%04x",
           source, frame_len, frame_type_name(fc), fc);
    if (ssid[0])
        printf(" ssid=\"%s\"", ssid);
    printf(" addr1=%02x:%02x:%02x:%02x:%02x:%02x"
           " addr2=%02x:%02x:%02x:%02x:%02x:%02x"
           " addr3=%02x:%02x:%02x:%02x:%02x:%02x\n",
           frame[4], frame[5], frame[6], frame[7], frame[8], frame[9],
           frame[10], frame[11], frame[12], frame[13], frame[14], frame[15],
           frame[16], frame[17], frame[18], frame[19], frame[20], frame[21]);
    if (registration_hint)
        printf("[rx] *** registration hint matched: Nintendo/NWCUSB bytes present ***\n");

    uint8_t subtype = (uint8_t)((fc >> 4) & 0xf);
    if (subtype == 4)
        describe_probe_ies(frame, frame_len);
    if (registration_hint || subtype == 11 || subtype == 0 || subtype == 2) {
        int dump_len = frame_len < 128 ? frame_len : 128;
        hexdump(frame, dump_len);
    }
}

static void log_rx_packet(libusb_device_handle *handle, const uint8_t *buf, int len,
                          const uint8_t *ap_mac, const char *ap_ssid, uint8_t channel,
                          bool privacy)
{
    if (len < 2) {
        printf("[rx] short packet len=%d\n", len);
        hexdump(buf, len);
        return;
    }

    /* Diagnostic: with NWC_RXDUMP set, dump the FULL raw buffer for small
     * (management/auth-sized) frames, so the 16-byte RT2570 RX descriptor
     * (W0 status bits: UNICAST_TO_ME/MY_BSS/CRC_ERROR/...) is visible whether it
     * is prepended or appended. This tells us if the hardware even treats the
     * DS's unicast auth frame as "to me" — the precondition for the auto-ACK. */
    {
        static int rxdump = -1;
        if (rxdump < 0)
            rxdump = getenv("NWC_RXDUMP") ? 1 : 0;
        /* Decode the 16-byte RT2570 RX descriptor carried as a TRAILER (the last
         * 16 bytes: frame + 4-byte FCS + 16-byte RXD). W0 status bits tell us how
         * the HARDWARE classified this frame — crucially UNICAST_TO_ME, the exact
         * gate that also arms the SIFS auto-ACK. If an auth frame from the DS is
         * NOT flagged UNICAST_TO_ME, the radio never ACKs it (address-match miss);
         * if it IS, the ACK is generated and the failure is timing/PHY downstream. */
        if (len >= 40) {
            uint32_t tw0 = get32(buf + len - 16);
            uint16_t fc0 = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
            uint8_t  st  = (uint8_t)((fc0 >> 4) & 0xf);
            uint8_t  ty  = (uint8_t)((fc0 >> 2) & 0x3);
            int is_auth = (ty == 0 && st == 11);
            if (is_auth || rxdump) {
                printf("[rxd] len=%d tw0=0x%08x  UNI_TO_ME=%d MULTI=%d BCAST=%d MY_BSS=%d CRC_ERR=%d  rxd_len=%u%s\n",
                       len, tw0,
                       (tw0 & 0x00000002) ? 1 : 0, (tw0 & 0x00000004) ? 1 : 0,
                       (tw0 & 0x00000008) ? 1 : 0, (tw0 & 0x00000010) ? 1 : 0,
                       (tw0 & 0x00000020) ? 1 : 0, (unsigned)((tw0 >> 16) & 0xfff),
                       is_auth ? "  <== DS AUTH" : "");
            }
        }
        if (rxdump && len <= 120) {
            printf("[rxdump] len=%d raw bytes:\n", len);
            hexdump(buf, len);
        }
    }

    uint16_t raw_fc = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    if ((raw_fc & 0x0003) == 0 && ((raw_fc >> 2) & 0x3) <= 2) {
        log_80211_frame(buf, len, "raw", ap_mac, ap_ssid);
        if (ap_mac && ap_ssid) {
            maybe_answer_management(handle, buf, len, ap_mac, ap_ssid, channel, privacy);
#ifndef _WIN32
            tap_rx_dsdata(buf, len, ap_ssid);
#else
            win_rx_dsdata(handle, buf, len, ap_mac, ap_ssid, privacy);
#endif
        }
        return;
    }

    if (len < 16) {
        printf("[rx] short descriptor packet len=%d\n", len);
        hexdump(buf, len);
        return;
    }

    uint32_t w0 = get32(buf + 0);
    uint32_t w1 = get32(buf + 4);
    uint16_t data_len = (uint16_t)((w0 >> 16) & 0x0fff);
    uint8_t rssi = (uint8_t)(w1 & 0xff);
    uint8_t signal = (uint8_t)((w1 >> 8) & 0xff);
    const uint8_t *frame = buf + 16;
    int frame_avail = len - 16;
    if (data_len < frame_avail)
        frame_avail = data_len;

    if (frame_avail >= 24) {
        bool interesting = contains_registration_hint(frame, frame_avail) ||
                           is_interesting_frame(frame, frame_avail, ap_mac, ap_ssid);
        if (interesting)
            printf("[rx] descriptor len=%d data=%u rssi=%u signal=0x%02x flags=0x%08x\n",
                   len, data_len, rssi, signal, w0);
        log_80211_frame(frame, frame_avail, "desc", ap_mac, ap_ssid);
        if (ap_mac && ap_ssid) {
            maybe_answer_management(handle, frame, frame_avail, ap_mac, ap_ssid, channel, privacy);
#ifndef _WIN32
            tap_rx_dsdata(frame, frame_avail, ap_ssid);
#else
            win_rx_dsdata(handle, frame, frame_avail, ap_mac, ap_ssid, privacy);
#endif
        }
    } else {
        printf("[rx] len=%d data=%u rssi=%u signal=0x%02x flags=0x%08x\n",
               len, data_len, rssi, signal, w0);
        hexdump(buf, len < 96 ? len : 96);
    }
}

static void poll_rx(libusb_device_handle *handle, unsigned int timeout_ms,
                    const uint8_t *ap_mac, const char *ap_ssid, uint8_t channel,
                    bool privacy)
{
    uint8_t buf[4096];
    int transferred = 0;
#ifdef NWC_BACKEND_KMDF
    (void)handle; (void)timeout_ms; (void)transferred;
    {
        DWORD ret = 0;
        /* Drain one frame from the kernel continuous-reader ring. Returns 0
         * bytes (still success) when the ring is empty. */
        if (DeviceIoControl(g_kmdf, IOCTL_NWC_RX_FRAME, NULL, 0, buf, sizeof(buf), &ret, NULL)) {
            if (ret > 0)
                log_rx_packet(handle, buf, (int)ret, ap_mac, ap_ssid, channel, privacy);
        } else {
            printf("[rx] RX_FRAME ioctl gle=%lu\n", GetLastError());
        }
    }
#else
    int rc = libusb_bulk_transfer(handle, BULK_IN_EP, buf, sizeof(buf), &transferred, timeout_ms);
    if (rc == 0 && transferred > 0) {
        log_rx_packet(handle, buf, transferred, ap_mac, ap_ssid, channel, privacy);
    } else if (rc != LIBUSB_ERROR_TIMEOUT) {
        printf("[rx] bulk read: %s transferred=%d\n", libusb_error_name(rc), transferred);
    }
#endif
}

static void set_state_awake(libusb_device_handle *handle);   /* fwd: AWAKE edge fired late, below */

static int config_ap_regs(libusb_device_handle *handle, const uint8_t mac[6])
{
    int rc = write_mac_words(handle, 0x0404, mac);
    if (rc != 0)
        return rc;
    rc = write_mac_words(handle, 0x040a, mac);
    if (rc != 0)
        return rc;

    /*
     * AWAKE SET_STATE transition — fired LATE, HERE, after radio_init has done the
     * BBP loop + RF channel + antenna AND the BSSID/MAC address is now programmed
     * (rt25usbap.sys fr1382 MAC addr -> fr1402/1407 MAC_CSR17 AWAKE). This is the
     * edge that latches the MAC/PHY operating config and ARMS the SIFS hardware
     * auto-responder. Previously we fired it inside basic_init BEFORE PHY/RF/antenna
     * and the MAC address existed, so the responder never armed (DS auths but never
     * gets a hardware ACK -> 51301/51303). NWC_OLDAWAKE restores the old early edge.
     */
    if (!(env_on("NWC_OLDAWAKE"))) {
        /* Zero MAC_CSR18 (wakeup timer: DELAY_AFTER_BEACON/BEACONS_BEFORE_WAKEUP/
         * AUTO_WAKE) IMMEDIATELY before the AWAKE transition — the gold XP capture's
         * register STATE does exactly this (MAC_CSR18<=0x0000 at fr1397, then MAC_CSR17
         * AWAKE at fr1402). basic_init leaves MAC_CSR18=0x005a; latching AWAKE with a
         * non-zero wakeup timer still loaded is the ONE auto-ACK-relevant register-state
         * mismatch vs the working driver (readback diff). NWC_KEEPCSR18 keeps the old
         * behavior for A/B testing. */
        if (!(getenv("NWC_KEEPCSR18") && *getenv("NWC_KEEPCSR18"))) {
            write16(handle, MAC_CSR18, 0x0000);
            printf("[init] MAC_CSR18 zeroed before AWAKE (match XP fr1397 wakeup-timer clear)\n");
        }
        printf("[init] firing AWAKE SET_STATE late (post-PHY, post-MACaddr) per rt25usbap.sys order\n");
        set_state_awake(handle);
    }

    /* TXRX_CSR20 (beacon offset/expect-window). Original rt25usbap.sys writes 0x0140
     * (verified in the live init capture, fr5105); the old 0x4006 was a guess. Now
     * default to the proven 0x0140; NWC_CSR20 overrides for bench testing. */
    {
        uint16_t csr20 = 0x0140;
        const char *e = getenv("NWC_CSR20");
        if (e && *e) csr20 = (uint16_t)strtol(e, NULL, 0);
        write16(handle, TXRX_CSR20, csr20);
    }
    /*
     * TXRX_CSR18: OFFSET = bits 0-3 (0x000f), INTERVAL = bits 4-15 (0xfff0).
     * The rt2500usb reference programs INTERVAL = beacon_int * 4, so the raw
     * register value must be (beacon_int * 4) << 4. The previous code wrote the
     * value un-shifted, landing beacon_int*4 across OFFSET+low INTERVAL bits and
     * yielding a beacon period ~16x too short (broken TBTT / no stable beacons).
     */
    /* TXRX_CSR18 beacon interval. usbmon diff vs rt2500usb (LIVE HW beacon): rt2500usb
     * writes 0x0640 (INTERVAL=100 in bits 4-15 => 100 TU), we wrote 0x1900 (INTERVAL=400,
     * 4x too long) — a prime reason the HW beacon/TBTT engine never fires. NWC_CSR18
     * overrides; e.g. NWC_CSR18=0x0640 to match rt2500usb exactly. */
    {
        const char *e = getenv("NWC_CSR18");
        uint16_t csr18 = e && *e ? (uint16_t)strtol(e, NULL, 0)
                                 : (uint16_t)(((100u * 4u) << 4) & 0xfff0u);
        write16(handle, TXRX_CSR18, csr18);
    }
    /* TXRX_CSR10 auto-responder control = 0x000a (rt25usbap.sys go-live, fr5066).
     * We previously wrote 0x0000, which clears bits 1 and 3 that the SIFS auto-ACK
     * responder relies on. This value was never tried (MATCHORIG did NOT set it).
     * NWC_CSR10 overrides for bench testing. */
    {
        /* Δ1 (RE rt25usbap.sys 0x1f2e0 + rt2500usb.c:476-479): TXRX_CSR10 is the
         * AUTO-RESPONDER CONTROL register. The real driver does a READ-MODIFY-WRITE
         * touching ONLY AUTORESPOND_PREAMBLE (bit2), preserving the chip's power-on
         * auto-responder defaults. Our old full write of 0x000a (bits 1,3, no bit2)
         * clobbered that control register -> a prime suspect for the disarmed SIFS
         * auto-ACK. Default now: RMW, clear bit2 (long preamble, DS-safe), preserve
         * the rest. NWC_CSR10 forces a full literal for A/B testing. */
        const char *e = getenv("NWC_CSR10");
        if (e && *e) {
            write16(handle, TXRX_CSR10, (uint16_t)strtol(e, NULL, 0));
        } else {
            uint16_t csr10 = 0;
            read16(handle, TXRX_CSR10, &csr10);
            csr10 &= (uint16_t)~0x0004u;   /* AUTORESPOND_PREAMBLE=0 (long preamble) */
            write16(handle, TXRX_CSR10, csr10);
        }
    }
    write16(handle, TXRX_CSR11, 0x0003);
    /*
     * MAC timing — EXACTLY matching the ORIGINAL rt25usbap.sys config_erp
     * operational branch (disassembly VMA 0x2148d-0x214a1, verified by RE):
     *   MAC_CSR10 (0x0414) = slot time = 20us  (0x14)
     *   MAC_CSR11 (0x0416) = SIFS      = 5      (0x05)  <-- ORIGINAL value
     *   MAC_CSR12 (0x0418) = EIFS      = 364us (0x16c)
     * NOTE: an earlier theory set SIFS=10 ("ACK fires 5us too early"); the
     * disassembly proves the original connector that DSes DO associate with
     * ships SIFS=5. SIFS=10 delays our hardware ACK past the DS receive window.
     * The original slot=20/SIFS=5/EIFS=364 triple is the proven-correct set.
     */
    write16(handle, 0x0414, 20);
    {
        /* SIFS default 5 (matches original rt25usbap.sys config_erp @0x2148d).
         * NWC_SIFS lets us bench-test the "ACK fires too early" theory (10/11)
         * against a real DS without recompiling. */
        uint16_t sifs = 5;
        const char *e = getenv("NWC_SIFS");
        if (e && *e) {
            sifs = (uint16_t)atoi(e);
            printf("[timing] SIFS override MAC_CSR11=%u (NWC_SIFS)\n", sifs);
        }
        write16(handle, 0x0416, sifs);
    }
    write16(handle, 0x0418, 364);

    /*
     * RX filter for AP mode. DROP_NOT_TO_ME (0x0010) MUST be set: it makes the
     * radio operate as a normal station/AP that HARDWARE-ACKs unicast frames
     * addressed to us (auth/assoc/data). With it clear the radio is effectively
     * promiscuous/monitor (hears everything, incl. other stations) and does NOT
     * auto-ACK, so the DS never sees an ACK for its authentication frame, keeps
     * retransmitting it, and eventually fails (WFC error 51301) without ever
     * sending an association request. We still keep broadcast + multicast + ToDS
     * so we can hear probe requests and station uplink traffic.
     * Value 0x0056 EXACTLY matches the ORIGINAL rt25usbap.sys operational
     * filter (RE VMA 0x18a1a / 0x1fc4c / 0x27fa2): DROP_CONTROL is CLEARED so
     * control frames (incl. the DS's ACKs to our frames) reach RX, which the
     * original relies on. (We previously used 0x005e which dropped control.)
     *   bit0 DISABLE_RX=0  bit1 DROP_CRC=1        bit2 DROP_PHYSICAL=1
     *   bit3 DROP_CONTROL=0 bit4 DROP_NOT_TO_ME=1 bit5 DROP_TODS=0
     *   bit6 DROP_VERSION_ERR=1 bit9 DROP_MCAST=0 bit10 DROP_BCAST=0
     */
    /*
     * TXRX_CSR19 TSF/BSS enable — THE hardware auto-ACK gate (RE rt25usbap.sys
     * go-live @0x21366). The auto-responder only SIFS-ACKs a unicast frame
     * addressed to our BSSID when the hardware TSF is actually RUNNING. The prior
     * code set only TSF_SYNC (0x0004) and the TSF never counted (missing TSF_COUNT
     * 0x0001), so the chip did not consider itself the active BSS and never
     * auto-ACKed the DS's authentication -> DS retransmits -> WFC 51301/51303.
     *   0x0001 TSF_COUNT   0x0004 TSF_SYNC=2   0x0008 TBCN   0x0010 BEACON_GEN
     * Default 0x0005 (TSF_COUNT|TSF_SYNC2): TSF runs so the auto-ACK arms, but no
     * hardware beacon is generated (our software beacons still stand, no conflict).
     * The original uses the full 0x001D (adds hardware beacons). NWC_CSR19 lets us
     * bench 0x001D vs 0x0005 without recompiling. The original also does a
     * disable(0)->enable transition, which we mirror.
     */
    {
        uint16_t csr19 = 0x0005;
        const char *e = getenv("NWC_CSR19");
        if (e && *e) {
            csr19 = (uint16_t)strtol(e, NULL, 0);
            printf("[timing] TXRX_CSR19 override=0x%04x (NWC_CSR19)\n", csr19);
        }
        write16(handle, TXRX_CSR19, 0x0000);
        write16(handle, TXRX_CSR19, csr19);
        printf("[ack] TXRX_CSR19=0x%04x (TSF_COUNT arms hardware auto-ACK)\n", csr19);
    }

    /*
     * RX filter LAST — the original opens RX only after the TSF/auto-responder
     * engine is fully armed (RE go-live order @0x189da). 0x0056 sets DROP_NOT_TO_ME
     * so the radio hardware-ACKs unicast frames addressed to us.
     */
    write16(handle, TXRX_CSR2, 0x0056);

    /* Diagnostic: read back the ACK/mode-relevant registers to verify they
     * actually stuck in hardware (values matter for hardware auto-ACK). */
    {
        uint16_t v_csr0=0, v_csr1=0, v_csr2=0, v_csr10=0, v_csr11=0, v_csr19=0;
        uint16_t m2=0, m3=0, m4=0, b5=0, b6=0, b7=0;
        read16(handle, TXRX_CSR0, &v_csr0);
        read16(handle, TXRX_CSR1, &v_csr1);
        read16(handle, TXRX_CSR2, &v_csr2);
        read16(handle, TXRX_CSR10, &v_csr10);
        read16(handle, TXRX_CSR11, &v_csr11);
        read16(handle, TXRX_CSR19, &v_csr19);
        read16(handle, 0x0404, &m2); read16(handle, 0x0406, &m3); read16(handle, 0x0408, &m4);
        read16(handle, 0x040a, &b5); read16(handle, 0x040c, &b6); read16(handle, 0x040e, &b7);
        printf("[regcheck] TXRX_CSR0=%04x CSR1=%04x CSR2=%04x CSR10=%04x CSR11=%04x CSR19=%04x\n",
               v_csr0, v_csr1, v_csr2, v_csr10, v_csr11, v_csr19);
        printf("[regcheck] MAC(CSR2..4)=%04x %04x %04x  BSSID(CSR5..7)=%04x %04x %04x\n",
               m2, m3, m4, b5, b6, b7);
        /* Auto-responder (ACK/CTS) BBP-rate map + CCK/OFDM RX BBP id. If these are
         * not 8c8d/8b8a/8687/0085 the hardware ACK generator is misconfigured. */
        {
            uint16_t c3=0,c4=0,c5=0,c6=0,c7=0,c8=0;
            read16(handle, TXRX_CSR3, &c3); read16(handle, TXRX_CSR4, &c4);
            read16(handle, TXRX_CSR5, &c5); read16(handle, TXRX_CSR6, &c6);
            read16(handle, TXRX_CSR7, &c7); read16(handle, TXRX_CSR8, &c8);
            printf("[regcheck] auto-resp CSR3=%04x CSR4=%04x CSR5=%04x CSR6=%04x CSR7=%04x CSR8=%04x\n",
                   c3, c4, c5, c6, c7, c8);
        }
    }
    return 0;
}

static int set_led(libusb_device_handle *handle, bool on)
{
    int rc;
    rc = write16(handle, MAC_CSR21, 0x1e46);
    if (rc != 0)
        return rc;
    rc = write16(handle, MAC_CSR20, on ? 0x0003 : 0x0000);
    if (rc != 0)
        return rc;
    uint16_t confirm = 0;
    read16(handle, MAC_CSR20, &confirm);
    return 0;
}

/* Active RF/BBP SET_STATE -> AWAKE convergence (original set_state @0x21000):
 * write DESIRE=AWAKE, trigger SET_STATE, poll MAC_CSR17 until CURR_STATE
 * (&0x1e0)==0x1e0, re-triggering up to ~15x. This is the edge that latches the
 * MAC/PHY operating config (and arms the SIFS auto-responder). */
static void set_state_awake(libusb_device_handle *handle)
{
    write16(handle, MAC_CSR17, BBP_DESIRE_STATE_AWAKE | RF_DESIRE_STATE_AWAKE);
    write16(handle, MAC_CSR17, BBP_DESIRE_STATE_AWAKE | RF_DESIRE_STATE_AWAKE | SET_STATE_TRIGGER);
    for (int attempt = 0; attempt < 15; attempt++) {
        sleep_ms(attempt == 0 ? 30 : 5);
        uint16_t st = 0;
        read16(handle, MAC_CSR17, &st);
        if ((st & 0x01e0u) == 0x01e0u) {
            printf("[state] RF/BBP AWAKE converged after %d tr%s (MAC_CSR17=0x%04x)\n",
                   attempt + 1, attempt == 0 ? "y" : "ies", st);
            return;
        }
        write16(handle, MAC_CSR17,
                BBP_DESIRE_STATE_AWAKE | RF_DESIRE_STATE_AWAKE | SET_STATE_TRIGGER);
    }
    {
        uint16_t st = 0;
        read16(handle, MAC_CSR17, &st);
        printf("[state] WARNING: RF/BBP never reached AWAKE (MAC_CSR17=0x%04x, want &0x1e0==0x1e0)\n", st);
    }
}

/* NWC_MATCHORIG env test toggle (original-exact init ordering + power regs). */
static int match_orig_init(void)
{
    const char *e = getenv("NWC_MATCHORIG");
    return e && *e;
}

static int basic_init(libusb_device_handle *handle)
{
    uint16_t reg;
    uint16_t asic_rev = 0;   /* MAC_CSR0 & 0x000f = RT2570 ASIC revision */
    int rc;

    rc = write_single(handle, USB_DEVICE_MODE, 0x0001, USB_MODE_TEST, "device-mode-test");
    if (rc != 0)
        return rc;
    rc = write_single(handle, USB_SINGLE_WRITE, 0x0308, 0x00f0, "single-write-0308");
    if (rc != 0)
        return rc;

    read16(handle, MAC_CSR0, &reg);
    asic_rev = (uint16_t)(reg & 0x000f);   /* rt2500usb.c: revision = MAC_CSR0 & 0x0f */
    printf("[asic] MAC_CSR0=0x%04x rev=%u (%s)\n", reg, asic_rev,
           asic_rev >= 3 ? "VERSION_C+ (rev>=3)" : "VERSION_B (rev<3)");

    read16(handle, TXRX_CSR2, &reg);
    write16(handle, TXRX_CSR2, reg | 0x0001);

    write16(handle, MAC_CSR13, 0x1111);
    write16(handle, MAC_CSR14, 0x1e11);

    read16(handle, MAC_CSR1, &reg);
    write16(handle, MAC_CSR1, (reg | 0x0003) & (uint16_t)~0x0004);
    read16(handle, MAC_CSR1, &reg);
    write16(handle, MAC_CSR1, reg & (uint16_t)~0x0007);

    read16(handle, TXRX_CSR5, &reg);
    write16(handle, TXRX_CSR5, 0x8c8d);
    read16(handle, TXRX_CSR6, &reg);
    write16(handle, TXRX_CSR6, 0x8b8a);
    read16(handle, TXRX_CSR7, &reg);
    write16(handle, TXRX_CSR7, 0x8687);
    read16(handle, TXRX_CSR8, &reg);
    write16(handle, TXRX_CSR8, 0x0085);

    write16(handle, TXRX_CSR19, 0x0000);
    write16(handle, TXRX_CSR21, 0xe78f);
    write16(handle, MAC_CSR9, 0xff1d);

    /*
     * Bring the RF/BBP state machine to AWAKE and WAIT for it to actually
     * converge before asserting HOST_READY. The original rt25usbap.sys set_state
     * routine (RE VMA 0x21070) writes DESIRE=AWAKE, triggers SET_STATE, then
     * POLLS MAC_CSR17 until CURR_STATE (bits 0x1e0 = BBP_CURR 0x60 + RF_CURR
     * 0x180) reads AWAKE, re-triggering SET_STATE up to ~15x with 30ms/5ms
     * stalls. Only THEN does it set HOST_READY. Previously we fired the two
     * writes and continued immediately: read-back of the DESIRE bits "matched",
     * but the RF may never have truly converged, leaving the hardware auto-ACK/
     * CTS responder DISARMED. Host-driven bulk TX (probe/auth responses) still
     * works off a half-settled RF, which is exactly why the DS could discover
     * and authenticate against us yet never receive a SIFS hardware ACK.
     */
    /*
     * HOST_READY early, UNCONDITIONALLY (rt25usbap.sys fr276: MAC_CSR1<=0x0004
     * right after MAC_CSR9). The AWAKE SET_STATE transition is NOT fired here any
     * more — it is deferred to config_ap_regs, AFTER the BBP/RF/antenna init and
     * the MAC address are programmed, matching the real driver (fr1402). Firing it
     * here (pre-PHY, pre-MACaddr) is what left the SIFS auto-responder disarmed.
     * The MAC_CSR13/14 -> 0x2121/0x1e1e reprogramming from the old MATCHORIG path is
     * dropped: the live capture keeps 0x1111/0x1e11 (fr249/254) and never rewrites them.
     */
    read16(handle, MAC_CSR1, &reg);
    write16(handle, MAC_CSR1, (reg & (uint16_t)~0x0003) | 0x0004);   /* HOST_READY */
    if (env_on("NWC_OLDAWAKE")) {
        printf("[init] NWC_OLDAWAKE: firing AWAKE early (pre-PHY) for regression\n");
        set_state_awake(handle);
    } else {
        printf("[init] HOST_READY set early; AWAKE deferred to post-PHY (config_ap_regs)\n");
    }

    /*
     * PHY_CSR2 (TX-MAC config) is REVISION-DEPENDENT in the original driver
     * (rt25usbap.sys reg_init @VMA 0x1b0e9; Linux rt2500usb.c:841-849):
     *   rev >= 3 (VERSION_C+): clear LNA bit           -> PHY_CSR2 = read & ~0x0002
     *   rev <  3 (VERSION_B):  set LNA + LNA_MODE=3     -> PHY_CSR2 = 0x3002
     * We previously ALWAYS took the rev>=3 branch. Since a hardware SIFS auto-ACK
     * is emitted through this raw TX-MAC path (unlike descriptor-based bulk TX,
     * which works regardless), a wrong PHY_CSR2 on a VERSION_B part could disable
     * the auto-ACK while leaving normal TX functional — matching our symptom.
     * NWC_PHYCSR2 can force a raw value for bench testing.
     */
    {
        const char *e = getenv("NWC_PHYCSR2");
        if (e && *e) {
            uint16_t v = (uint16_t)strtoul(e, NULL, 0);
            write16(handle, PHY_CSR2, v);
            printf("[phy] PHY_CSR2 override=0x%04x (NWC_PHYCSR2)\n", v);
        } else if (asic_rev >= 3) {
            read16(handle, PHY_CSR2, &reg);
            write16(handle, PHY_CSR2, reg & (uint16_t)~0x0002);   /* clear LNA */
            printf("[phy] PHY_CSR2 rev>=3 path: cleared LNA\n");
        } else {
            write16(handle, PHY_CSR2, 0x3002);                    /* LNA=1, LNA_MODE=3 */
            printf("[phy] PHY_CSR2 rev<3 path: wrote 0x3002 (VERSION_B)\n");
        }
    }
    write16(handle, MAC_CSR11, 0x0002);
    write16(handle, MAC_CSR22, 0x0053);
    write16(handle, MAC_CSR15, 0x0122);   /* original rt25usbap.sys value (RE VMA 0x1afdf); Linux used 0x01ee */
    write16(handle, MAC_CSR16, 0x0000);

    read16(handle, MAC_CSR8, &reg);
    write16(handle, MAC_CSR8, (reg & (uint16_t)~0x0fff) | 0x0780);   /* MAX_FRAME_UNIT=1920, original rt25usbap.sys (RE VMA 0x1b197); Linux used 0x0a00 */
    read16(handle, TXRX_CSR0, &reg);
    {
        /* TXRX_CSR0 = SECURITY control (ALGORITHM 0x7 | IV_OFFSET 0x1f8 | KEY_ID 0x1e00). The
         * usbmon capture of the WORKING original XP driver during a full DS auth has this =0x1ec2
         * (KEY_ID=0xf); we had KEY_ID=0 (final 0x02c2). This register governs UNICAST encryption
         * handling, so it's a prime suspect for why our unicast seq2->DS never radiates while
         * broadcast beacons/probe-responses do. NWC_CSR0 forces the low 13 bits (e.g. 0x1ec2). */
        uint16_t low = (uint16_t)(24u << 3);   /* IV_OFFSET=24, KEY_ID=0 (old default) */
        const char *e = getenv("NWC_CSR0");
        if (e && *e) low = (uint16_t)(strtol(e, NULL, 0) & 0x1fff);
        write16(handle, TXRX_CSR0, (uint16_t)((reg & (uint16_t)~0x1fff) | low));
    }
    read16(handle, MAC_CSR18, &reg);
    write16(handle, MAC_CSR18, (reg & (uint16_t)~0x00ff) | 90);
    read16(handle, PHY_CSR4, &reg);
    write16(handle, PHY_CSR4, reg | 0x0001);
    /* TXRX_CSR1 |= 0x8000 (AUTO_SEQUENCE) is an EXTRA write the original driver
     * NEVER does (it leaves TXRX_CSR1 at HW default — verified in the live init
     * capture). Dropped by default; NWC_CSR1_AUTOSEQ restores it for regression. */
    if (getenv("NWC_CSR1_AUTOSEQ") && *getenv("NWC_CSR1_AUTOSEQ")) {
        read16(handle, TXRX_CSR1, &reg);
        write16(handle, TXRX_CSR1, reg | 0x8000);
    }

    set_led(handle, true);
    printf("[init] basic RT2570 register init complete\n");
    return 0;
}

static int radio_init(libusb_device_handle *handle, uint8_t channel)
{
    uint8_t eeprom[256];
    int rc = read_eeprom(handle, eeprom, sizeof(eeprom));
    if (rc != 0)
        return rc;
    rc = basic_init(handle);
    if (rc != 0)
        return rc;
    rc = init_bbp(handle, eeprom);
    if (rc != 0)
        return rc;
    /*
     * TX power (RF3 bits[13:9]). The original rt25usbap.sys uses the PER-CHANNEL
     * EEPROM-CALIBRATED value (EEPROM_TXPOWER_START = word 0x1e = byte 0x3c),
     * clamped to 31 — NOT a flat 24 like we hardcoded. Driving 4 steps ABOVE the
     * chip's calibrated power can push the PA into nonlinear/distorted output: a
     * long beacon still gets detected (DS lists the AP), but the precisely-timed
     * 10us auto-ACK has far less decode margin -> "ACK sent but DS won't honor it".
     * NWC_TXPOWER overrides for a bench sweep.
     */
    {
        uint8_t txpower = eeprom[0x3c + (channel - 1)];
        if (txpower == 0xff || txpower > 31)
            txpower = 24;                      /* fallback if EEPROM blank */
        const char *e = getenv("NWC_TXPOWER");
        if (e && *e)
            txpower = (uint8_t)atoi(e);
        printf("[rf] TX power RF3=%u (EEPROM ch%u @0x%02x=0x%02x, was hardcoded 24)\n",
               txpower, channel, 0x3c + (channel - 1), eeprom[0x3c + (channel - 1)]);
        rc = config_channel_rf2525e(handle, channel, txpower);
    }
    if (rc != 0)
        return rc;
    rc = config_ant_rf2525e(handle, eeprom);
    if (rc != 0)
        return rc;
    set_led(handle, true);
    printf("[radio] register+BBP init complete\n");
    return 0;
}

static int beacon_once(libusb_device_handle *handle, uint8_t channel, const char *ssid_override,
                       bool privacy)
{
    uint8_t eeprom[256];
    int rc = read_eeprom(handle, eeprom, sizeof(eeprom));
    if (rc != 0)
        return rc;
    rc = radio_init(handle, channel);
    if (rc != 0)
        return rc;
    uint8_t mac[6] = { eeprom[4], eeprom[5], eeprom[6], eeprom[7], eeprom[8], eeprom[9] };
    rc = config_ap_regs(handle, mac);
    if (rc != 0)
        return rc;
    char ssid[33];
    uint8_t wep_key[13];
    if (ssid_override && strlen(ssid_override) <= 32) {
        snprintf(ssid, sizeof(ssid), "%s", ssid_override);
    } else {
        make_original_ssid(ssid, mac);
    }
    printf("[ap] original-style ssid=\"%s\"\n", ssid);
    if (privacy && strlen(ssid) == 32) {
        derive_original_wep_key(ssid + 12, wep_key);
        printf("[ap] derived WEP128 key0=");
        print_key_hex(wep_key);
        printf("\n");
        rc = config_wep128_key0(handle, wep_key);
        if (rc != 0)
            return rc;
    } else if (privacy) {
        printf("[ap] WEP derivation skipped: SSID is %zu bytes, original expects 32\n", strlen(ssid));
    } else {
        printf("[ap] diagnostic open mode: privacy/WEP disabled\n");
    }
    uint16_t reg = 0;
    read16(handle, TXRX_CSR19, &reg);
    uint16_t on = (uint16_t)(reg | 0x0019); /* TSF count + TBCN + beacon gen */
    uint16_t off = (uint16_t)(on & (uint16_t)~0x0010);
    rc = send_beacon_and_enable(handle, mac, ssid, channel, on, off, privacy);
    if (rc != 0)
        return rc;
    printf("[beacon] beacon generator toggled on\n");
    return 0;
}

/*
 * ============================ ASYNC ALWAYS-ARMED RX ============================
 *
 * The kernel rt2500usb driver keeps several RX URBs submitted at all times, so
 * the RT2570 always has a buffer to DMA a freshly-received frame into. Our first
 * implementation read one frame at a time synchronously, leaving gaps with no
 * pending IN URB. The hypothesis: the RT2570 only hardware-auto-ACKs a received
 * unicast frame it can immediately DMA into a *pending* buffer, so those gaps
 * suppressed the SIFS ACK and the DS retransmitted auth forever.
 *
 * Here we submit RX_URB_COUNT bulk-IN transfers that are continuously re-armed
 * from their completion callback (which only copies the frame into a ring queue
 * and resubmits — no TX, to avoid libusb re-entrancy). The main loop drains the
 * queue and does all TX (responses/beacons) synchronously.
 */
static libusb_context *g_ctx = NULL;

/* Deep RX pipeline. On the Linux kernel/libusb path 8 URBs suffice, but on Windows/libusbK a DS
 * data burst fills the URBs faster than the busy main loop re-arms them, so the dongle's internal
 * RX FIFO overflows (observed STA_CSR4 "RXFIFO" spiking to ~10000) and the MAC wedges -> "goes dark
 * after a successful communication". Far more in-flight URBs + a deeper queue absorb the burst so
 * the hardware FIFO never overruns. NWC_RX_URBS overrides the count at runtime. */
#define RX_URB_COUNT 128
#define RX_URB_SIZE  4096
#define RXQ_SIZE     512

struct rx_frame { uint8_t buf[RX_URB_SIZE]; int len; };
static struct rx_frame g_rxq[RXQ_SIZE];
static int g_rxq_head = 0;   /* written by callback */
static int g_rxq_tail = 0;   /* read by main loop   */
static struct libusb_transfer *g_rx_xfers[RX_URB_COUNT];
static int g_rx_stop = 0;
static int g_rx_submit_err = 0;

#ifndef NWC_BACKEND_KMDF
static void LIBUSB_CALL rx_transfer_cb(struct libusb_transfer *t)
{
    if (t->status == LIBUSB_TRANSFER_COMPLETED && t->actual_length > 0) {
        int next = (g_rxq_head + 1) % RXQ_SIZE;
        if (next != g_rxq_tail) {          /* drop if queue full */
            int n = t->actual_length;
            if (n > RX_URB_SIZE) n = RX_URB_SIZE;
            memcpy(g_rxq[g_rxq_head].buf, t->buffer, (size_t)n);
            g_rxq[g_rxq_head].len = n;
            g_rxq_head = next;
        }
    }
    if (!g_rx_stop) {
        int rc = libusb_submit_transfer(t);   /* re-arm immediately */
        if (rc != 0)
            g_rx_submit_err = rc;
    }
}
#endif /* !NWC_BACKEND_KMDF */

static int start_async_rx(libusb_device_handle *handle)
{
#ifdef NWC_BACKEND_KMDF
    (void)handle;
    /* The kernel driver runs the always-armed continuous reader; user mode just
     * drains it via poll_rx/IOCTL_NWC_RX_FRAME. No user-mode async URBs here, so
     * g_rx_xfers[0] stays NULL and ap_loop takes the poll_rx path. */
    printf("[rx] KMDF backend: kernel continuous reader owns RX; polling via RX_FRAME\n");
    return LIBUSB_ERROR_NOT_SUPPORTED;
#else
    for (int i = 0; i < RX_URB_COUNT; i++) {
        g_rx_xfers[i] = libusb_alloc_transfer(0);
        if (!g_rx_xfers[i])
            return LIBUSB_ERROR_NO_MEM;
        uint8_t *buf = (uint8_t *)malloc(RX_URB_SIZE);
        if (!buf)
            return LIBUSB_ERROR_NO_MEM;
        /* timeout 0 = never time out; the URB stays pending until a frame lands */
        libusb_fill_bulk_transfer(g_rx_xfers[i], handle, BULK_IN_EP,
                                  buf, RX_URB_SIZE, rx_transfer_cb, NULL, 0);
        int rc = libusb_submit_transfer(g_rx_xfers[i]);
        if (rc != 0) {
            printf("[rx-async] submit URB %d failed: %s\n", i, libusb_error_name(rc));
            return rc;
        }
    }
    printf("[rx-async] %d always-armed RX URBs submitted\n", RX_URB_COUNT);
    return 0;
#endif
}

static int ap_loop(libusb_device_handle *handle, uint8_t channel, const char *ssid_override,
                   bool privacy)
{
    uint8_t eeprom[256];
    int rc = read_eeprom(handle, eeprom, sizeof(eeprom));
    if (rc != 0)
        return rc;
    rc = radio_init(handle, channel);
    if (rc != 0)
        return rc;

    uint8_t mac[6] = { eeprom[4], eeprom[5], eeprom[6], eeprom[7], eeprom[8], eeprom[9] };
    char ssid[33];
    uint8_t wep_key[13];
    if (ssid_override && strlen(ssid_override) <= 32) {
        snprintf(ssid, sizeof(ssid), "%s", ssid_override);
    } else {
        make_original_ssid(ssid, mac);
    }
    rc = config_ap_regs(handle, mac);
    if (rc != 0)
        return rc;
    if (match_orig_init()) {
        /* RANK 1b: re-run the active AWAKE SET_STATE transition at the very end,
         * with HOST_READY already asserted (matches the original's final
         * set_state @0x21000) so the auto-responder arms against a live MAC. */
        printf("[init] MATCHORIG: final AWAKE transition (HOST_READY live)\n");
        set_state_awake(handle);
    }
#ifdef NWC_BACKEND_KMDF
    /* Radio register init is complete; kick the pipes so the in-kernel reader
     * arms against a live RX engine (and frees the OUT pipe for TX). */
    kmdf_postinit();
#endif

    printf("[ap] test AP active on channel %u ssid=\"%s\" bssid=%02x:%02x:%02x:%02x:%02x:%02x\n",
           channel, ssid, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (privacy && strlen(ssid) == 32) {
        derive_original_wep_key(ssid + 12, wep_key);
        printf("[ap] derived WEP128 key0=");
        print_key_hex(wep_key);
        printf("\n");
        rc = config_wep128_key0(handle, wep_key);
        if (rc != 0)
            return rc;
    } else if (privacy) {
        printf("[ap] WEP derivation skipped: SSID is %zu bytes, original expects 32\n", strlen(ssid));
    } else {
        printf("[ap] diagnostic open mode: privacy/WEP disabled\n");
    }
    printf("[ap] scan from the DS now; logging RX packets from endpoint 0x%02x\n", BULK_IN_EP);

    uint16_t reg = 0;
    read16(handle, TXRX_CSR19, &reg);
    uint16_t on = (uint16_t)(reg | 0x0019);
    {
        /*
         * NWC_CSR19 forces the EXACT final TXRX_CSR19 the beacon-enable leaves set.
         * Decisive auto-ACK test (RE rt25usbap.sys, in-kernel-servicing agent): set
         * 0x0005 (TSF_COUNT|TSF_SYNC2, NO BEACON_GEN) — the hardware TSF still runs
         * (required to arm the auto-responder) but the RT2570 does NOT try to emit a
         * hardware beacon from an unloaded beacon ring, which can wedge the shared
         * TX/ACK path while we software-beacon. If the DS's auth then gets ACKed and
         * it associates, BEACON_GEN starvation was the wall. Without this override
         * the default OR (|0x0019) always re-adds BEACON_GEN.
         */
        const char *e = getenv("NWC_CSR19");
        if (e && *e) {
            on = (uint16_t)strtol(e, NULL, 0);
            printf("[timing] beacon-enable forcing TXRX_CSR19 on=0x%04x (NWC_CSR19)\n", on);
        }
    }
    uint16_t off = (uint16_t)(on & (uint16_t)~0x0010);

    /*
     * Upload the beacon into the hardware beacon buffer and enable autonomous
     * TBTT beacon generation exactly ONCE. The RT2570 then transmits the beacon
     * every beacon interval on its own. Re-uploading and toggling BEACON_GEN in
     * a tight loop (as the previous code did every ~5ms) constantly tears down
     * the beacon generator so it never emits a stable beacon train.
     */
    /* Enable hardware autonomous beaconing (belt-and-suspenders). */
    rc = send_beacon_and_enable(handle, mac, ssid, channel, on, off, privacy);
    if (rc != 0)
        return rc;
    {
        /* DECISIVE TSF/beacon-engine check (hardware-beacon/auto-ACK hypothesis):
         * as a TSF MASTER our SIFS scheduling needs the hardware TSF counter
         * running. STA_CSR5 is the "beacon sent" counter; if it stays 0 the
         * beacon/TSF engine is wedged -> no timebase -> no SIFS auto-ACK. */
        uint16_t csr19rb = 0, sta5a = 0;
        read16(handle, TXRX_CSR19, &csr19rb);
        read16(handle, STA_CSR5, &sta5a);
        sleep_ms(400);
        uint16_t sta5b = 0;
        read16(handle, STA_CSR5, &sta5b);
        printf("[tsf] TXRX_CSR19 readback=0x%04x  STA_CSR5 beacon-count %u -> %u (%s)\n",
               csr19rb, sta5a, sta5b,
               (sta5b != sta5a) ? "COUNTING (hw beacon/TSF alive)" : "STUCK (hw beacon/TSF DEAD)");
    }

    if (env_on("NWC_APSTART")) {
        /*
         * Original AP-START responder-arming tail (rt25usbap.sys 0x1fb90): the
         * CLOSING action of SoftAP activation is a hard RX disable -> 100ms
         * settle -> re-enable edge on TXRX_CSR2, done AFTER the beacon/TSF engine
         * is already live. We otherwise perform the CSR2 edge EARLY (in
         * basic_init, before beacon-gen and with no settle). If the RT2570
         * latches its SIFS auto-responder on this trailing post-beacon RX
         * re-enable edge, our early edge misses it. Replicate it verbatim here.
         */
        printf("[ap-arm] AP-START responder arming (TXRX_CSR2 0x0001->100ms->0x0056, post-beacon)\n");
        write16(handle, MAC_CSR13, 0x1111);
        write16(handle, MAC_CSR14, 0x1e11);
        write16(handle, TXRX_CSR2, 0x0001);   /* DISABLE_RX */
        sleep_ms(100);
        write16(handle, TXRX_CSR2, 0x0056);   /* re-enable operational — the arming edge */
        sleep_ms(100);
        send_beacon_and_enable(handle, mac, ssid, channel, on, off, privacy);  /* re-upload beacon after RX reset */
    }

    /*
     * SOFTWARE BEACONING.
     *
     * The RT2570 hardware autonomous beacon generator (TXRX_CSR19 BEACON_GEN)
     * does not visibly radiate on this dongle from user mode: neither a phone
     * nor the DS ever saw the beacon, even though on-demand probe responses
     * (normal one-shot TX) DO reach the DS. The DS needs a real beacon to lock
     * onto/sync before it will complete the connector handshake. So we emit the
     * beacon ourselves through the same normal-TX path that already works, on a
     * ~100 ms software timer. The label ("beacon-sw") is NOT exactly "beacon",
     * so send_80211_frame sends it with no guardian byte (normal frame) and
     * suppresses per-beacon logging.
     */
    uint8_t beacon_frame[256];
    size_t beacon_len = build_beacon(beacon_frame, sizeof(beacon_frame), mac, ssid, channel, privacy);
    printf("[ap] software beaconing active (~100ms) + on-demand probe responses; entering RX loop\n");

    /* Keep the receiver continuously armed (see async-RX note above). */
    rc = start_async_rx(handle);
    if (rc != 0) {
        printf("[rx-async] failed to start; falling back to synchronous RX\n");
    }

    unsigned long last_beacon = GetTickCount();
    /* Software beacon cadence. Default 75ms (faster than the 100ms standard) so that even with CCA
     * back-off from a busy channel, the *radiated* rate stays high enough that the DS's brief
     * per-channel scan never lands in a gap (fixes the intermittent 51099 "can't see it"). The
     * advertised beacon-interval IE stays 100 TU; we just emit a bit more often. NWC_BEACON_MS tunes. */
    unsigned beacon_ms = 75;
    { const char *e = getenv("NWC_BEACON_MS"); if (e && *e) { int v = atoi(e); if (v >= 20 && v <= 200) beacon_ms = (unsigned)v; } }
    unsigned long last_hw_refresh = last_beacon;
    /* NWC_NOSWBEACON: rely ONLY on the hardware beacon generator (BEACON_GEN),
     * suppressing the ~100ms software beacon. Frees the single TX engine so the
     * hardware SIFS auto-ACK is never contending with a bulk-OUT software TX.
     * (The old "hw beacon doesn't radiate" note predates the KMDF bulk fix.) */
    int no_sw_beacon = getenv("NWC_NOSWBEACON") ? 1 : 0;
#ifndef _WIN32
    /* NWC_DATAPATH: bridge the DS's data traffic to the internet. Attach to the pre-created TAP
     * (default nwc0; NWC_TAP overrides) so DHCP (dnsmasq on nwc0) + NAT (iptables) carry the DS
     * online. Getting past 52003 needs this — the probe was management-only before. */
    if (env_on("NWC_DATAPATH")) {
        const char *tn = getenv("NWC_TAP"); if (!tn || !*tn) tn = "nwc0";
        g_tap_fd = tap_open(tn);
        if (g_tap_fd >= 0) printf("[bridge] DS<->internet data path ENABLED via %s\n", tn);
    }
#else
    /* NWC_DATAPATH (Windows): create the Wintun adapter + in-probe DHCP/ARP; New-NetNat carries it. */
    if (env_on("NWC_DATAPATH")) {
        if (win_datapath_init() == 0)
            printf("[bridge] DS<->internet data path ENABLED via Wintun 'NWC-DS'\n");
        dns_init_prewarm(); /* resolve every gamespy name NOW (off the packet thread) + start the
                             * async resolver, so win_handle_dns never blocks ap_loop on getaddrinfo
                             * (the stall that stranded the 45-byte GPCM segment; Linux resolves async) */
        rawret_open();   /* arm the raw-socket return path (bypasses New-NetNat's flaky reverse delivery) */
        wd_open();       /* arm WinDivert: intercept gamespy returns before New-NetNat swallows them */
    }
#endif
    /* NWC_HWBEACON: load the beacon into the RT2570 HARDWARE beacon buffer (0x2c00) so the
     * chip free-runs the beacon at each TBTT — the original-driver behavior. Implies no
     * software beacon (the HW does it). This is what should bring the TSF/auto-responder
     * fully alive so seq2/auth is serviced in AP mode. */
    int hw_beacon = getenv("NWC_HWBEACON") ? 1 : 0;
    if (hw_beacon) {
        /* Arm the hardware beacon engine (starts the TSF/TBTT ticking — STA_CSR5 counts).
         * Do NOT force the software beacon off: the HW beacon content doesn't radiate yet, so
         * the SW beacon still carries discovery while the HW engine keeps the TSF alive for
         * the auto-responder. NWC_NOSWBEACON=1 can still disable the SW beacon for HW-only tests. */
        hw_load_beacon(handle, mac, ssid, channel, privacy, on, off);
    }
    if (no_sw_beacon)
        printf("[ap] software beacon DISABLED (NWC_NOSWBEACON): hardware BEACON_GEN only\n");
    if (hw_beacon)
        printf("[ap] hardware beacon engine ARMED (NWC_HWBEACON); software beacon %s\n",
               no_sw_beacon ? "OFF" : "ON (for discovery)");
    /* Replicate rt25usbap.sys's continuous post-init runtime loop: the XP driver re-writes
     * the slot/SIFS/EIFS timing triple (MAC_CSR10/11/12) and polls STA_CSR0 (FCS err) HUNDREDS
     * of times after init. Hypothesis: the RT2570 loses/drifts the SIFS value after events
     * (beacon TBTT, RX), so the original keeps re-asserting it to hold the ~10us SIFS hardware
     * auto-ACK aligned to the DS's ACK window. We only wrote it once. Default ON; NWC_NORUNTIMELOOP off. */
    int runtime_loop = getenv("NWC_NORUNTIMELOOP") ? 0 : 1;
    unsigned long rt_ms = 150, last_rt = 0;   /* throttle so it does not starve the SW beacon */
    { const char *e = getenv("NWC_RTLOOP_MS"); if (e && *e) rt_ms = (unsigned long)strtol(e, NULL, 0); }
    if (runtime_loop)
        printf("[ap] XP runtime timing re-assert loop ENABLED (MAC_CSR10/11/12 + STA_CSR0 poll, every %lums)\n", rt_ms);
#ifdef NWC_BACKEND_KMDF
    unsigned long last_stats = last_beacon;
    kmdf_print_stats("-init");
    if (env_on("NWC_KERNMAC")) {
        /* FULL-MAC-PORT mode: hand the AP config to the KERNEL responder, which now owns
         * the beacon + ALL probe/auth/assoc/connector-grant responses (operating in-kernel
         * like rt2500usb, not over IOCTL round-trips). User mode is policy-only from here. */
        NWC_APCONFIG apc; DWORD kret = 0; size_t sl = strlen(ssid); if (sl > 32) sl = 32;
        memset(&apc, 0, sizeof(apc));
        memcpy(apc.mac, mac, 6);
        memcpy(apc.ssid, ssid, sl); apc.ssid_len = (unsigned char)sl;
        apc.channel = channel; apc.privacy = privacy ? 1 : 0;
        if (DeviceIoControl(g_kmdf, IOCTL_NWC_SET_APCONFIG, &apc, sizeof(apc), NULL, 0, &kret, NULL))
            printf("[kmdf] SET_APCONFIG ok -- KERNEL responder owns the MAC now (policy-only user mode)\n");
        else
            printf("[kmdf] SET_APCONFIG failed gle=%lu\n", GetLastError());
        for (;;) { Sleep(2000); kmdf_print_stats(""); }
    }
#endif
    for (;;) {
        /* Per-iteration wall-clock: catches a single loop pass that stalled on a blocking libusbK
         * sync TX (the thing that would inflate the cleared-on-read CCA counter). */
        { static unsigned long g_iter_prev = 0; unsigned long inow = GetTickCount();
          if (g_iter_prev) { unsigned long it = inow - g_iter_prev; if (it > g_loop_max) g_loop_max = it; }
          g_iter_prev = inow; }
        /* SECTION TIMER: find which part of one loop pass blocks for seconds. */
        unsigned long _sT = GetTickCount();
        #define CHK(n) do { unsigned long _d = GetTickCount()-_sT; if(_d>300) printf("[stall] %s %lums\n", n, _d); _sT = GetTickCount(); } while(0)
        /* BEACON PRIORITY: emit the beacon FIRST, before the (synchronous, libusbK-blocking) RX
         * drain + downstream data TX. Otherwise a DS data burst stretches the loop iteration, the
         * beacon misses its ~100ms slot, and the DS momentarily loses the AP ("goes dark"). The
         * NUC/kernel path doesn't block like this; this is the TX-side mirror of the deep-RX fix. */
        { unsigned long bnow = GetTickCount();
          if (!no_sw_beacon && beacon_len && (bnow - last_beacon) >= beacon_ms) {
              send_80211_frame(handle, beacon_frame, beacon_len, true, false, "beacon-sw");
              last_beacon = bnow;
          } }
        CHK("beacon");
        if (runtime_loop) {
            /* STA_CSR DIAGNOSTIC: dump the RT2570 statistics counters (0x04e0-0x04f4,
             * cleared-on-read so each print is the delta since last). c6..c10 are the
             * TX-status counters -- watching them during a DS attempt shows whether our
             * seq2 auth-response is being ACKed by the DS (TX success) or not (TX fail),
             * splitting "DS never gets seq2" from "seq1 auto-ACK is the wall". */
            unsigned long nrt = GetTickCount();
            if (nrt - last_rt >= rt_ms) {
                uint16_t s[11]; memset(s, 0, sizeof s);
                /* Read ONLY the two counters we use -- CCA (for the link tuner) + RXFIFO + BCN.
                 * The old full 11-register dump was 11 synchronous control transfers every interval,
                 * which blocked the loop long enough to jitter the ~100ms beacon cadence (0.2-0.3s
                 * gaps the DS scan misses). NWC_FULL_STACSR restores the full dump for diagnosis. */
                if (env_on("NWC_FULL_STACSR")) {
                    int k; for (k = 0; k < 11; k++) read16(handle, (uint16_t)(0x04e0 + k*2), &s[k]);
                } else if (!env_on("NWC_NO_STACSR")) {
                    read16(handle, 0x04e6, &s[3]);   /* STA_CSR3 = CCA (always) */
                    /* Each read16 is a ~10ms libusbK SYNC control transfer that stalls the loop and
                     * jitters the beacon. During a LIVE connection we only need CCA (the diagnostic);
                     * RXFIFO/BCN feed the discovery-only recovery, so read them ONLY when discovering. */
                    if (!g_sta_known) {
                        read16(handle, 0x04e8, &s[4]);   /* STA_CSR4 = RXFIFO */
                        read16(handle, 0x04ea, &s[5]);   /* STA_CSR5 = BCN   */
                    }
                }
                /* NWC_NO_STACSR (2026-07-26, server adaptation): skip the telemetry control-reads
                 * entirely. They only feed the CCA tuner + auto-recovery, BOTH disabled on this box;
                 * on a USB-congested host (28 game servers + storage) each read16 is a blocking
                 * libusbK sync transfer that stalls the loop for hundreds of ms -> beacon dark ->
                 * green->red. The Linux golden reference proved the "CCA saturation" is a fake
                 * cleared-on-read artifact of exactly this stall, so losing the CCA telemetry costs
                 * nothing real; loopmax is still printed below, and the [stall] lines still fire. */
                /* dt = actual ms since last read. CCA is cleared-on-read, so a big dt inflates the
                 * count: if a "CCA spike" comes with dt >> rt_ms, it's a LOOP STALL (libusbK sync TX
                 * blocking), not real RF. g_loop_max = worst single main-loop iteration since last read. */
                unsigned long dt = nrt - last_rt;
                extern volatile unsigned long g_loop_max;
                /* Heartbeat is spam at 150ms cadence. In quiet mode (the default for the
                 * all-in-one launcher) print it only every ~10s so the rotating log grows
                 * slowly; g_loop_max keeps accumulating across the suppressed intervals so
                 * the printed worst-case still covers the whole 10s window. The CCA read +
                 * stall auto-recovery below keep running every interval regardless. */
                { static unsigned long last_stacsr_print = 0;
                  unsigned long pnow = GetTickCount();
                  if (!nwc_quiet() || (pnow - last_stacsr_print) >= 10000) {
                      last_stacsr_print = pnow;
                      printf("[stacsr] FCS=%u PLCP=%u LONG=%u CCA=%u RXFIFO=%u BCN=%u dt=%lums loopmax=%lums CCA/150ms=%lu\n",
                             s[0],s[1],s[2],s[3],s[4],s[5], dt, g_loop_max,
                             dt ? (unsigned long)((unsigned long long)s[3]*150ull/dt) : (unsigned long)s[3]);
                      g_loop_max = 0;
                  } }
                last_rt = nrt;
                /* Stall detection + auto-recovery. s[5]=STA_CSR5 is the beacon-sent counter and this
                 * block is now its only reader, so several consecutive zero intervals is a genuine
                 * RT2570 carrier-sense jam (not a read race). Re-init alone doesn't clear it; a
                 * USB-level reset does. Recover in place so a connection survives. Require a long
                 * run of zeros so we never reset mid-auth. NWC_NO_AUTORECOVER disables. */
                { static int bstall = 0;
                  if (s[5] == 0) bstall++; else bstall = 0;
                  /* Only recover when the beacon engine is dark AND the DS is NOT actively talking.
                   * That's the fix for both failure modes: it heals the genuine post-burst TX-engine
                   * stall (dongle goes dark after gamespy, DS then can't see it -> 51099/51303) yet
                   * never fires mid-connection on transient burst CCA back-off (which used to USB-
                   * reset and drop a live connection -> 52003). g_last_ds_rx is the DS's last DATA
                   * frame; during the scan it stays old (probe-reqs don't touch it), so a start-up
                   * dark still recovers. NWC_NO_AUTORECOVER disables entirely. */
                  int ds_quiet = (GetTickCount() - g_last_ds_rx) > 2500u;
                  /* NEVER reset while a DS is associated (g_sta_known): a USB reset kills a LIVE
                   * gamespy/online session mid-flight (this dropped a green connection -> 61010). If
                   * the DS truly drops, win_poll_wintun's 6s-silence check clears g_sta_known first,
                   * and only THEN may we recover for the next attempt. So recovery is discovery-only. */
                  if (bstall >= 8 && ds_quiet && !g_sta_known && !env_on("NWC_NO_AUTORECOVER")) {
                      printf("[recover] beacon dark %d intervals + DS quiet + not-associated -> USB reset + re-init\n", bstall);
                      int rrc = libusb_reset_device(handle);
                      printf("[recover] libusb_reset_device rc=%d (%s)\n", rrc, libusb_error_name(rrc));
                      if (rrc == LIBUSB_ERROR_NOT_FOUND || rrc == LIBUSB_ERROR_NO_DEVICE) {
                          printf("[recover] device re-enumerated; exiting for clean relaunch\n"); return; }
                      libusb_claim_interface(handle, 0);
                      radio_init(handle, channel);
                      config_ap_regs(handle, mac);
                      if (hw_beacon) hw_load_beacon(handle, mac, ssid, channel, privacy, on, off);
                      else send_beacon_and_enable(handle, mac, ssid, channel, on, off, privacy);
                      g_sta_known = 0;
                      bstall = 0;
                  } }
                /* Dynamic CCA link-tuner (mirrors rt2500usb_link_tuner): continuously trim BBP R17
                 * against the false-CCA count so the carrier-sense doesn't jam. s[3]=CCA count this
                 * interval. High CCA -> raise R17 (less sensitive); quiet -> lower it (recover
                 * sensitivity). Keeps the beacon/TX engine winning channel access. NWC_NO_CCATUNE off. */
                if (!env_on("NWC_NO_CCATUNE")) {
                    static int cur_r17 = 0;
                    if (cur_r17 == 0) { const char *e=getenv("NWC_BBP_R17"); cur_r17=(e&&*e)?(int)strtol(e,NULL,0):0x38; }
                    /* Cap raised 0x40 -> 0x54 (NWC_R17_CAP): the DS sits at ~-38 dBm (very strong),
                     * so the radio can run a much higher carrier-sense/ED threshold -- ignoring the
                     * intermittent 2.4GHz noise that spikes CCA to ~800 and defers our beacon (DS
                     * then "can't see it" -> 51303) -- without going deaf to the DS. Empirically the
                     * runs that reached gamespy had CCA ~584; when it climbs to ~800 the old 0x40
                     * cap wasn't enough to keep the beacon winning channel access. Step harder on a
                     * big spike so we recover fast. Floor unchanged so a quiet channel restores full
                     * sensitivity. NWC_R17_CAP overrides the ceiling; NWC_NO_CCATUNE disables. */
                    /* BALANCE, not maximum. R17 is BOTH the CCA/TX-defer threshold AND the RX
                     * sensitivity: too low -> the beacon defers on a noise spike (51303); too high
                     * -> the radio goes DEAF to the DS's probe/auth frames (also 51303 -- pushing the
                     * cap to 0x62 drove R17 to 0x5f, CCA read ~0, and the DS vanished). ~0x40 is the
                     * practical ceiling where the radio still hears the DS AND mostly beats noise --
                     * the level that reached gamespy. Cap there and RETURN TO BASELINE FAST so a spike
                     * can never leave us stuck deaf. NWC_R17_CAP overrides. */
                    int cap = 0x42; { const char *ce=getenv("NWC_R17_CAP"); if (ce&&*ce) cap=(int)strtol(ce,NULL,0); }
                    int base = 0x38; { const char *be=getenv("NWC_BBP_R17"); if (be&&*be) base=(int)strtol(be,NULL,0); }
                    int nr = cur_r17;
                    if (s[3] > 600 && cur_r17 < cap) nr = cur_r17 + 2;         /* noisy -> less sensitive */
                    else if (s[3] < 400 && cur_r17 > base) nr = cur_r17 - 2;    /* calm -> back to sensitive baseline FAST */
                    if (nr > cap) nr = cap; if (nr < 0x24) nr = 0x24;
                    if (nr != cur_r17) { bbp_write(handle, 17, (uint8_t)nr); cur_r17 = nr;
                        printf("[cca-tune] CCA=%u -> BBP R17=0x%02x (cap 0x%02x)\n", s[3], cur_r17, cap); }
                }
            }
        }
        CHK("stacsr");
#ifndef NWC_BACKEND_KMDF
        if (g_rx_xfers[0]) {
            /* Drive async RX completions (callbacks copy frames into g_rxq). */
            struct timeval tv = { 0, 5000 };   /* 5 ms */
            libusb_handle_events_timeout_completed(g_ctx, &tv, NULL);
            CHK("rxpump");
            /* Process everything the callbacks queued. Re-pump libusb every few frames so completed
             * RX URBs get re-armed DURING this (TX-heavy) drain -- otherwise a big DS burst keeps us
             * here long enough that the dongle's hardware RX FIFO overflows and the MAC wedges
             * ("goes dark after a successful communication"). Non-blocking (0 timeout). */
            /* CAP the per-iteration RX processing. Unbounded, a foreign+DS RX burst on busy channel 1
             * fills g_rxq and draining it ALL in one pass (WEP-decrypt+forward each) stalled the loop
             * 100ms+ -> beacon starved -> DS dropped mid-green. The 512-entry g_rxq absorbs the burst;
             * leftover frames drain next iteration, so the beacon still fires on time. NWC_RX_DRAIN. */
            static int rx_cap = -1;
            if (rx_cap < 0) { const char *e=getenv("NWC_RX_DRAIN"); rx_cap=(e&&*e)?atoi(e):8; if(rx_cap<2)rx_cap=2; if(rx_cap>256)rx_cap=256; }
            int drained = 0;
            int fskip = 0;
            while (g_rxq_tail != g_rxq_head && drained < rx_cap) {
                struct rx_frame *f = &g_rxq[g_rxq_tail];
                /* ACTIVE-CONNECTION RX FILTER (Windows GPCM/crash fix). Once our DS is
                 * associated and bridging data (g_sta_known), the ONLY frames that
                 * matter are DATA frames (802.11 type 2) from our DS. Skip everything
                 * else cheaply BEFORE the expensive log_rx_packet:
                 *   (a) all MANAGEMENT frames (type 0: auth/assoc/probe). The DS fires
                 *       spurious re-auth/re-assoc retries (observed auth x30) whose
                 *       libusbK response TX stalls the drain ~200-800ms EACH -> the
                 *       server->DS GPCM message (and the DS's uplink ACK) are delayed
                 *       both ways -> a late login packet is never delivered/ACKed ->
                 *       61010 / null-deref Data-Abort. The DS is already associated;
                 *       these retries need no answer. A REAL drop stops data flow ->
                 *       the 6s bridge-close clears g_sta_known -> mgmt is processed
                 *       again so genuine reconnect still works.
                 *   (b) frames from any other transmitter (foreign channel-1 noise).
                 * Skips don't count against rx_cap, so bursts clear in one cheap pass.
                 * NWC_RX_ALL disables the filter for A/B. */
                if (g_sta_known && !env_on("NWC_RX_ALL") && f->len >= 16) {
                    uint8_t _ftype = (uint8_t)((f->buf[0] >> 2) & 0x3);   /* 0=mgmt 1=ctrl 2=data */
                    if (_ftype != 2 || memcmp(f->buf + 10, g_sta_mac, 6) != 0) {
                        g_rxq_tail = (g_rxq_tail + 1) % RXQ_SIZE;
                        if (++fskip > RXQ_SIZE) break;   /* safety bound */
                        continue;
                    }
                }
                uint8_t _fty = (f->len>=2) ? (uint8_t)((f->buf[0]>>4)&0xf) : 0xff;
                unsigned long _ft = GetTickCount();
                log_rx_packet(handle, f->buf, f->len, mac, ssid, channel, privacy);
                { unsigned long _fd = GetTickCount()-_ft; if(_fd>150) printf("[stall] rxframe subtype=%u len=%d %lums\n", _fty, f->len, _fd); }
                g_rxq_tail = (g_rxq_tail + 1) % RXQ_SIZE;
                if ((++drained & 3) == 0) { struct timeval z = {0,0};
                    libusb_handle_events_timeout_completed(g_ctx, &z, NULL); }
            }
            CHK("rxdrain");
            if (g_rx_submit_err != 0) {
                printf("[rx-async] resubmit error %s; device likely gone\n",
                       libusb_error_name(g_rx_submit_err));
                return g_rx_submit_err;
            }
        } else
#endif
        {
            poll_rx(handle, 20, mac, ssid, channel, privacy);
        }
#ifndef _WIN32
        tap_poll_to_ds(handle, mac, ssid, privacy);   /* drain TAP (NAT replies) -> WEP -> TX to DS */
#else
        win_poll_wintun(handle, mac, ssid, privacy);  /* drain Wintun (NAT replies) -> WEP -> TX to DS */
        CHK("wintun");
        rawret_poll(handle, mac, ssid, privacy);      /* raw-socket return: server replies New-NetNat dropped */
        wd_drain(handle, mac, ssid, privacy);         /* WinDivert return: gamespy replies -> DS (single-threaded USB TX) */
        CHK("wddrain");
#endif
        unsigned long now = GetTickCount();
        if (!no_sw_beacon && beacon_len && (now - last_beacon) >= beacon_ms) {
            send_80211_frame(handle, beacon_frame, beacon_len, true, false, "beacon-sw");
            last_beacon = now;
        }
        /* NWC_TXTEST: fire synthetic frames to dummy dests every 2s so the AR9271 sniff
         * shows WHICH frame kinds actually radiate — no DS needed. Distinct dest MACs:
         *   ->..11 probe-resp (baseline that radiates)   ->..22 full 160B auth-resp
         *   ->..33 short auth-resp (no challenge)         ->..44 raw 160B via probe path */
        static unsigned long last_txtest = 0;
        if (env_on("NWC_TXTEST") && (now - last_txtest) >= 1000u) {
            /* Radiation self-test (no DS needed): fire a burst of DATA frames to a dummy dest and a
             * baseline probe-response. Sniff type-2 frames from our BSSID and compare to the counts
             * printed here to get the data-frame RADIATION RATE (Linux ~91%, Windows was ~40%).
             * NWC_TXTEST_BURST sets frames/cycle; NWC_TX_GAP_US inserts a spin-gap between them. */
            static const uint8_t d_probe[6] = {0x02,0,0,0,0,0x11};
            static const uint8_t d_data[6]  = {0x02,0,0,0,0,0x44};
            int burst = 10; const char *be = getenv("NWC_TXTEST_BURST"); if (be&&*be) burst = atoi(be);
            uint8_t testpay[64]; memset(testpay, 0xA5, sizeof testpay);
            send_probe_response(handle, mac, d_probe, ssid, channel, privacy);   /* radiating control */
            for (int k = 0; k < burst; k++)
                send_eth_to_ds(handle, mac, ssid, privacy, d_data, mac, 0x0800, testpay, sizeof testpay);
            static int total_test = 0; total_test += burst;
            printf("[txtest] fired 1 probe + %d DATA frames (data total=%d)\n", burst, total_test);
            last_txtest = now;
        }
        /* Occasionally re-arm the hardware beacon generator too, harmlessly. Note: do NOT read
         * STA_CSR5 here -- it clears on read and would race the stall detector in the [stacsr]
         * block, causing false "beacon idle" triggers. */
        if ((now - last_hw_refresh) >= 5000u) {
            uint16_t csr19 = 0;
            read16(handle, TXRX_CSR19, &csr19);
            printf("[tsf] TXRX_CSR19=0x%04x\n", csr19);
            if (hw_beacon)
                hw_load_beacon(handle, mac, ssid, channel, privacy, on, off);
            else
                send_beacon_and_enable(handle, mac, ssid, channel, on, off, privacy);
            last_hw_refresh = now;
        }
#ifdef NWC_BACKEND_KMDF
        if ((now - last_stats) >= 2000u) {
            kmdf_print_stats("");
            last_stats = now;
        }
#endif
        fflush(stdout);
    }
}

#ifndef NWC_BACKEND_KMDF
static void dump_config(libusb_device *dev)
{
    struct libusb_config_descriptor *cfg = NULL;
    int rc = libusb_get_active_config_descriptor(dev, &cfg);
    if (rc != 0) {
        printf("[desc] active config unavailable: %s\n", libusb_error_name(rc));
        rc = libusb_get_config_descriptor(dev, 0, &cfg);
    }
    if (rc != 0) {
        printf("[desc] config 0 unavailable: %s\n", libusb_error_name(rc));
        return;
    }

    printf("[desc] config value=%u interfaces=%u attributes=0x%02x max_power=%umA\n",
           cfg->bConfigurationValue, cfg->bNumInterfaces, cfg->bmAttributes,
           cfg->MaxPower * 2);

    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *intf = &cfg->interface[i];
        for (int a = 0; a < intf->num_altsetting; a++) {
            const struct libusb_interface_descriptor *alt = &intf->altsetting[a];
            printf("[desc] interface=%u alt=%u class=0x%02x endpoints=%u\n",
                   alt->bInterfaceNumber, alt->bAlternateSetting,
                   alt->bInterfaceClass, alt->bNumEndpoints);
            for (int e = 0; e < alt->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
                printf("[desc]   endpoint=0x%02x attrs=0x%02x max_packet=%u interval=%u\n",
                       ep->bEndpointAddress, ep->bmAttributes,
                       ep->wMaxPacketSize, ep->bInterval);
            }
        }
    }

    libusb_free_config_descriptor(cfg);
}
#endif /* !NWC_BACKEND_KMDF */

/* Full live register snapshot for offline comparison against the original
 * driver's programming (esp. BBP/RF PHY). Run AFTER radio init. */
static void dump_all_regs(libusb_device_handle *handle)
{
    uint16_t v;
    printf("=== MAC_CSR (0x0400-0x042e) ===\n");
    for (uint16_t a = 0x0400; a <= 0x042e; a += 2) { v = 0; read16(handle, a, &v); printf("  0x%04x = 0x%04x\n", a, v); }
    printf("=== TXRX_CSR (0x0440-0x046a) ===\n");
    for (uint16_t a = 0x0440; a <= 0x046a; a += 2) { v = 0; read16(handle, a, &v); printf("  0x%04x = 0x%04x\n", a, v); }
    printf("=== SEC_CSR (0x0480-0x049e) ===\n");
    for (uint16_t a = 0x0480; a <= 0x049e; a += 2) { v = 0; read16(handle, a, &v); printf("  0x%04x = 0x%04x\n", a, v); }
    printf("=== PHY_CSR (0x04c0-0x04d4) ===\n");
    for (uint16_t a = 0x04c0; a <= 0x04d4; a += 2) { v = 0; read16(handle, a, &v); printf("  0x%04x = 0x%04x\n", a, v); }
    printf("=== BBP registers (0-127) ===\n");
    for (uint8_t r = 0; r < 128; r++) {
        uint8_t bv = 0;
        if (bbp_read(handle, r, &bv) == 0) printf("  BBP R%-3u = 0x%02x\n", r, bv);
        else printf("  BBP R%-3u = <read failed>\n", r);
    }
}

int main(int argc, char **argv)
{
    /* stdout FULLY BUFFERED (was _IONBF/unbuffered). Unbuffered made every printf an
     * immediate blocking write() to the redirected log file; log_rx_packet emits a
     * multi-line per-frame hexdump, so under the connection's frame rate the main loop
     * drowned in synchronous file-I/O syscalls -- THE 200-800ms per-frame "rxdrain"
     * stalls that starved the beacon and the server->DS GPCM delivery (ring backed up
     * to 184 queued), stranding the final login packet -> 61010 / Data-Abort. A 1 MiB
     * full buffer makes printf a memcpy; the main loop's periodic fflush(stdout) keeps
     * the log current for diagnostics. NWC_UNBUF restores the old behaviour for A/B. */
    static char g_outbuf[1 << 20];
    if (getenv("NWC_UNBUF") && *getenv("NWC_UNBUF"))
        setvbuf(stdout, NULL, _IONBF, 0);
    else
        setvbuf(stdout, g_outbuf, _IOFBF, sizeof g_outbuf);

#ifdef NWC_BACKEND_KMDF
    /*
     * KMDF backend main: the dongle is owned by nwcusbap.sys, so we open the
     * driver's control device instead of enumerating with libusb. Command
     * parsing and dispatch mirror the libusb path below; the SoftAP handlers
     * (basic_init/radio_init/beacon_once/ap_loop) are the exact same functions.
     */
    const char *command = argc > 1 ? argv[1] : "probe";
    uint8_t channel = 1;
    if (argc > 2) {
        int parsed = atoi(argv[2]);
        if (parsed >= 1 && parsed <= 14)
            channel = (uint8_t)parsed;
        else
            printf("[args] invalid channel \"%s\", using channel 1\n", argv[2]);
    }
    const char *ssid_override = argc > 3 ? argv[3] : NULL;
    if (ssid_override && strlen(ssid_override) > 32) {
        printf("[args] SSID override is longer than 32 bytes; ignoring it\n");
        ssid_override = NULL;
    }
    bool privacy = true;
    if (argc > 4 && strcmp(argv[4], "open") == 0)
        privacy = false;
    if (ssid_override && strcmp(ssid_override, "open") == 0) {
        ssid_override = NULL;
        privacy = false;
    }

    printf("[init] command=%s KMDF backend via \\\\.\\NWCUSBAP (VID_%04X PID_%04X)\n",
           command, NWCUSB_VID, NWCUSB_PID);
    g_kmdf = CreateFileA("\\\\.\\NWCUSBAP", GENERIC_READ | GENERIC_WRITE,
                         0, NULL, OPEN_EXISTING, 0, NULL);
    if (g_kmdf == INVALID_HANDLE_VALUE) {
        printf("[open] CreateFile(\\\\.\\NWCUSBAP) failed gle=%lu; is nwcusbap.sys installed and started?\n",
               GetLastError());
        return 2;
    }
    printf("[open] opened \\\\.\\NWCUSBAP (kernel continuous-reader RX active)\n");

    libusb_device_handle *handle = (libusb_device_handle *)g_kmdf; /* opaque token; leaves use g_kmdf */
    int mrc = 0;
    if (strcmp(command, "led-on") == 0)             mrc = set_led(handle, true);
    else if (strcmp(command, "led-off") == 0)       mrc = set_led(handle, false);
    else if (strcmp(command, "init-basic") == 0)    mrc = basic_init(handle);
    else if (strcmp(command, "init-radio") == 0)    mrc = radio_init(handle, channel);
    else if (strcmp(command, "beacon-once") == 0)   mrc = beacon_once(handle, channel, ssid_override, privacy);
    else if (strcmp(command, "ap-loop") == 0)       mrc = ap_loop(handle, channel, ssid_override, privacy);
    else if (strcmp(command, "dumpregs") == 0) {
        mrc = radio_init(handle, channel);
        if (mrc == 0) {
            uint8_t eeprom[256];
            if (read_eeprom(handle, eeprom, sizeof(eeprom)) == 0) {
                printf("=== EEPROM (0-255) ===\n");
                hexdump(eeprom, sizeof(eeprom));
            }
            dump_all_regs(handle);
        }
    }
    else if (strcmp(command, "rxdiag") == 0) {
        /* DS-free RX-pipe health check: init the radio, then open the RX filter
         * to promiscuous (clear DROP_NOT_TO_ME) and watch whether the kernel
         * continuous reader completes from ambient 2.4GHz traffic. */
        mrc = radio_init(handle, channel);
        if (mrc == 0) {
            uint8_t eeprom[256];
            read_eeprom(handle, eeprom, sizeof(eeprom));
            uint8_t mac[6] = { eeprom[4], eeprom[5], eeprom[6], eeprom[7], eeprom[8], eeprom[9] };
            config_ap_regs(handle, mac);
            write16(handle, TXRX_CSR2, 0x0046); /* keep DROP_CRC/PHYSICAL, clear DROP_NOT_TO_ME */
            kmdf_postinit();
            printf("[rxdiag] promiscuous RX (TXRX_CSR2=0x0046); watching reader ~10s for ambient traffic\n");
            for (int i = 0; i < 10; i++) {
                Sleep(1000);
                poll_rx(handle, 0, mac, "rxdiag", channel, false);
                kmdf_print_stats("");
            }
        }
    }
    else {
        uint8_t eeprom[256];
        memset(eeprom, 0, sizeof(eeprom));
        int erc = vendor_read(handle, USB_EEPROM_READ, 0, eeprom, sizeof(eeprom), "eeprom[0..255]");
        if (erc > 0)
            hexdump(eeprom, erc);
        uint16_t offsets[] = { 0x0000, 0x0002, 0x0004, 0x0008, 0x0308 };
        for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
            uint8_t csr[2] = { 0, 0 };
            int crc = vendor_read(handle, USB_MULTI_READ, offsets[i], csr, sizeof(csr), "csr16");
            if (crc == 2)
                printf("[csr] offset=0x%04x value=0x%04x\n", offsets[i],
                       (uint16_t)csr[0] | ((uint16_t)csr[1] << 8));
        }
    }
    CloseHandle(g_kmdf);
    g_kmdf = INVALID_HANDLE_VALUE;
    return mrc == 0 ? 0 : 4;
#else
    libusb_context *ctx = NULL;
    /* g_ctx is used by the async-RX event loop in ap_loop(). */
    libusb_device_handle *handle = NULL;
    int rc = libusb_init(&ctx);
    if (rc != 0) {
        printf("[init] libusb_init failed: %s\n", libusb_error_name(rc));
        return 1;
    }
    g_ctx = ctx;   /* async-RX event loop uses this context */

    libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_INFO);
    const char *command = argc > 1 ? argv[1] : "probe";
    uint8_t channel = 1;
    if (argc > 2) {
        int parsed = atoi(argv[2]);
        if (parsed >= 1 && parsed <= 14)
            channel = (uint8_t)parsed;
        else
            printf("[args] invalid channel \"%s\", using channel 1\n", argv[2]);
    }
    const char *ssid_override = argc > 3 ? argv[3] : NULL;
    if (ssid_override && strlen(ssid_override) > 32) {
        printf("[args] SSID override is longer than 32 bytes; ignoring it\n");
        ssid_override = NULL;
    }
    bool privacy = true;
    if (argc > 4 && strcmp(argv[4], "open") == 0)
        privacy = false;
    if (ssid_override && strcmp(ssid_override, "open") == 0) {
        ssid_override = NULL;
        privacy = false;
    }
    printf("[init] command=%s searching for VID_%04X PID_%04X\n",
           command, NWCUSB_VID, NWCUSB_PID);

    handle = libusb_open_device_with_vid_pid(ctx, NWCUSB_VID, NWCUSB_PID);
    if (!handle) {
        printf("[open] device not opened. It may not be inserted or may not be bound to WinUSB/libusb.\n");
        libusb_exit(ctx);
        return 2;
    }

    libusb_device *dev = libusb_get_device(handle);
    struct libusb_device_descriptor desc;
    rc = libusb_get_device_descriptor(dev, &desc);
    if (rc == 0) {
        printf("[desc] usb=%x.%02x class=0x%02x vendor=0x%04x product=0x%04x configs=%u\n",
               desc.bcdUSB >> 8, desc.bcdUSB & 0xff, desc.bDeviceClass,
               desc.idVendor, desc.idProduct, desc.bNumConfigurations);
    }
    dump_config(dev);

    if (strcmp(command, "usb-reset") == 0 || strcmp(command, "reset") == 0) {
        printf("[reset] issuing USB device reset\n");
        rc = libusb_reset_device(handle);
        printf("[reset] result=%s\n", libusb_error_name(rc));
        libusb_close(handle);
        libusb_exit(ctx);
        return rc == 0 ? 0 : 4;
    }

    rc = libusb_claim_interface(handle, 0);
    if (rc != 0) {
        printf("[claim] interface 0 failed: %s\n", libusb_error_name(rc));
        printf("[claim] this usually means the dongle is still using a non-WinUSB driver.\n");
        libusb_close(handle);
        libusb_exit(ctx);
        return 3;
    }
    printf("[claim] interface 0 claimed\n");

    if (strcmp(command, "led-on") == 0) {
        rc = set_led(handle, true);
        libusb_release_interface(handle, 0);
        libusb_close(handle);
        libusb_exit(ctx);
        return rc == 0 ? 0 : 4;
    }
    if (strcmp(command, "led-off") == 0) {
        rc = set_led(handle, false);
        libusb_release_interface(handle, 0);
        libusb_close(handle);
        libusb_exit(ctx);
        return rc == 0 ? 0 : 4;
    }
    if (strcmp(command, "init-basic") == 0) {
        rc = basic_init(handle);
        libusb_release_interface(handle, 0);
        libusb_close(handle);
        libusb_exit(ctx);
        return rc == 0 ? 0 : 4;
    }
    if (strcmp(command, "init-radio") == 0) {
        rc = radio_init(handle, channel);
        libusb_release_interface(handle, 0);
        libusb_close(handle);
        libusb_exit(ctx);
        return rc == 0 ? 0 : 4;
    }
    if (strcmp(command, "beacon-once") == 0) {
        rc = beacon_once(handle, channel, ssid_override, privacy);
        libusb_release_interface(handle, 0);
        libusb_close(handle);
        libusb_exit(ctx);
        return rc == 0 ? 0 : 4;
    }
    if (strcmp(command, "ap-loop") == 0) {
        rc = ap_loop(handle, channel, ssid_override, privacy);
        libusb_release_interface(handle, 0);
        libusb_close(handle);
        libusb_exit(ctx);
        return rc == 0 ? 0 : 4;
    }

    uint8_t eeprom[256];
    memset(eeprom, 0, sizeof(eeprom));
    rc = vendor_read(handle, USB_EEPROM_READ, 0, eeprom, sizeof(eeprom), "eeprom[0..255]");
    if (rc > 0)
        hexdump(eeprom, rc);

    uint8_t csr[2];
    uint16_t offsets[] = { 0x0000, 0x0002, 0x0004, 0x0008, 0x0308 };
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        memset(csr, 0, sizeof(csr));
        rc = vendor_read(handle, USB_MULTI_READ, offsets[i], csr, sizeof(csr), "csr16");
        if (rc == 2) {
            uint16_t value = (uint16_t)csr[0] | ((uint16_t)csr[1] << 8);
            printf("[csr] offset=0x%04x value=0x%04x\n", offsets[i], value);
        }
    }

    libusb_release_interface(handle, 0);
    libusb_close(handle);
    libusb_exit(ctx);
    return 0;
#endif
}
