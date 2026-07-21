/*
 * Shared IOCTL contract between nwcusbap.sys (KMDF) and the user-mode SoftAP
 * service. Open the device via \\.\NWCUSBAP.
 */
#pragma once

#ifndef CTL_CODE
#include <winioctl.h>
#endif

#define NWC_DEVICE_TYPE  0x8000

/* 16-bit register access (0x0400+id region), matching nwcusb_probe.c read16/write16. */
typedef struct _NWC_REG_IO {
    unsigned short reg;    /* full register address, e.g. 0x0444 = TXRX_CSR2 */
    unsigned short value;  /* value for SET; filled by GET */
} NWC_REG_IO;

#define IOCTL_NWC_SET_REG   CTL_CODE(NWC_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_NWC_GET_REG   CTL_CODE(NWC_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS)

/* Raw 802.11 frame TX (input buffer = full bulk-OUT payload incl. guardian handling
 * done by the caller if needed) and RX-poll (output buffer receives one queued
 * frame incl. the 16-byte RXD prefix; 0 bytes returned if the ring is empty). */
#define IOCTL_NWC_TX_FRAME  CTL_CODE(NWC_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_NWC_RX_FRAME  CTL_CODE(NWC_DEVICE_TYPE, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS)

/* Generic vendor control-transfer passthrough. SET_REG/GET_REG only cover the
 * 16-bit CSR path (USB request 6/7); the RT2570 init also needs EEPROM read
 * (request 9, multi-byte), the single control-write (request 2, e.g. 0x0308),
 * and the device-mode test (request 1). This IOCTL carries any vendor request
 * so nwcusb_probe.c's vendor_read/vendor_write (and thus read16/write16,
 * read_eeprom, write_single) map onto it 1:1 with the init sequence unchanged.
 *
 * METHOD_BUFFERED layout:
 *   dir_in == 0 (host->device): input buffer = NWC_CONTROL header immediately
 *             followed by `length` data bytes. Output buffer unused.
 *   dir_in == 1 (device->host): input buffer = NWC_CONTROL header only; the
 *             output buffer receives `length` data bytes (bytesReturned = count).
 */
typedef struct _NWC_CONTROL {
    unsigned char  request;   /* bRequest (1/2/6/7/9 ...) */
    unsigned char  dir_in;    /* 0 = host->device (OUT), 1 = device->host (IN) */
    unsigned short value;     /* wValue */
    unsigned short index;     /* wIndex (register address / EEPROM offset) */
    unsigned short length;    /* data-stage byte count (follows header for OUT) */
} NWC_CONTROL;

#define IOCTL_NWC_CONTROL   CTL_CODE(NWC_DEVICE_TYPE, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Diagnostics: driver-internal counters so user mode can tell whether the bulk
 * data path is alive without a kernel debugger. rx_complete/rx_bytes track
 * continuous-reader completions; rx_failed/last_reader_status/last_usbd_status
 * track reader failures; tx_count/tx_fail/last_tx_status track raw-TX writes;
 * reader_config_status is the WdfUsbTargetPipeConfigContinuousReader result;
 * pipe_flags bit0=BulkIn found, bit1=BulkOut found. */
typedef struct _NWC_STATS {
    unsigned int   rx_complete;
    unsigned int   rx_bytes;
    unsigned int   rx_dropped;
    unsigned int   rx_failed;
    unsigned int   tx_count;
    unsigned int   tx_fail;
    long           last_reader_status;   /* NTSTATUS */
    unsigned int   last_usbd_status;     /* USBD_STATUS */
    long           last_tx_status;       /* NTSTATUS */
    long           reader_config_status; /* NTSTATUS */
    unsigned int   pipe_flags;           /* bit0 in, bit1 out */
    long           reset_status;         /* Stage-0 usb_reset_device() NTSTATUS */
    unsigned short csr12_17_before[6];   /* ACK/CTS-time regs 0x0458-0x0462 before device reset */
    unsigned short csr12_17_after[6];    /* ...and after (did the reset change them?) */
    long           select_status;        /* WdfUsbTargetDeviceSelectConfig NTSTATUS */
    long           responder_status;     /* NwcResponderStart NTSTATUS */
    unsigned int   start_stage;          /* how far PrepareHardware got: 1..5 (5=full) */
    unsigned char  num_configs;          /* device descriptor bNumConfigurations */
    long           config_desc_status;   /* RetrieveConfigDescriptor NTSTATUS */
    unsigned int   config_desc_len;      /* config descriptor total length */
    unsigned char  config_desc[48];      /* raw config descriptor (first 48 bytes) */
} NWC_STATS;

#define IOCTL_NWC_STATS   CTL_CODE(NWC_DEVICE_TYPE, 0x805, METHOD_BUFFERED, FILE_READ_ACCESS)

/* Post-init pipe kick: called by user mode AFTER the radio register init is
 * complete. Resets the bulk pipes and stops+restarts the IN continuous reader
 * so the always-armed reads begin against a live RX engine (the reader
 * otherwise auto-starts at D0, before init, and can wedge the half-duplex
 * radio — blocking both RX and TX). No payload. */
#define IOCTL_NWC_POSTINIT   CTL_CODE(NWC_DEVICE_TYPE, 0x806, METHOD_BUFFERED, FILE_WRITE_ACCESS)

/* AP responder config: user mode hands down MAC/SSID/channel/privacy AFTER its
 * proven radio init; the in-kernel responder then owns the beacon + all probe/
 * auth/assoc/connector-grant responses (user mode becomes policy-only). */
typedef struct _NWC_APCONFIG {
    unsigned char  mac[6];
    unsigned char  ssid[32];
    unsigned char  ssid_len;   /* 0..32 */
    unsigned char  channel;    /* 1..14 */
    unsigned char  privacy;    /* WEP on */
} NWC_APCONFIG;

#define IOCTL_NWC_SET_APCONFIG  CTL_CODE(NWC_DEVICE_TYPE, 0x807, METHOD_BUFFERED, FILE_WRITE_ACCESS)

/* TEST-ONLY: inject a raw 802.11 frame into the RX ring as if the continuous reader
 * delivered it, exercising the in-kernel responder (parse/WEP/build/TX) with no real
 * dongle traffic. Used by the VM safety harness to crash-test every responder path. */
#define IOCTL_NWC_INJECT_RX     CTL_CODE(NWC_DEVICE_TYPE, 0x808, METHOD_BUFFERED, FILE_WRITE_ACCESS)
