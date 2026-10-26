const {chromium}=require('playwright');
const fs=require('node:fs/promises');
(async()=>{
 const [url,out]=process.argv.slice(2);await fs.mkdir(out,{recursive:true});
 const browser=await chromium.launch({headless:true,executablePath:process.env.BROWSER_EXECUTABLE,args:process.platform==='darwin'?['--enable-webgl','--use-angle=metal']:[]});
 try{
  const page=await browser.newPage({viewport:{width:1500,height:1100}});await page.goto(url);
  await page.waitForFunction(()=>window.cityViewer?.ready,{},{timeout:180000});
  const m=await page.evaluate(async()=>(await(await fetch('city.json')).json()));
  const decks=[...(m.platforms||[])].sort((a,b)=>m.buildings.filter(x=>x.source===b.block).length-m.buildings.filter(x=>x.source===a.block).length);
  if(!decks.length)throw Error('No native raised platform');
  const d=decks[0],lo=[0,1,2].map(i=>Math.min(...d.boundary.map(p=>p[i]))),hi=[0,1,2].map(i=>Math.max(...d.boundary.map(p=>p[i])));
  const c=[(lo[0]+hi[0])/2,d.level+3,(lo[2]+hi[2])/2],span=Math.max(hi[0]-lo[0],hi[2]-lo[2]);
  for(const [name,dir] of [['platform',[-.85,.65,-.85]],['platform-top',[0,1.3,.001]],['platform-reverse',[.85,.65,.85]]]){
   const eye=c.map((v,i)=>v+span*dir[i]);await page.evaluate(({eye,c})=>window.cityViewer.inspectCamera(eye,c),{eye,c});
   await page.waitForTimeout(500);await page.screenshot({path:out+'/'+name+'.png'});
  }
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
