#!/usr/bin/env python3
"""Build a local review of native timeline renders and persistent citizen IDs."""
import argparse
import json
from pathlib import Path
import shutil


def safe_ids(value):
    # Tagged household IDs exceed JavaScript's exact integer range.
    if isinstance(value, bool):
        return value
    if isinstance(value, int) and abs(value) > 2**53 - 1:
        return str(value)
    if isinstance(value, list):
        return [safe_ids(v) for v in value]
    if isinstance(value, dict):
        return {k: safe_ids(v) for k, v in value.items()}
    return value


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--history-root', type=Path, required=True)
    parser.add_argument('--views-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--graph-root', type=Path)
    parser.add_argument('--world-root', type=Path)
    parser.add_argument('--world-timeline-root', type=Path,
                        help='Use dated full-world models for the primary comparison')
    parser.add_argument('--years', type=int, nargs='+', default=[20, 100, 1000],
                        help='Explicit milestones to review; missing requested world models are errors')
    parser.add_argument('--review-note', default='', help='Visible limitations of this particular run')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    data = []
    for year in args.years:
        folder = args.history_root / f'year-{year}' / 'settlement-100'
        if not (folder / 'history.json').exists():
            continue
        history = json.loads((folder / 'history.json').read_text())
        model_path = (args.world_timeline_root / f'year-{year}' / 'city-model-c10.json'
                      if args.world_timeline_root else folder / 'city-model.json')
        if args.world_timeline_root and not model_path.exists():
            raise FileNotFoundError(f'Full-world milestone has no native model: {model_path}')
        mesh = json.loads(model_path.read_text()) if model_path.exists() else {}
        history['meshBuildings'] = len(mesh.get('buildings', []))
        history['meshRejections'] = mesh.get('rejections', [])
        history['bindings'] = mesh.get('historicalBindings', [])
        history['unresolvedMeshes'] = sum(b.get('generatedBuilding') is None for b in history['bindings'])
        history['viewer'] = f'../history-{"world-" if args.world_timeline_root else ""}year-{year}/'
        data.append(safe_ids(history))
        for view in ('top', 'front', 'back', 'side', 'perspective'):
            image = args.views_root / f'year-{year}' / 'views' / f'{view}.png'
            if image.exists():
                shutil.copyfile(image, args.output / f'{year}-{view}.png')
    graph = None
    if args.graph_root and (args.graph_root / 'history.json').exists():
        story = json.loads((args.graph_root / 'history.json').read_text())
        graph = {'years': story['years'], 'checkpoint': story['checkpoint'],
                 'realms': story['snapshots'][-1]['realms'],
                 'people': story['snapshots'][-1]['people'],
                 'settlements': [s for s in story['snapshots'][-1]['settlements'] if not s['origin']]}
        image = args.graph_root / 'views' / 'perspective.png'
        if image.exists():
            shutil.copyfile(image, args.output / 'graph-perspective.png')
    (args.output / 'graph.json').write_text(json.dumps(graph))
    world = None
    if args.world_root:
        model = json.loads((args.world_root / 'city-model-c10.json').read_text())
        bindings = model.get('historicalBindings', [])
        world = {'requests': len(bindings),
                 'resolved': sum(b.get('generatedBuilding') is not None for b in bindings),
                 'physicalBuildings': len(model.get('buildings', []))}
        image = args.world_root / 'views' / 'perspective.png'
        if image.exists():
            shutil.copyfile(image, args.output / 'world-perspective.png')
    (args.output / 'world.json').write_text(json.dumps(world))
    (args.output / 'history.json').write_text(json.dumps(data, separators=(',', ':')))
    html = HTML
    if args.world_timeline_root:
        html = html.replace('These are native engine meshes of the focal settlement at each date.',
                            'Each dated snapshot is projected through the full world terrain, cell, block, road, lot and native mesh pipeline.')
    if args.review_note:
        import html as html_module
        html = html.replace('<div id="cards"', '<p class="note">' + html_module.escape(args.review_note) + '</p><div id="cards"', 1)
    (args.output / 'index.html').write_text(html)
    print(f'{len(data)} historical milestones -> {args.output}')


HTML = '''<!doctype html><html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>City history · native mesh review</title>
<style>
:root{color-scheme:dark;font:16px system-ui;background:#102326;color:#edf2e9}body{max-width:1500px;margin:40px auto;padding:0 24px}h1{font:40px Georgia;margin-bottom:12px}p{max-width:1000px;line-height:1.6;color:#b9cecc}select,input,button{font:inherit;padding:10px;border:1px solid #577370;border-radius:5px;background:#203c3d;color:inherit}label{display:inline-block;margin:8px 14px 16px 0}.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:20px}.card,.detail{background:#1b3436;border:1px solid #42605c;border-radius:8px;padding:18px}.card img{width:100%;aspect-ratio:15/11;object-fit:contain;background:#112427}.card h2{font:28px Georgia;margin:0 0 12px}.stats{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin:15px 0}.stats b{display:block;font-size:24px}.stats span{font-size:13px;color:#bdd1c8}table{border-collapse:collapse;width:100%;margin:15px 0}td,th{text-align:left;border-bottom:1px solid #42605c;padding:10px}button{cursor:pointer}.note{color:#edc898}.detail{margin:24px 0}a{color:#9dd3cb}
</style><h1>A city through its history</h1>
<p>One seed, two settlements, one weekly timeline. These are native engine meshes of the focal settlement at each date. Social behavior uses the existing scale fixture, not the graph matcher. The current construction simulation still uses a bounded terraced site and the fixture retains a 2,000 living-person cap across the simulated society.</p>
<label>Camera <select id="view"><option>perspective</option><option>top</option><option>front</option><option>side</option><option>back</option></select></label>
<div id="cards" class="cards"></div><div id="graph"></div><div id="world"></div>
<div class="detail"><h2>Citizen and family inspection</h2>
<p>Home and workplace are historical IDs. Unresolved rooms are left unknown; this is a schedule query, not a movement simulation. Tagged household IDs are preserved exactly.</p>
<label>Date <select id="year"></select></label><label>Citizen ID <input id="person" type="number" min="0" value="0"></label><button id="inspect">Inspect</button>
<div id="personDetail"></div><details><summary>Recorded migration and current household residence events</summary><div id="biography"></div></details><table><thead><tr><th>Relative</th><th>ID</th><th>Status</th><th>Settlement</th></tr></thead><tbody id="family"></tbody></table></div>
<p class="note">Known limits: room binding, free expansion beyond reserved parcels, and historical wall reconstruction in the full world generator remain unfinished. Missing native meshes and rejected buildings are reported explicitly.</p>
<script>
let snapshots=[];const el=id=>document.getElementById(id);function text(tag,value){const n=document.createElement(tag);n.textContent=value;return n}
function cards(){el('cards').replaceChildren();for(const s of snapshots){const c=document.createElement('article');c.className='card';c.append(text('h2','Year '+s.year));const img=document.createElement('img');img.src=s.year+'-'+el('view').value+'.png';img.alt='Native '+el('view').value+' view, year '+s.year;const link=document.createElement('a');link.href=img.src;link.append(img);c.append(link);const explore=text('a','Explore native mesh');explore.href=s.viewer;c.append(explore);const stats=document.createElement('div');stats.className='stats';for(const [label,value] of [['Residents',s.population],['Households',s.households],['Completed records',s.buildings.length],['Waiting households',s.waitingHouseholds]]){const d=document.createElement('div');d.append(text('b',value),text('span',label));stats.append(d)}c.append(stats,text('p','Defence: '+s.defenceStage+' · '+s.unresolvedMeshes+' unresolved building mesh(es).'));el('cards').append(c)}}
function inspect(){const s=snapshots.find(x=>x.year===Number(el('year').value));if(!s)return;const id=Number(el('person').value),p=s.ancestry.find(x=>x.id===id),c=s.citizens.find(x=>x.id===id);el('personDetail').replaceChildren();el('family').replaceChildren();el('biography').replaceChildren();if(!p){el('personDetail').append(text('p','No recorded person with that ID.'));return}const age=Math.floor((s.week-p.bornWeek)/52);el('personDetail').append(text('p','Citizen '+id+' · '+(p.alive?'living, age '+age:'deceased')+' · settlement '+(p.settlement??'unknown')));if(c){const activity=(s.activities||s.activitiesAtNoon||[]).find(x=>x.person===id);const placed=(logical)=>(s.bindings||[]).find(b=>String(b.historicalBuilding)===String(logical))?.generatedBuilding;el('personDetail').append(text('p','Native home '+(placed(c.home)??'unresolved')+' · native workplace '+(placed(c.workBuilding)??'unresolved')));el('personDetail').append(text('p','Household '+c.household+' · home '+(c.home??'unhoused')+' · occupation '+(c.occupation??'none')+' · duty '+(c.duty||'none')+' · work building '+(c.workBuilding??'unresolved')));el('personDetail').append(text('p','Daily query: '+(activity?.activity??'disabled')+' · building '+(activity?.building??'unresolved')+' · room '+(activity?.room??'unresolved')))}const migration=(s.migrationEvents||[]).filter(e=>e.person===id);for(const e of migration)el('biography').append(text('p','Week '+e.week+' · '+e.kind+' · '+(e.before??'unknown')+' → '+(e.after??'unknown')));if(c){for(const e of (s.residenceEvents||[]).filter(e=>String(e.household)===String(c.household)))el('biography').append(text('p','Household '+c.household+' · week '+e.week+' · '+e.kind+' · building '+e.building+' · '+e.detail))}if(!el('biography').children.length)el('biography').append(text('p','No matching recorded events.'));const relatives=[];for(const parent of p.parents)relatives.push(['Parent',parent]);if(p.spouse!==null)relatives.push(['Spouse',p.spouse]);for(const child of s.ancestry)if(child.parents.includes(id))relatives.push(['Child',child.id]);for(const [relation,rid] of relatives){const r=s.ancestry.find(x=>x.id===rid),row=document.createElement('tr');row.append(text('td',relation));const cell=document.createElement('td'),b=text('button',rid);b.onclick=()=>{el('person').value=rid;inspect()};cell.append(b);row.append(cell,text('td',r?(r.alive?'Living':'Deceased'):'Unknown'),text('td',r?.settlement??'unknown'));el('family').append(row)}}
fetch('graph.json').then(r=>r.json()).then(s=>{if(!s)return;const c=el('graph');c.className='detail';c.append(text('h2','Production graph matcher · year '+s.years));c.append(text('p',s.settlements.map(x=>x.name+': '+x.residents+' residents').join(' · ')+'. Replay and reload: '+(s.checkpoint.replayMatches&&s.checkpoint.reloadMatches?'matched':'FAILED')+'. This is a separate authored scenario, not the scale timeline above.'));for(const realm of s.realms||[]){const name=id=>(s.people||[]).find(p=>p.id===id)?.name??(id===null?'none':String(id));c.append(text('p',realm.name+' · ruler '+name(realm.ruler)+' · consort '+name(realm.consort)+' · heir '+name(realm.heir)+' · authority '+realm.authority))}const img=document.createElement('img');img.src='graph-perspective.png';img.alt='Wealdholt, actual graph-driven story, year 20';img.style='width:100%;max-width:750px';c.append(img)});
fetch('world.json').then(r=>r.json()).then(s=>{if(!s)return;const c=el('world');c.className='detail';c.append(text('h2','Full-world projection · placement gate incomplete'));c.append(text('p',s.resolved+' of '+s.requests+' ordinary historical buildings have native meshes. '+(s.requests-s.resolved)+' remain unresolved. The planned envelope still contains too much empty space; historical wall reconstruction and terrain seam failures remain open.'));const img=document.createElement('img');img.src='world-perspective.png';img.alt='Full-world historical projection with unresolved placements';img.style='width:100%;max-width:750px';c.append(img)});
fetch('history.json').then(r=>r.json()).then(data=>{snapshots=data;for(const s of data){const o=text('option','Year '+s.year);o.value=s.year;el('year').append(o)}cards();inspect()});el('view').onchange=cards;el('year').onchange=inspect;el('inspect').onclick=inspect;
</script></html>'''

if __name__ == '__main__':
    main()
