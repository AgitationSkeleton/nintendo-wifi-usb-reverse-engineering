# rt2500usb reference source — why it's here

Upstream Linux kernel `rt2500usb` + `rt2x00` driver for the RT2570 (same chip as the Nintendo
Wi-Fi USB Connector dongle). Fetched from `torvalds/linux` master,
`drivers/net/wireless/ralink/rt2x00/`. This is the **ground truth** for how the RT2570's hardware
beacon / TSF / auto-responder is actually driven — the thing our probe must match to get the DS
online (the original Windows driver rt25usbap.sys is a Ralink SoftAP driver of the same family).

## The beacon mechanism (the current blocker)

Our probe fakes beaconing in software (bulk-send a beacon every ~100ms). The hardware beacon /
TSF engine therefore never runs (STA_CSR5 frozen), which is why AP-mode TSF_SYNC=3 destabilizes
us and the auto-responder never carries seq2. `rt2500usb_write_beacon` shows the real procedure:

```c
static void rt2500usb_write_beacon(struct queue_entry *entry, struct txentry_desc *txdesc)
{
    int pipe = usb_sndbulkpipe(usb_dev, entry->queue->usb_endpoint); /* BEACON queue endpoint */
    u16 reg, reg0;

    reg = rt2500usb_register_read(rt2x00dev, TXRX_CSR19);
    rt2x00_set_field16(&reg, TXRX_CSR19_BEACON_GEN, 0);              /* disable while loading  */
    rt2500usb_register_write(rt2x00dev, TXRX_CSR19, reg);

    skb_push(entry->skb, TXD_DESC_SIZE);                            /* prepend TX descriptor  */
    memset(entry->skb->data, 0, TXD_DESC_SIZE);
    rt2500usb_write_tx_desc(entry, txdesc);

    length = ...get_tx_data_len(entry);
    usb_fill_bulk_urb(bcn_priv->urb, usb_dev, pipe, entry->skb->data, length,
                      rt2500usb_beacondone, entry);                 /* beacon URB (not submitted)*/
    bcn_priv->guardian_data = 0;
    usb_fill_bulk_urb(bcn_priv->guardian_urb, usb_dev, pipe,
                      &bcn_priv->guardian_data, 1, rt2500usb_beacondone, entry);
    usb_submit_urb(bcn_priv->guardian_urb, GFP_ATOMIC);            /* 1) submit GUARDIAN first */

    rt2x00_set_field16(&reg, TXRX_CSR19_TSF_COUNT, 1);
    rt2x00_set_field16(&reg, TXRX_CSR19_TBCN, 1);
    reg0 = reg;                                                    /* reg0 = ...|TSF_COUNT|TBCN */
    rt2x00_set_field16(&reg, TXRX_CSR19_BEACON_GEN, 1);            /* reg  = reg0 | BEACON_GEN  */
    rt2500usb_register_write(rt2x00dev, TXRX_CSR19, reg);          /* on/off/on/off/on toggle   */
    rt2500usb_register_write(rt2x00dev, TXRX_CSR19, reg0);
    rt2500usb_register_write(rt2x00dev, TXRX_CSR19, reg);
    rt2500usb_register_write(rt2x00dev, TXRX_CSR19, reg0);
    rt2500usb_register_write(rt2x00dev, TXRX_CSR19, reg);
}

static void rt2500usb_beacondone(struct urb *urb) /* completion callback */
{
    if (bcn_priv->guardian_urb == urb)            /* guardian finished ...                    */
        usb_submit_urb(bcn_priv->urb, GFP_ATOMIC); /* 2) ...NOW submit the beacon frame        */
}
```

### What our probe gets wrong
1. We send the beacon on the **data endpoint**; rt2500usb sends it on the **beacon queue's own
   endpoint** (`entry->queue->usb_endpoint`). Enumerate the RT2570 OUT endpoints and find the
   beacon one (see `rt2x00usb.c` endpoint-assignment + `rt2x00queue.h` QID_BEACON).
2. Sequence: **guardian (1 byte) first, then — on its completion — the beacon frame**, both to the
   beacon pipe. (`hw_load_beacon`'s register write to 0x2c00 is wrong; there is no 0x2c00 write.)
3. Then the CSR19 `BEACON_GEN` on/off/on/off/on toggle (ends ON), with `TSF_COUNT|TBCN` set.

### Register facts confirmed from rt2500usb.h
```
TXRX_CSR18  0x0464   INTERVAL=FIELD16(0xfff0)  OFFSET=FIELD16(0x000f)   (rt2500usb writes 0x0640 => INTERVAL=100 TU)
TXRX_CSR19  0x0466   TSF_COUNT=0x0001  TSF_SYNC=0x0006  TBCN=0x0008  BEACON_GEN=0x0010
                     (TSF_SYNC: 1=INFRA 2=ADHOC 3=AP; ours=2, rt2500usb=3)
TXRX_CSR20  0x0468   OFFSET=FIELD16(0x1fff)  BCN_EXPECT_WINDOW=FIELD16(0xe000)
```

Also see `rt2x00usb.c` `rt2x00usb_vendor_request_buff` for the 64-byte-chunk splitting of
`USB_MULTI_WRITE` register writes, and `rt2x00dev.c` for init ordering.
