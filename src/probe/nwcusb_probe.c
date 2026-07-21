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
#include <windows.h>
#endif
#include "libusb.h"

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
    /* NOTE: the DS's registration probe also embeds the console's user nickname
     * (UTF-16LE) alongside these constant markers -- matching it makes detection
     * stricter, but it is per-console, so we match only the constant markers here. */
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
                                     length, 2000);
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
                                     length, 2000);
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
    libusb_sleep(0);
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
    static const uint8_t fallback_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };  /* placeholder; the real MAC is read from EEPROM offset 0x04 */

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
        memcpy(eeprom + 4, fallback_mac, sizeof(fallback_mac));
        printf("[eeprom] using cached fallback MAC %02x:%02x:%02x:%02x:%02x:%02x; BBP EEPROM overrides disabled\n",
               fallback_mac[0], fallback_mac[1], fallback_mac[2],
               fallback_mac[3], fallback_mac[4], fallback_mac[5]);
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

static int send_80211_frame(libusb_device_handle *handle, const uint8_t *frame,
                            size_t frame_len, bool request_timestamp,
                            bool request_ack, const char *label)
{
    uint8_t packet[20 + 512 + 4];
    if (frame_len > 512)
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
    int noguard = (getenv("NWC_NOGUARDIAN") && *getenv("NWC_NOGUARDIAN"));
    int rc = 0;
    if (!noguard) {
        rc = libusb_bulk_transfer(handle, BULK_OUT_EP, &guardian, 1, &transferred, 2000);
        if (rc != 0) {
            printf("[tx] %s guardian failed: %s\n", label, libusb_error_name(rc));
            return rc;
        }
    }
    rc = libusb_bulk_transfer(handle, BULK_OUT_EP, packet, transfer_len, &transferred, 2000);
    if (rc != 0) {
        printf("[tx] %s frame failed (noguard=%d): %s\n", label, noguard, libusb_error_name(rc));
        return rc;
    }
    if (strcmp(label, "beacon") != 0 || VERBOSE_USB_REG)
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
    return send_80211_frame(handle, frame, frame_len, false, true, "auth-response");
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

static bool should_answer_probe(const uint8_t *frame, int frame_len, const char *ssid)
{
    (void)ssid;
    if (!is_probe_request(frame, frame_len))
        return false;
    /* P1: only answer probes aimed at OUR connector — the SSID IE must begin "NWCUSBAP"
     * (the DS's registration probe, or a directed probe for our SSID). Skip the
     * broadcast/wildcard/foreign probe STORM from unrelated nearby stations: the gold XP
     * capture shows ~0 probe-responses during the auth window, and answering them all
     * (tx=9673 in our logs) keeps the single TX engine busy and blocks the DS's auth
     * SIFS auto-ACK. Passive discovery still works via the beacon. NWC_ALLPROBE = answer all. */
    if (getenv("NWC_ALLPROBE") && *getenv("NWC_ALLPROBE"))
        return true;
    int ie = mgmt_ie_offset(frame, frame_len);
    if (ie >= 0 && ie + 2 + 8 <= frame_len && frame[ie] == 0 && frame[ie + 1] >= 8 &&
        memcmp(frame + ie + 2, "NWCUSBAP", 8) == 0)
        return true;
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
    if (getenv("NWC_NODEDUP") && *getenv("NWC_NODEDUP")) return false;
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
            send_auth_response(handle, mac, src, alg, 2, 0, true);
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

static void log_80211_frame(const uint8_t *frame, int frame_len, const char *source,
                            const uint8_t *ap_mac, const char *ap_ssid)
{
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
        printf("[rx] *** registration hint matched: <ds-nickname>/Nintendo/NWCUSB bytes present ***\n");

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
        if (ap_mac && ap_ssid)
            maybe_answer_management(handle, buf, len, ap_mac, ap_ssid, channel, privacy);
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
        if (ap_mac && ap_ssid)
            maybe_answer_management(handle, frame, frame_avail, ap_mac, ap_ssid, channel, privacy);
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
    if (!(getenv("NWC_OLDAWAKE") && *getenv("NWC_OLDAWAKE"))) {
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
    write16(handle, TXRX_CSR18, (uint16_t)(((100u * 4u) << 4) & 0xfff0u));
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
    if (getenv("NWC_OLDAWAKE") && *getenv("NWC_OLDAWAKE")) {
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
    write16(handle, TXRX_CSR0, (uint16_t)((reg & (uint16_t)~0x1fff) | (24 << 3)));
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

#define RX_URB_COUNT 8
#define RX_URB_SIZE  4096
#define RXQ_SIZE     64

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

    if (getenv("NWC_APSTART") && *getenv("NWC_APSTART")) {
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
    unsigned long last_hw_refresh = last_beacon;
    /* NWC_NOSWBEACON: rely ONLY on the hardware beacon generator (BEACON_GEN),
     * suppressing the ~100ms software beacon. Frees the single TX engine so the
     * hardware SIFS auto-ACK is never contending with a bulk-OUT software TX.
     * (The old "hw beacon doesn't radiate" note predates the KMDF bulk fix.) */
    int no_sw_beacon = getenv("NWC_NOSWBEACON") ? 1 : 0;
    if (no_sw_beacon)
        printf("[ap] software beacon DISABLED (NWC_NOSWBEACON): hardware BEACON_GEN only\n");
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
    if (getenv("NWC_KERNMAC") && *getenv("NWC_KERNMAC")) {
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
        if (runtime_loop) {
            /* STA_CSR DIAGNOSTIC: dump the RT2570 statistics counters (0x04e0-0x04f4,
             * cleared-on-read so each print is the delta since last). c6..c10 are the
             * TX-status counters -- watching them during a DS attempt shows whether our
             * seq2 auth-response is being ACKed by the DS (TX success) or not (TX fail),
             * splitting "DS never gets seq2" from "seq1 auto-ACK is the wall". */
            unsigned long nrt = GetTickCount();
            if (nrt - last_rt >= rt_ms) {
                uint16_t s[11]; int k;
                for (k = 0; k < 11; k++) { s[k] = 0; read16(handle, (uint16_t)(0x04e0 + k*2), &s[k]); }
                printf("[stacsr] FCS=%u PLCP=%u LONG=%u CCA=%u RXFIFO=%u BCN=%u c6=%u c7=%u c8=%u c9=%u c10=%u\n",
                       s[0],s[1],s[2],s[3],s[4],s[5],s[6],s[7],s[8],s[9],s[10]);
                last_rt = nrt;
            }
        }
#ifndef NWC_BACKEND_KMDF
        if (g_rx_xfers[0]) {
            /* Drive async RX completions (callbacks copy frames into g_rxq). */
            struct timeval tv = { 0, 5000 };   /* 5 ms */
            libusb_handle_events_timeout_completed(g_ctx, &tv, NULL);
            /* Process everything the callbacks queued. */
            while (g_rxq_tail != g_rxq_head) {
                struct rx_frame *f = &g_rxq[g_rxq_tail];
                log_rx_packet(handle, f->buf, f->len, mac, ssid, channel, privacy);
                g_rxq_tail = (g_rxq_tail + 1) % RXQ_SIZE;
            }
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
        unsigned long now = GetTickCount();
        if (!no_sw_beacon && beacon_len && (now - last_beacon) >= 100u) {
            send_80211_frame(handle, beacon_frame, beacon_len, true, false, "beacon-sw");
            last_beacon = now;
        }
        /* Occasionally re-arm the hardware beacon generator too, harmlessly. */
        if ((now - last_hw_refresh) >= 5000u) {
            uint16_t sta5 = 0, csr19 = 0;
            read16(handle, STA_CSR5, &sta5);
            read16(handle, TXRX_CSR19, &csr19);
            printf("[tsf] STA_CSR5 beacon-count=%u TXRX_CSR19=0x%04x\n", sta5, csr19);
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
    setvbuf(stdout, NULL, _IONBF, 0);

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

    printf("[init] command=%s KMDF backend via <path> (VID_%04X PID_%04X)\n",
           command, NWCUSB_VID, NWCUSB_PID);
    g_kmdf = CreateFileA("<path>", GENERIC_READ | GENERIC_WRITE,
                         0, NULL, OPEN_EXISTING, 0, NULL);
    if (g_kmdf == INVALID_HANDLE_VALUE) {
        printf("[open] CreateFile(<path> failed gle=%lu; is nwcusbap.sys installed and started?\n",
               GetLastError());
        return 2;
    }
    printf("[open] opened <path> (kernel continuous-reader RX active)\n");

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
