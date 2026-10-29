#!/usr/bin/env python3
"""Read Ame's bounded gadget and controller snapshots over diagnostic EP0."""
import argparse,json,struct,time
from pathlib import Path
import usb.core,usb.util
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
devices=list(usb.core.find(find_all=True,idVendor=0x1209,idProduct=0x0001))
if len(devices)!=1:raise SystemExit('Expected exactly one Ame USB device')
d=devices[0];report={'ok':False,'endpoints':{}}
try:
    raw=bytes(d.ctrl_transfer(0xc0,0x5a,0,0,64,timeout=2000))
    values=struct.unpack('<16I',raw)
    if values[0]!=0x31475355:raise ValueError('unsupported gadget snapshot')
    keys=['magic','previous_ep0_phase','configuration','speed','cdc_flags',
          'cdc_received','cdc_forwarded','cdc_tx_bytes','controller_commands',
          'controller_events','acm_data_to_function','acm_data_from_function',
          'acm_control_to_function','acm_control_from_function',
          'last_acm_request_and_expected_bytes','last_acm_received_bytes']
    report['gadget']=dict(zip(keys,values))
    raw=bytes(d.ctrl_transfer(0xc0,0x5c,0,0,48,timeout=2000))
    values=struct.unpack('<QQ8I',raw)
    keys=['id','generation','setup_done_out_control','setup_done_out_size',
          'last_rx_status','receive_out_control','receive_out_size',
          'software_flags','data_out_bytes','setup_type_request_length']
    report['control_history']=dict(zip(keys,values))
    flags=report['control_history']['software_flags']
    report['control_history']['trial_mode']=(flags>>10)&3
    report['control_history']['receive_registers_after_trial']=bool(flags&(1<<9))
    for ep in (0,0x80,1,0x82,4,0x85):
        raw=bytes(d.ctrl_transfer(0xc0,0x5b,0,ep,48,timeout=2000))
        values=struct.unpack('<QQ8I',raw)
        keys=['id','generation','control','interrupt','transfer_size','global_interrupt',
              'flags','length','actual','tx_bytes']
        row=dict(zip(keys,values));flags=row['flags']
        row.update(configured=bool(flags&1),active=bool(flags&2),queued=bool(flags&4),
                   terminal=bool(flags&8),hardware_enabled=bool(row['control']&(1<<31)))
        report['endpoints'][f'{ep:#04x}']=row
    report['ok']=True
except Exception as error:report['error']=str(error)
finally:usb.util.dispose_resources(d)
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
raise SystemExit(0 if report['ok'] else 1)
