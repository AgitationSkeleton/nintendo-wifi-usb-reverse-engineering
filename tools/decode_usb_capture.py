#!/usr/bin/env python3
"""Decode usbmon-format (LinkType 220) pcapng of the RT2570 -> register R/W + 802.11 timeline."""
import struct, os, sys

if len(sys.argv) < 2:
    sys.exit("usage: decode_usb_capture.py <capture.pcapng>  -- decodes a usbmon-format (LinkType 220) USB capture of the RT2570 dongle into a timeline of register reads/writes and 802.11 frames.")
CAPTURE = sys.argv[1]
HERE = os.path.dirname(os.path.abspath(CAPTURE)) or "."
data = open(CAPTURE, "rb").read()

def blocks(buf):
    off = 0
    while off + 12 <= len(buf):
        bt, bl = struct.unpack_from("<II", buf, off)
        if bl < 12 or off + bl > len(buf): break
        yield bt, buf[off:off+bl]; off += bl

REQ = {0x01:"MACwr",0x02:"W",0x03:"R",0x04:"r4",0x06:"mW",0x07:"mR",0x09:"EE",0x0a:"ra",0x10:"r10"}
def reg(idx):
    if 0x0400<=idx<=0x042e: return f"MAC_CSR{(idx-0x0400)//2}"
    if 0x0440<=idx<=0x046a: return f"TXRX_CSR{(idx-0x0440)//2}"
    if 0x0480<=idx<=0x049e: return f"SEC_CSR{(idx-0x0480)//2}"
    if 0x04c0<=idx<=0x04d4: return f"PHY_CSR{(idx-0x04c0)//2}"
    if 0x04e0<=idx<=0x04f4: return f"STA_CSR{(idx-0x04e0)//2}"
    return f"@{idx:#06x}"
def fct(fc):
    t=(fc>>2)&3; st=(fc>>4)&0xf; rt=(fc>>11)&1
    if t==0: n={0:"assoc-req",1:"assoc-rsp",4:"probe-req",5:"probe-rsp",8:"beacon",0xb:"AUTH",0xc:"deauth",0xa:"disassoc"}.get(st,f"m{st}")
    elif t==1: n={0xb:"RTS",0xc:"CTS",0xd:"ACK",9:"BA"}.get(st,f"c{st}")
    else: n=f"data{st}"
    return n+("/R" if rt else "")

ev=[]
for bt, blk in blocks(data):
    if bt != 6:  # EPB only
        continue
    if len(blk) < 28: continue
    caplen = struct.unpack_from("<I", blk, 20)[0]
    tsh, tsl = struct.unpack_from("<II", blk, 12)
    ts = ((tsh<<32)|tsl)
    pkt = blk[28:28+caplen]
    if len(pkt) < 64: continue
    xfer = pkt[9]; epnum = pkt[10]; flag_setup = pkt[14]
    length = struct.unpack_from("<I", pkt, 32)[0]
    body = pkt[64:]
    typ = pkt[8]  # 'S'=0x53 submit 'C'=0x43 complete
    if xfer == 2:  # control
        if flag_setup == 0:  # setup present
            bmR,bReq,wVal,wIdx,wLen = struct.unpack_from("<BBHHH", pkt, 40)
            d = "IN" if bmR&0x80 else "OUT"
            val = ""
            if body and len(body)>=2: val = "="+body[:min(4,len(body))].hex()
            ev.append((ts, chr(typ), f"CTRL {d} {REQ.get(bReq,hex(bReq))} wV={wVal:#06x} wI={wIdx:#06x}({reg(wIdx)}){val}"))
    elif xfer == 3:  # bulk
        if typ==0x43 and (epnum&0x80)==0:  # skip OUT-complete dupes
            pass
        tag = "IN " if epnum&0x80 else "OUT"
        f=""
        if body:
            for p in range(0, min(28, max(1,len(body)-1))):
                fc = struct.unpack_from("<H", body, p)[0] if p+2<=len(body) else 0
                nm=fct(fc)
                if any(k in nm for k in ("AUTH","assoc","ACK","probe","beacon","deauth","CTS","RTS")):
                    a2=body[p+10:p+16].hex(':') if p+16<=len(body) else ""
                    f=f"[{nm}]@+{p} fc={fc:#06x} a2={a2}"; break
            if not f: f="raw="+body[:12].hex()
        ev.append((ts, chr(typ), f"BULK {tag} ep{epnum:#04x} n={len(body)} {f}"))

print(f"events={len(ev)}")
with open(os.path.join(HERE,"timeline.txt"),"w") as fo:
    t0 = ev[0][0] if ev else 0
    for ts,ty,s in ev: fo.write(f"{(ts-t0)/1e6:9.3f} {ty} {s}\n")
print("wrote timeline.txt")
auth=[i for i,(ts,ty,s) in enumerate(ev) if "AUTH" in s]
print(f"AUTH events: {len(auth)}")
t0 = ev[0][0] if ev else 0
for ai in auth[:4]:
    print(f"\n--- window around AUTH #{ai} ---")
    for j in range(max(0,ai-16), min(len(ev),ai+16)):
        ts,ty,s=ev[j]; print(f"  {(ts-t0)/1e6:9.3f} {ty} {s}"+("  <<<" if j==ai else ""))
