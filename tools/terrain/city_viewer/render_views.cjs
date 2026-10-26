// Repeatable perspectives of the native mesh: no replacement geometry.
const { chromium } = require('playwright');
const fs = require('node:fs/promises');
const path = require('node:path');
(async()=>{
  const url=process.argv[2],out=process.argv[3];await fs.mkdir(out,{recursive:true});
  const browser=await chromium.launch({headless:true,executablePath:process.env.BROWSER_EXECUTABLE,
    args:process.platform==='darwin'?['--enable-webgl','--use-angle=metal']:[]});
  try {
    const records=[];
    const page=await browser.newPage({viewport:{width:1500,height:1100}});await page.goto(url);
    await page.waitForFunction(()=>window.cityViewer?.ready,{},{timeout:180000});
    const bounds=await page.evaluate(async()=>{const m=await(await fetch('city.json')).json();return m.bounds;});
    const lo=bounds[0],hi=bounds[1],c=lo.map((v,i)=>(v+hi[i])/2),r=Math.max(hi[0]-lo[0],hi[2]-lo[2]);
    const views=[['top',[0,1.25,.001]],['front',[0,.35,-1.15]],['back',[0,.35,1.15]],
      ['side',[1.15,.35,0]],['left',[-1.15,.35,0]],['perspective',[-.8,.7,-.85]]];
    for(const [name,d] of views){const eye=c.map((v,i)=>v+d[i]*r);
      await page.evaluate(({eye,c})=>window.cityViewer.inspectCamera(eye,c),{eye,c});
      await page.waitForTimeout(500);await page.screenshot({path:path.join(out,name+'.png')});
      const state=await page.evaluate(()=>({position:window.cityViewer.position,triangles:window.cityViewer.loadedTriangles,error:window.cityViewer.glError}));
      if(state.error)throw Error('WebGL render failed');
      records.push({view:name,eye,target:c,...state});
    }
    await page.keyboard.press('KeyG');
    await page.waitForTimeout(500);
    await page.screenshot({path:path.join(out,'street.png')});
    const street=await page.evaluate(()=>({position:window.cityViewer.position,triangles:window.cityViewer.loadedTriangles,error:window.cityViewer.glError}));
    if(street.error)throw Error('WebGL street render failed');
    records.push({view:'street',...street});
    await fs.writeFile(path.join(out,'views.json'),JSON.stringify({url,reviewed:false,views:records},null,2));
  }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
