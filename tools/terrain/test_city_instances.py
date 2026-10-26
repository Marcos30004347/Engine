"""Compact instance IO preserves UV seams, material ownership and CPU compatibility."""
import json
from pathlib import Path
import tempfile
import unittest
import numpy as np
from load_city_mesh import load_city_mesh


class CityInstanceTests(unittest.TestCase):
    def fixture(self, path):
        q=np.array([[0,0,0, 2,0,0, 2,3,0, 0,3,0, -.5,0, 1.5,0, 1.5,3, -.5,3, 12,9]],dtype='<f4')
        q.tofile(path.parent/'panels.bin')
        data={'vertices':[[0,0,0],[1,0,0],[0,0,1]],'triangles':[[0,1,2]],
              'materials':[50],'surfaceMaterials':[11],'triangleUVs':[[[0,0],[1,0],[0,1]]],
              'groundWeights':[[[1,0,0]]*3],
              'quadInstances':{'file':'panels.bin','count':1,'stride':88,'origin':[100000,2,200000]}}
        path.write_text(json.dumps(data,separators=(',',':')))
        return q

    def test_compact_and_expanded_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'city.json';q=self.fixture(path)
            compact=load_city_mesh(path,preserve_instances=True)
            self.assertEqual(len(compact['triangles']),1)
            np.testing.assert_allclose(compact['quadInstances'][0,:3],[100000,2,200000])
            expanded=load_city_mesh(path)
            self.assertEqual(len(expanded['triangles']),3)
            np.testing.assert_array_equal(expanded['surfaceMaterials'],[11,9,9])
            np.testing.assert_array_equal(expanded['materials'],[50,12,12])
            np.testing.assert_array_equal(expanded['triangleUVs'][1:],q[:,12:20].reshape(4,2)[[0,1,2,0,2,3]].reshape(2,3,2))
            np.testing.assert_array_equal(expanded['groundWeights'][1:],0)

    def test_crop_keeps_intersecting_instances_and_drops_distant_mesh(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'city.json';self.fixture(path)
            clipped=load_city_mesh(path,crop=(100001,199999,100003,200001),preserve_instances=True)
            self.assertEqual(len(clipped['triangles']),0)
            self.assertEqual(len(clipped['quadInstances']),1)
            far=load_city_mesh(path,crop=(-1,-1,2,2),preserve_instances=True)
            self.assertEqual(len(far['quadInstances']),0)
            self.assertEqual(len(far['triangles']),1)

    def test_export_keeps_instances_compact_and_streamable(self):
        from export_city_viewer import export
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'city.json';self.fixture(path)
            source=json.loads(path.read_text());source['quadInstances']['origin']=[0,0,0]
            source.update(sourcePipeline='native-district',buildings=[],
                          textureManifest='assets/procedural/textures/medieval-pixel/materials.json')
            path.write_text(json.dumps(source,separators=(',',':')))
            scene=export(path,path.parent/'viewer')
            self.assertEqual(scene['triangles'],3)
            self.assertEqual(scene['instancedPanels'],1)
            self.assertEqual(scene['instanceBytes'],88)
            offset=0
            raw=(path.parent/'viewer/city.bin').read_bytes()
            for c in scene['chunks']:
                self.assertEqual(c['offset'],offset)
                offset+=c['vertexBytes']+c['indexBytes']+c.get('instanceBytes',0)
                if c.get('instanceCount'):
                    q=np.frombuffer(raw,dtype='<f4',count=22,offset=c['offset'])
                    self.assertEqual(q[21],9)
                    np.testing.assert_allclose(q[:3]+scene['origin'],[0,0,0])
                    self.assertEqual(c['category'],0)
            self.assertEqual(offset,scene['bytes'])

    def test_reject_truncated_instance_buffer(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'city.json';self.fixture(path)
            (path.parent/'panels.bin').write_bytes(b'')
            with self.assertRaisesRegex(ValueError,'Incomplete instance'):
                load_city_mesh(path,preserve_instances=True)

if __name__=='__main__': unittest.main()
