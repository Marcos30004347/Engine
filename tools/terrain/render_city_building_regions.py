#!/usr/bin/env python3
"""Actual C++ building masks and native massing; no invented footprint geometry."""
import argparse,json,subprocess
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont
FONT='/System/Library/Fonts/Supplemental/Arial.ttf'
def font(n):return ImageFont.truetype(FONT,n)
def main(root,native):
    log=(root/'tests.log').read_text()
    if 'FAIL ' in log or 'failures=0' not in log:raise ValueError('Passing tests required')
    panels=[]
    for scene,title in enumerate(['Street-bounded courtyard blocks','Oblique street frontage','Curved street boundaries','Waterfront reservation']):
        data=json.loads((root/f'{scene}.json').read_text())
        for view in ['top','oblique']:subprocess.run([str(native.resolve()),str(root/f'{scene}-mesh.json'),str(root/f'{scene}-native-{view}.png'),view],check=True)
        im=Image.new('RGB',(1100,850),'#f4f0e7');d=ImageDraw.Draw(im)
        d.text((30,20),title,font=font(28),fill='#31463d')
        d.text((30,62),'Building regions with courtyard reservations and open passages',font=font(19),fill='#51645b')
        def xy(p):return (40+p[0]*6.2,720-p[2]*6)
        for c in data['sourceContours']:d.polygon([xy(p) for p in c['loop']],fill={11:'#cbd0b6',1:'#e3d1ac',9:'#78a7b4'}.get(c['material'],'#d0d5bf'))
        for block in data['blocks']:
            for court in block['courtyards']:d.polygon([xy(p) for p in court['outer']],fill='#a8ba8e')
        for region in data['regions']:
            p=region['footprint'];d.polygon([xy(v) for v in p['outer']],fill='#a66e5e')
            for hole in p['holes']:d.polygon([xy(v) for v in hole],fill='#a8ba8e')
            d.polygon([xy(v) for v in region['passage']],fill='#e3d1ac')
            x,y=xy(region['streetAccess'][0]);d.ellipse((x-5,y-5,x+5,y+5),fill='#455f53')
        for c in data['sourceContours']:
            if c['material']==9:d.polygon([xy(v) for v in c['loop']],fill='#78a7b4')
        d.text((30,765),f"{len(data['blocks'])} final street blocks | {len(data['regions'])} connected regions | green: reserved courts",font=font(20),fill='#405a4b')
        d.text((30,801),'Native preview uses flat-roof massing. WFC, doors and final architecture remain future work.',font=font(17),fill='#405a4b')
        im.save(root/f'{scene}-regions.png');panels.append(im)
    sheet=Image.new('RGB',(2200,745),'#f4f0e7')
    for i,p in enumerate(panels):sheet.paste(p.resize((550,425)),(i*550,0))
    for i in range(4):
        im=Image.open(root/f'{i}-native-oblique.png');im.thumbnail((550,310));sheet.paste(im,(i*550,435))
    sheet.save(root/'city-building-regions-contact.png');print('PASS 4 building-region plans and 8 native captures')
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('directory',type=Path);p.add_argument('--native',type=Path,default=Path('build/tools/TerrainRewriteNativeCapture'));a=p.parse_args();main(a.directory,a.native)
