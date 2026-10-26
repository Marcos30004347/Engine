#!/usr/bin/env python3
"""Validate the complete PLY tile archive; requires NumPy."""
import argparse
import json
from pathlib import Path
import numpy as np


def validate(root):
    manifest = json.loads((root / 'manifest.json').read_text())
    assert manifest['format'] == 'engine.terrain-tiles' and manifest['version'] == 1
    vertex_dtype = np.dtype([('position', '<f4', (3,)), ('color', 'u1', (3,))])
    face_dtype = np.dtype([('count', 'u1'), ('indices', '<u4', (3,)), ('material', 'u1')])
    ocean_ids = [i for i, m in enumerate(manifest['materials']) if m['role'] == 7 and m['cover'] == 0]
    vertices = triangles = constraints = included = total_bytes = ocean_faces = 0
    ocean_min, ocean_max = np.inf, -np.inf
    for tile in manifest['tiles']:
        path = root / tile['file']
        with path.open('rb') as stream:
            header = []
            while True:
                line = stream.readline()
                assert line, f'{path}: truncated header'
                header.append(line.decode('ascii').strip())
                if line == b'end_header\n':
                    offset = stream.tell()
                    break
        assert 'format binary_little_endian 1.0' in header
        nv, nf = tile['vertices'], tile['triangles']
        assert f'element vertex {nv}' in header and f'element face {nf}' in header
        assert path.stat().st_size == offset + nv * vertex_dtype.itemsize + nf * face_dtype.itemsize
        v = np.memmap(path, mode='r', dtype=vertex_dtype, offset=offset, shape=(nv,))
        f = np.memmap(path, mode='r', dtype=face_dtype, offset=offset + nv * vertex_dtype.itemsize, shape=(nf,))
        assert np.isfinite(v['position']).all(), f'{path}: non-finite vertex'
        assert np.all(f['count'] == 3) and (not nf or np.max(f['indices']) < nv), f'{path}: invalid index'
        assert not nf or np.max(f['material']) < len(manifest['materials']), f'{path}: unknown material'
        ocean = np.isin(f['material'], ocean_ids)
        if np.any(ocean):
            heights = v['position'][f['indices'][ocean], 1]
            ocean_min = min(ocean_min, float(np.min(heights)))
            ocean_max = max(ocean_max, float(np.max(heights)))
            ocean_faces += int(np.count_nonzero(ocean))
        vertices += nv
        triangles += nf
        constraints += tile['constraints']
        included += tile['constraintsIncluded']
        total_bytes += path.stat().st_size
        del v, f
    assert vertices == manifest['vertices'] and triangles == manifest['triangles']
    assert constraints == included
    assert ocean_faces and ocean_min == ocean_max, 'ocean faces are not flat at one datum'
    river_report = {}
    if 'riverMesh' in manifest:
        component = manifest['riverMesh']
        river_vertices = river_triangles = river_bytes = 0
        for tile in component['tiles']:
            path = root / tile['file']
            with path.open('rb') as stream:
                header = []
                while True:
                    line = stream.readline()
                    assert line, f'{path}: truncated river header'
                    header.append(line.decode('ascii').strip())
                    if line == b'end_header\n':
                        offset = stream.tell()
                        break
            nv, nf = tile['vertices'], tile['triangles']
            assert 'format binary_little_endian 1.0' in header
            assert f'element vertex {nv}' in header and f'element face {nf}' in header
            assert path.stat().st_size == offset + nv * vertex_dtype.itemsize + nf * face_dtype.itemsize
            v = np.memmap(path, mode='r', dtype=vertex_dtype, offset=offset, shape=(nv,))
            f = np.memmap(path, mode='r', dtype=face_dtype, offset=offset + nv * vertex_dtype.itemsize, shape=(nf,))
            assert np.isfinite(v['position']).all() and np.all(f['count'] == 3)
            assert not nf or np.max(f['indices']) < nv
            assert np.all(f['material'] == 0), f'{path}: river water has an unexpected material'
            river_vertices += nv
            river_triangles += nf
            river_bytes += path.stat().st_size
            del v, f
        assert river_vertices == component['vertices'] and river_triangles == component['triangles']
        river_report = dict(river_tiles=len(component['tiles']), river_vertices=river_vertices,
                            river_triangles=river_triangles, river_bytes=river_bytes)
    return dict(tiles=len(manifest['tiles']), vertices=vertices, triangles=triangles,
                constraints=constraints, constraints_included=included, bytes=total_bytes,
                ocean_faces=ocean_faces, ocean_min=ocean_min, ocean_max=ocean_max, **river_report)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.directory), indent=2))
