#!/usr/bin/env python3
"""Read the fixed 8-KiB retained kernel-log tail after USB recovers (vendor 0x5f)."""
import argparse,json,struct
from pathlib import Path
import usb.core,usb.util
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
devices=list(usb.core.find(find_all=True,idVendor=0x1209,idProduct=0x0001))
if len(devices)!=1:raise SystemExit('Expected exactly one Ame USB device')
d=devices[0]
try:
    magic,start,length,end=struct.unpack('<4I',bytes(d.ctrl_transfer(0xc0,0x5f,0,0,16,timeout=2000)))
    if magic!=0x31474c42 or length>8192 or start+length!=end:raise RuntimeError('unsupported log-tail metadata')
    data=bytearray()
    for index in range((length+63)//64):
        count=min(64,length-len(data))
        chunk=bytes(d.ctrl_transfer(0xc0,0x5f,1,index,count,timeout=2000))
        if len(chunk)!=count:raise RuntimeError('short log-tail response')
        data.extend(chunk)
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_bytes(data)
    print(json.dumps(dict(start=start,length=length,end=end,output=str(a.output))))
finally:usb.util.dispose_resources(d)
