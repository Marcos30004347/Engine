#!/usr/bin/env python3
"""Wall diagnostics and actual native Metal mesh captures from C++ evidence."""
import argparse,json,subprocess
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont
FONT='/System/Library/Fonts/Supplemental/Arial.ttf'
def font(n):return ImageFont.truetype(FONT,n)
def main(root,native):
    text=(root/'tests.log').read_text()
    if 'FAIL ' in text or 'failures=0' not in text:raise ValueError('Passing evidence required')
    panels=[]
    for scene,title in enumerate(['Three historical enclosures','Concave later expansion','Oblique gate approaches','Irregular shared-cell boundaries']):
        data=json.loads((root/f'{scene}.json').read_text())
        for view in ['top','oblique']:
            subprocess.run([str(native.resolve()),str(root/f'{scene}-mesh.json'),str(root/f'{scene}-native-{view}.png'),view],check=True)
        im=Image.new('RGB',(1100,1000),'#f3f0e8');d=ImageDraw.Draw(im)
        d.text((35,20),title,font=font(30),fill='#273d38')
        d.text((35,65),'Solid wall foundations and open gates | outlines derived from defended cell unions',font=font(17),fill='#49645a')
        def xy(p):return (75+p[0]*6,900-p[2]*5.5)
        colors=['#718877','#b39174','#597489']
        for c in data['circuits'][::-1]:
            points=[xy(p) for p in c['boundary']];d.polygon(points,fill=['#dedccc','#e5e4d8','#ebebe2'][c['era']-1]);d.line(points+[points[0]],fill=colors[c['era']-1],width=2)
        for r in data['routes']:d.line([xy(p) for p in r],fill='#b79e7d',width=5)
        for p in data['foundations']:d.polygon([xy(v) for v in p],fill='#4c514b')
        for gate in data['gates']:
            points=[xy(p) for p in gate['ends']];d.line(points,fill='#dfac3e',width=6)
        d.text((35,945),f"{len(data['circuits'])} preserved circuits | {len(data['gates'])} portals | gold marks open gate intervals",font=font(20),fill='#3e5349')
        im.save(root/f'{scene}-plan.png');panels.append(im)
    sheet=Image.new('RGB',(2200,830),'white')
    for i,p in enumerate(panels):sheet.paste(p.resize((550,500)),(i*550,0))
    for i in range(4):
        p=Image.open(root/f'{i}-native-oblique.png').convert('RGB');p.thumbnail((550,490));sheet.paste(p,(i*550,510))
    sheet.save(root/'city-walls-contact.png')
    print('PASS 4 wall plans and 8 native Metal captures')
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('directory',type=Path);p.add_argument('--native',type=Path,default=Path('build/tools/TerrainRewriteNativeCapture'));a=p.parse_args();main(a.directory,a.native)
