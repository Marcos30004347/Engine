#!/usr/bin/env python3
"""Step 11 diagnostics: consume C++ layouts and native mesh renders, never invent geometry."""
import argparse,json,subprocess
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont
FONT='/System/Library/Fonts/Supplemental/Arial.ttf'
NAMES=['Crossroads envelope','River town / reserved pond','Walled market envelope','Port / concave quay','Prepared hill terrace']
def font(n):return ImageFont.truetype(FONT,n)
def render(root,native):
    log=(root/'tests.log').read_text()
    if 'FAIL ' in log or 'failures=0' not in log:raise ValueError('Failed test evidence cannot produce acceptance prints')
    panels={name:[] for name in ['partitions','frontage-areas','access','final']}
    metrics=[]
    for preset in range(5):
      for recipe in range(2):
        tag=f'{preset}-{recipe}';data=json.loads((root/f'{tag}.json').read_text())
        for view in ['top','oblique']:
          subprocess.run([str(native),str(root/f'{tag}-mesh.json'),str(root/f'{tag}-native-{view}.png'),view],check=True)
        for mode in panels:
          im=Image.new('RGB',(840,810),'#f0eee4');draw=ImageDraw.Draw(im)
          draw.text((24,20),f'{NAMES[preset]} / {"Voronoi" if recipe==0 else "Recursive"}',font=font(21),fill='#263f3b')
          def xy(p):return (34+p[0]*5.5,725-p[2]*5.5)
          def poly(points,**kwargs):draw.polygon([xy(p) for p in points],**kwargs)
          if mode=='final':
            photo=Image.open(root/f'{tag}-native-top.png').convert('RGB');photo.thumbnail((790,670));im.paste(photo,((840-photo.width)//2,60))
          else:
            poly(data['envelope'],fill='#c5cfaa',outline='#425b4a')
            for ward in data['wards']:
              for p in ward['pieces']:
                if mode=='partitions':
                  i=ward['id'];poly(p,fill=(160+(i*17)%50,174+(i*11)%45,140+(i*7)%50),outline='#6e7f68')
            for p in data['holes']:poly(p,fill='#8ebbc2',outline='#4c7f8f')
            for p in data['reservations']:poly(p,fill='#eadab2',outline='#978350')
            for contour in data['contours']:
              if contour['material']==1:poly(contour['loop'],fill='#efe0b5')
              if contour['material']==9:poly(contour['loop'],fill='#8ebbc2')
            if mode=='access':
              for street in data['streets']:draw.line([xy(p) for p in street['path']],fill='#ad7952' if street['arterial'] else '#65819b',width=3)
            for obj in data['objects']:
              if obj['kind']==3:
                if mode=='frontage-areas':
                  x,y=xy(obj['entrance']);draw.ellipse((x-2,y-2,x+2,y+2),fill='#b65d50')
                continue
              if mode=='frontage-areas':poly(obj['clearance'],fill=None,outline='#bda66d')
              if mode!='partitions':
                if obj['kind']==2:poly(obj['footprint'],fill='#7f9e62',outline='#66564b')
                if obj.get('courtyard'):poly(obj['courtyard'],fill='#d9cca6')
                for part in obj.get('parts', [{'footprint':obj['footprint']}]) if obj['kind']!=2 else []:
                  poly(part['footprint'],fill='#9c6650',outline='#66564b')
              if mode=='access':draw.line([xy(obj['entrance']),xy(obj['access'])],fill='#f5f4d5',width=2)
            for points,width in data['walls']:
              draw.line([xy(p) for p in points],fill='#69634d',width=max(2,round(width*11)))
          occupied=sum(o['kind']!=3 for o in data['objects'])
          notes={'partitions':'Map-owned ward boundaries; pale street ribbons; tan reservation.', 'frontage-areas':'Brown: objects; green: gardens; outlined roof clearance; red: rejected gaps.', 'access':'Arteries brown; connected local streets blue; entrances join retained streets.', 'final':'Native Metal render of C++ CDT + building massing. Final asset art pending.'}
          draw.text((24,749),notes[mode],font=font(17),fill='#43574e')
          draw.text((24,778),f'{occupied} object areas / {len(data["streets"])} streets / {data["mapFaces"]} labeled faces',font=font(16),fill='#43574e')
          im.save(root/f'{tag}-11-city-{mode}.png');panels[mode].append(im)
        metrics.append({'scene':tag,'name':NAMES[preset],'recipe':recipe,'wards':len(data['wards']),'streets':len(data['streets']),'occupied':sum(o['kind']!=3 for o in data['objects']),'gaps':data['rejected'],'faces':data['mapFaces'],'triangles':data['triangles']})
    for mode,images in panels.items():
      sheet=Image.new('RGB',(1680,4050),'#f0eee4')
      for i,im in enumerate(images):sheet.paste(im,((i%2)*840,(i//2)*810))
      sheet.save(root/f'11-city-{mode}.png')
    (root/'metrics.json').write_text(json.dumps(metrics,indent=2)+'\n')
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('directory',type=Path);p.add_argument('--native',type=Path,default=Path('build/tools/TerrainRewriteNativeCapture'));a=p.parse_args();render(a.directory,a.native.resolve())
