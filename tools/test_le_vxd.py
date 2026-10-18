import hashlib
import json
import struct
import tempfile
import unittest
from pathlib import Path
from le_vxd import LE

ROOT = Path(__file__).resolve().parents[1]

class VxdExtractionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mini = LE(ROOT/'modified_driver/NVMINI.VXD')
        cls.core = LE(ROOT/'modified_driver/NVCORE.VXD')

    def test_original_hashes_unchanged(self):
        manifest = json.loads((ROOT/'analysis/manifest.json').read_text())
        for name, entry in manifest.items():
            self.assertEqual(hashlib.sha256((ROOT/name).read_bytes()).hexdigest(), entry['sha256'], name)

    def test_ddb_control_relocations(self):
        self.assertEqual(self.mini.ddb['control'], 0x31690)
        self.assertEqual(self.core.ddb['control'], 0x2df9ff)
        self.assertEqual(self.core.ddb['pm_api'], 0x2dfa10)
        for le in [self.mini, self.core]:
            self.assertEqual(struct.unpack_from('<I',le.objects[0]['raw'],le.ddb['offset']+24)[0],0)

    def test_cross_object_relative_call(self):
        # Independently decoded E8 in NVMINI dispatch must land in ICOD+0x80.
        b = self.mini.objects[0]['image']
        self.assertEqual(b[0x31696],0xe8)
        self.assertEqual(0x3169b+struct.unpack_from('<i',b,0x31697)[0],0x3000080)

    def test_page_mapping_and_relocations(self):
        for le in [self.mini,self.core]:
            for o in le.objects:
                for j,p in enumerate(o['pages']):
                    if p['file_offset'] is not None:
                        off=p['file_offset']; n=p['size']
                        self.assertEqual(o['raw'][j*le.page_size:j*le.page_size+n],le.data[off:off+n])
            seen=set()
            for f in le.fixups:
                key=(f['object'],f['offset'])
                seen.add(key)
                obj=le.objects[f['object']-1]
                value=struct.unpack_from('<I',obj['image'],f['offset'])[0]
                if f['kind']==8:
                    value=(value+obj['base']+f['offset']+4)&0xffffffff
                target=le.objects[f['target_object']-1]
                self.assertEqual(value,target['base']+f['target_offset'])
            self.assertTrue(seen)

    def test_rejects_unsupported_fixup(self):
        data=bytearray(self.mini.data)
        record=self.mini.fixups[0]['record_file_offset']
        data[record+1] |= 1  # External import, deliberately unsupported.
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'invalid.vxd';p.write_bytes(data)
            with self.assertRaises(AssertionError):LE(p)

if __name__=='__main__':unittest.main()
