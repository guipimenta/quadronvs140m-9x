#!/usr/bin/env python3
"""Reproduce the static no-chip-match trace. Never executes or patches a driver."""
import hashlib
import json
import struct
import subprocess
from pathlib import Path
from le_vxd import LE

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'analysis/reverse'


def trace():
    path = ROOT / 'modified_driver/NVCORE.VXD'
    manifest = json.loads((ROOT / 'analysis/manifest.json').read_text())
    assert hashlib.sha256(path.read_bytes()).hexdigest() == manifest['modified_driver/NVCORE.VXD']['sha256']
    le = LE(path)
    b = le.objects[0]['image']
    # Fingerprints anchor the object layout and the unchecked call, rather than
    # relying on potentially misaligned linear-sweep disassembly search results.
    anchors = {
        0x18efa7: '8b44240405a4000000c3',  # descriptor = object + a4
        0x140e49: 'c78084000000bc131400',  # selector virtual slot
        0x140e33: 'c7401848111400',        # object lookup dispatcher
        0x1413e8: 'ff90e0000000',          # a4 + descriptor predicate offset 3c
        0x1413ca: '6a1f5b',               # default selected ID
        0x1413f4: 'ff45fc83c610837dfc1f7cd3eb028b1e',
        0x141409: '891833c083fb1f5b0f95c048c9c3',
        0x140e0c: '4183c01083f91f7cec',    # initialized IDs 0..30
        0x141270: '33c05733c9',           # lookup starts with null result
        0x141283: '4183c61083f91f7cef',    # lookup checks 31 entries
        0x14128c: 'eb26',                  # miss skips to return, EAX still zero
        0x18bd12: 'ff978400000083c40c8bf856e806000000598bc75f5ec3',
        0x18bd34: 'ffb7840000006a0650ff501883c40c8bf068010000f056ff9698000000',
        0x18bbed: '85c059740583c8ffeb4d',  # outer error check happens too late
    }
    for address, hexbytes in anchors.items():
        expected = bytes.fromhex(hexbytes)
        assert b[address:address+len(expected)] == expected, hex(address)
    assert 0xa4 + 0x3c == 0xe0
    assert b[0x141197] == 0xe8
    assert 0x14119c + struct.unpack_from('<i', b, 0x141198)[0] == 0x14126b

    ranges = {
        'core_chip_object_layout': (0x18ef48, 0x18efbc),
        'core_chip_slot_initialization': (0x140ddc, 0x140e86),
        'core_chip_selector': (0x1413bc, 0x141417),
        'core_chip_lookup': (0x14126b, 0x1412b7),
        'core_chip_select_and_bind': (0x18bce1, 0x18bdfc),
        'core_chip_start_caller': (0x18bb70, 0x18bc49),
    }
    binary = OUT / 'NVCORE.VXD.1.LCOD.relocated.bin'
    binary.write_bytes(b)
    for name, (start, end) in ranges.items():
        result = subprocess.run([
            'objdump', '-D', '-b', 'binary', '-m', 'i386', '-M', 'intel',
            f'--start-address={start}', f'--stop-address={end}', str(binary)
        ], check=True, capture_output=True, text=True)
        (OUT / (name + '.asm')).write_text(result.stdout)

    points = []
    for offset, purpose in [
        (0x18bd12, 'Chip-selection call: inspect ID argument and output pointer'),
        (0x18bd18, 'Selection returned: inspect EAX and device+0x84'),
        (0x18bd40, 'Type-6 lookup returned: inspect EAX for null'),
        (0x18bd4b, 'First unchecked indirect call through descriptor object'),
        (0x18bbed, 'Outer caller finally checks selection status'),
    ]:
        page = le.objects[0]['pages'][offset // le.page_size]
        points.append({'lcod_offset': hex(offset),
                       'original_file_offset': hex(page['file_offset'] + offset % le.page_size),
                       'purpose': purpose})
    report = {
        'source_sha256': manifest['modified_driver/NVCORE.VXD']['sha256'],
        'analysis': 'Static conditional failure path; not an observed runtime fault',
        'selector': '0x1413bc', 'no_match_id': '0x1f',
        'no_match_status': '0xffffffff', 'valid_slot_ids': '0x00..0x1e',
        'unchecked_bind_call': '0x18bd1e', 'first_null_based_call': '0x18bd4b',
        'assumptions': ['Recovered object methods are active at runtime',
                        'All active chip predicates reject the actual ID read',
                        'Slot identifiers retain their initialized values'],
        'debugger_points': points,
        'address_warning': 'LCOD offsets require the live object base; file offsets are not breakpoint addresses',
    }
    (OUT / 'chip_failure_path.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Verified selector/lookup/binding instruction anchors; exported six excerpts and debugger points.')
    return report


if __name__ == '__main__':
    trace()
