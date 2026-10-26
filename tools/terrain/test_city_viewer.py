"""The browser bundle must preserve native geometry, seams and source provenance."""
import json
from pathlib import Path
import tempfile
import unittest
import numpy as np
from export_city_viewer import export, VERTEX


class CityViewerExportTests(unittest.TestCase):
    def test_native_corner_attributes_and_region_filter(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)
            source={'sourcePipeline':'world → site → city mesh', 'generation':{'seed':17},
                    'textureManifest':'assets/procedural/textures/medieval-pixel/materials.json',
                    'buildings':[{'id':2**63+1,'kind':'house','base':10,'footprint':[[100,10,100],[104,10,100],[104,10,104],[100,10,104]],'roomList':[{'id':2**63+2,'floor':0,'kind':'corridor','area':16,'boundary':[[100,10,100],[104,10,100],[104,10,104],[100,10,104]]}],'rooms':2,'interiorViews':[[102,11.55,102]]}],
                    'vertices':[[100,10,100],[104,10,100],[104,10,104],[100,10,104],[999,10,999],[1000,10,999],[999,10,1000]],
                    'triangles':[[0,1,2],[0,2,3],[4,5,6]],'materials':[12,13,50], 'surfaceMaterials':[8,1,11],
                    'triangleUVs':[[[0,0],[1,0],[1,1]],[[.25,.25],[1,1],[0,1]],[[0,0],[1,0],[0,1]]],
                    'groundWeights':[[[0,0,0]]*3,[[0,0,0]]*3,[[1,0,0]]*3]}
            model=path/'native.json';model.write_text(json.dumps(source,separators=(',',':')))
            scene=export(model,path/'viewer')
            self.assertEqual(scene['triangles'],2)
            self.assertEqual(scene['buildings'][0]['id'],str(2**63+1))
            self.assertAlmostEqual(scene['buildings'][0]['interiorViews'][0][1],1.55)
            raw=(path/'viewer/city.bin').read_bytes();recovered=[]
            for chunk in scene['chunks']:
                start=chunk['offset']; vertices=np.frombuffer(raw,dtype=VERTEX,count=chunk['vertexBytes']//28,offset=start)
                indices=np.frombuffer(raw,dtype='<u4',count=chunk['indices'],offset=start+chunk['vertexBytes'])
                recovered.extend(vertices[indices].reshape(-1,3))
            for triangle in recovered:
                index=source['surfaceMaterials'].index(int(triangle['material'][0]))
                np.testing.assert_allclose(triangle['position']+scene['origin'],np.array(source['vertices'])[source['triangles'][index]])
                np.testing.assert_allclose(triangle['uv'],source['triangleUVs'][index])
                self.assertTrue(np.all(triangle['material']==source['surfaceMaterials'][index]))
            self.assertEqual(scene['bytes'],len(raw))
            room=scene['buildings'][0]['roomList'][0]
            self.assertEqual(room['id'],str(2**63+2))
            self.assertEqual(room['area'],16)
            np.testing.assert_allclose(np.array(room['boundary'])+scene['origin'],source['buildings'][0]['roomList'][0]['boundary'])
            history=path/'history.json';history.write_text(json.dumps({'settlement':100,'year':60,'week':3120}))
            (path/'settlement.json').write_text(json.dumps({'historyProvenance':{'settlement':200,'year':60,'week':3120}}))
            with self.assertRaisesRegex(ValueError,'does not match'):
                export(model,path/'wrong-history',history)


    def test_rejects_demo_layouts(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory);source=path/'fixture.json'
            source.write_text(json.dumps({'vertices':[[0,0,0]],'triangles':[],'buildings':[]},separators=(',',':')))
            with self.assertRaisesRegex(ValueError,'world'):
                export(source,path/'viewer')


if __name__=='__main__':unittest.main()
