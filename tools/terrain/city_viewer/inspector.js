// Read-only inspection of exported room geometry and authoritative history links.
const node=(tag,text,cls)=>{const e=document.createElement(tag);if(text!=null)e.textContent=text;if(cls)e.className=cls;return e;};
const words=s=>(s||'Unassigned').replace(/([a-z])([A-Z])/g,'$1 $2').replaceAll('-',' ');
const svgNode=(tag,attrs)=>{const e=document.createElementNS('http://www.w3.org/2000/svg',tag);for(const [k,v] of Object.entries(attrs))e.setAttribute(k,v);return e;};
const roomLabel=r=>words(r.kind&&r.kind!=='Count'?r.kind:r.use||'Unassigned room');
export function createInspector(manifest,actions){
 const root=document.getElementById('buildingDetails'),history=manifest.history;
 const people=new Map((history?.people||[]).map(p=>[p.id,p]));
 const buildings=new Map(manifest.buildings.map((b,i)=>[b.id,{b,i}]));
 let current=null,floor=0,room=null;
 const floorLabel=n=>n<0?`Basement ${-n}`:n===0?'Ground floor':`Floor ${n}`;
 const button=(label,fn)=>{const b=node('button',label);b.type='button';b.onclick=fn;return b;};
 function destination(id,label,historicalId){
  if(buildings.has(id))return button(label,()=>actions.select(buildings.get(id).i,true));
  return node('span',historicalId?`${label}: recorded, not realised in this scene`:`${label}: not assigned`,'muted');
 }
 function showPerson(person){
  let panel=document.getElementById('personDetail');if(!panel){panel=node('section');panel.id='personDetail';root.append(panel);}panel.replaceChildren();
  panel.append(node('h3',person.name),node('p',`${person.age??'Unknown'} years · ${words(person.occupation)}${person.apprentice?' apprentice':''}`));
  if(person.activity)panel.append(node('p',`Current activity: ${words(person.activity)}`));
  const links=node('div',null,'person-links');links.append(destination(person.homeId,'Home',person.homeHistoricalId),destination(person.workId,'Workplace',person.workHistoricalId));panel.append(links);
  if(person.careerStage&&person.careerStage!=='none')panel.append(node('p',`Career: ${words(person.careerStage)}`));
  if(person.ambition)panel.append(node('p',`Ambition: open a ${words(person.ambition)} business.`));
  if(person.parentNames?.length)panel.append(node('p',`Family: child of ${person.parentNames.join(' and ')}.`));
  if(person.mentorId)panel.append(node('p',`Mentor: ${people.get(person.mentorId)?.name||'Person '+person.mentorId}`));
  if(person.spouse)panel.append(node('p',`Spouse: ${person.spouseName||people.get(person.spouse)?.name||'Person '+person.spouse}`));
  const events=node('ol',null,'chronicle');for(const e of [...person.events].reverse())events.append(node('li',`Year ${e.year}: ${words(e.kind)}${e.detail?' — '+e.detail:''}`));
  if(events.children.length)panel.append(node('h4','Life events'),events);
  panel.scrollIntoView({block:'nearest'});
 }
 function roster(ids,title){
  const section=node('section');section.append(node('h4',`${title} · ${ids.length}`));
  if(!ids.length)section.append(node('p',`No ${title.toLowerCase()} recorded.`,'muted'));
  for(const id of ids){const p=people.get(id);if(!p)continue;const b=button(`${p.name} · ${p.age??"?"} years · ${words(p.occupation)}`,()=>showPerson(p));b.className='person-row';section.append(b);}
  return section;
 }
 function drawPlan(b,holder,info){
  holder.replaceChildren();const rooms=(b.roomList||[]).filter(r=>r.floor===floor&&r.boundary?.length>=3);
  if(!rooms.length){holder.append(node('p','A room blueprint is not available for this floor.','muted'));return;}
  const points=rooms.flatMap(r=>r.boundary),xs=points.map(p=>p[0]),zs=points.map(p=>p[2]);
  const x=Math.min(...xs),z=Math.min(...zs),w=Math.max(...xs)-x,h=Math.max(...zs)-z,pad=Math.max(w,h)*.07+.1;
  const svg=svgNode('svg',{viewBox:`${x-pad} ${z-pad} ${w+pad*2} ${h+pad*2}`,role:'group','aria-label':`${floorLabel(floor)} room blueprint`});
  svg.classList.add('blueprint');
  for(const r of rooms){const poly=svgNode('polygon',{points:r.boundary.map(p=>`${p[0]},${p[2]}`).join(' '),tabindex:0,role:'button','aria-label':`${roomLabel(r)}, room ${r.id}`,'data-room-id':r.id});
   poly.classList.add('room');if(room?.id===r.id)poly.classList.add('active');
   const title=svgNode('title',{});title.textContent=`${roomLabel(r)} · ${r.area?.toFixed(1)||'?'} m²`;poly.append(title);
   const select=()=>{room=r;drawPlan(b,holder,info);};poly.onclick=select;poly.onkeydown=e=>{if(e.key==='Enter'||e.key===' '){e.preventDefault();select();}};svg.append(poly);
   const cx=r.boundary.reduce((n,p)=>n+p[0],0)/r.boundary.length,cz=r.boundary.reduce((n,p)=>n+p[2],0)/r.boundary.length;
   const text=svgNode('text',{x:cx,y:cz,'font-size':Math.max(w,h)*.042,'text-anchor':'middle','dominant-baseline':'middle'});text.textContent=roomLabel(r);svg.append(text);
  }
  holder.append(svg);info.replaceChildren();
  if(room){info.append(node('p',`${roomLabel(room)} · ${room.area?.toFixed(1)||'?'} m² · ${floorLabel(floor)}`),button('Enter this room',()=>actions.enterRoom(room)));}
  else info.append(node('p','Select a room to inspect or enter it.','muted'));
 }
 function update(b){
  if(current===b)return;current=b;room=null;root.replaceChildren();if(!b)return;
  const rooms=b.roomList||[],floors=[...new Set(rooms.map(r=>r.floor))].sort((a,b)=>a-b);floor=floors.includes(0)?0:floors[0];
  if(b.architecture)root.append(node('p',`${b.architecture} · ${b.architecturalAgeYears||0} years old · ${b.weathering>.5?'Heavily weathered':b.weathering>0?'Weathered':'Maintained'}`));
  if(b.colonnadeColumns)root.append(node('p',`${b.colonnadeColumns} stone columns · ${b.cofferBeams||0} ceiling members`));
  const heading=node('div',null,'section-heading');heading.append(node('h3','Floor plan'));
  const choose=node('select');choose.setAttribute('aria-label','Blueprint floor');choose.id='blueprintFloor';
  for(const n of floors){const o=node('option',floorLabel(n));o.value=n;choose.append(o);}choose.value=floor;choose.disabled=!floors.length;heading.append(choose);root.append(heading);
  const holder=node('div'),info=node('div');holder.id='blueprintHolder';info.id='roomDetail';root.append(holder,info);
  choose.onchange=()=>{floor=Number(choose.value);room=null;drawPlan(b,holder,info);};drawPlan(b,holder,info);
  const record=b.history;
  if(record){root.append(node('h3',words(record.trade||record.use)),node('p',record.builtYear!=null?`Built in year ${record.builtYear}`:'Construction date not recorded'),roster(record.residents,'Residents'),roster(record.workers,'Workers'));
   const person=node('section');person.id='personDetail';root.append(person);
   if(record.events?.length){const details=node('details');details.append(node('summary','Building history'));const list=node('ol',null,'chronicle');for(const e of [...record.events].reverse())list.append(node('li',`Year ${e.year}: ${words(e.kind)}${e.detail?' — '+e.detail:''}`));details.append(list);root.append(details);}
  }else root.append(node('p',history?'This structure has no historical occupancy binding.':'No history snapshot is attached to this preview.','muted'));
 }
 const region=document.getElementById('regionNav');
 if(manifest.region?.settlements?.length){
  const label=node('label','City & countryside');label.htmlFor='settlementSelect';const select=node('select');select.id='settlementSelect';
  for(const place of manifest.region.settlements){const opt=node('option',`${place.name} · ${place.role}`);opt.value=place.id;select.append(opt);}select.value=history?.settlementId;
  select.onchange=()=>{const place=manifest.region.settlements.find(p=>p.id===select.value);if(!place)return;const url=new URL(place.url,location.href);if(url.origin===location.origin)location.assign(url.href);};region.append(label,select);
 }
 if(history?.stories?.length){
  const stories=node('details');stories.id='settlementStories';stories.append(node('summary',`Stories of ${history.name} · ${history.stories.length}`));
  stories.append(node('p','Stories follow recorded events. Open threads have no invented ending.','muted'));
  for(const story of history.stories){const card=node('details');card.className='story-card';card.dataset.storyId=story.id;
   card.append(node('summary',story.title),node('p',words(story.status),'story-status'));
   const list=node('ol',null,'chronicle');for(const e of story.episodes){const item=node('li',`Year ${e.year}: ${e.text}`);item.title=e.evidence;list.append(item);}card.append(list);
   for(const id of story.participants){const p=people.get(id);if(p)card.append(button(`Meet ${p.name}`,()=>showPerson(p)));}stories.append(card);
  }region.append(stories);
 }
 if(history){const stats=node('p',`Year ${history.year} · ${history.population} people · ${history.linkedBuildings} linked buildings`,'muted');region.append(stats);
  const details=node('details');details.append(node('summary','Settlement life'));
  details.append(node('p',`${history.name} · ${history.role}. ${history.unrealisedBuildings} historical buildings are not realised in this scene. ${history.unplacedResidents} residents have no realised home.`));
  details.append(node('p',`${history.recordedArrivals} recorded arrivals · ${history.waitingHouseholds} households awaiting homes.`));
  const chronicle=node('ol',null,'chronicle');for(const e of history.chronicle||[])chronicle.append(node('li',e.text));details.append(chronicle);
  for(const [job,count] of Object.entries(history.occupations))details.append(node('p',`${words(job)} · ${count}`));
  if(manifest.region){const connected=manifest.region.settlements.filter(p=>history.regionalLinks.includes(p.id));details.append(node('p',`Trade routes: ${connected.map(p=>p.name).join(', ')||'None recorded'}`));}
  region.append(details);document.querySelector('h1').textContent=history.name;
 }
 return {update,get room(){return room;}};
}
