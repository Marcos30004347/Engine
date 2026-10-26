"""Load numeric city arrays without expanding millions of Python list objects.

The preview writes the mesh as a side-car binary (`city-model-c10.bin`) with
the offsets recorded under "arrays" in the JSON: eleven million triangles as
text was a 2.3 GB file that cost 143 s to write and as long again to parse.
Older exports that carry the numbers inline are still read, by the text path
below. `crop` selects triangles intersecting an XZ rectangle, preserving their
original vertices.
"""
import json
import mmap
from pathlib import Path

import numpy as np

# What each array is called in the JSON, and what the caller knows it as.
_BINARY_KEYS = {'vertices': 'vertices', 'triangles': 'triangles', 'materials': 'materials',
                'surfaceMaterials': 'surfaceMaterials', 'triangleUVs': 'triangleUVs',
                'weights': 'groundWeights'}

_METADATA = ('quadInstances', 'historicalBindings', 'groundPaint', 'buildings', 'textureManifest', 'groundMaterialIds', 'counts', 'sourcePipeline',
             'generation', 'landforms', 'platforms', 'structure', 'rejections', 'surfaces', 'entranceStructures',
             'roadOwners', 'roadSegments', 'infrastructureOnly', 'blockStrategies',
             'gatewayHouses', 'publicSpaceCount', 'terraces', 'terraceLinks', 'terraceRoutes')


def _load_binary(path, document, crop):
    blob = np.memmap(Path(path).parent / document.get('arrayFile', 'city-model-c10.bin'), dtype=np.uint8, mode='r')
    out = {key: document.get(key) for key in _METADATA}
    arrays = document['arrays']

    def read(name):
        record = arrays.get(name)
        if record is None:
            return None
        piece = blob[record['offset']:record['offset'] + record['bytes']]
        values = piece.view(np.dtype(record['type']))
        shape = tuple(record['shape'])
        return values.reshape((-1,) + shape) if shape else values

    out['vertices'] = read('vertices')
    triangles = read('triangles')
    selected = slice(None)

    if crop is not None:
        selected = np.ones(len(triangles), dtype=bool)
        x0, z0, x1, z1 = crop
        for begin in range(0, len(triangles), 100000):
            p = out['vertices'][triangles[begin:begin + 100000]]
            selected[begin:begin + len(p)] = (p[:, :, 0].max(axis=1) >= x0) & (p[:, :, 0].min(axis=1) <= x1) & \
                                             (p[:, :, 2].max(axis=1) >= z0) & (p[:, :, 2].min(axis=1) <= z1)

    out['triangles'] = triangles[selected]

    for name, key in _BINARY_KEYS.items():
        if name in ('vertices', 'triangles'):
            continue
        values = read(name)
        if values is not None:
            out[key] = values[selected]

    return out


def _load_city_mesh(path, crop=None):
    # A modern export keeps its numbers in the side-car; read the small JSON
    # whole rather than scanning it.
    with Path(path).open('rb') as stream:
        head = stream.read(4096)
    if b'"arrays"' in head or b'"arrayFile"' in head:
        return _load_binary(path, json.loads(Path(path).read_text()), crop)

    return _load_city_mesh_text(path, crop)


def _load_city_mesh_text(path, crop=None):
    with open(path, 'rb') as source, mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as blob:
        def start(key):
            token=('"'+key+'":').encode()
            at=blob.find(token)
            if at<0:
                return None
            return at+len(token)

        def metadata(key, default=None):
            at=start(key)
            if at is None:
                return default
            size=65536
            while True:
                try:
                    return json.JSONDecoder().raw_decode(blob[at:at+size].decode())[0]
                except json.JSONDecodeError:
                    if at+size>=len(blob):
                        raise
                    size*=2

        def numbers(key, shape, dtype):
            at=start(key)
            if at is None:
                return None
            # Numeric arrays contain no quoted keys. The next object member
            # therefore marks their end, even for per-corner nested arrays.
            end=blob.find(b'],"',at)
            if end<0:
                end=blob.rfind(b']}')
            raw=blob[at:end+1].translate(None,b'[]')
            return np.fromstring(raw.decode(),sep=',',dtype=dtype).reshape((-1,)+shape)

        out={key:metadata(key) for key in ('quadInstances','historicalBindings','buildings','textureManifest','groundMaterialIds','counts','sourcePipeline','generation','structure','rejections','terraces','terraceLinks','terraceRoutes')}
        out['vertices']=numbers('vertices',(3,),np.float64)
        # Inline fixture exports store UVs per vertex, unlike city binaries.
        for key,shape,dtype in [('uvs',(2,),np.float32),('uvIslands',(),np.uint32)]:
            values=numbers(key,shape,dtype)
            if values is not None:out[key]=values
        triangles=numbers('triangles',(3,),np.uint32)
        selected=slice(None)
        if crop is not None:
            selected=np.ones(len(triangles),dtype=bool)
            x0,z0,x1,z1=crop
            for begin in range(0,len(triangles),100000):
                p=out['vertices'][triangles[begin:begin+100000]]
                selected[begin:begin+len(p)]=(p[:,:,0].max(axis=1)>=x0)&(p[:,:,0].min(axis=1)<=x1)&(p[:,:,2].max(axis=1)>=z0)&(p[:,:,2].min(axis=1)<=z1)
        out['triangles']=triangles[selected]
        for key,shape,dtype in [('materials',(),np.uint32),('surfaceMaterials',(),np.uint32),
                                ('triangleUVs',(3,2),np.float32),('groundWeights',(3,3),np.float32)]:
            values=numbers(key,shape,dtype)
            if values is not None:
                out[key]=values[selected]
        return out


def load_city_mesh(path, crop=None, preserve_instances=False):
    out=_load_city_mesh(path,crop)
    record=out.pop('quadInstances',None)
    q=np.empty((0,22),dtype=np.float64)
    if record:
        if record['stride']!=88:raise ValueError('Unsupported instance stride')
        q=np.fromfile(Path(path).parent/record['file'],dtype='<f4').reshape(-1,22).astype(np.float64)
        if len(q)!=record['count']:raise ValueError('Incomplete instance buffer')
        q[:,:12]= (q[:,:12].reshape(-1,4,3)+np.asarray(record['origin'])).reshape(-1,12)
        if crop is not None:
            p=q[:,:12].reshape(-1,4,3);x0,z0,x1,z1=crop
            q=q[(p[:,:,0].max(1)>=x0)&(p[:,:,0].min(1)<=x1)&(p[:,:,2].max(1)>=z0)&(p[:,:,2].min(1)<=z1)]
    if preserve_instances:
        out['quadInstances']=q
    elif len(q):
        # Explicit compatibility path for CPU renderers and geometry analysis.
        fan=np.array([0,1,2,0,2,3])
        offset=len(out['vertices'])
        out['vertices']=np.concatenate([out['vertices'],q[:,:12].reshape(-1,3)])
        triangles=(np.arange(len(q))[:,None]*4+fan+offset).reshape(-1,3)
        out['triangles']=np.concatenate([out['triangles'],triangles]).astype(np.uint32)
        out['triangleUVs']=np.concatenate([out['triangleUVs'],q[:,12:20].reshape(-1,4,2)[:,fan].reshape(-1,3,2)])
        for key,col in [('materials',20),('surfaceMaterials',21)]:
            out[key]=np.concatenate([out[key],np.repeat(q[:,col],2)]).astype(np.uint32)
        out['groundWeights']=np.concatenate([out['groundWeights'],np.zeros((len(q)*2,3,3))])
    return out
