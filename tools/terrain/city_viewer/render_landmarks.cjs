// Frame actual native compound wings, using their source district IDs.
const { chromium } = require('playwright');
const fs = require('node:fs/promises');
const path = require('node:path');
(async () => {
  const [url, output] = process.argv.slice(2);
  await fs.mkdir(output, { recursive: true });
  const browser = await chromium.launch({ headless: true,
    executablePath: process.env.BROWSER_EXECUTABLE,
    args: process.platform === 'darwin' ? ['--enable-webgl', '--use-angle=metal'] : [] });
  try {
    const page = await browser.newPage({ viewport: { width: 1500, height: 1100 } });
    await page.goto(url);
    await page.waitForFunction(() => window.cityViewer?.ready, {}, { timeout: 180000 });
    const buildings = await page.evaluate(async () => (await (await fetch('city.json')).json()).buildings);
    for (const [name, kind] of [['castle', 'keep'], ['manor', 'manor']]) {
      const landmark = buildings.find(b => b.kind === kind);
      if (!landmark) throw Error(`Missing ${kind}`);
      const wings = buildings.filter(b => b.source === landmark.source);
      const points = wings.flatMap(b => b.footprint.flatMap(p => [p,
        [p[0], b.base + b.wallHeight + b.roofHeight, p[2]]]));
      const lo = [0,1,2].map(i => Math.min(...points.map(p => p[i])));
      const hi = [0,1,2].map(i => Math.max(...points.map(p => p[i])));
      const target = lo.map((v,i) => (v + hi[i]) / 2);
      const span = Math.max(hi[0]-lo[0], hi[2]-lo[2], hi[1]-lo[1]);
      for (const [view, direction] of [['perspective',[-.85,.7,-.85]], ['reverse',[.85,.6,.85]], ['top',[0,1.35,.001]]]) {
        const eye = target.map((v,i) => v + span * direction[i]);
        await page.evaluate(({eye,target}) => window.cityViewer.inspectCamera(eye,target), {eye,target});
        await page.waitForTimeout(500);
        await page.screenshot({ path: path.join(output, `${name}-${view}.png`) });
        if (await page.evaluate(() => window.cityViewer.glError)) throw Error('WebGL render failed');
      }
    }
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
