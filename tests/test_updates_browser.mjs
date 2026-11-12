import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
export async function testUpdatesBrowser({page,run,check,viewport,noOverflow}){
 const fixtures=JSON.parse(await readFile(new URL('./fixtures/api-v1.json',import.meta.url)));
 const checkReloadSpacing=async()=>{
  for(const [width,scale] of [[360,'100%'],[768,'100%'],[1440,'100%'],[360,'200%']]){
   await viewport(width);await page.evaluate(`document.documentElement.style.fontSize=${JSON.stringify(scale)}`);await noOverflow();
   const gaps=await run(()=>{const b=document.querySelector('#update-reload').getBoundingClientRect(),d=document.querySelector('#view>dl.details').getBoundingClientRect(),f=document.querySelector('#update-form');return {above:b.top-d.bottom,below:f.hidden?null:f.getBoundingClientRect().top-b.bottom};});
   assert(gaps.above>=20,'Reload button touches information divider: '+JSON.stringify(gaps));if(gaps.below!==null)assert(gaps.below>=20,'Reload button touches upload form: '+JSON.stringify(gaps));
  }
  await viewport(1440);await run(()=>document.documentElement.style.fontSize='');
 };

 const mock=()=>{
  const real=window.fetch.bind(window),t=window.__updatesTest;
  const response=(v,status=200)=>new Response(typeof v==='object'&&!(v instanceof Uint8Array)?JSON.stringify(v):v,{status,headers:{'Content-Type':'application/json'}});
  window.confirm=()=>true;
  window.fetch=async(url,opts={})=>{
   const path=new URL(url,location.href).pathname;
   if(path==='/api/v1/status'){if(t.offline)throw new TypeError('Synthetic reboot disconnect');return response(t.status);}
   if(path==='/api/v1/firmware')return response(t.firmware);
   if(path==='/api/v1/firmware/activate'){t.activations++;if(t.failBoot)t.offline=true;else{t.status.firmware.slot='user2';t.status.firmware.version='esp-serial2wifi expected';t.status.system.uptime_ms=10;}return response('');}
   if(path==='/api/v1/pages/clear'){t.removals++;t.stored=[];return response({...t.pages,count:0,files:[],used_bytes:16});}
   if(path.startsWith('/api/v1/pages')){const index=Number(path.split('/')[4]||0);return response({...t.pages,index,count:t.stored.length,next:index+1<t.stored.length?index+1:null,files:t.stored[index]?[{name:t.stored[index].name,bytes:t.stored[index].bytes.length}]:[]});}
   const file=t.stored.find(f=>'/user/'+encodeURIComponent(f.name)===path);if(file)return response(new Uint8Array(file.bytes));
   return real(url,opts);
  };
  window.XMLHttpRequest=class {
   constructor(){this.upload={};this.status=0;this.responseText='';this.headers={};}
   open(method,path){this.method=method;this.path=path;}
   setRequestHeader(k,v){this.headers[k]=v;}
   abort(){this.aborted=true;this.onabort?.();}
   async send(body){t.uploads++;if(this.headers['X-Serial2WiFi-Request']!=='1')throw Error('Missing upload guard');await new Promise(r=>setTimeout(r,25));if(this.aborted)return;
    this.upload.onprogress?.({loaded:100,total:100,lengthComputable:true});
    if(t.fail){this.onerror?.();return;}
    if(this.path.includes('/pages/')){t.stored=[];for(const [,f] of body.entries()){const prefix=f.name.endsWith('.html')?t.pages.html_prefix_bytes:0,b=new Uint8Array(f.size+prefix).fill(32);b.set(new Uint8Array(await f.arrayBuffer()),prefix);t.stored.push({name:f.name,bytes:[...b]});}}
    if(t.stored.length)t.pages.used_bytes=16+t.stored.reduce((n,f)=>n+16+((new TextEncoder().encode(f.name).length+4)&~3)+((f.bytes.length+3)&~3),0);this.status=204;this.onload?.();
   }
  };
  location.hash='#/firmware';
 };
 const source=`window.__updatesTest=${JSON.stringify({firmware:fixtures['/api/v1/firmware'],pages:fixtures['/api/v1/pages'],status:fixtures['/api/v1/status'],stored:[],uploads:0,activations:0,removals:0,fail:false})};(${mock.toString()})();`;
 const script=await page.send('Page.addScriptToEvaluateOnNewDocument',{source});await page.send('Page.reload',{ignoreCache:true});
 await page.wait("document.querySelector('#view')?.dataset.updateState==='ready'");
 await check('Firmware unavailable state explains serial recovery without upload controls',async()=>{
  assert.equal(await run(()=>document.querySelector('#update-form').hidden),true);
  assert.equal(await run(()=>document.querySelector('#update-activation').hidden),true);
  assert.match(await run(()=>document.querySelector('#update-state').textContent),/serial/i);
  await checkReloadSpacing();
 });
 await check('Firmware transfer, explicit activation and post-boot identity are distinct',async()=>{
  await run(()=>{window.__updatesTest.firmware.ota_supported=true;window.__updatesTest.firmware.reason=null;document.querySelector('#view button').click();});
  await page.wait("!document.querySelector('#update-form').hidden");
  await checkReloadSpacing();
  await run(()=>{const b=new Uint8Array(1028),v=new DataView(b.buffer);b.set([0xea,4,0,1]);v.setUint32(4,0x40100000,true);v.setUint32(12,16,true);b.set(new TextEncoder().encode('ELSDK306-MAP4-CFG1'),32);const dt=new DataTransfer();dt.items.add(new File([b],'user2.bin'));const input=document.querySelector('#update-files');input.files=dt.files;input.dispatchEvent(new Event('change'));});
  await page.wait("document.querySelector('#update-file-state').textContent.includes('Header and layout checks passed')");
  await run(()=>{document.querySelector('#update-version').value='esp-serial2wifi expected';const c=document.querySelector('#update-replace-confirm');c.checked=true;c.dispatchEvent(new Event('change'));document.querySelector('#update-form').requestSubmit();});
  await page.wait("document.querySelector('#view').dataset.updateState==='uploaded'");
  assert.equal(await run(()=>window.__updatesTest.activations),0);assert.equal(await run(()=>document.querySelector('#update-activate').hidden),false);
  const warning=await run(()=>document.querySelector('#update-boot-warning').textContent);
  assert(warning.includes('boot messages'));assert(warning.includes('log out'));assert(warning.includes('ESP TX'));
  assert.equal(await run(()=>document.querySelector('#update-activation').hidden),false);
  assert.equal(await run(()=>document.querySelector('#update-activate').getAttribute('aria-describedby')),'update-boot-warning');
  await run(()=>{window.confirm=message=>{window.__activationPrompt=message;return false;};document.querySelector('#update-activate').click();});
  assert.equal(await run(()=>window.__updatesTest.activations),0);
  assert.equal(await run(()=>document.querySelector('#view').dataset.updateState),'uploaded');
  assert((await run(()=>window.__activationPrompt)).includes(warning));
  await run(()=>window.confirm=()=>true);
  for(const [width,scale] of [[360,'100%'],[768,'100%'],[1440,'100%'],[360,'200%']]){
   await viewport(width);await page.evaluate(`document.documentElement.style.fontSize=${JSON.stringify(scale)}`);await noOverflow();
   const gap=await run(()=>document.querySelector('#update-activate').getBoundingClientRect().top-document.querySelector('#update-boot-warning').getBoundingClientRect().bottom);assert(gap>=8,'Boot notice/button overlap: '+gap);
  }
  await viewport(1440);await run(()=>{document.documentElement.style.fontSize='';document.querySelector('#update-boot-warning').scrollIntoView({block:'center'});});await page.screenshot('ota-boot-notice.png');

  await run(()=>{window.__updatesTest.failBoot=true;window.__originalTimeout=window.setTimeout;window.setTimeout=(fn,ms,...args)=>window.__originalTimeout(fn,ms>=12000?5:ms,...args);document.querySelector('#update-activate').click();});
  await page.wait("document.querySelector('#view').dataset.updateState==='unverified' && document.querySelector('#update-state').textContent.includes('Couldn’t reconnect')");
  assert.equal(await run(()=>window.__updatesTest.activations),1);
  await run(()=>{window.setTimeout=window.__originalTimeout;const t=window.__updatesTest;t.offline=false;t.status.firmware.slot='user2';t.status.firmware.version='esp-serial2wifi expected';t.status.system.uptime_ms=10;document.querySelector('#retry').click();});
  await page.wait("document.querySelector('#view').dataset.updateState==='verified'");
  assert.equal(await run(()=>window.__updatesTest.activations),1);
  await page.screenshot('firmware.png');
 });
 await check('Custom pages retain ambiguous upload selections and verify readback/removal',async()=>{
  await run(()=>location.hash='#/pages');await page.wait("document.querySelector('#view').dataset.updateState==='ready'");
  assert.equal(await run(()=>document.querySelector('#update-boot-warning')),null);
  assert.equal(await run(()=>document.querySelector('#title').textContent),'Custom web pages');
  assert.equal(await run(()=>document.querySelector('#custom-page-help').open),false);
  const posts=await run(()=>window.__updatesTest.uploads);
  await run(()=>document.querySelector('#custom-page-help').open=true);
  assert((await run(()=>document.querySelector('#custom-page-help').textContent)).includes('Uploading a page alone does not connect it to the serial device.'));
  assert.equal(await run(()=>document.querySelector('#custom-page-help a[href="#/integrations"]').textContent),'Microcontroller integration settings');
  for(const [width,scale] of [[1440,'100%'],[360,'100%'],[360,'200%']]){await viewport(width);await page.evaluate(`document.documentElement.style.fontSize='${scale}'`);await noOverflow();}
  await run(()=>document.documentElement.style.fontSize='');await viewport(1440);await run(()=>document.querySelector('#custom-page-help').scrollIntoView({block:'start'}));await page.screenshot('custom-pages-help.png');
  assert.equal(await run(()=>window.__updatesTest.uploads),posts);
  await run(()=>document.querySelector('#custom-page-help').open=false);

  await checkReloadSpacing();
  await run(()=>{const dt=new DataTransfer();dt.items.add(new File(['<p>fragment</p>'],'panel.html'));dt.items.add(new File([new Uint8Array([0,255,13,10])],'bytes.bin'));const input=document.querySelector('#update-files');input.files=dt.files;input.dispatchEvent(new Event('change'));});
  await page.wait("document.querySelector('#update-file-state').textContent.includes('Capacity and filename checks passed')");
  await run(()=>{window.__updatesTest.fail=true;const c=document.querySelector('#update-replace-confirm');c.checked=true;c.dispatchEvent(new Event('change'));document.querySelector('#update-form').requestSubmit();});
  await page.wait("document.querySelector('#view').dataset.updateState==='unverified'");
  assert.match(await run(()=>document.querySelector('#update-state').textContent),/connection closed/);
  assert.equal(await run(()=>document.querySelector('#update-files').files.length),2);
  await run(()=>{window.__updatesTest.fail=false;document.querySelector('#update-form').requestSubmit();});await page.wait("document.querySelector('#view').dataset.updateState==='verified'");
  assert.equal(await run(()=>window.__updatesTest.uploads),3);
  assert.equal(await run(()=>document.querySelector('#update-inventory a').textContent),'panel.html');
  assert.equal(await run(()=>document.querySelector('#update-pager').hidden),false);
  await run(()=>[...document.querySelectorAll('#update-pager button')].find(b=>b.textContent==='Next file').click());await page.wait("document.querySelector('#update-inventory a')?.textContent==='bytes.bin'");
  await run(()=>[...document.querySelectorAll('#update-pager button')].find(b=>b.textContent==='Previous file').click());await page.wait("document.querySelector('#update-inventory a')?.textContent==='panel.html'");
  for(const [width,scale] of [[360,1],[1440,1],[360,2]]){await viewport(width);await run(()=>scrollTo(0,0));await page.evaluate(`document.documentElement.style.fontSize='${scale*100}%'`);await noOverflow();const boxes=await run(()=>[...document.querySelectorAll('#update-pager button')].map(b=>{const r=b.getBoundingClientRect();return {top:r.top,height:r.height,left:r.left,right:r.right};}));assert(Math.abs(boxes[0].top-boxes[1].top)<1);assert(Math.abs(boxes[0].height-boxes[1].height)<1);assert(boxes[0].right<boxes[1].left);}
  await run(()=>document.documentElement.style.fontSize='');await viewport(1440);await run(()=>document.querySelector('.installed-files').scrollIntoView({block:'center'}));await page.screenshot('custom-pages.png');
  await run(()=>document.querySelector('#update-remove').click());await page.wait("document.querySelector('#update-state').textContent.includes('All custom pages removed')");assert.equal(await run(()=>window.__updatesTest.removals),1);
 });
 await check('Empty/unreadable storage uses plain status, hides pagination and separates clear action',async()=>{
  assert.equal(await run(()=>document.querySelector('#update-pager').hidden),true);
  assert.equal(await run(()=>document.querySelector('#update-remove').parentElement.hidden),true);
  assert.equal(await run(()=>document.querySelector('#update-inventory-state').textContent),'No custom pages installed.');
  await run(()=>{window.__updatesTest.pages.valid=false;document.querySelector('#update-reload').click();});
  await page.wait("document.querySelector('#update-remove').textContent==='Clear custom-page storage'");
  assert.equal(await run(()=>document.querySelector('#update-inventory-state').tagName),'P');
  assert.equal(await run(()=>document.querySelector('#update-inventory').hidden),true);
  assert.equal(await run(()=>document.querySelector('#update-pager').hidden),true);
  assert.equal(await run(()=>document.querySelector('#update-remove').parentElement.hidden),false);
  for(const width of [360,1440]){await viewport(width);await noOverflow();await run(()=>document.querySelector('.installed-files').scrollIntoView({block:'center'}));await page.screenshot('custom-pages-unreadable-'+width+'.png');}
  await run(()=>window.__updatesTest.pages.valid=true);
 });
 await check('Separate UI updates require a matching slot package and retain explicit activation',async()=>{
  await run(()=>{Object.assign(window.__updatesTest.firmware,{external_ui:true,image_abi:'ELSDK306-MAP4-CFG2',max_image_bytes:768000});location.hash='#/firmware';});
  await page.wait("document.querySelector('#update-files')?.accept==='.elu'");
  assert.equal(await run(()=>document.querySelector('#update-form button').textContent),'Upload firmware and web interface');
  assert.equal(await run(()=>document.querySelector('#update-activation').hidden),false);
  await run(()=>{const dt=new DataTransfer();dt.items.add(new File([new Uint8Array(1028)],'user2.bin'));const input=document.querySelector('#update-files');input.files=dt.files;input.dispatchEvent(new Event('change'));});
  await page.wait("document.querySelector('#update-file-state').textContent.includes('complete firmware/UI update package')");
  await run(()=>{const bytes=new Uint8Array(4096),v=new DataView(bytes.buffer),e=new TextEncoder();bytes.set(e.encode('ELUPD001'));v.setUint32(8,1028,true);v.setUint32(12,16,true);v.setUint32(16,1,true);const app=bytes.subarray(1024,2052),a=new DataView(app.buffer,app.byteOffset,app.byteLength);app.set([0xea,4,0,1]);a.setUint32(4,0x40100000,true);a.setUint32(12,16,true);app.set(e.encode('ELSDK306-MAP4-CFG2'),32);app.set(e.encode('ELUI-SHA256:'+'0'.repeat(64)),100);const dt=new DataTransfer();dt.items.add(new File([bytes],'update-user2.elu'));const input=document.querySelector('#update-files');input.files=dt.files;input.dispatchEvent(new Event('change'));});
  await page.wait("document.querySelector('#update-file-state').textContent.includes('Header and layout checks passed')");
  const before=await run(()=>window.__updatesTest.activations);
  await run(()=>{const c=document.querySelector('#update-replace-confirm');c.checked=true;c.dispatchEvent(new Event('change'));document.querySelector('#update-form').requestSubmit();});
  await page.wait("document.querySelector('#view').dataset.updateState==='uploaded'");
  assert.equal(await run(()=>window.__updatesTest.activations),before);assert.equal(await run(()=>document.querySelector('#update-activate').hidden),false);
 });
 await page.send('Page.removeScriptToEvaluateOnNewDocument',{identifier:script.identifier});await page.send('Page.reload',{ignoreCache:true});await page.wait("document.body?.dataset.ready==='true'");
}
