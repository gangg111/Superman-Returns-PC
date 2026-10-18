import sys, unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools/shaders'))
from vulkan_contract import compile_arguments, build_contract
from fetch_xenosrecomp import patch_fingerprint
class ContractTests(unittest.TestCase):
 def test_contract_has_disjoint_sets_and_fixed_arrays(self):
  c=build_contract(); b=c['bindings']
  self.assertEqual([(x['set'],x['binding'],x['count']) for x in b],[(0,0,1),(0,1,1),(0,2,1),(1,0,32),(1,1,32),(1,2,32),(1,3,32),(2,0,32),(3,0,32)])
 def test_vulkan_arguments_pin_target_and_y_inversion(self):
  vs,ps=compile_arguments('vs'),compile_arguments('ps')
  self.assertIn('-fspv-target-env=vulkan1.1',vs);self.assertIn('-fspv-target-env=vulkan1.1',ps)
  self.assertIn('-fvk-invert-y',vs);self.assertNotIn('-fvk-invert-y',ps)
  with self.assertRaises(ValueError):compile_arguments('cs')
 def test_patch_stamp_changes_with_content(self):
  import tempfile
  with tempfile.TemporaryDirectory() as tmp:
   p=Path(tmp)/'a.patch';p.write_bytes(b'a');a=patch_fingerprint([p]);p.write_bytes(b'b');self.assertNotEqual(a,patch_fingerprint([p]))
 def test_locations_are_unique(self):
  vals=list(build_contract()['vertex_locations'].values());self.assertEqual(len(vals),len(set(vals)));self.assertLess(max(vals),32)
if __name__=='__main__':unittest.main()
