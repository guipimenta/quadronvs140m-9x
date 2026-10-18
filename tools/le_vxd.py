#!/usr/bin/env python3
"""Extract LE VxDs and resolve internal 32-bit fixups for static analysis only.

Synthetic bases: object 1 = 0, subsequent objects = index * 0x01000000.
These are NOT runtime addresses. Reject unsupported formats rather than guess.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def unpack(fmt, data, off):
    return struct.unpack_from('<' + fmt, data, off)


class LE:
    def __init__(self, path):
        self.path = Path(path)
        self.data = b = self.path.read_bytes()
        assert b[:2] == b'MZ'
        self.h = unpack('I', b, 0x3c)[0]
        assert b[self.h:self.h+4] == b'LE\0\0'
        u = lambda off: unpack('I', b, self.h+off)[0]
        self.page_size, self.page_count = u(0x28), u(0x14)
        self.objects = []
        self.fixups = []
        page_owners = {}
        for i in range(u(0x44)):
            size, base, flags, first, count, name = unpack('6I', b, self.h+u(0x40)+i*24)
            assert size < 0x01000000
            name = name.to_bytes(4, 'little').rstrip(b'\0').decode('ascii')
            obj = dict(index=i+1, name=name, size=size, flags=flags,
                       base=i*0x01000000, original_base=base, pages=[])
            image = bytearray(size)
            for j in range(count):
                logical = first-1+j
                assert logical not in page_owners
                page_owners[logical] = (i, j*self.page_size)
                p = self.h+u(0x48)+logical*4
                physical, kind = int.from_bytes(b[p:p+3], 'big'), b[p+3]
                off = None
                n = min(self.page_size, size-j*self.page_size)
                assert n > 0
                if kind == 0:
                    assert 1 <= physical <= self.page_count
                    off = u(0x80)+(physical-1)*self.page_size
                    avail = n
                    if physical == self.page_count:
                        # The final physical page may hold fewer bytes on disk
                        # than the object's logical page; the remainder is BSS
                        # zero-fill. NVIDIA's VxDs happen to fill it, but a
                        # Watcom-built VxD does not, so clamp the file read.
                        avail = min(n, (u(0x2c) or self.page_size))
                    assert off+avail <= len(b)
                    image[j*self.page_size:j*self.page_size+avail] = b[off:off+avail]
                else:
                    assert kind == 3, ('unsupported page type', kind)
                obj['pages'].append(dict(logical=logical, file_offset=off, size=n))
            obj['raw'] = bytes(image)
            obj['image'] = image
            self.objects.append(obj)
        for page, (oi, pageoff) in page_owners.items():
            start, end = unpack('II', b, self.h+u(0x68)+page*4)
            p, limit = self.h+u(0x6c)+start, self.h+u(0x6c)+end
            while p < limit:
                record = p
                source, flags = unpack('BB', b, p); p += 2
                assert source & ~0x2f == 0 and (source & 15) in (7, 8), ('source', hex(source), hex(record))
                assert flags & ~0x50 == 0, ('unsupported target flags', hex(flags), hex(record))
                multiple = bool(source & 0x20)
                if multiple:
                    count = b[p]; p += 1
                else:
                    srcoff = unpack('h', b, p)[0]; p += 2
                width = 2 if flags & 0x40 else 1
                target_obj = int.from_bytes(b[p:p+width], 'little'); p += width
                width = 4 if flags & 0x10 else 2
                target_off = int.from_bytes(b[p:p+width], 'little'); p += width
                if multiple:
                    sources = unpack('h'*count, b, p); p += count*2
                else:
                    sources = [srcoff]
                assert p <= limit
                assert 1 <= target_obj <= len(self.objects)
                target = self.objects[target_obj-1]
                assert target_off <= target['size']
                for s in sources:
                    at = pageoff+s
                    obj = self.objects[oi]
                    assert 0 <= at <= obj['size']-4
                    value = target['base']+target_off
                    if source & 15 == 8:
                        value -= obj['base']+at+4
                    value &= 0xffffffff
                    self.fixups.append(dict(object=oi+1, offset=at, target_object=target_obj,
                                            target_offset=target_off, kind=source&15,
                                            record_file_offset=record, value=value))
            assert p == limit
        applied = {}
        for f in self.fixups:
            key = (f['object'], f['offset'])
            assert key not in applied or applied[key] == f['value']
            applied[key] = f['value']
            struct.pack_into('<I', self.objects[f['object']-1]['image'], f['offset'], f['value'])
        e = self.h+u(0x5c)
        assert b[e] >= 1 and b[e+1] == 3
        dobj = unpack('H', b, e+2)[0]
        doff = unpack('I', b, e+5)[0]
        d = self.objects[dobj-1]['image']
        self.ddb = dict(object=dobj, offset=doff, name=bytes(d[doff+12:doff+20]).decode('ascii').strip(),
                        control=unpack('I', d, doff+24)[0], v86_api=unpack('I', d, doff+28)[0],
                        pm_api=unpack('I', d, doff+32)[0])

    def export(self, dest):
        dest = Path(dest); dest.mkdir(parents=True, exist_ok=True)
        objects = []
        for o in self.objects:
            stem = f'{self.path.name}.{o["index"]}.{o["name"]}'
            (dest/(stem+'.raw.bin')).write_bytes(o['raw'])
            (dest/(stem+'.relocated.bin')).write_bytes(o['image'])
            objects.append({k:v for k,v in o.items() if k not in ('image','raw')})
        report = dict(source=str(self.path), sha256=hashlib.sha256(self.data).hexdigest(),
                      page_size=self.page_size, objects=objects, ddb=self.ddb, fixups=self.fixups,
                      warning='Synthetic analysis addresses only; never install these extracted images.')
        (dest/(self.path.name+'.json')).write_text(json.dumps(report, indent=2)+'\n')
        print(self.path.name, 'DDB', self.ddb, 'fixups', len(self.fixups))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('files', nargs='+')
    p.add_argument('--out', default='analysis/reverse')
    a = p.parse_args()
    for f in a.files:
        LE(f).export(a.out)
