#!/usr/bin/env python3
"""Centroid-sampled terrain clearance; this is not an exhaustive collision proof."""
import argparse
import json, numpy as np
from pathlib import Path
parser=argparse.ArgumentParser(description='Audit native binary meshes and sample terrain intrusion under realized lots')
parser.add_argument('--input',type=Path,required=True)
args=parser.parse_args()
root=args.input.parent
m=json.loads(args.input.read_text()); binary=root/m['arrayFile']; a={}
for name,s in m['arrays'].items():
 dtype=np.dtype('<'+s['type']); a[name]=np.memmap(binary,dtype=dtype,offset=s['offset'],mode='r',shape=(s['bytes']//dtype.itemsize,))
 assert np.isfinite(a[name]).all(),name
v=a['vertices'].reshape(-1,3); t=a['triangles'].reshape(-1,3)
assert t.max()<len(v)
assert len(a['triangleUVs'])==t.size*2
centres=v[t[np.isin(a['materials'],[50,51,52])]].mean(axis=1)
violations=[]
for b in m['buildings']:
 if b['kind'] not in ['house','workshop','manor']:continue
 ring=np.asarray(b['footprint']); lo=ring.min(axis=0);hi=ring.max(axis=0)
 q=centres[(centres[:,0]>lo[0])&(centres[:,0]<hi[0])&(centres[:,2]>lo[2])&(centres[:,2]<hi[2])]
 if not len(q):continue
 inside=np.zeros(len(q),dtype=bool)
 for i in range(len(ring)):
  p=ring[i];r=ring[i-1]
  if abs(r[2]-p[2])<1e-12:continue
  inside^=((p[2]>q[:,2])!=(r[2]>q[:,2]))&(q[:,0]<(r[0]-p[0])*(q[:,2]-p[2])/(r[2]-p[2])+p[0])
 high=q[inside,1]
 if len(high) and high.max()>b['base']+.02:violations.append({'building':b['id'],'penetration':float(high.max()-b['base'])})
bindings=m['historicalBindings'];resolved=[b['generatedBuilding'] for b in bindings if b.get('generatedBuilding') is not None]
assert len(resolved)==len(set(resolved))
r={'structures':len(m['buildings']),'historicalResolved':len(resolved),'historicalRequests':len(bindings),'finite':True,'indicesValid':True,'groundCentroidPenetrations':violations,'raisedDistricts':len(m.get('platforms',[]))}
(root/'lot-audit.json').write_text(json.dumps(r,indent=2));print(json.dumps(r,indent=2))

if violations: raise SystemExit(1)
