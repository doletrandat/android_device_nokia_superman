#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import unittest
spec=importlib.util.spec_from_file_location('decode',Path(__file__).with_name('decode-metadata.py'))
decode=importlib.util.module_from_spec(spec);spec.loader.exec_module(decode)
ARM='m69-0123456789abcdef'
def record(role,phase,extra,seq=1,pid=12):
 return f'10-11 12:00:00 I/test: superman_meta role={role} phase={phase} arm={ARM} pid={pid} seq={seq} {extra}'
def sf():
 return [record('sf','begin','ns=100 target=5 known=1 dev=3 ino=4 requests=1 truncated=0'),
         record('sf','input','index=0 request_id=6 actual_id=6 frame=7 known=1 dev=3 ino=4'),
         record('sf','matrix','index=0 kind=position values='+','.join(['1']*16)),
         record('sf','matrix','index=0 kind=texture values='+','.join(['1']*16)),
         record('sf','end','ns=120 cache=hit drawn=0 draw_status=0 ready_present=0')]
def mapping(fail=False):
 lines=[record('gralloc','map_begin','ns=101 known=1 dev=3 ino=4'),record('gralloc','map_end',f'ns=111 status={-5 if fail else 0}')]
 if not fail:lines += [record('gralloc','unmap_begin','ns=150'),record('gralloc','unmap_end','ns=160 status=-61')]
 return lines
class Tests(unittest.TestCase):
 def test_paired(self):
  v=decode.analyze(sf()+mapping());self.assertFalse(v['validation_errors']);self.assertEqual(v['summary']['sf_complete_sample'],1);self.assertEqual(v['summary']['map_complete_sample'],1);self.assertEqual(v['shared_sf_gralloc_backing_count'],1);self.assertEqual(v['cache_record_counts'],{'hit':1});self.assertEqual(v['duration_samples_ns']['ordinary_map_ns'],[10])
 def test_no_logs(self):self.assertEqual(decode.analyze(['GPU done'])['summary'],{})
 def test_incomplete(self):
  v=decode.analyze(sf()[:-1]+mapping()[:-1]);self.assertEqual(v['summary']['sf_incomplete_sample'],1);self.assertEqual(v['summary']['map_incomplete_sample'],1)
 def test_failed_map(self):
  v=decode.analyze(mapping(True));self.assertEqual(v['summary']['map_complete_sample'],1);self.assertEqual(v['summary']['map_nonzero_status'],1)
 def test_duplicate(self):self.assertTrue(decode.analyze(sf()+sf())['validation_errors'])
 def test_invalid(self):
  for change in [('seq=1','seq=481'),('arm='+ARM,'arm=button'),('cache=hit','cache=bogus'),('drawn=0','drawn=1'),('requests=1','requests=20'),('index=0','index=16'),('known=1','known=0')]:
   self.assertTrue(decode.analyze([l.replace(*change) for l in sf()])['validation_errors'],change)
 def test_truncation(self):
  v=decode.analyze([sf()[0].replace('requests=1 truncated=0','requests=17 truncated=1')]+sf()[1:]);self.assertEqual(v['summary']['sf_truncated_sample'],1);self.assertEqual(v['summary']['sf_incomplete_sample'],1)
 def test_absent_ready_is_normal(self):self.assertEqual(decode.analyze(sf())['summary']['draw_nonzero_status'],0)
 def test_unknown_unmap(self):self.assertEqual(decode.analyze(mapping())['summary']['map_nonzero_status'],0)
 def test_cross_arm_no_alias(self):
  v=decode.analyze(sf()+[l.replace(ARM,'m69-fedcba9876543210') for l in mapping()]);self.assertEqual(v['shared_sf_gralloc_backing_count'],0)
 def test_clock(self):self.assertTrue(decode.analyze([l.replace('ns=120','ns=90') for l in sf()])['validation_errors'])
if __name__=='__main__':unittest.main()
