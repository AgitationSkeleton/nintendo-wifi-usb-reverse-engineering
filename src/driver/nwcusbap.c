/*
 * nwcusbap.sys — minimal KMDF USB client driver for the Nintendo Wi-Fi USB
 * Connector (Ralink RT2570, VID_0411/PID_008B).
 *
 * PURPOSE (staged experiment): move the USB access into the kernel so the
 * RT2570's receiver is driven by an in-kernel, always-armed CONTINUOUS READER
 * (microsecond URB turnaround) instead of user-mode WinUSB. Hypothesis: the
 * hardware SIFS auto-ACK only fires when a received frame is DMA'd to the host
 * fast enough; WinUSB's user-mode latency misses that window, the kernel path
 * does not. This driver exposes raw 802.11 frame TX/RX + register access to the
 * existing user-mode SoftAP service (nwcusb_probe.c logic) via IOCTLs.
 *
 * This is a SHIM: hardware register init is done from user mode over the
 * SET_REG/GET_REG IOCTLs (reusing nwcusb_probe.c's proven sequence), so the
 * kernel side stays small and the decisive ACK test can run quickly.
 *
 * Build: WDK 10.0.19041 (KMDF 1.11). Load requires test-signing (Secure Boot
 * OFF). See REBOOT_READINESS.md.
 */

#include <ntddk.h>
#include <wdf.h>
#include <usb.h>        /* USBD_STATUS, USB_REQUEST_* (usbspec.h) */
#include <usbdlib.h>
#include <wdfusb.h>

#include "nwcusbap_ioctl.h"

#define NWC_VID 0x0411
#define NWC_PID 0x008b
#define NWC_POOL_TAG 'apcN'

/* Ring of received raw frames delivered up to user mode. */
#define RX_RING_DEPTH 64
#define RX_FRAME_MAX  2400

typedef struct _RX_SLOT {
    ULONG len;
    UCHAR buf[RX_FRAME_MAX];
} RX_SLOT;

typedef struct _DEVICE_CONTEXT {
    WDFUSBDEVICE       UsbDevice;
    WDFUSBINTERFACE    UsbInterface;
    WDFUSBPIPE         BulkInPipe;    /* 0x81 */
    WDFUSBPIPE         BulkOutPipe;   /* 0x01 */
    WDFSPINLOCK        RxLock;
    RX_SLOT            Rx[RX_RING_DEPTH];
    LONG               RxHead;        /* producer (reader completion) */
    LONG               RxTail;        /* consumer (IOCTL) */
    /* Diagnostics (see NWC_STATS). */
    volatile LONG      RxComplete;
    volatile LONG      RxBytes;
    volatile LONG      RxDropped;
    volatile LONG      RxFailed;
    volatile LONG      TxCount;
    volatile LONG      TxFail;
    NTSTATUS           LastReaderStatus;
    USBD_STATUS        LastUsbdStatus;
    NTSTATUS           LastTxStatus;
    NTSTATUS           ReaderConfigStatus;
    ULONG              PipeFlags;
    NTSTATUS           ResetStatus;        /* Stage 0 usb_reset_device() result */
    NTSTATUS           SelectStatus;       /* WdfUsbTargetDeviceSelectConfig result */
    NTSTATUS           ResponderStatus;    /* NwcResponderStart result */
    ULONG              StartStage;         /* how far PrepareHardware got: 1..5 */
    UCHAR              NumConfigs;         /* device descriptor bNumConfigurations */
    NTSTATUS           ConfigDescStatus;   /* RetrieveConfigDescriptor result */
    ULONG              ConfigDescLen;      /* config descriptor total length */
    UCHAR              ConfigDesc[48];     /* raw config descriptor (first 48 bytes) */
    USHORT             Csr12_17Before[6];  /* ACK/CTS-time regs 0x0458-0x0462 before reset */
    USHORT             Csr12_17After[6];   /* ...and after (did the reset change them?) */

    /* ---- in-kernel MAC responder (nwcusbap_responder.c) ---- */
    UCHAR              Mac[6];
    CHAR               Ssid[33];
    ULONG              SsidLen;
    UCHAR              Channel;
    BOOLEAN            Privacy;
    volatile LONG      Configured;
    PVOID              ThreadObj;
    KEVENT             RxEvent;
    KEVENT             StopEvent;
    volatile LONG      Stopping;
    WDFWAITLOCK        TxLock;
    UCHAR              TxScratch[20 + 512 + 8];
    UCHAR              RxWork[RX_FRAME_MAX];
    UCHAR              WepDecrypt[256];
    UCHAR              AuthPlain[192];
    struct { UCHAR mac[6]; ULONGLONG firstMs; LONG used; } Reg[16];
    struct { UCHAR mac[6]; USHORT seq; ULONGLONG ms; LONG used; } Auth[16];
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

/* Single-instance driver: file-scope context pointer for the readers-failed
 * callback, which receives no context argument. */
static PDEVICE_CONTEXT g_devctx = NULL;

DRIVER_INITIALIZE                 DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD         NwcEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE   NwcEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE   NwcEvtReleaseHardware;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL NwcEvtIoDeviceControl;
EVT_WDF_USB_READER_COMPLETION_ROUTINE   NwcEvtBulkInComplete;
EVT_WDF_USB_READERS_FAILED             NwcEvtReadersFailed;

/* ---- 16-bit register access via USB vendor control transfers (rt2570) ---- */
/* Matches nwcusb_probe.c read16/write16: vendor request to 0x0400+id region. */
#define USB_MULTI_WRITE 6
#define USB_MULTI_READ  7

static NTSTATUS NwcRegWrite(PDEVICE_CONTEXT ctx, USHORT reg, USHORT value)
{
    WDF_USB_CONTROL_SETUP_PACKET setup;
    WDF_MEMORY_DESCRIPTOR desc;
    USHORT le = value; /* device is little-endian; value stored as-is */
    ULONG xfer = 0;
    WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(&setup,
        BmRequestHostToDevice, BmRequestToDevice, USB_MULTI_WRITE, 0, reg);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&desc, &le, sizeof(le));
    return WdfUsbTargetDeviceSendControlTransferSynchronously(
        ctx->UsbDevice, NULL, NULL, &setup, &desc, &xfer);
}

static NTSTATUS NwcRegRead(PDEVICE_CONTEXT ctx, USHORT reg, USHORT *value)
{
    WDF_USB_CONTROL_SETUP_PACKET setup;
    WDF_MEMORY_DESCRIPTOR desc;
    USHORT le = 0;
    ULONG xfer = 0;
    NTSTATUS st;
    WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(&setup,
        BmRequestDeviceToHost, BmRequestToDevice, USB_MULTI_READ, 0, reg);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&desc, &le, sizeof(le));
    st = WdfUsbTargetDeviceSendControlTransferSynchronously(
        ctx->UsbDevice, NULL, NULL, &setup, &desc, &xfer);
    if (NT_SUCCESS(st)) *value = le;
    return st;
}

/* In-kernel 802.11 SoftAP MAC responder (frame builders + PASSIVE responder thread
 * + beacon). Uses DEVICE_CONTEXT, the RX ring, and BulkOutPipe defined above. */
#include "nwcusbap_responder.c"

/* --------------------------- DriverEntry --------------------------------- */
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, NwcEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath,
                           WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

/* --------------------------- EvtDeviceAdd -------------------------------- */
NTSTATUS NwcEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDFDEVICE device;
    PDEVICE_CONTEXT ctx;
    WDF_IO_QUEUE_CONFIG qcfg;
    WDFQUEUE queue;
    UNICODE_STRING symlink;

    UNREFERENCED_PARAMETER(Driver);
    RtlInitUnicodeString(&symlink, L"\\DosDevices\\NWCUSBAP");

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = NwcEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = NwcEvtReleaseHardware;   /* stop responder thread */
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attrs, &device);
    if (!NT_SUCCESS(status)) return status;

    ctx = GetDeviceContext(device);
    RtlZeroMemory(ctx, sizeof(*ctx));
    g_devctx = ctx;

    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = device;
    status = WdfSpinLockCreate(&attrs, &ctx->RxLock);
    if (!NT_SUCCESS(status)) return status;

    status = WdfWaitLockCreate(&attrs, &ctx->TxLock);   /* serializes in-kernel TX */
    if (!NT_SUCCESS(status)) return status;

    /* Default parallel queue for user-mode IOCTLs. */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchParallel);
    qcfg.EvtIoDeviceControl = NwcEvtIoDeviceControl;
    status = WdfIoQueueCreate(device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) return status;

    status = WdfDeviceCreateSymbolicLink(device, &symlink);
    return status;
}

/* -------- EvtPrepareHardware: open USB, pipes, start continuous reader --- */
NTSTATUS NwcEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    NTSTATUS status;
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS cfgParams;
    WDF_USB_CONTINUOUS_READER_CONFIG readerCfg;
    WDF_USB_PIPE_INFORMATION pipeInfo;
    UCHAR i, count;

    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);

    if (ctx->UsbDevice == NULL) {
        WDF_USB_DEVICE_CREATE_CONFIG createParams;
        WDF_USB_DEVICE_CREATE_CONFIG_INIT(&createParams, USBD_CLIENT_CONTRACT_VERSION_602);
        status = WdfUsbTargetDeviceCreateWithParameters(Device, &createParams,
                     WDF_NO_OBJECT_ATTRIBUTES, &ctx->UsbDevice);
        if (!NT_SUCCESS(status)) return status;
    }

    /* STAGE 0 (rt2x00usb.c:805 usb_reset_device): a full USB port reset restores the RT2570
     * auto-responder / ACK-CTS-TIME registers that rt2500usb NEVER writes and relies on at
     * power-on-reset default — chiefly TXRX_CSR10 responder-enable bits and TXRX_CSR12-17
     * (ACK/CTS TIME, 0x0458-0x0462). We only ever pipe-reset, so those can be STALE from a
     * prior driver/run -> the chip emits its SIFS ACK at the wrong boundary -> the DS never
     * registers a valid ACK -> retransmits auth seq1 forever -> 51303. This is invisible to
     * the register set we readback-verified (writes, not reset-default dependencies).
     * ResetPort (not CyclePort) does NOT re-enumerate, so this will not loop PrepareHardware. */
    {
        /* Stage-0 port reset REMOVED: it was refuted (CSR12-17 read identical before/after,
         * did not fix the SIFS auto-ACK) AND WdfUsbTargetDeviceResetPortSynchronously must
         * NOT be called from EvtDevicePrepareHardware -- on the x86/1809 stack it faulted the
         * start path (CM_PROB_FAILED_START, ProblemStatus 0xC0000005). We keep only the passive
         * CSR12-17 readback for the STATS diagnostic; no port reset. */
        USHORT b[6] = {0};
        int i;
        for (i = 0; i < 6; i++) NwcRegRead(ctx, (USHORT)(0x0458 + i * 2), &b[i]);
        ctx->ResetStatus = STATUS_SUCCESS;
        for (i = 0; i < 6; i++) { ctx->Csr12_17Before[i] = b[i]; ctx->Csr12_17After[i] = b[i]; }
        DbgPrint("nwcusbap: (no port reset) CSR12-17=%04x %04x %04x %04x %04x %04x\n",
                 b[0],b[1],b[2],b[3],b[4],b[5]);
    }

    /* DIAGNOSTIC: dump the config descriptor so we can see why SelectConfig(SINGLE_INTERFACE)
     * rejects this device with STATUS_INVALID_PARAMETER (num interfaces / alt settings / EPs). */
    {
        USB_DEVICE_DESCRIPTOR dd;
        ULONG cdl = sizeof(ctx->ConfigDesc);
        RtlZeroMemory(&dd, sizeof(dd));
        WdfUsbTargetDeviceGetDeviceDescriptor(ctx->UsbDevice, &dd);
        ctx->NumConfigs = dd.bNumConfigurations;
        ctx->ConfigDescStatus = WdfUsbTargetDeviceRetrieveConfigDescriptor(ctx->UsbDevice,
                                    ctx->ConfigDesc, &cdl);
        ctx->ConfigDescLen = cdl;
    }

    /* DIAGNOSTIC MODE: never fail PrepareHardware -- record each call's status into the
     * context and always return SUCCESS, so the device starts and the STATS IOCTL can
     * report exactly which call rejected a parameter (0xC000000D on the VBox/x86 stack).
     * start_stage: 1=select failed, 2=no pipes, 3=reader failed, 4=responder failed, 5=full. */
    ctx->StartStage = 1;
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&cfgParams);
    status = WdfUsbTargetDeviceSelectConfig(ctx->UsbDevice, WDF_NO_OBJECT_ATTRIBUTES, &cfgParams);
    ctx->SelectStatus = status;
    if (NT_SUCCESS(status)) {
        ctx->StartStage = 2;
        ctx->UsbInterface = cfgParams.Types.SingleInterface.ConfiguredUsbInterface;
        count = WdfUsbInterfaceGetNumConfiguredPipes(ctx->UsbInterface);
        for (i = 0; i < count; i++) {
            WDFUSBPIPE pipe;
            WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);
            pipe = WdfUsbInterfaceGetConfiguredPipe(ctx->UsbInterface, i, &pipeInfo);
            if (pipeInfo.PipeType != WdfUsbPipeTypeBulk) continue;
            if (WdfUsbTargetPipeIsInEndpoint(pipe)) { ctx->BulkInPipe = pipe; ctx->PipeFlags |= 1; }
            else if (WdfUsbTargetPipeIsOutEndpoint(pipe)) { ctx->BulkOutPipe = pipe; ctx->PipeFlags |= 2; }
        }
        if (ctx->BulkInPipe != NULL && ctx->BulkOutPipe != NULL) {
            ctx->StartStage = 3;
            /* Allow zero-length/short reads without a size check. */
            WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(ctx->BulkInPipe);
            /* THE POINT: always-armed in-kernel continuous reader on the IN pipe. */
            WDF_USB_CONTINUOUS_READER_CONFIG_INIT(&readerCfg, NwcEvtBulkInComplete,
                                                  ctx, RX_FRAME_MAX);
            readerCfg.NumPendingReads = 8;
            readerCfg.EvtUsbTargetPipeReadersFailed = NwcEvtReadersFailed;
            status = WdfUsbTargetPipeConfigContinuousReader(ctx->BulkInPipe, &readerCfg);
            ctx->ReaderConfigStatus = status;
            if (NT_SUCCESS(status)) {
                ctx->StartStage = 4;
                /* Start the in-kernel MAC responder (inits RxEvent/StopEvent + PASSIVE
                 * thread) BEFORE the framework auto-starts the reader at D0 entry. */
                ctx->ResponderStatus = NwcResponderStart(ctx);
                if (NT_SUCCESS(ctx->ResponderStatus)) ctx->StartStage = 5;
            }
        }
    }

    /* Continuous reader auto-starts on device D0 entry (framework-managed). */
    return STATUS_SUCCESS;
}

/* Stop the in-kernel responder thread on hardware release (PASSIVE-level). */
NTSTATUS NwcEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    NwcResponderStop(GetDeviceContext(Device));
    return STATUS_SUCCESS;
}

/* RX completion: copy the frame into the ring for user mode to poll. */
VOID NwcEvtBulkInComplete(WDFUSBPIPE Pipe, WDFMEMORY Buffer, size_t NumBytes, WDFCONTEXT Context)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;
    PUCHAR data;
    LONG next;
    UNREFERENCED_PARAMETER(Pipe);
    InterlockedIncrement(&ctx->RxComplete);
    if (NumBytes == 0 || NumBytes > RX_FRAME_MAX) return;
    InterlockedExchangeAdd(&ctx->RxBytes, (LONG)NumBytes);
    data = (PUCHAR)WdfMemoryGetBuffer(Buffer, NULL);
    WdfSpinLockAcquire(ctx->RxLock);
    next = (ctx->RxHead + 1) % RX_RING_DEPTH;
    if (next != ctx->RxTail) {
        ctx->Rx[ctx->RxHead].len = (ULONG)NumBytes;
        RtlCopyMemory(ctx->Rx[ctx->RxHead].buf, data, NumBytes);
        ctx->RxHead = next;
    } else {
        InterlockedIncrement(&ctx->RxDropped);
    }
    WdfSpinLockRelease(ctx->RxLock);
    if (ctx->ThreadObj != NULL)
        KeSetEvent(&ctx->RxEvent, IO_NO_INCREMENT, FALSE);   /* wake responder (DISPATCH-safe) */
}

BOOLEAN NwcEvtReadersFailed(WDFUSBPIPE Pipe, NTSTATUS Status, USBD_STATUS UsbdStatus)
{
    PDEVICE_CONTEXT ctx = g_devctx;
    UNREFERENCED_PARAMETER(Pipe);
    if (ctx) {
        InterlockedIncrement(&ctx->RxFailed);
        ctx->LastReaderStatus = Status;
        ctx->LastUsbdStatus = UsbdStatus;
    }
    return TRUE; /* let framework reset the pipe and continue */
}

/* -------- IOCTL: register R/W, raw TX, raw RX-poll ----------------------- */
VOID NwcEvtIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLen, size_t InLen, ULONG Code)
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PDEVICE_CONTEXT ctx = GetDeviceContext(device);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;

    switch (Code) {
    case IOCTL_NWC_SET_REG: {
        NWC_REG_IO *p; size_t len;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*p), (PVOID*)&p, &len);
        if (NT_SUCCESS(status)) status = NwcRegWrite(ctx, p->reg, p->value);
        break;
    }
    case IOCTL_NWC_GET_REG: {
        NWC_REG_IO *pin, *pout; size_t len;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*pin), (PVOID*)&pin, &len);
        if (!NT_SUCCESS(status)) break;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*pout), (PVOID*)&pout, &len);
        if (!NT_SUCCESS(status)) break;
        pout->reg = pin->reg;
        status = NwcRegRead(ctx, pin->reg, &pout->value);
        if (NT_SUCCESS(status)) info = sizeof(*pout);
        break;
    }
    case IOCTL_NWC_TX_FRAME: {
        PVOID buf; size_t len; WDF_MEMORY_DESCRIPTOR md; ULONG sent = 0;
        status = WdfRequestRetrieveInputBuffer(Request, 1, &buf, &len);
        if (!NT_SUCCESS(status)) break;
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&md, buf, (ULONG)len);
        status = WdfUsbTargetPipeWriteSynchronously(ctx->BulkOutPipe, NULL, NULL, &md, &sent);
        InterlockedIncrement(&ctx->TxCount);
        ctx->LastTxStatus = status;
        if (!NT_SUCCESS(status)) InterlockedIncrement(&ctx->TxFail);
        break;
    }
    case IOCTL_NWC_RX_FRAME: {
        PVOID out; size_t len;
        status = WdfRequestRetrieveOutputBuffer(Request, 1, &out, &len);
        if (!NT_SUCCESS(status)) break;
        WdfSpinLockAcquire(ctx->RxLock);
        if (ctx->RxTail != ctx->RxHead) {
            ULONG n = ctx->Rx[ctx->RxTail].len;
            if (n > len) n = (ULONG)len;
            RtlCopyMemory(out, ctx->Rx[ctx->RxTail].buf, n);
            ctx->RxTail = (ctx->RxTail + 1) % RX_RING_DEPTH;
            info = n;
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_SUCCESS; /* empty: 0 bytes */
            info = 0;
        }
        WdfSpinLockRelease(ctx->RxLock);
        break;
    }
    case IOCTL_NWC_CONTROL: {
        NWC_CONTROL *p; size_t inlen; NTSTATUS xst;
        WDF_USB_CONTROL_SETUP_PACKET setup;
        WDF_MEMORY_DESCRIPTOR md;
        ULONG xfer = 0;
        UCHAR request, dir_in;
        USHORT value, index, dlen;
        PVOID dbuf = NULL;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*p), (PVOID*)&p, &inlen);
        if (!NT_SUCCESS(status)) break;
        /* Copy header out before the transfer: for the IN case the USB read
         * targets the same METHOD_BUFFERED system buffer and would clobber it. */
        request = p->request; dir_in = p->dir_in;
        value = p->value; index = p->index; dlen = p->length;

        if (dir_in) {
            WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(&setup,
                BmRequestDeviceToHost, BmRequestToDevice, request, value, index);
            if (dlen) {
                size_t outcap;
                status = WdfRequestRetrieveOutputBuffer(Request, dlen, &dbuf, &outcap);
                if (!NT_SUCCESS(status)) break;
            }
        } else {
            WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(&setup,
                BmRequestHostToDevice, BmRequestToDevice, request, value, index);
            if (dlen) {
                if (inlen < sizeof(*p) + dlen) { status = STATUS_BUFFER_TOO_SMALL; break; }
                dbuf = (PUCHAR)p + sizeof(*p);
            }
        }

        if (dlen) {
            WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&md, dbuf, dlen);
            xst = WdfUsbTargetDeviceSendControlTransferSynchronously(
                      ctx->UsbDevice, NULL, NULL, &setup, &md, &xfer);
        } else {
            xst = WdfUsbTargetDeviceSendControlTransferSynchronously(
                      ctx->UsbDevice, NULL, NULL, &setup, NULL, &xfer);
        }
        status = xst;
        if (NT_SUCCESS(status) && dir_in) info = xfer;
        break;
    }
    case IOCTL_NWC_STATS: {
        NWC_STATS *s; size_t len;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*s), (PVOID*)&s, &len);
        if (!NT_SUCCESS(status)) break;
        s->rx_complete = (unsigned int)ctx->RxComplete;
        s->rx_bytes = (unsigned int)ctx->RxBytes;
        s->rx_dropped = (unsigned int)ctx->RxDropped;
        s->rx_failed = (unsigned int)ctx->RxFailed;
        s->tx_count = (unsigned int)ctx->TxCount;
        s->tx_fail = (unsigned int)ctx->TxFail;
        s->last_reader_status = (long)ctx->LastReaderStatus;
        s->last_usbd_status = (unsigned int)ctx->LastUsbdStatus;
        s->last_tx_status = (long)ctx->LastTxStatus;
        s->reader_config_status = (long)ctx->ReaderConfigStatus;
        s->pipe_flags = ctx->PipeFlags;
        s->reset_status = (long)ctx->ResetStatus;
        s->select_status = (long)ctx->SelectStatus;
        s->responder_status = (long)ctx->ResponderStatus;
        s->start_stage = ctx->StartStage;
        s->num_configs = ctx->NumConfigs;
        s->config_desc_status = (long)ctx->ConfigDescStatus;
        s->config_desc_len = ctx->ConfigDescLen;
        RtlCopyMemory(s->config_desc, ctx->ConfigDesc, sizeof(s->config_desc));
        for (int i = 0; i < 6; i++) { s->csr12_17_before[i] = ctx->Csr12_17Before[i]; s->csr12_17_after[i] = ctx->Csr12_17After[i]; }
        info = sizeof(*s);
        status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_NWC_POSTINIT: {
        WDFIOTARGET inTarget = WdfUsbTargetPipeGetIoTarget(ctx->BulkInPipe);
        /* Stop the continuous reader, clear any pipe stall accumulated while the
         * radio was un-initialized, then restart the reader against the live
         * RX engine. Errors here are non-fatal to the kick. */
        WdfIoTargetStop(inTarget, WdfIoTargetCancelSentIo);
        WdfUsbTargetPipeResetSynchronously(ctx->BulkInPipe, NULL, NULL);
        WdfUsbTargetPipeResetSynchronously(ctx->BulkOutPipe, NULL, NULL);
        status = WdfIoTargetStart(inTarget);
        break;
    }
    case IOCTL_NWC_SET_APCONFIG: {
        NWC_APCONFIG *p; size_t inlen; ULONG sl;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*p), (PVOID*)&p, &inlen);
        if (!NT_SUCCESS(status)) break;
        RtlCopyMemory(ctx->Mac, p->mac, 6);
        sl = p->ssid_len; if (sl > 32) sl = 32;
        RtlZeroMemory(ctx->Ssid, sizeof(ctx->Ssid));
        RtlCopyMemory(ctx->Ssid, p->ssid, sl);
        ctx->SsidLen = sl;
        ctx->Channel = p->channel ? p->channel : 1;
        ctx->Privacy = p->privacy ? TRUE : FALSE;
        RtlZeroMemory(ctx->Reg,  sizeof(ctx->Reg));
        RtlZeroMemory(ctx->Auth, sizeof(ctx->Auth));
        InterlockedExchange(&ctx->Configured, 1);   /* responder thread begins beaconing */
        status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_NWC_INJECT_RX: {   /* TEST: push a frame into the RX ring for the responder */
        PVOID buf; size_t len; LONG next;
        status = WdfRequestRetrieveInputBuffer(Request, 1, &buf, &len);
        if (!NT_SUCCESS(status)) break;
        if (len > RX_FRAME_MAX) len = RX_FRAME_MAX;
        WdfSpinLockAcquire(ctx->RxLock);
        next = (ctx->RxHead + 1) % RX_RING_DEPTH;
        if (next != ctx->RxTail) {
            ctx->Rx[ctx->RxHead].len = (ULONG)len;
            RtlCopyMemory(ctx->Rx[ctx->RxHead].buf, buf, len);
            ctx->RxHead = next;
        } else { InterlockedIncrement(&ctx->RxDropped); }
        WdfSpinLockRelease(ctx->RxLock);
        InterlockedIncrement(&ctx->RxComplete);
        if (ctx->ThreadObj) KeSetEvent(&ctx->RxEvent, IO_NO_INCREMENT, FALSE);
        status = STATUS_SUCCESS;
        break;
    }
    default:
        break;
    }
    UNREFERENCED_PARAMETER(OutLen);
    UNREFERENCED_PARAMETER(InLen);
    WdfRequestCompleteWithInformation(Request, status, info);
}
