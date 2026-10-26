#!/usr/bin/env python3
"""Run reproducible terrain evidence without presenting source plots as native captures.

Requires Pillow for diagnostic PNGs. Creates a new output directory per run.
Examples:
  python3 tools/terrain/run_rewrite_fixture.py contracts --build build-release --output build/terrain-rewrite/contracts
  python3 tools/terrain/run_rewrite_fixture.py legacy-map --build build-release --output build/terrain-rewrite/legacy
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def canonical_hash(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()).hexdigest()


def git(*args):
    return subprocess.check_output(['git', '-C', str(ROOT), *args])


def provenance():
    # Include untracked source files; git diff alone misses most current terrain work.
    paths = set(git('ls-files', '-z').decode().split('\0'))
    paths.update(git('ls-files', '--others', '--exclude-standard', '-z').decode().split('\0'))
    sources = {}
    roots = ('src/', 'samples/', 'tests/', 'tools/terrain/', 'cmake/', 'assets/shaders/')
    for name in sorted(paths):
        if not name or not (name.startswith(roots) or name == 'CMakeLists.txt'):
            continue
        p = ROOT / name
        if p.is_file():
            sources[name] = digest(p)
    patch = git('diff', '--binary', 'HEAD')
    return dict(commit=git('rev-parse', 'HEAD').decode().strip(),
                dirty_patch_sha256=hashlib.sha256(patch).hexdigest(),
                source_files=sources, source_sha256=canonical_hash(sources))


def execute(command, output, timeout):
    started = time.monotonic()
    with output.open('w') as log:
        try:
            process = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                                     timeout=timeout, check=False)
            code = process.returncode
        except subprocess.TimeoutExpired:
            code = 124
            log.write('\nTIMEOUT: fixture did not finish\n')
    return dict(command=[str(x) for x in command], exit_code=code,
                seconds=time.monotonic()-started)


def diagnostics(report, root):
    from PIL import Image, ImageDraw
    def canvas(title, subtitle):
        im = Image.new('RGB', (1200, 850), '#17232e')
        d = ImageDraw.Draw(im)
        d.text((25, 20), title, fill='white')
        d.text((25, 45), 'CONTRACT SOURCE DIAGNOSTIC / seed 7 / 1x heights / not a native mesh capture', fill='#a9baca')
        d.text((25, 68), subtitle, fill='#a9baca')
        return im, d
    im, d = canvas('01 SHARED BOUNDARIES', 'X/Z metres; labels are owner:index; endpoints shared at the three-region junction')
    for edge in report['boundaries']:
        pts = [(600+v['position'][0]*4, 720-v['position'][2]*5) for v in edge['vertices']]
        d.line(pts, fill='#67dab5', width=4)
        for point, v in zip(pts, edge['vertices']):
            x,y=point
            d.ellipse((x-4,y-4,x+4,y+4), fill='#ffce80')
            d.text((x+8,y+8), f"{v['owner']}:{v['index']}", fill='white')
    d.line((70,790,270,790),fill='white',width=3);d.text((70,805),'50 m (horizontal)',fill='white')
    im.save(root/'01-boundary-ids.png')
    im,d=canvas('07 CONSERVATIVE DISCHARGE', 'Steady fixture: 2 + 3 + 0.5 = 5.5 m3/s; downstream branches split 30/70')
    loc={1:(250,230),2:(900,230),3:(600,450),4:(300,690),5:(900,690)}
    for a,b in [(1,3),(2,3),(3,4),(3,5)]:d.line((*loc[a],*loc[b]),fill='#68bfff',width=5)
    for f in report['flow']:
        x,y=loc[f['id']];d.ellipse((x-12,y-12,x+12,y+12),fill='#ffce80')
        d.text((x+15,y),f"Node {f['id']}: {f['outgoing']:.3f} m3/s",fill='white')
    d.text((25,800),'Graph diagram: node positions are schematic, not geographic coordinates.',fill='white')
    im.save(root/'07-discharge-ledger.png')
    im,d=canvas('09 ROAD CROSS-SECTION', 'Station 50 m; road grade 5%, cross-slope 2%; natural hillside cross-slope 30%')
    section=report['roadSections'][5]['samples']
    def points(key):return [(600+p['lateral']*95,440-(p[key]-12.5)*95) for p in section]
    d.line(points('ground'),fill='#edb77a',width=4);d.line(points('road'),fill='#72d4ff',width=5)
    d.text((30,125),'Orange: natural ground | Blue: road design | X/Y metres at equal scale',fill='white')
    for p in section:
        d.text((600+p['lateral']*95,580),str(p['lateral']),fill='white')
    d.text((520,610),'Lateral offset (m)',fill='white')
    d.line((60,730,155,730),fill='white',width=3);d.text((60,750),'1 m',fill='white')
    d.text((25,800),'Design evaluator only: cut/fill and rendered road geometry are not implemented by this fixture.',fill='white')
    im.save(root/'09-road-cross-sections.png')



def read_ply(path):
    import struct
    with path.open('rb') as stream:
        header=[]
        while True:
            line=stream.readline()
            if not line:raise ValueError('truncated PLY header')
            header.append(line.decode('ascii').strip())
            if line==b'end_header\n':break
            if len(header)>100:raise ValueError('unexpected PLY header')
        if 'format binary_little_endian 1.0' not in header:raise ValueError('unsupported PLY format')
        nv=int(next(x.split()[-1] for x in header if x.startswith('element vertex ')))
        nf=int(next(x.split()[-1] for x in header if x.startswith('element face ')))
        vertex=struct.Struct('<ffffffBBB'+('B' if 'property uchar alpha' in header else ''))
        face=struct.Struct('<BIIIB')
        if nv<=0 or nf<=0:raise ValueError('empty mesh')
        vertices=[]
        for _ in range(nv):
            v=vertex.unpack(stream.read(vertex.size))
            if not all(math.isfinite(x) for x in v[:6]):raise ValueError('nonfinite vertex')
            vertices.append(v[:3])
        triangles=[]; degenerates=0
        for _ in range(nf):
            f=face.unpack(stream.read(face.size))
            if f[0]!=3 or max(f[1:4])>=nv:raise ValueError('invalid triangle index')
            a,b,c=(vertices[i] for i in f[1:4])
            u=[b[i]-a[i] for i in range(3)];v=[c[i]-a[i] for i in range(3)]
            cross=[u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]]
            if sum(x*x for x in cross)==0:degenerates+=1
            triangles.append(f[1:4])
        if stream.read(1):raise ValueError('unexpected trailing PLY bytes')
    return dict(id=0,vertices=vertices,triangles=triangles,constraints=[],degenerate_triangles=degenerates)


def mesh_diagnostics(meshes,root,name,title):
    from PIL import Image,ImageDraw
    all_vertices=[v for mesh in meshes for v in mesh['vertices']]
    lo=[min(v[i] for v in all_vertices) for i in range(3)]
    hi=[max(v[i] for v in all_vertices) for i in range(3)]
    scale=min(1000/max(1e-9,hi[0]-lo[0]),650/max(1e-9,hi[2]-lo[2]))
    im=Image.new('RGB',(1200,850),'#17232e');d=ImageDraw.Draw(im)
    d.text((25,20),title,fill='white')
    d.text((25,45),'ACTUAL CDT MESH / software top-down diagnostic / not a native renderer capture',fill='#a9baca')
    d.text((25,68),'Map X/Z metres; colored patches, wireframe triangles, yellow constrained boundaries',fill='#a9baca')
    colors=['#397c78','#706c9d','#745b43']
    def xy(v):return (100+(v[0]-lo[0])*scale,760-(v[2]-lo[2])*scale)
    for k,mesh in enumerate(meshes):
        for t in mesh['triangles']:
            d.polygon([xy(mesh['vertices'][i]) for i in t],fill=colors[k%len(colors)],outline='#20333c')
        for edge,a,b in mesh['constraints']:
            d.line([xy(mesh['vertices'][a]),xy(mesh['vertices'][b])],fill='#ffce80',width=3)
    bar=10**math.floor(math.log10(150/scale));px=bar*scale
    d.line((30,800,30+px,800),fill='white',width=3);d.text((30,812),f'{bar:g} m',fill='white')
    im.save(root/name)


def land_diagnostics(report, root):
    from PIL import Image, ImageDraw
    for key, filename in [('detailed','02-coast-detail.png'), ('generated','02-generated-land.png'), ('land','02-land-domains.png'), ('archipelago','02-archipelago.png'), ('inlandWater','02-inland-water.png')]:
        im=Image.new('RGB',(1100,1140),'#17232e'); d=ImageDraw.Draw(im)
        d.text((20,20), '02 LAND PRODUCT / CPU topology diagnostic / seed 7 / 2000 m square', fill='white')
        d.text((20,45), 'Physical shared boundaries; bounded detail in coast-detail image; no native rendering', fill='#a9baca')
        for cell in report[key]['cells']:
            pts=[(50+x*.5,1090-z*.5) for x,z in cell['ring']]
            d.polygon(pts,fill='#638c56' if cell['land'] else '#244a62',outline='#36505e')
        for loop in report[key]['coasts']:
            pts=[(50+x*.5,1090-z*.5) for x,z in loop['ring']];d.line(pts+[pts[0]],fill='#ffd074',width=3)
        im.save(root/filename)
    for column, filename, title in [(2,'02-river-slope.png','River slope'),(3,'02-terrain-lambda.png','Terrain lambda')]:
        im=Image.new('RGB',(1100,1100),'#17232e');d=ImageDraw.Draw(im)
        values=[s[column] for s in report['slopes']]; high=max(values)
        for s in report['slopes']:
            x,z=s[:2];v=s[column]/high;px=50+x*.5;pz=1050-z*.5
            d.rectangle((px-12,pz-12,px+12,pz+12),fill=(int(220*v),int(110+100*v),int(150*(1-v))))
        d.rectangle((0,0,1100,70),fill='#17232e');d.text((20,20),f'02 {title} / CPU field samples / max {high:.5f}',fill='white')
        d.text((20,45),'Two compact elongated provinces, lowland corridor; this is a control field, not elevation',fill='#a9baca')
        im.save(root/filename)


def river_diagnostics(report, root):
    from PIL import Image, ImageDraw
    graph=report['graph'];nodes=graph['nodes']; high=max(n['position'][1] for n in nodes) or 1
    for mode, filename in [('order','03-river-orders.png'),('height','03-node-elevations.png')]:
        im=Image.new('RGB',(1100,1140),'#17232e');d=ImageDraw.Draw(im)
        d.text((20,20),f'03 RIVER GRAPH / CPU source diagnostic / {len(nodes)} nodes / {mode}',fill='white')
        d.text((20,45),f"Mouth-first grammar; budget reached: {graph['budgetReached']}; no watershed terrain or native capture",fill='#a9baca')
        for ring in report['coasts']:
            pts=[(50+x*.5,1090-z*.5) for x,z in ring];d.polygon(pts,fill='#3b5348',outline='#ffd074')
        for n in nodes:
            x,y,z=n['position'];p=(50+x*.5,1090-z*.5)
            if n['receiver']<len(nodes):
                receiver=nodes[n['receiver']]['position'];q=(50+receiver[0]*.5,1090-receiver[2]*.5)
                color='#73caff' if mode=='order' else (int(255*y/high),int(230-130*y/high),90)
                d.line((p,q),fill=color,width=max(1,n['order']*2) if mode=='order' else 3)
                # Arrow points downstream toward the receiver.
                mx=(p[0]+q[0])*.5;my=(p[1]+q[1])*.5;dx=q[0]-p[0];dy=q[1]-p[1];length=math.hypot(dx,dy)
                if length:d.line(((mx-dx/length*5-dy/length*3,my-dy/length*5+dx/length*3),(mx,my),(mx-dx/length*5+dy/length*3,my-dy/length*5-dx/length*3)),fill='white',width=1)
            else:d.ellipse((p[0]-6,p[1]-6,p[0]+6,p[1]+6),fill='#ffd074')
        im.save(root/filename)
    im=Image.new('RGB',(900,360),'#17232e');d=ImageDraw.Draw(im)
    lines=['03 REJECTION REPORT / CPU algorithm counters',f"Productions [continue, symmetric, asymmetric, fill]: {graph['productions']}",f"Rejections [coast, spacing, Lipschitz]: {graph['rejections']}",f"Attempts: {graph['attempts']}; exhausted child slots: {graph['exhaustedChildren']}",f"Node budget reached: {graph['budgetReached']}",'Finite termination does not establish complete catchment coverage.']
    for i,line in enumerate(lines):d.text((25,25+i*45),line,fill='white')
    im.save(root/'03-rejection-reasons.png')


def watershed_diagnostics(report, root):
    from PIL import Image, ImageDraw
    import colorsys
    w=report['watersheds'];im=Image.new('RGB',(1100,1140),'#17232e');d=ImageDraw.Draw(im)
    d.text((20,20),'04 WATERSHEDS / CPU polygon diagnostic / 1000 metre analytical fixture',fill='white')
    d.text((20,45),'Gold: crests; cyan: river crossing edges; circles: crest/coast endpoints',fill='#a9baca')
    for cell in w['cells']:
        color=tuple(int(255*c) for c in colorsys.hsv_to_rgb((cell['node']*.381966)%1,.3,.48))
        for ring in cell['fragments']:d.polygon([(50+x,1090-z) for x,z in ring],fill=color)
    for a,b,left,right,crossing in w['edges']:
        pa=w['vertices'][a]['position'];pb=w['vertices'][b]['position']
        d.line(((50+pa[0],1090-pa[2]),(50+pb[0],1090-pb[2])),fill='#73caff' if crossing else '#ffd074',width=4)
    for vertex in w['vertices']:
        x,h,z=vertex['position'];px=50+x;pz=1090-z
        d.ellipse((px-4,pz-4,px+4,pz+4),fill='white');d.text((min(1000,px+8),max(80,pz-16)),f'{h:.2f} m',fill='white')
    im.save(root/'04-watersheds-crests.png')
    for key, extent, filename in [('samples',1000,'04-height-slope.png'),('generatedSamples',2000,'04-generated-height.png')]:
        samples=report[key];high=max((s[2] for s in samples),default=1) or 1
        im=Image.new('RGB',(1100,1140),'#17232e');d=ImageDraw.Draw(im)
        scale=1000/extent;half=10 if extent==1000 else 12.5
        for x,z,h in samples:
            color=tuple(int(255*c) for c in colorsys.hsv_to_rgb(.55*(1-max(0,min(1,h/high))),.65,.85))
            px=50+x*scale;pz=1090-z*scale;d.rectangle((px-half,pz-half,px+half,pz+half),fill=color)
        d.rectangle((0,0,1100,75),fill='#17232e');d.text((20,20),f'04 CONTINUOUS RELIEF / CPU samples / {extent} m / height 0 to {high:.2f} m',fill='white')
        d.text((20,45),'No channel carving yet; generated graph is diagnostic and truncated' if key=='generatedSamples' else 'Compact primitive blend with shoreline transition; no channel carving yet',fill='#a9baca')
        im.save(root/filename)
    mesh=report['mesh'];vertices=mesh['vertices'];im=Image.new('RGB',(1200,900),'#17232e');d=ImageDraw.Draw(im)
    def project(v):
        x,y,z=v;return (600+(x-z)*.5,250+(x+z)*.25-y*5)
    triangles=sorted(mesh['triangles'],key=lambda t:sum(vertices[i][0]+vertices[i][2] for i in t))
    high=max(v[1] for v in vertices) or 1
    for t in triangles:
        h=sum(vertices[i][1] for i in t)/3;u=max(0,min(1,h/high))
        d.polygon([project(vertices[i]) for i in t],fill=(int(70+125*u),int(95+65*u),int(80+30*u)),outline='#4f655d')
    d.text((20,20),'04 ACTUAL CDT RELIEF PATCH / CPU oblique mesh projection',fill='white')
    d.text((20,45),'Vertical exaggeration: 5x; fixed coast constraints; native renderer NOT-RUN',fill='#a9baca')
    im.save(root/'04-range-oblique.png')
    row=[s for s in report['samples'] if s[1]==500];im=Image.new('RGB',(1100,400),'#17232e');d=ImageDraw.Draw(im)
    points=[(50+s[0],340-s[2]*5) for s in row];d.line(points,fill='#ffd074',width=3)
    d.text((20,20),'04 TERRAIN SECTION / z=500 m / x=0..1000 m / vertical scale 5 pixels per metre',fill='white')
    d.text((20,45),'Blend diagnostic, not a carved channel profile',fill='#a9baca');im.save(root/'04-valley-section.png')


def relief_diagnostics(report, root):
    from PIL import Image, ImageDraw
    im=Image.new('RGB',(1100,1150),'#17232e');d=ImageDraw.Draw(im)
    d.text((20,20),'04 MULTI-RANGE WATERSHEDS / source graph / seed 7 / 2000 m square',fill='white')
    d.text((20,45),'Gold: crest edges; cyan: river crossings. Prominence computed independently on sampled terrain.',fill='#a9baca')
    for a,b,crossing in report['crestEdges']:
        pa=report['crestPositions'][a];pb=report['crestPositions'][b]
        d.line(((50+pa[0]*.5,1100-pa[2]*.5),(50+pb[0]*.5,1100-pb[2]*.5)),fill='#73caff' if crossing else '#ffd074',width=2)
    im.save(root/'04-watersheds-crests.png')
    im=Image.new('RGB',(1200,500),'#17232e');d=ImageDraw.Draw(im)
    d.text((20,20),'04 RANGE SECTION / x=600 m, z=0..2000 m / seed 7 / height in metres',fill='white')
    d.text((20,45),'Physical terrain samples; two summits separated by a pass. Channel profiles belong to Step 07.',fill='#a9baca')
    pts=[(50+z*.55,420-h*5) for z,h in report['section']];d.line(pts,fill='#ffd074',width=3)
    for height in [0,20,40,60]:d.text((5,420-height*5),str(height),fill='white')
    for z in [0,500,1000,1500,2000]:d.text((50+z*.55,450),str(z),fill='white')
    im.save(root/'04-valley-section.png')


def spline_diagnostics(report, root):
    from PIL import Image, ImageDraw
    colors=['#72d8b0','#f3ce7c','#df9eeb']
    im=Image.new('RGB',(1100,1150),'#17232e');d=ImageDraw.Draw(im)
    d.text((25,20),'05 SPLINE CONTROLS / nested plateau / 1000 m square / source diagnostic',fill='white')
    d.text((25,45),'Dots: authored controls. Lines: adaptive centripetal Catmull-Rom. Inner rim emits vertical faces.',fill='white')
    for controls,line,color in zip(report['controls'],report['polylines'],colors):
        d.line([(50+x,1100-z) for x,z in line],fill=color,width=3)
        for x,z in controls:d.ellipse((46+x,1096-z,54+x,1104-z),fill='white')
    d.line((50,1120,250,1120),fill='white',width=3);d.text((50,1130),'200 metres',fill='white')
    im.save(root/'05-spline-controls.png')
    im=Image.new('RGB',(1200,800),'#17232e');d=ImageDraw.Draw(im)
    d.text((25,20),'05 CLIFF CROSS SECTIONS / metres / source samples (axes independently scaled)',fill='white')
    d.text((25,45),'Nested cap heights 90 / 150 / 195 m. Gold jump: actual 29.25 m vertical free face.',fill='#f3ce7c')
    palette=['#728092','#72d8b0','#ffffbc','#f9af54','#cd8b64','#bfb19c']
    for a,b in zip(report['section'],report['section'][1:]):d.line((50+a[0]*1.1,370-a[1]*1.4,50+b[0]*1.1,370-b[1]*1.4),fill=palette[b[2]],width=3)
    for y in [0,90,150,195]:d.text((5,365-y*1.4),str(y),fill='white')
    d.text((30,405),'Open cliff: along-strike height at the brink; both ends taper to the original ground.',fill='white')
    d.line([(50+x*1.1,620-h*1.6) for x,h in report['openAlong']],fill='#72d8b0',width=3)
    d.text((30,650),'Open cross-strike profile: bounded high side, free face, weathered face, apron.',fill='white')
    d.line([(50+z*1.1,785-h) for z,h in report['openAcross']],fill='#f3ce7c',width=3)
    im.save(root/'05-cliff-cross-sections.png')


def physical_diagnostics(report, root):
    from PIL import Image, ImageDraw
    im=Image.new('RGB',(1200,550),'#17232e');d=ImageDraw.Draw(im)
    d.text((25,20),'06 RAMP GRADE / source section / horizontal metres / heights in metres',fill='white')
    d.text((25,45),'Green: final tread. Gold: pre-ramp terrain. 20 m rise / 200 m run = 10%; cross slope = 0%.',fill='white')
    for col,color in [(1,'#f3ce7c'),(2,'#72d8b0')]:d.line([(50+x*1.8,430-h*15) for x,*heights in report['rampSection'] for h in [heights[col-1]]],fill=color,width=3)
    for y in [0,10,20]:d.text((15,430-y*15),str(y),fill='white')
    for x in [0,150,350,600]:d.text((50+x*1.8,460),str(x),fill='white')
    im.save(root/'06-ramp-grade.png')
    im=Image.new('RGB',(1200,550),'#17232e');d=ImageDraw.Draw(im)
    d.text((25,20),'06 LANDFORM BEFORE / AFTER / cross section at x=400 m / source samples',fill='white')
    d.text((25,45),'Gold: plateau. Green: resolved canyon cut. Bank transition is geometric, not a color stripe.',fill='white')
    for col,color in [(1,'#f3ce7c'),(2,'#72d8b0')]:d.line([(50+(z-180)*5.5,430-v*15) for row in report['across'] for z,v in [(row[0],row[col])]],fill=color,width=3)
    for y in [-2,0,10,20]:d.text((10,430-y*15),str(y),fill='white')
    for z in [180,230,280,330,380]:d.text((50+(z-180)*5.5,495),str(z),fill='white')
    im.save(root/'06-landform-before-after.png')

def channel_diagnostics(report, root):
    from PIL import Image, ImageDraw, ImageFont
    im=Image.new('RGB',(1200,530),'#17232e');d=ImageDraw.Draw(im)
    try:font=ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial.ttf',20)
    except OSError:font=ImageFont.load_default()
    d.text((25,20),'07 DISCHARGE LEDGER / incremental runoff only / m3/s',fill='white',font=font)
    for x,label in zip([30,155,340,510,690,830],['Node','Area (m2)','Local Q','Incoming','Loss','Outgoing']):d.text((x,60),label,fill='#a9baca',font=font)
    for k,row in enumerate(report['ledger']):
        fields=[row['id'],int(row['area']),f"{row['local']:.2f}",f"{row['incoming']:.2f}",f"{row['loss']:.2f}",f"{row['outgoing']:.2f}"]
        for x,value in zip([30,155,340,510,690,830],fields):d.text((x,115+k*65),str(value),fill='white',font=font)
    d.text((25,425),'PASS  2 + 3 + 0.5 = 5.5 | With 0.25 loss: 5.25 | 30/70 split conserves total',fill='#72d8b0',font=font)
    im.save(root/'07-discharge-ledger.png')
    im=Image.new('RGB',(1200,850),'#17232e');d=ImageDraw.Draw(im)
    d.text((25,20),'07 CONFLUENCE SECTIONS / metres / nominal mid-reach sections',fill='white',font=font)
    for k,row in enumerate(report['sections']):
        y=160+k*240;b=row['bottomWidth']/2;w=row['width']/2;depth=row['depth'];cx=400;scale=35
        points=[(cx-w*scale,y-depth*scale),(cx-b*scale,y),(cx+b*scale,y),(cx+w*scale,y-depth*scale)]
        d.line(points,fill='#d9ad79',width=4);d.line((cx-w*scale,y-depth*scale,cx+w*scale,y-depth*scale),fill='#56badd',width=4)
        text=f"{row['from']} -> {row['to']} | Q {row['Q']:.2f} m3/s | velocity {row['velocity']:.3f} m/s"
        d.text((25,y-95),text,fill='white',font=font)
        d.text((660,y-35),f"width {row['width']:.3f} m / depth {depth:.3f} m",fill='white',font=font)
        d.text((660,y),f"water {row['water']:.3f} m / bed {row['bed']:.3f} m",fill='white',font=font)
    im.save(root/'07-confluence-sections.png')


def routing_diagnostics(report, root):
    from PIL import Image, ImageDraw, ImageFont
    try:font=ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial.ttf',20)
    except OSError:font=ImageFont.load_default()
    im=Image.new('RGB',(1200,1000),'#17232e');d=ImageDraw.Draw(im)
    d.text((25,20),'03b / VALLEY ROUTING / source fields and accepted centerlines',fill='white',font=font)
    d.text((25,50),'Terrain preference + heading cost; fixed junctions and outlet; metres',fill='#a9baca',font=font)
    def xy(x,z):return (420+x*2.6,480-z*2.6)
    for j,row in enumerate(report['terrainHeights']):
        for i,h in enumerate(row):
            t=min(1,max(0,h/30));color=(int(56+95*t),int(99+29*t),int(83-16*t))
            x,z=-100+i*2,-160+j*2;px,py=xy(x,z);d.rectangle((px,py-5.2,px+5.2,py),fill=color)
    for path in report['paths']:
        if not path:continue
        d.line([xy(p[0],p[2]) for p in [path[0],path[-1]]],fill='#f4ce87',width=1)
        d.line([xy(p[0],p[2]) for p in path],fill='#5dcff0',width=4)
        for p in (path[0],path[-1]):
            x,y=xy(p[0],p[2]);d.ellipse((x-4,y-4,x+4,y+4),fill='white')
    d.text((740,100),'Cyan: routed centerline',fill='#5dcff0',font=font)
    d.text((740,130),'Gold: original straight reach',fill='#f4ce87',font=font)
    for k,row in enumerate(report['routes']):
        y=205+k*145
        for j,text in enumerate([f"Reach {row['upstream']}: {row['length']:.2f} m",f"Sinuosity {row['sinuosity']:.4f}",f"Max curvature {row['curvature']:.4f} /m",f"Max incision {row['incision']:.2f} m"]):d.text((740,y+j*28),text,fill='white',font=font)
    d.line((160,940,290,940),fill='white',width=3);d.text((160,950),'50 m',fill='white',font=font)
    im.save(root/'03b-valley-route-cost.png')
    im=Image.new('RGB',(1200,850),'#17232e');d=ImageDraw.Draw(im)
    d.text((25,20),'03b / CONFINEMENT AND RESISTANCE / source diagnostics',fill='white',font=font)
    for column,key,title in [(0,'confinedPaths','Steep confined stream: no forced meanders'),(1,'detourPaths','Resistant core: feasible detour')]:
        cx=300+column*600
        d.text((25+column*600,70),title,fill='white',font=font)
        if column:d.ellipse((cx-30,415,cx+30,505),fill='#a57c58')
        for path in report[key]:
            if path:d.line([(cx+p[0]*4,760-p[2]*3) for p in path],fill='#5dcff0',width=4)
        d.line((cx-80,800,cx,800),fill='white',width=3);d.text((cx-80,810),'20 m horizontal',fill='white',font=font)
    im.save(root/'03b-confined-and-resistant.png')
    im=Image.new('RGB',(1200,600),'#17232e');d=ImageDraw.Draw(im)
    d.text((25,25),'03b / MERGE CONSERVATION / normal reach cross-sections',fill='white',font=font)
    columns=[25,245,435,620,805,1000]
    for x,label in zip(columns,['Reach','Q (m3/s)','Width (m)','Depth (m)','Area (m2)','Speed (m/s)']):d.text((x,110),label,fill='#a9baca',font=font)
    for k,row in enumerate(sorted(report['hydraulics'],key=lambda r:-r['upstream'])):
        values=[f"{'Merged' if row['upstream']==1 else 'Tributary'} {row['upstream']}"]+[f"{row[key]:.3f}" for key in ['discharge','width','depth','area','velocity']]
        for x,value in zip(columns,values):d.text((x,180+k*70),value,fill='white',font=font)
    d.text((25,435),'2.0 + 3.0 + 0.5 local runoff = 5.5 m3/s downstream',fill='#72d8b0',font=font)
    d.text((25,475),'Q = wetted area x mean velocity; larger merged width and flow area: PASS',fill='#72d8b0',font=font)
    d.text((25,535),'Source diagnostic. Rendered terrain screenshots use the actual native Metal CDT mesh.',fill='#a9baca',font=font)
    im.save(root/'03b-merge-discharge.png')



def suitability_diagnostics(report, root):
    from PIL import Image, ImageDraw, ImageFont
    try:font=ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial.ttf',19)
    except OSError:font=ImageFont.load_default()
    legend=[(1,'Water','#347aab'),(2,'Flood / bank setback','#526e8b'),(4,'Cliff face','#9d5353'),(8,'Talus / apron','#b28659'),(16,'Brink setback','#d29b70'),(32,'Steep ground','#774e54'),(64,'Local relief','#775c73'),(128,'Inaccessible','#515260'),(256,'Ramp corridor','#929398'),(512,'Poor soil','#735f4a'),(1024,'Saturated','#506975')]
    scale=2.5;step=report['spacing']*scale
    def xy(p):return (30+p[0]*scale,720-p[2]*scale)
    for reservation in (False,True):
        im=Image.new('RGB',(1200,850),'#17232e');d=ImageDraw.Draw(im)
        d.text((25,20),'08 / '+('SETTLEMENT RESERVATIONS' if reservation else 'FINAL SUITABILITY AND EXCLUSIONS'),fill='white',font=font)
        d.text((25,50),'Source product diagnostic / 4 m cells / final terrain + curved water / not a mesh render',fill='#a9baca',font=font)
        for c in report['cells']:
            color=next((color for bit,label,color in legend if c['reasons']&bit),None)
            if color is None:color=(int(45+c['score']*40),int(85+c['score']*100),75)
            x,y=xy(c['position']);d.rectangle((x-step/2,y-step/2,x+step/2,y+step/2),fill=color)
        for k,(bit,label,color) in enumerate(legend):
            y=115+k*31;d.rectangle((820,y,837,y+17),fill=color);d.text((847,y),label,fill='white',font=font)
        d.text((820,475),'Green: suitable (brighter = higher)',fill='#9ad991',font=font)
        if reservation:
            for r in report['settlements']:
                color='#ffd477' if r['town'] else '#82e4d3'
                for id in r['expansion']:
                    x,y=xy(report['cells'][id]['position']);d.rectangle((x-step/2,y-step/2,x+step/2,y+step/2),outline=color,width=1)
                for id in r['core']:
                    x,y=xy(report['cells'][id]['position']);d.rectangle((x-step/2,y-step/2,x+step/2,y+step/2),fill=color)
                x,y=xy(r['center']);d.text((x+8,y-24),f"{'T' if r['town'] else 'V'}{r['id']}",fill='white',font=font)
            y=550
            for r in report['settlements']:
                d.text((820,y),f"{'Town' if r['town'] else 'Village'} {r['id']}: {r['area']:.0f} m2",fill='white',font=font)
                if not r['town']:d.text((820,y+25),f"Parent town: {r['parent']}",fill='#a9baca',font=font)
                y+=65
        else:
            d.text((820,550),'All hazard bits retained in JSON.',fill='white',font=font)
            d.text((820,582),'Map shows the first matching reason.',fill='#a9baca',font=font)
            for p,label in [([90,0,62],'Flat center / enclosed'),([215,0,160],'Cap / access via ramp')]:
                x,y=xy(p);d.ellipse((x-4,y-4,x+4,y+4),fill='white');d.text((x-70,y-25),label,fill='white',font=font)
        d.line((30,780,155,780),fill='white',width=3);d.text((30,793),'50 m',fill='white',font=font)
        if reservation:d.text((250,790),'Filled: core / outlined: expansion / reservation cells are not final visual boundaries',fill='#a9baca',font=font)
        im.save(root/('08-settlement-reservations.png' if reservation else '08-suitability.png'))
    im=Image.new('RGB',(1200,750),'#17232e');d=ImageDraw.Draw(im)
    d.text((25,20),'08 / PLATEAU ACCESS / same site before and after ramp',fill='white',font=font)
    for side in (0,1):
        x0=30+side*600;d.text((x0,65),'No ramp: cap unavailable' if side==0 else 'Validated ramp: cap connected',fill='white',font=font)
        for id,c in enumerate(report['cells']):
            x,y,z=c['position']
            if not(100<=x<=270 and 100<=z<=215):continue
            reasons=report['blockedReasons'][id] if side==0 else c['reasons'];color=next((color for bit,label,color in legend if reasons&bit),'#78ae67')
            px=x0+(x-100)*3;py=500-(z-100)*3;d.rectangle((px-6,py-6,px+6,py+6),fill=color)
        d.line((x0,565,x0+150,565),fill='white',width=3);d.text((x0,578),'50 m',fill='white',font=font)
    d.text((25,650),'The ramp replaces the cliff surface for travel. Its corridor remains excluded from settlement cores.',fill='white',font=font)
    im.save(root/'08-plateau-access.png')


def native_capture(build, product, root, timeout, manifest, assertions, scene=""):
    exe=build/'tools/TerrainRewriteNativeCapture'
    if not exe.is_file():raise RuntimeError(f'native capture executable missing: {exe}')
    from PIL import Image, ImageDraw, ImageFont
    manifest['native_renderer']={'executable':str(exe),'sha256':digest(exe),'implementation':'tools/terrain/render_rewrite_metal.swift'}
    directory=root/('native'+scene);directory.mkdir()
    try:font=ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial.ttf',20)
    except OSError:font=ImageFont.load_default()
    report=json.loads(product.read_text());cameras=[]
    splines='polylines' in report
    phase=report.get('phase',5 if splines else 4)
    open_scene=bool(scene)
    for view in ('top','oblique'):
        outputs=[]
        for repeat in range(2):
            output=directory/f'{view}-{repeat}.png';outputs.append(output)
            run=execute([str(exe),str(product),str(output),view],directory/f'{view}-{repeat}.log',timeout)
            if run['exit_code']!=0:raise RuntimeError(f'native {view} capture failed: {run}')
            metadata=json.loads(Path(str(output)+'.json').read_text())
            if metadata['status']!='PASS' or metadata['verticalScale']!=1:raise RuntimeError('invalid native capture metadata')
        identical=digest(outputs[0])==digest(outputs[1])
        assertions.append({'name':f'native{scene}_{view}_repeat_pixels','pass':identical})
        if not identical:raise RuntimeError(f'native {view} repeat pixels differ')
        cameras.append(metadata)
        im=Image.open(outputs[0]).convert('RGB');d=ImageDraw.Draw(im)
        title={3:'confined mountain stream' if open_scene else 'curved valley channels',4:'multi-range',5:'open cliff' if open_scene else 'nested plateau',6:'ramps and protected canyon',7:'high flow' if open_scene else 'low flow'}[phase]
        d.text((25,18),f"STEP {'03b' if phase==3 else str(phase).zfill(2)} / {title} / NATIVE METAL / {view} / vertical scale 1x",fill='white',font=font)
        d.text((25,45),f"{len(report['mesh']['triangles'])} actual triangles | Fixed exposure 1 | "+('Blue: actual water mesh' if phase==7 else 'Physical terrain geometry'),fill='white',font=font)
        if view=='top' and phase==4:
            spanX,spanY=metadata['orthographicSpanMetres'];target=metadata['target']
            for kind,color in [('peaks','#ffd074'),('passes','#73caff')]:
                for i,feature in enumerate(report['coarseFeatures'][kind]):
                    x,y,z=feature['position'];px=im.width/2+(x-target[0])*im.width/spanX;pz=im.height/2-(z-target[2])*im.height/spanY
                    d.ellipse((px-5,pz-5,px+5,pz+5),fill=color)
                    label=f"{'P' if kind=='peaks' else 'S'}{i+1} {y:.1f}m / prominence {feature['prominence']:.1f}m"
                    d.text((px+10,pz),label,fill='white',font=font)
        bar_metres=20 if phase in (3,7) else 100 if phase==6 else 500
        bar=bar_metres*im.width/metadata['orthographicSpanMetres'][0];y=im.height-45
        d.line((25,y,25+bar,y),fill='white',width=3);d.text((25,y-28),f'{bar_metres} m (view plane)',fill='white',font=font)
        name='04-native-height-slope.png' if view=='top' else '04-native-range-oblique.png'
        if splines:name='05-cap-face-talus.png' if view=='top' else '05-nested-plateau-oblique.png'
        if open_scene:name=f'05-open-cliff-{view}.png'
        if phase==6:name='06-physical-terrain-top.png' if view=='top' else '06-river-cut-plateau.png'
        if phase==3:name=f"03b-{'confined-stream' if open_scene else 'curved-confluence'}-{view}.png"
        if phase==7:name=f"07-{'high' if open_scene else 'low'}-flow-{view}.png"
        im.save(root/'prints'/name)
    manifest['native_cameras'+scene]=cameras


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixture',choices=['contracts','patches','land','rivers','watersheds','relief','splines','physical','channels','routing','suitability','legacy-map'])
    parser.add_argument('--build',type=Path,default=Path('build-release'))
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--timeout',type=float,default=180)
    parser.add_argument('--seed',type=int,default=7)
    parser.add_argument('--extent-km',type=float,default=8)
    parser.add_argument('--native',action='store_true',help='Render relief acceptance geometry with the native Metal fixture')
    args=parser.parse_args()
    if args.native and args.fixture not in ('relief','splines','physical','channels','routing'):parser.error('--native requires relief, splines, physical, channels or routing')
    if not math.isfinite(args.timeout) or args.timeout<=0 or not math.isfinite(args.extent_km) or args.extent_km<3:
        parser.error('timeout must be positive and extent must be at least 3 km')
    if args.fixture!='legacy-map' and args.seed!=7:parser.error('contract fixture has fixed seed 7')
    root=args.output.resolve()
    if root.exists():parser.error('output must be a new directory to preserve evidence')
    root.mkdir(parents=True);(root/'prints').mkdir();(root/'products').mkdir()
    build=args.build.resolve()
    exe=build/{'contracts':'tests/WorldGenerationContractsTests', 'patches':'tests/WorldPatchMeshTests', 'land':'tests/WorldLandProductTests', 'rivers':'tests/WorldRiverGraphTests', 'watersheds':'tests/WorldWatershedTests', 'relief':'tests/WorldReliefAcceptanceTests', 'splines':'tests/WorldSplineLandformsTests', 'physical':'tests/WorldPhysicalTerrainTests', 'channels':'tests/WorldRiverChannelsTests', 'routing':'tests/WorldRiverRoutingTests', 'suitability':'tests/WorldSuitabilityTests', 'legacy-map':'samples/LowPolyTerrainSample'}[args.fixture]
    status='FAIL';runs=[];assertions=[];failure=None;native_status='NOT-RUN'
    manifest=dict(schema=1,fixture=args.fixture,seed=args.seed,hardware=platform.platform(),
                  machine=platform.machine(),cpu_count=os.cpu_count(),backend='CPU',
                  provenance=provenance(),executable=str(exe),
                  executable_sha256=digest(exe) if exe.is_file() else None,
                  generator='rewrite-'+args.fixture+'-v1' if args.fixture!='legacy-map' else 'legacy-map',
                  native_render_status='NOT-RUN',visual_review='PENDING',
                  settings=dict(extent_km=1 if args.fixture in ('watersheds','splines') else 2 if args.fixture in ('land','rivers','relief') else .6 if args.fixture=='physical' else .14 if args.fixture=='channels' else .29 if args.fixture=='routing' else .3 if args.fixture=='suitability' else args.extent_km,timeout_seconds=args.timeout))
    try:
        if not exe.is_file():raise RuntimeError(f'executable not found: {exe}')
        for i in range(2):
            product=root/'products'/f'run-{i}.json'
            if args.fixture!='legacy-map':cmd=[str(exe),str(product)]
            else:
                product=product.with_suffix('.ply')
                cmd=[str(exe),'--headless','--source','map','--seed',str(args.seed),
                     '--extent',str(args.extent_km),'--spacing','0.25','--levels','1',
                     '--tile','2000','--tolerance','8','--rounds','2','--workers','1',
                     '--ply',str(product)]
            runs.append(execute(cmd,root/f'run-{i}.log',args.timeout))
            if runs[-1]['exit_code']!=0:raise RuntimeError(f"run {i} failed: {runs[-1]['exit_code']}")
            if not product.is_file() or not product.stat().st_size:raise RuntimeError('missing/empty product')
        products=sorted((root/'products').iterdir())
        same=digest(products[0])==digest(products[1])
        assertions.append({'name':'repeat_product_hash','pass':same})
        if not same:raise RuntimeError('repeat output differs')
        if args.fixture!='legacy-map':
            report=json.loads(products[0].read_text());assertions.extend(report['assertions'])
            if report['status']!='PASS' or not report['assertions'] or not all(x['pass'] for x in report['assertions']):
                raise RuntimeError('contract assertions failed')
            if args.fixture=='contracts':diagnostics(report,root/'prints')
            elif args.fixture=='land':land_diagnostics(report,root/'prints')
            elif args.fixture=='rivers':river_diagnostics(report,root/'prints')
            elif args.fixture=='watersheds':watershed_diagnostics(report,root/'prints')
            elif args.fixture=='relief':relief_diagnostics(report,root/'prints')
            elif args.fixture=='splines':spline_diagnostics(report,root/'prints')
            elif args.fixture=='physical':physical_diagnostics(report,root/'prints')
            elif args.fixture=='channels':channel_diagnostics(report,root/'prints')
            elif args.fixture=='routing':routing_diagnostics(report,root/'prints')
            elif args.fixture=='suitability':suitability_diagnostics(report,root/'prints')
            else: mesh_diagnostics(report['meshes'], root/'prints', '14-patch-mesh-ids.png', 'CDT land parcel with a reserved hole and independent inclined road')
        else:
            mesh=read_ply(products[0])
            assertions.append({'name':'valid_mesh_payload','pass':mesh['degenerate_triangles']==0,'vertices':len(mesh['vertices']),'triangles':len(mesh['triangles']),'degenerate_triangles':mesh['degenerate_triangles']})
            mesh_diagnostics([mesh],root/'prints','00-baseline-mesh.png','LEGACY map mesh / baseline only')
            if mesh['degenerate_triangles']:raise RuntimeError(f"{mesh['degenerate_triangles']} degenerate triangles in exported mesh")
        if args.native:
            native_status='FAIL'
            native_capture(build, products[0], root, args.timeout, manifest, assertions)
            if args.fixture=='splines':
                open_product=root/'products'/'open-cliff.json'
                open_report=dict(report);open_report['mesh']=report['openMesh']
                open_product.write_text(json.dumps(open_report))
                native_capture(build, open_product, root, args.timeout, manifest, assertions, '-open')
            if args.fixture=='splines':
                import tempfile
                invalid=json.loads(products[0].read_text())
                index=next(i for i,n in enumerate(invalid['mesh']['expectedNormals']) if n[1]==0)
                t=invalid['mesh']['triangles'][index];t[1],t[2]=t[2],t[1]
                with tempfile.TemporaryDirectory(prefix='terrain-winding-') as tmp:
                    bad=Path(tmp)/'invalid.json';bad.write_text(json.dumps(invalid))
                    log=root/'native-winding-rejection.log'
                    rejected=execute([str(build/'tools/TerrainRewriteNativeCapture'),str(bad),str(Path(tmp)/'invalid.png'),'oblique'],log,args.timeout)
                ok=rejected['exit_code']!=0 and 'vertical face winding/normal mismatch' in log.read_text()
                assertions.append({'name':'native_rejects_reversed_vertical_face','pass':ok})
                if not ok:raise RuntimeError('native renderer did not reject corrupted vertical winding')
            if args.fixture=='channels':
                high_product=root/'products'/'high-flow.json'
                high_report=dict(report);high_report['mesh']=report['highMesh']
                high_product.write_text(json.dumps(high_report))
                native_capture(build,high_product,root,args.timeout,manifest,assertions,'-high')
                from PIL import Image, ImageDraw
                low=Image.open(root/'prints'/'07-low-flow-top.png').convert('RGB');high=Image.open(root/'prints'/'07-high-flow-top.png').convert('RGB')
                combined=Image.new('RGB',(low.width*2,low.height));combined.paste(low,(0,0));combined.paste(high,(low.width,0))
                combined.save(root/'prints'/'07-river-low-high-flow.png')
            if args.fixture=='routing':
                confined_product=root/'products'/'confined-stream.json'
                confined_report=dict(report);confined_report['mesh']=report['confinedMesh']
                confined_product.write_text(json.dumps(confined_report))
                native_capture(build,confined_product,root,args.timeout,manifest,assertions,'-confined')
            native_status='PASS'
        status='PASS'
    except Exception as e:
        failure=str(e)
    # The algorithm run can pass while native capture/visual acceptance remains pending.
    metrics=dict(status=status,failure=failure,runs=runs,assertions=assertions,
                 native_capture=native_status,step_acceptance='INCOMPLETE')
    (root/'metrics.json').write_text(json.dumps(metrics,indent=2)+'\n')
    manifest['native_render_status']=native_status
    (root/'review.md').write_text(f'# Visual review\n\nPENDING: diagnostic prints require inspection.\nNative renderer capture: {native_status}. Full checklist step acceptance: INCOMPLETE.\n')
    manifest['outputs']={str(p.relative_to(root)):digest(p) for p in sorted(root.rglob('*')) if p.is_file()}
    (root/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps(dict(status=status,failure=failure,output=str(root))))
    return 0 if status=='PASS' else 1

if __name__=='__main__':raise SystemExit(main())
