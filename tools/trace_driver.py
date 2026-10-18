#!/usr/bin/env python3
"""Reproduce the first static trace for the supplied 82.69 / 178.74 drivers.
Offsets are specific to their recorded hashes. Does not execute driver code.
"""
import json
import hashlib
import re
import struct
import subprocess
from pathlib import Path
from le_vxd import LE

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT/'analysis/reverse'


def u32(b, p):
    return struct.unpack_from('<I', b, p)[0]


def make_report():
    manifest = json.loads((ROOT/'analysis/manifest.json').read_text())
    for name in ['modified_driver/NVMINI.VXD', 'modified_driver/NVCORE.VXD',
                 'analysis/xp_extracted/nv4_mini.sys']:
        assert hashlib.sha256((ROOT/name).read_bytes()).hexdigest() == manifest[name]['sha256'], name
    mini = LE(ROOT/'modified_driver/NVMINI.VXD')
    core = LE(ROOT/'modified_driver/NVCORE.VXD')
    for driver in [mini, core]:
        driver.export(OUT)
    b = core.objects[0]['image']
    assert b[0x134ccd:0x134cd0] == bytes.fromhex('55 8b ec')
    registrations = []
    for call in range(0x134cd0, 0x134d3e, 5):
        assert b[call] == 0xe8
        constructor = call+5+struct.unpack_from('<i', b, call+1)[0]
        sites = list(re.finditer(rb'\xc7\x40\x3c(.{4})', b[constructor:constructor+0xc0], re.S))
        assert len(sites) == 1
        predicate = int.from_bytes(sites[0].group(1), 'little')
        code = b[predicate:predicate+40]
        # Known pure predicates begin with the second stack argument as BOOT0-like ID.
        assert code[:4] == bytes.fromhex('8b 4c 24 08')
        m = re.search(rb'\x81([\xe1\xe2])\x00\x00\x00\x1f\x81([\xf9\xfa])(.{4})\x75', code, re.S)
        assert m and ((m[1][0] & 7) == (m[2][0] & 7))
        family = int.from_bytes(m[3], 'little')
        registrations.append(dict(call=hex(call), constructor=hex(constructor), predicate=hex(predicate),
                                  required_mask='0x1f000000', required_value=hex(family),
                                  rejects_g86_family=(0x08600000 & 0x1f000000) != family))
    assert len(registrations) == 22
    assert b[0x134d3e:0x134d45] == bytes.fromhex('b8 01 00 00 00 5d c3')
    report = {'scope':'Recovered registration routine, not an exhaustive proof of every possible execution path',
              'registration_routine':'NVCORE LCOD+0x134ccd', 'registrations':registrations,
              'all_22_reject_g86_family':all(x['rejects_g86_family'] for x in registrations),
              'mini_ddb':mini.ddb, 'core_ddb':core.ddb}
    (OUT/'chip_registration.json').write_text(json.dumps(report, indent=2)+'\n')
    excerpts = [
        ('mini_control', 'NVMINI.VXD.1.LCOD', 0, 0x31690, 0x316f4),
        ('mini_dynamic_init', 'NVMINI.VXD.4.ICOD', 0x3000000, 0x3000080, 0x300011c),
        ('mini_load_core', 'NVMINI.VXD.1.LCOD', 0, 0x32029, 0x32059),
        ('mini_api_wrapper', 'NVMINI.VXD.1.LCOD', 0, 0x335b4, 0x335e6),
        ('core_control', 'NVCORE.VXD.1.LCOD', 0, 0x2df9ff, 0x2dfa19),
        ('core_dispatch', 'NVCORE.VXD.1.LCOD', 0, 0x12a1e0, 0x12a23b),
        ('core_dynamic_init', 'NVCORE.VXD.1.LCOD', 0, 0x12a2a9, 0x12a319),
        ('core_registrations', 'NVCORE.VXD.1.LCOD', 0, 0x134ccd, 0x134d45),
        ('core_nv46_registration', 'NVCORE.VXD.1.LCOD', 0, 0x2854d9, 0x28559b),
    ]
    for name, stem, base, start, end in excerpts:
        result = subprocess.run(['objdump','-D','-b','binary','-m','i386','-M','intel',
                                 f'--adjust-vma={base}',f'--start-address={start}',f'--stop-address={end}',
                                 str(OUT/(stem+'.relocated.bin'))],check=True,capture_output=True,text=True)
        (OUT/(name+'.asm')).write_text(result.stdout)
    result = subprocess.run(['objdump','-d','-M','intel','--start-address=0xe0230','--stop-address=0xe0300',
                             str(ROOT/'analysis/xp_extracted/nv4_mini.sys')],check=True,capture_output=True,text=True)
    (OUT/'xp_g86_predicate_and_registration.asm').write_text(result.stdout)
    print('Registered chip families:', sorted(set(x['required_value'] for x in registrations)))
    print('All 22 recovered predicates reject G86 family:',report['all_22_reject_g86_family'])


if __name__ == '__main__':
    make_report()
