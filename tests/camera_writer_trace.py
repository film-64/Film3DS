"""Exact IPC forms, bounded buffers and stop register proof, no hidden writes."""
from pathlib import Path
import struct
import sys
import unittest
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
import camera_writer_trace as trace


class TraceTests(unittest.TestCase):
    def record(self,command,normal,buffers):
        words=[(command<<16)|(len(normal)<<6)|(2*len(buffers)),*normal]
        blobs={}
        for index,b in enumerate(buffers):
            pointer=0x8000000+index*4096;blobs[pointer]=b
            words.extend(((len(b)<<14)|(index<<10)|2,pointer))
        words.extend([0]*(64-len(words)))
        captured=trace.static_buffers(words,lambda a,n:blobs[a][:n])
        return dict(words=words,buffers=captured)
    def test_multi_write_preserves_device_register_value(self):
        w,known=trace.writes(self.record(8,[0x32d4,128,1],[b'\2']))
        self.assertTrue(known);self.assertEqual(w,[dict(device=2,addr=0x32d4,value=128)])
    def test_contiguous_writes_cover_overlapping_IFP_channels(self):
        w,known=trace.writes(self.record(12,[2,0x32d2,5],[struct.pack('<5H',77,133,133,133,133)]))
        self.assertTrue(known);self.assertEqual([v['addr'] for v in w],[0x32d2,0x32d4,0x32d6,0x32d8,0x32da])
    def test_read_modify_write_is_not_misreported_as_plain_value(self):
        w,known=trace.writes(self.record(4,[0x32d4,65535,133,1],[b'\2']))
        self.assertTrue(known);self.assertTrue(w[0]['rmw']);self.assertEqual(w[0]['set'],133)
    def test_read_and_unknown_command_are_distinct(self):
        self.assertEqual(trace.writes(self.record(10,[2,0x32d4],[])),([],True))
        self.assertEqual(trace.writes(self.record(6,[2,4],[b'\x32\xd4\x00\x85'])),([],False))
    def test_missing_or_truncated_or_oversized_buffers_reject(self):
        good=self.record(8,[0x32d4,128,1],[b'\2'])
        good['words'][4]=(2049<<14)|2
        with self.assertRaisesRegex(ValueError,'bounds'):trace.static_buffers(good['words'],lambda a,n:b'')
        good=self.record(8,[0x32d4,128,1],[b'\2']);good['buffers']=[]
        with self.assertRaisesRegex(ValueError,'proof'):trace.writes(good)
        good['words'][4]=0
        with self.assertRaisesRegex(ValueError,'descriptor'):trace.static_buffers(good['words'],lambda a,n:b'')
    def test_incomplete_stop_cannot_identify_writer(self):
        with self.assertRaisesRegex(ValueError,'register proof'):trace.fields('T05thread:p17.1;0:09000500;')
        self.assertEqual(trace.word('85000000'),133)
        with self.assertRaises(ValueError):trace.word('85')


if __name__=='__main__':unittest.main()
