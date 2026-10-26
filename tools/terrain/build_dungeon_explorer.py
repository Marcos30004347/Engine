#!/usr/bin/env python3
"""Build a self-contained exploration prototype from the native dungeon graph."""
import argparse
import json
from pathlib import Path

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('input',type=Path)
parser.add_argument('output',type=Path)
args=parser.parse_args()
document=json.loads(args.input.read_text())
if not document.get('dungeonRooms'):
    parser.error('The input contains no dungeon rooms; generate a citadel with at least one dungeon level.')
data=json.dumps({'rooms':document['dungeonRooms'],'links':document['dungeonConnections']}).replace('<','\\u003c')
page='''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Under the Gothic Keep</title><style>
*{box-sizing:border-box}body{margin:0;background:#141a20;color:#e4ddcc;font:16px system-ui;line-height:1.5}main{max-width:1180px;margin:auto;padding:40px 24px}small{color:#b49a6e;letter-spacing:.16em}h1{font:42px Georgia;margin:8px 0}p{color:#b6b5b0}.layout{display:grid;grid-template-columns:1fr 290px;gap:24px;margin-top:30px}svg{width:100%;background:#1c242c;border:1px solid #4c5259;border-radius:8px}aside{background:#20282f;padding:24px;border-radius:8px}button,select{font:inherit;padding:10px 14px;border:1px solid #6a6c68;border-radius:5px;background:#303a42;color:#eee6d5;cursor:pointer;margin:5px 0}button:hover{background:#495860}#moves button{display:block;width:100%;text-align:left}.room{cursor:pointer}.room text{pointer-events:none}.muted{font-size:13px;color:#a8aaa8}#status{color:#dcc89f}@media(max-width:740px){.layout{grid-template-columns:1fr}h1{font-size:32px}}
</style><main><small>NATIVE GENERATOR · EXPLORATION PROTOTYPE</small><h1>Under the Gothic Keep</h1><p>Follow the generated doors and stair links. Reach the vault on the deepest level, then return to the keep.</p><div class="layout"><section><label>Map level <select id="level"></select></label> <button id="reveal">Reveal map</button><svg id="map" viewBox="0 0 640 640" role="img" aria-label="Dungeon chamber map"></svg><p class="muted">Gold: your chamber · pale stone: visited · dark: unexplored. Clicking an adjacent room moves through its door.</p></section><aside><small>CURRENT CHAMBER</small><h2 id="room"></h2><p id="status"></p><div id="moves"></div><p id="progress"></p><button id="reset">Restart exploration</button><p class="muted">Movement follows the native room graph. This prototype does not simulate 3D collision, enemies or inventory.</p></aside></div></main><script>
const data=DATA;
const rooms=new Map(data.rooms.map(r=>[r.id,r]));let current=0,visited=new Set([0]),steps=0,reveal=false;
const level=document.querySelector('#level'),map=document.querySelector('#map');
const levels=[...new Set(data.rooms.map(r=>r.level))].sort((a,b)=>a-b);
for(const n of levels){const o=document.createElement('option');o.value=n;o.textContent='Level '+(n+1)+' · '+(-4*(n+1))+' m';level.append(o)}
const links=id=>data.links.filter(e=>e.from===id||e.to===id).map(e=>({id:e.from===id?e.to:e.from,stairs:e.stairs}));
function move(id){if(!links(current).some(e=>e.id===id))return;current=id;visited.add(id);steps++;if(rooms.has(id))level.value=rooms.get(id).level;draw()}
const title=id=>id===0?'Keep entrance':rooms.get(id).role.replaceAll('-',' ');
function draw(){
 const selected=Number(level.value),rs=data.rooms.filter(r=>r.level===selected),points=rs.flatMap(r=>r.footprint);
 const x0=Math.min(...points.map(p=>p[0])),z0=Math.min(...points.map(p=>p[2])),x1=Math.max(...points.map(p=>p[0])),z1=Math.max(...points.map(p=>p[2]));
 const scale=560/Math.max(x1-x0,z1-z0),xy=p=>[40+(p[0]-x0)*scale,600-(p[2]-z0)*scale];
 map.replaceChildren();const ns='http://www.w3.org/2000/svg';
 for(const r of rs){const group=document.createElementNS(ns,'g');group.classList.add('room');const polygon=document.createElementNS(ns,'polygon');polygon.setAttribute('points',r.footprint.map(p=>xy(p).join(',')).join(' '));polygon.setAttribute('fill',r.id===current?'#b18d50':visited.has(r.id)?'#6f7777':reveal?'#394954':'#232c34');polygon.setAttribute('stroke','#111820');polygon.setAttribute('stroke-width','8');group.append(polygon);group.addEventListener('click',()=>move(r.id));
 if(reveal||visited.has(r.id)){const centre=r.footprint.reduce((a,p)=>[a[0]+p[0]/r.footprint.length,0,a[2]+p[2]/r.footprint.length],[0,0,0]),p=xy(centre);const text=document.createElementNS(ns,'text');text.setAttribute('x',p[0]);text.setAttribute('y',p[1]);text.setAttribute('text-anchor','middle');text.setAttribute('fill','#f5eedf');text.setAttribute('font-size','12');text.textContent=title(r.id);group.append(text)}map.append(group)}
 for(const e of data.links){if(e.stairs||rooms.get(e.from).level!==selected)continue;if(!reveal&&!visited.has(e.from)&&!visited.has(e.to))continue;const p=xy(e.position),dot=document.createElementNS(ns,'circle');dot.setAttribute('cx',p[0]);dot.setAttribute('cy',p[1]);dot.setAttribute('r','7');dot.setAttribute('fill','#d8c594');map.append(dot)}
 document.querySelector('#room').textContent=title(current);
 const goal=data.rooms.find(r=>r.level===levels.at(-1)&&r.role==='vault').id;
 document.querySelector('#status').textContent=visited.has(goal)?current===0?'Vault explored. You returned to the keep.':'Vault reached. Find your way back to the keep.':'Explore the passages to find the lowest vault.';
 const moves=document.querySelector('#moves');moves.replaceChildren();for(const e of links(current)){const button=document.createElement('button');button.textContent=(e.stairs?'Stairs → ':'Door → ')+title(e.id)+(e.id?' · L'+(rooms.get(e.id).level+1):'');button.onclick=()=>move(e.id);moves.append(button)}
 document.querySelector('#progress').textContent=(visited.size-1)+' / '+data.rooms.length+' chambers visited · '+steps+' moves';
}
level.onchange=draw;document.querySelector('#reveal').onclick=()=>{reveal=!reveal;document.querySelector('#reveal').textContent=reveal?'Hide unexplored':'Reveal map';draw()};document.querySelector('#reset').onclick=()=>{current=0;visited=new Set([0]);steps=0;level.value=0;draw()};draw();
</script></html>'''
args.output.write_text(page.replace('DATA',data))
print(args.output)
