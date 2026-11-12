import assert from 'node:assert/strict';
export async function testBrandingBrowser({page,run,check,viewport,noOverflow}){
 let generation=0;async function reload(ready){const id=++generation,script=await page.send('Page.addScriptToEvaluateOnNewDocument',{source:'window.__brandingReload='+id});await page.send('Page.reload',{ignoreCache:true});await page.wait('window.__brandingReload==='+id+' && ('+ready+')');await page.send('Page.removeScriptToEvaluateOnNewDocument',{identifier:script.identifier});}
 const posts=[];const off=page.on('Network.requestWillBeSent',p=>{if(p.request.method==='POST')posts.push(p.request.url);});await page.send('Network.enable');
 const seed=await page.send('Page.addScriptToEvaluateOnNewDocument',{source:"localStorage.clear();location.hash='#/dashboard';"});
 try{
  await reload("document.querySelector('#connection')?.dataset.state==='connected'");
  await check('ESP Serial2WiFi branding preserves configured identity',async()=>{
   assert.equal(await run(()=>document.title),'ESP Serial2WiFi · Device overview');
   assert.equal(await run(()=>document.querySelector('.brand').getAttribute('aria-label')),'ESP Serial2WiFi dashboard');
   assert.equal(await run(()=>document.querySelector('.brand').textContent),'ESPSerial2WiFi');
   assert.equal(await run(()=>document.querySelector('#device-name').textContent),'recovery-demo');
   assert.equal(await run(()=>document.documentElement.dataset.theme),await run(()=>matchMedia('(prefers-color-scheme:dark)').matches?'dark':'light'));
   const icon=await run(async()=>{const r=await fetch('/favicon.ico'),b=new Uint8Array(await r.arrayBuffer());return [...b.slice(0,6)];});assert.deepEqual(icon,[0,0,1,0,2,0]);

  });
  await run(()=>location.hash='#/serial');await page.wait("document.querySelector('#serial-uart')?.dataset.state==='clean'");
  await check('Default serial preferences do not send commands',async()=>{
   assert.equal(await run(()=>document.querySelector('#serial-raw-capture').open),false);assert.equal(await run(()=>document.querySelector('#serial-raw-format').value),'text');
   assert.deepEqual(posts,[]);
  });
  await page.send('Page.removeScriptToEvaluateOnNewDocument',{identifier:seed.identifier});
  await run(()=>{localStorage.setItem('esp-serial2wifi-theme','light');localStorage.setItem('esp-serial2wifi-serial-mode','terminal');localStorage.setItem('esp-serial2wifi-raw-format','text');});await reload("document.querySelector('#serial-uart')?.dataset.state==='clean'");
  await check('Product preferences persist across reloads',async()=>{
   assert.equal(await run(()=>document.documentElement.dataset.theme),'light');assert.equal(await run(()=>document.querySelector('#serial-raw-capture').open),false);assert.equal(await run(()=>document.querySelector('#serial-raw-format').value),'text');assert.deepEqual(posts,[]);
  });
  const denied=await page.send('Page.addScriptToEvaluateOnNewDocument',{source:"Object.defineProperty(window,'localStorage',{configurable:true,get(){throw new DOMException('Denied','SecurityError');}});"});await reload("document.querySelector('#serial-uart')?.dataset.state==='clean'");
  await check('Storage-denied browsers retain usable theme and serial defaults',async()=>{
   assert.equal(await run(()=>document.documentElement.dataset.theme),await run(()=>matchMedia('(prefers-color-scheme:dark)').matches?'dark':'light'));assert.equal(await run(()=>document.querySelector('#serial-raw-capture').open),false);assert.equal(await run(()=>document.querySelector('#serial-raw-format').value),'text');assert.deepEqual(posts,[]);
  });
  await page.send('Page.removeScriptToEvaluateOnNewDocument',{identifier:denied.identifier});
 }finally{off();}
}
