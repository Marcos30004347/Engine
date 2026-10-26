// Integration checks against a generated regional viewer, including real 3D picking.
const {chromium}=require('playwright'),assert=require('node:assert/strict'),fs=require('node:fs/promises');
(async()=>{
 const output=process.argv[3]||'build/phase7/living-region-checks';await fs.mkdir(output,{recursive:true});
 const browser=await chromium.launch({headless:true,executablePath:process.env.BROWSER_EXECUTABLE,args:process.platform==='darwin'?['--enable-webgl','--use-angle=metal']:[]});
 try{
  const page=await browser.newPage({viewport:{width:1500,height:1050}}),errors=[];
  page.on('pageerror',e=>errors.push(String(e)));page.on('console',e=>{if(e.type()==='error')errors.push(e.text());});
  await page.goto(process.argv[2]);await page.waitForFunction(()=>window.cityViewer?.ready,{}, {timeout:180000});
  const scene=await page.evaluate(async()=>await(await fetch('city.json')).json());
  if(scene.history.stories?.length){
   assert(scene.history.stories.length<=16);
   assert(scene.history.stories.every(s=>s.episodes.every(e=>e.year<=scene.history.year)));
   await page.locator('#settlementStories > summary').click();
   const crisis=page.locator('.story-card').filter({hasText:'The lean harvests'}).first();
   assert(await crisis.count());await crisis.locator('summary').click();
   assert.match(await crisis.innerText(),/Farm yields/);
   await page.screenshot({path:output+'/settlement-stories.png'});
   await crisis.locator('summary').click();
   const career=page.locator('.story-card').filter({has:page.locator('button')}).first();
   assert(await career.count());
   if(await career.count()){
    await career.locator('summary').click();await career.getByRole('button',{name:/^Meet /}).first().click();
    assert.match(await page.locator('#personDetail').innerText(),/years/);
    await page.screenshot({path:output+'/personal-story.png'});
   }
   await page.locator('#settlementStories > summary').click();
  }
  const index=scene.buildings.findIndex(b=>b.kind==='house'&&b.history?.residents?.length&&b.roomList?.some(r=>r.boundary?.length));assert(index>=0);
  const b=scene.buildings[index],c=b.footprint.reduce((a,p)=>a.map((v,i)=>v+p[i]/b.footprint.length),[0,0,0]);
  await page.selectOption('#landmark',String((index+1)%scene.buildings.length));
  await page.evaluate(({c,b})=>window.cityViewer.inspectCamera([c[0]+5,b.base+b.wallHeight+b.roofHeight+18,c[2]+8],[c[0],b.base+b.wallHeight,c[2]]),{c,b});
  await page.mouse.click(750,525);await page.waitForFunction(id=>window.cityViewer.selectedBuildingId===id,b.id,{timeout:5000});
  assert(await page.locator('.blueprint polygon').count()>0);
  await page.locator('.blueprint polygon').first().click();
  assert.match(await page.locator('#roomDetail').innerText(),/m²/);
  await page.waitForFunction(()=>window.cityViewer.selectedRoomId);
  const selected=await page.evaluate(()=>window.cityViewer.selectedRoomId),room=b.roomList.find(r=>r.id===selected);assert(room);
  await page.getByRole('button',{name:'Enter this room',exact:true}).click();await page.waitForTimeout(100);
  const pos=await page.evaluate(()=>window.cityViewer.position);assert(Math.abs(pos[1]-room.boundary[0][1]-1.65)<.001);
  const floors=[...new Set(b.roomList.map(r=>r.floor))];if(floors.length>1){await page.selectOption('#blueprintFloor',String(floors[floors.length-1]));assert(await page.locator('.blueprint polygon').count()>0);}
  await page.locator('.person-row').first().click();assert.match(await page.locator('#personDetail').innerText(),/years/);
  await page.click('#visit');await page.waitForTimeout(300);await page.screenshot({path:output+'/city-inspector.png'});
  const person=scene.history.people.find(p=>p.homeId&&p.workId&&p.homeId!==p.workId);assert(person);
  const homeIndex=scene.buildings.findIndex(b=>b.id===person.homeId);await page.selectOption('#landmark',String(homeIndex));
  await page.locator('.person-row').filter({hasText:person.name+' ·'}).first().click();
  await page.locator('#personDetail').getByRole('button',{name:'Workplace',exact:true}).click();
  await page.waitForFunction(id=>window.cityViewer.selectedBuildingId===id,person.workId);
  const villages=[];
  for(const sid of ['200','300','400']){
   const destination=new URL(scene.region.settlements.find(s=>s.id===sid).url,process.argv[2]).href;
   await page.selectOption('#settlementSelect',sid);await page.waitForURL(destination);await page.waitForFunction(()=>window.cityViewer?.ready,{}, {timeout:180000});
   const report=await page.evaluate(async()=>{const m=await(await fetch('city.json')).json();return {id:m.history.settlementId,name:m.history.name,buildings:m.buildings.length,people:m.history.people.length,glError:window.cityViewer.glError};});
   assert.equal(report.id,sid);assert.equal(report.glError,0);villages.push(report);
   await page.click('#overview');await page.waitForTimeout(300);await page.screenshot({path:output+'/settlement-'+sid+'.png'});
  }
  assert.deepEqual(errors,[]);await fs.writeFile(output+'/report.json',JSON.stringify({ok:true,pickedBuilding:b.id,enteredRoom:selected,workplace:person.workId,villages,errors},null,2));
  console.log('PASS real building picking, room blueprint/floors, room entry, resident/workplace links and all three countryside scenes');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
