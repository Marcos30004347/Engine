"""Draw the native terraced castle reservations and access route as an SVG."""
import argparse
from pathlib import Path
from html import escape
import numpy as np
from load_city_mesh import load_city_mesh


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('input',type=Path);ap.add_argument('output',type=Path)
    args=ap.parse_args();data=load_city_mesh(args.input)
    terraces=data.get('terraces') or []
    if not terraces:raise SystemExit('Input has no planned castle terraces')
    all_points=np.array([p for t in terraces for p in t['footprint']])
    lo=all_points.min(axis=0);hi=all_points.max(axis=0)
    scale=min(900/(hi[0]-lo[0]),760/(hi[2]-lo[2]))
    def xy(p):return 60+(p[0]-lo[0])*scale,110+(hi[2]-p[2])*scale
    def points(r):return ' '.join(f'{x:.2f},{y:.2f}' for x,y in map(xy,r))
    svg=['<svg xmlns="http://www.w3.org/2000/svg" width="1100" height="1000" viewBox="0 0 1100 1000">',
         '<rect width="1100" height="1000" fill="#f3efe4"/>',
         '<g font-family="sans-serif" fill="#203534"><text x="60" y="42" font-size="27">Terraced citadel — native site blueprint</text>',
         '<text x="60" y="72" font-size="16">Height bands • building reservations • alternating stair route</text>']
    colors=['#dce6dc','#bdd2c6','#99b8aa','#72988b','#4c796e']
    for i,t in enumerate(terraces):
        svg.append(f'<polygon points="{points(t["footprint"])}" fill="{colors[i%5]}" stroke="#34594c" stroke-width="2"/>')
    for r in data.get('terraceRoutes') or []:
        a,b=xy(r['a']),xy(r['b'])
        svg.append(f'<path d="M {a[0]},{a[1]} L {b[0]},{b[1]}" stroke="#fff6da" stroke-width="{r["width"]*scale}"/>')
    for b in data['buildings']:
        svg.append(f'<polygon points="{points(b["footprint"])}" fill="#f8f7ef" stroke="#445764" stroke-width="1.5"/>')
        if b['kind']!='defence-tower':
            c=np.array(b['footprint']).mean(axis=0);x,y=xy(c)
            label='Keep' if b['wallHeight']>25 else 'Wing'
            svg.append(f'<text x="{x}" y="{y}" text-anchor="middle" font-size="12">{escape(label)} · {b["wallHeight"]:.1f} m</text>')
    for r in data.get('terraceLinks') or []:
        a,b=xy(r['foot']),xy(r['head']);w=r['width']*scale
        svg.append(f'<path d="M {a[0]},{a[1]} L {b[0]},{b[1]}" stroke="#b97838" stroke-width="{w}"/>')
        dx,dy=b[0]-a[0],b[1]-a[1];length=np.hypot(dx,dy);nx,ny=-dy/length*w*.5,dx/length*w*.5
        for k in range(r['steps']+1):
            x,y=a[0]+dx*k/r['steps'],a[1]+dy*k/r['steps']
            svg.append(f'<path d="M {x-nx},{y-ny} L {x+nx},{y+ny}" stroke="#fff2dc" stroke-width=".8"/>')
    for i,t in enumerate(terraces):
        bounds=np.array(t['footprint']);low,high=bounds.min(axis=0),bounds.max(axis=0)
        label=(low+high)*.5;label[2]=low[2]+(4.5 if i+1==len(terraces) else (high[2]-low[2])*.55)
        x,y=xy(label);y+=42
        svg.append(f'<text x="{x}" y="{y-42}" text-anchor="middle" font-size="17" font-weight="bold">Court {i+1} · +{t["level"]:.1f} m</text>')
    svg.append('<text x="60" y="930" font-size="16">Amber: 4.8 m stairs · 24 risers per level · 17.5 cm rise / 35 cm going</text>')
    svg.append('<text x="60" y="960" font-size="14">Building labels show wall height above their terrace. Roofs are omitted from this site plan.</text></g></svg>')
    args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text('\n'.join(svg))


if __name__=='__main__':main()
