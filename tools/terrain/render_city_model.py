#!/usr/bin/env python3
"""Render city triangles with the same z-buffer rule as dwelling solids.

The input already is blueprint -> skeleton -> mesh output. This preview does
not invent massing or repair geometry; it only projects existing triangles.
"""
import argparse
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from dwelling_materials import Materials
from load_city_mesh import load_city_mesh

PALETTE = {
    12:(214,194,156), 13:(151,116,93), 14:(143,63,50), 15:(181,164,132),
    16:(91,126,145), 17:(105,78,55), 18:(174,166,137),
    31:(119,116,109), 32:(170,165,154), 33:(104,104,100), 34:(112,64,54),
    35:(138,129,117), 36:(112,91,85), 37:(169,142,125), 38:(103,83,79),
    39:(111,48,44), 40:(182,168,144), 41:(145,117,95), 42:(157,75,56),
    43:(180,162,129), 44:(84,121,140), 45:(110,85,66), 46:(173,164,135),
    50:(200,188,164),
    51:(139,139,133), 52:(157,133,101), 53:(68,72,76),
    55:(105,78,55), # Exterior wooden props and stair guards.
    54:(146,141,130), # Lot retaining band: masonry, distinct from the town wall.
}
BACKGROUND = (233,226,212)
LIGHT = (-.42,.82,-.39)

# A city overview requests the exterior groups of each dwelling. Interior wall
# skins and furniture remain in the exported mesh for structural inspection.
# Timber includes exterior frames, shutters and doors as well as roof beams;
# hiding that group opens the deliberately clipped half-timber infill.
OVERVIEW_MATERIALS = {12,14,15,16,17,18,31,32,33,34,35,36,37,38,39,
                      40,42,43,44,45,46,50,51,52,53,54,55}

def project(point,yaw,pitch):
    x,y,z=point
    cy,sy=math.cos(yaw),math.sin(yaw)
    ex,ez=x*cy-z*sy,x*sy+z*cy
    cp,sp=math.cos(pitch),math.sin(pitch)
    return ex,y*sp-ez*cp,ez*sp+y*cp

def normal(points):
    a,b,c=points
    u=(b[0]-a[0],b[1]-a[1],b[2]-a[2])
    v=(c[0]-a[0],c[1]-a[1],c[2]-a[2])
    n=(u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0])
    length=math.sqrt(sum(q*q for q in n)) or 1.
    return tuple(q/length for q in n)

def shade(base,points,unshaded=False):
    if unshaded: return base
    n=normal(points)
    lit=abs(sum(n[i]*LIGHT[i] for i in range(3)))
    factor=.48+.52*lit
    return tuple(min(255,int(channel*factor)) for channel in base)

def raster_triangle(colour,depth,a,b,c,rgb,uv=None,texture=None,light=1.,perspective=False,ground=None):
    height,width=depth.shape
    minx=max(0,int(math.floor(min(a[0],b[0],c[0])))); maxx=min(width-1,int(math.ceil(max(a[0],b[0],c[0]))))
    miny=max(0,int(math.floor(min(a[1],b[1],c[1])))); maxy=min(height-1,int(math.ceil(max(a[1],b[1],c[1]))))
    if minx>maxx or miny>maxy: return
    area=(b[0]-a[0])*(c[1]-a[1])-(c[0]-a[0])*(b[1]-a[1])
    if abs(area)<1e-9: return
    xs=np.arange(minx,maxx+1,dtype=np.float32)[None,:]+.5
    ys=np.arange(miny,maxy+1,dtype=np.float32)[:,None]+.5
    w0=((b[0]-a[0])*(ys-a[1])-(xs-a[0])*(b[1]-a[1]))/area
    w1=((xs-a[0])*(c[1]-a[1])-(c[0]-a[0])*(ys-a[1]))/area
    w2=1.-w0-w1
    inside=(w0>=-1e-4)&(w1>=-1e-4)&(w2>=-1e-4)
    if not inside.any(): return
    z=w2*a[2]+w1*b[2]+w0*c[2]
    window=depth[miny:maxy+1,minx:maxx+1]
    nearer=inside&(z>window)
    if nearer.any():
        window[nearer]=z[nearer]
        target=colour[miny:maxy+1,minx:maxx+1]
        if ground is not None and uv is not None:
            weights=(w2[nearer],w1[nearer],w0[nearer])
            if perspective:
                weights=tuple(w*p[2]/z[nearer] for w,p in zip(weights,(a,b,c)))
            u=sum(w*t[0] for w,t in zip(weights,uv))
            v=sum(w*t[1] for w,t in zip(weights,uv))
            vertex_weights,textures,scales=ground
            blend=sum(w[:,None]*t for w,t in zip(weights,vertex_weights))
            blend=np.maximum(blend,0);blend/=np.maximum(blend.sum(axis=1,keepdims=True),1e-8)
            linear=np.zeros((len(u),3))
            for layer,(tex,scale) in enumerate(zip(textures,scales)):
                h,tw=tex.shape[:2]
                ix=np.floor(np.mod(u/scale[0],1)*tw).astype(np.int64)%tw
                iy=np.floor((1-np.mod(v/scale[1],1))*h).astype(np.int64)%h
                srgb=tex[iy,ix].astype(float)/255
                sample=np.where(srgb<=.04045,srgb/12.92,((srgb+.055)/1.055)**2.4)
                linear+=sample*blend[:,layer,None]
            linear=np.clip(linear*light,0,1)
            srgb=np.where(linear<=.0031308,linear*12.92,1.055*linear**(1/2.4)-.055)
            target[nearer]=np.clip(srgb*255,0,255).astype(np.uint8)
        elif texture is None or uv is None:
            target[nearer]=rgb
        else:
            weights=(w2[nearer],w1[nearer],w0[nearer])
            if perspective:
                weights=tuple(w*p[2]/z[nearer] for w,p in zip(weights,(a,b,c)))
            u=sum(w*t[0] for w,t in zip(weights,uv))
            v=sum(w*t[1] for w,t in zip(weights,uv))
            h,w=texture.shape[:2]
            ix=np.floor(np.mod(u,1)*w).astype(np.int64)%w
            iy=np.floor((1-np.mod(v,1))*h).astype(np.int64)%h
            samples=texture[iy,ix].astype(float)
            target[nearer]=np.clip(samples*light,0,255).astype(np.uint8)

def render(data,path,yaw,pitch,crop=None,title='',allowed=None,width=1800,height=1260,frame_points=None):
    # Cropping changes the camera, never the set of world-space triangles.
    # A wall outside the ground rectangle can still occlude a roof inside it.
    frame=None
    if crop:
        nearby=[b for b in data['buildings'] if any(crop[0]<=p[0]<=crop[2] and crop[1]<=p[2]<=crop[3] for p in b['footprint'])]
        bases=[b.get('base',0.) for b in nearby]
        base=sorted(bases)[len(bases)//2] if bases else 0.
        top=max((b.get('base',base)+b.get('wallHeight',0.)+b.get('roofHeight',0.) for b in nearby),default=base+12.)
        frame=[project((x,y,z),yaw,pitch) for x in (crop[0],crop[2]) for z in (crop[1],crop[3]) for y in (base,top)]
    if frame_points:
        frame=[project(p,yaw,pitch) for p in frame_points]
    library=Materials(data)
    vertices=np.asarray(data['vertices'],dtype=float)
    triangles=np.asarray(data['triangles'])
    materials=np.asarray(data['materials'])
    chosen=np.arange(len(triangles)) if allowed is None else np.flatnonzero(np.isin(materials,list(allowed)))
    if len(chosen)==0:return
    cy,sy,cp,sp=math.cos(yaw),math.sin(yaw),math.cos(pitch),math.sin(pitch)
    ex=vertices[:,0]*cy-vertices[:,2]*sy
    ez=vertices[:,0]*sy+vertices[:,2]*cy
    projected=np.column_stack((ex,vertices[:,1]*sp-ez*cp,ez*sp+vertices[:,1]*cp))
    framing=np.asarray(frame) if frame else projected[np.unique(triangles[chosen])]
    minx,miny=framing[:,:2].min(axis=0);maxx,maxy=framing[:,:2].max(axis=0)
    pad=62
    scale=min((width-2*pad)/(maxx-minx or 1),(height-2*pad)/(maxy-miny or 1))
    offset_x=(width-(maxx-minx)*scale)*.5
    offset_y=(height-(maxy-miny)*scale)*.5
    screen=projected.copy();screen[:,0]=offset_x+(projected[:,0]-minx)*scale;screen[:,1]=height-offset_y-(projected[:,1]-miny)*scale
    colour=np.empty((height,width,3),dtype=np.uint8);colour[:]=BACKGROUND
    depth=np.full((height,width),-np.inf,dtype=np.float32)
    # Stream triangles through the rasterizer. Holding Python tuples for every
    # projected city triangle used several gigabytes before the first pixel.
    for ti in chosen:
        indices=triangles[ti];points=vertices[indices];a,b,c=screen[indices]
        if max(a[0],b[0],c[0])<0 or min(a[0],b[0],c[0])>=width or max(a[1],b[1],c[1])<0 or min(a[1],b[1],c[1])>=height:continue
        material=int(materials[ti])
        base,uv,texture=library.surface(data,ti,PALETTE.get(material,(183,170,145)))
        ground=library.ground(data,ti)
        if ground is not None:uv=np.asarray(data['triangleUVs'][ti],dtype=float)
        light=1.0 if ground is not None else shade((255,255,255),points,material in (18,46,50))[0]/255
        raster_triangle(colour,depth,a,b,c,shade(base,points,material in (18,46,50)),uv,texture,light,ground=ground)
    image=Image.fromarray(colour); draw=ImageDraw.Draw(image)
    draw.rectangle((18,18,width-18,height-18),outline=(77,70,61),width=3)
    draw.text((42,38),title,fill=(47,41,36),stroke_width=1,stroke_fill=BACKGROUND)
    image.save(path)

def main():
    parser=argparse.ArgumentParser(); parser.add_argument('input'); parser.add_argument('output'); args=parser.parse_args()
    # Through the loader: the mesh arrays live in the side-car binary now.
    data=load_city_mesh(Path(args.input)); prefix=Path(args.output); prefix.parent.mkdir(parents=True,exist_ok=True)
    building_points=[p for b in data['buildings'] for p in b['footprint']]
    city_crop=None
    if building_points:
        city_crop=(min(p[0] for p in building_points)-20,min(p[2] for p in building_points)-20,
                   max(p[0] for p in building_points)+20,max(p[2] for p in building_points)+20)
    render(data,prefix.with_name(prefix.name+'-roads-region.png'),-.72,.58,title='CITY AND OUTER ROAD NETWORK',allowed=OVERVIEW_MATERIALS)
    render(data,prefix.with_name(prefix.name+'-oblique.png'),-.72,.58,crop=city_crop,title='CITY MODEL - DWELLING EXTERIOR SOLIDS',allowed=OVERVIEW_MATERIALS)
    render(data,prefix.with_name(prefix.name+'-aerial.png'),-.18,.35,crop=city_crop,title='CITY MODEL - AERIAL / ROAD HIERARCHY',allowed=OVERVIEW_MATERIALS)
    dwellings=[building for building in data['buildings'] if building['kind'] in ('house','workshop','mansion','manor','inn','pub') and building['footprint']]
    if dwellings:
        centres=[]
        for building in dwellings:
            points=building['footprint']
            centres.append((sum(p[0] for p in points)/len(points),sum(p[2] for p in points)/len(points)))
        # Deterministically choose the densest accepted cluster. The crop is a
        # view of the same city mesh, not a separately regenerated gallery.
        cx,cz=max(centres,key=lambda c:sum((q[0]-c[0])**2+(q[1]-c[1])**2<42**2 for q in centres))
        render(data,prefix.with_name(prefix.name+'-neighbourhood.png'),-.72,.50,
               (cx-45,cz-45,cx+45,cz+45),'NEIGHBOURHOOD - VALID BLUEPRINT / SKELETON / MESH',
               OVERVIEW_MATERIALS,1600,1200)
    keeps=[building for building in data['buildings'] if building['kind']=='keep']
    if keeps:
        source=keeps[0]['source']
        points=[point for building in data['buildings'] if building['source']==source for point in building['footprint']]
        xs=[p[0] for p in points]; zs=[p[2] for p in points]
        pad=max(18,(max(xs)-min(xs)+max(zs)-min(zs))*.22)
        render(data,prefix.with_name(prefix.name+'-castle.png'),-.82,.48,
               (min(xs)-pad,min(zs)-pad,max(xs)+pad,max(zs)+pad),'CASTLE - STRUCTURAL DWELLING SOLIDS',
               {36,37,38,39,40,42,43,44,46})
    towers=[b for b in data['buildings'] if b['kind']=='defence-tower']
    if towers:
        tower=towers[0]; points=tower['footprint']
        x=sum(p[0] for p in points)/len(points); z=sum(p[2] for p in points)/len(points)
        radius=max(math.hypot(p[0]-x,p[2]-z) for p in points)+.6
        # An explicitly isolated structural inspection, not camera clipping.
        # Keep whole faces; the cutaway hides shell/roof material groups.
        pairs=[(t,m) for t,m in zip(data['triangles'],data['materials'])
               if m in (40,42,43,44) and all(math.hypot(data['vertices'][i][0]-x,data['vertices'][i][2]-z)<radius for i in t)]
        detail={'vertices':data['vertices'],'triangles':[t for t,m in pairs],
                'materials':[m for t,m in pairs],'buildings':[tower]}
        crop=(x-radius,z-radius,x+radius,z+radius)
        render(detail,prefix.with_name(prefix.name+'-tower.png'),-.7,.62,crop,
               'CITY TOWER - STRUCTURAL MESH',width=900,height=1100)
        render(detail,prefix.with_name(prefix.name+'-tower-cutaway.png'),-.7,.9,crop,
               'CITY TOWER - SHELL AND ROOF HIDDEN / STAIR AND DECK',allowed={43,44},width=900,height=1100)

if __name__=='__main__': main()
