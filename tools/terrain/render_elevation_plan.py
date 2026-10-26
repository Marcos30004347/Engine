"""Render exported native elevation constraints as a labelled SVG plan."""
import argparse,json,collections
from pathlib import Path
parser=argparse.ArgumentParser();parser.add_argument('source',type=Path);args=parser.parse_args()
j=json.loads(args.source.read_text());lines=j['elevationLines'];points=[p for edge in lines for p in edge]
x0=min(p[0] for p in points);z0=min(p[2] for p in points);x1=max(p[0] for p in points);z1=max(p[2] for p in points)
scale=min(1160/(x1-x0),970/(z1-z0));levels=sorted({round(e[0][1],6) for e in lines})
svg=['<svg xmlns="http://www.w3.org/2000/svg" width="2800" height="2200" viewBox="0 0 1400 1100"><rect width="1400" height="1100" fill="#f3eee0"/><text x="40" y="38" font-family="sans-serif" font-size="22" fill="#293c35">Constant-height contours · planned before CDT</text>']
degree=collections.Counter()
for a,b in lines:
 assert abs(a[1]-b[1])<1e-8
 for p in (a,b):degree[tuple(p)]+=1
 idx=levels.index(round(a[1],6));color=f'hsl({150-idx*5},35%,{55-idx*1.5}%)'
 svg.append(f'<path d="M {40+(a[0]-x0)*scale:.3f} {85+(a[2]-z0)*scale:.3f} L {40+(b[0]-x0)*scale:.3f} {85+(b[2]-z0)*scale:.3f}" stroke="{color}" stroke-width="1.6" fill="none"/>')
for i,level in enumerate(levels):svg.append(f'<text x="1230" y="{100+i*25}" font-family="sans-serif" font-size="15" fill="#293c35">{level:g} m</text>')
svg.append('</svg>');target=args.source.parent/'elevation-plan.svg';target.write_text(''.join(svg))
print(json.dumps({'levels':levels,'segments':len(lines),'nonLoopVertices':sum(d!=2 for d in degree.values()),'output':str(target)}))
