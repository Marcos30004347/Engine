"""Pack the actual native world surface as a linked, lightweight 3D overview."""
import argparse, json
from pathlib import Path
import numpy as np
from export_city_viewer import export, ROOT

def main():
    p=argparse.ArgumentParser();p.add_argument('surface',type=Path);p.add_argument('output',type=Path);p.add_argument('--city-url',default='../viewer-full-world-200/');a=p.parse_args()
    world=json.loads(a.surface.read_text());v=np.asarray(world['vertices']);t=np.asarray(world['triangles']);land=np.asarray(world['land'])
    mesh={'sourcePipeline':'native-district','generation':{'seed':world['seed'],'mode':'world-overview','extentMetres':world['extent']},
          'vertices':world['vertices'],'triangles':world['triangles'],'materials':[0]*len(t),
          'surfaceMaterials':np.where(land[t].sum(axis=1)>=2,11,28).tolist(),
          'triangleUVs':v[t][:,:,[0,2]].tolist(),'groundWeights':np.zeros((len(t),3,3)).tolist(),
          'groundMaterialIds':[11,12,13],'buildings':[], 'textureManifest':'assets/procedural/textures/medieval-pixel/materials.json'}
    a.output.mkdir(parents=True,exist_ok=True);native=a.output/'world-model.json';native.write_text(json.dumps(mesh,separators=(',',':')))
    manifest=export(native,a.output)
    manifest['generation']['cityUrl']=a.city_url
    (a.output/'city.json').write_text(json.dumps(manifest,separators=(',',':')))

if __name__=='__main__':main()
