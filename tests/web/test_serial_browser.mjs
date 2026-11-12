import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {sleep} from '../support/headless_browser.mjs';
export async function testSerialBrowser({page,run,check,viewport,noOverflow}){
 const fixture=JSON.parse(await readFile(new URL('../support/fixtures/api-v1.json',import.meta.url)))['/api/v1/serial'];
 const mock=()=>{
  const real=window.fetch.bind(window);window.__serialOriginalFetch=real;
  window.__serialTest={config:SERIAL_FIXTURE,posts:[],reads:0,bytes:[],end:0,boot:42,epoch:0,reason:null,lost:0,mode:'normal',history:[]};
  const s=window.__serialTest;s.configFailures=1;s.config.clients=2;s.bytes=Array.from(new TextEncoder().encode('device ready\n'+'saved output\r\n'.repeat(24)+'\x1b[31mhistory-tail\x1b[0m\x1b[6n\x1b[0c\x1b[18t'));
  s.append=bytes=>{s.end=(s.end+bytes.length)>>>0;s.history.push(...bytes);s.history=s.history.slice(-1023);};
  window.fetch=async(url,options={})=>{
   const path=new URL(url,location.href).pathname,o=options,s=window.__serialTest;
   if(path==='/pgm/sync'||path==='/pgm/upload'){
    if(o.method==='POST')s.posts.push({path,url:String(url),body:o.body,raw:ArrayBuffer.isView(o.body),header:o.headers?.['X-Serial2WiFi-Request']});
    if(path==='/pgm/sync')return new Response(o.method==='POST'?null:s.mode==='sync-fail'?'FAILED to SYNC':'SYNC at 115200 baud: bootloader v8.0; readback enabled',{status:o.method==='POST'?204:200});
    if(s.mode==='program-disconnect')throw new TypeError('Synthetic upload disconnect');
    const text=new TextDecoder().decode(o.body);s.lastHex=text;
    const count=text.trim().split('\n').filter(line=>line.slice(7,9)==='00').reduce((sum,line)=>sum+parseInt(line.slice(1,3),16),0);
    return new Response('Success. '+count+' bytes at 115200 baud; readback verified '+(s.mode==='bad-readback'?0:count)+' bytes.');
   }
   if(!path.startsWith('/api/v1/serial'))return real(url,o);
   s.append(s.bytes.splice(0));
   const response=(v,status=200)=>new Response(JSON.stringify(v),{status,headers:{'Content-Type':'application/json'}});

   if(path==='/api/v1/serial/live'){
    s.liveOpens=(s.liveOpens||0)+1;
    return new Response(new ReadableStream({start(controller){
     s.stream=controller;
     s.emit=(text,reason=null,lost=0)=>{const bytes=new TextEncoder().encode(text),start=s.end;s.append(bytes);controller.enqueue(new TextEncoder().encode('event: output\ndata: '+JSON.stringify({schema:1,boot:s.boot,epoch:s.epoch,start,end:s.end,reason,lost,encoding:'base64',data:btoa(String.fromCharCode(...bytes))})+'\n\n'));};
     const saved={schema:1,boot:s.boot,epoch:s.epoch,start:(s.end-s.history.length)>>>0,end:s.end,reason:null,lost:0,encoding:'base64',data:btoa(String.fromCharCode(...s.history))};s.snapshot=saved;
     controller.enqueue(new TextEncoder().encode('event: ready\ndata: '+JSON.stringify(saved)+'\n\n'));
     const heartbeat=setInterval(()=>{try{controller.enqueue(new TextEncoder().encode(': alive\n\n'));}catch(_){}},1000);
     o.signal.addEventListener('abort',()=>{clearInterval(heartbeat);s.stream=null;try{controller.error(new DOMException('Aborted','AbortError'));}catch(_){}},{once:true});
    }}),{headers:{'Content-Type':'text/event-stream'}});
   }
   if(o.method==='POST'){
    const v=Object.fromEntries(new URLSearchParams(o.body));s.posts.push({path,v,url:String(url)});
    if(s.mode==='ambiguous')throw new TypeError('Synthetic post disconnect');
    if(path.endsWith('/send')){if(s.holdSend)await new Promise(resolve=>s.releaseSend=resolve);return response({schema:1,accepted_bytes:v.hex.length/2});}
    if(path.endsWith('/uart'))Object.assign(s.config,{baud:Number(v.baud),data_bits:Number(v.data_bits),parity:v.parity,stop_bits:v.stop_bits});
    if(path.endsWith('/clear')){s.epoch++;s.end=0;s.bytes=[];s.history=[];s.snapshot=null;s.reason='cleared';}
    return response(s.config);
   }
   if(path.includes('/capture')){
    s.reads++;if(s.mode==='failure')return response({error:{}},503);
    let bytes=s.history.slice(),start=(s.end-bytes.length)>>>0,reason=s.reason,lost=s.lost;
    const parts=path.split('/');if(parts.length===8&&!reason){const boot=Number(parts[5]),epoch=Number(parts[6]),cursor=Number(parts[7]),distance=(s.end-cursor)>>>0;if(boot!==s.boot)reason='restart';else if(epoch!==s.epoch)reason='cleared';else if(distance<=bytes.length){bytes=bytes.slice(bytes.length-distance);start=cursor;}else{reason='gap';lost=distance-bytes.length;}}
    const packet={schema:1,boot:s.boot,epoch:s.epoch,start,end:s.end,reason,lost:reason&&reason!=='gap'?null:lost,encoding:'base64',data:btoa(String.fromCharCode(...bytes))};s.reason=null;s.lost=0;s.snapshot=packet;
    return response(packet);
   }
   if(s.configFailures){s.configFailures--;return response({error:{}},503);}
   return response(s.config);
  };
  location.hash='#/serial';
 };
 const script=await page.send('Page.addScriptToEvaluateOnNewDocument',{source:'const SERIAL_FIXTURE='+JSON.stringify(fixture)+';('+mock.toString()+')();'});
 await page.send('Page.reload',{ignoreCache:true});
 await page.wait("document.querySelector('#serial-uart')?.dataset.state==='clean' && document.querySelector('#serial-terminal textarea')!==null");
 await check('Terminal recovers initial reads, opens automatically, waits for busy ports, and pauses only on request',async()=>{
  assert.equal(await run(()=>document.querySelectorAll('.terminal-toolbar button').length),1);
  assert.equal(await run(()=>document.querySelector('#serial-terminal-toggle').textContent),'Pause terminal');
  await page.wait("document.querySelector('#serial-terminal-state').textContent.includes('Serial port in use')");
  await run(()=>{const original=window.Terminal.prototype.write;window.__originalReplayWrite=original;window.Terminal.prototype.write=function(bytes,callback){window.__serialTest.replayTerminal=this;const text=typeof bytes==='string'?bytes:new TextDecoder().decode(bytes);return original.call(this,bytes,text.includes('history-tail')?()=>{window.__serialTest.finishReplay=callback;}:callback);};window.__serialTest.config.clients=0;});
  await page.wait("typeof window.__serialTest.finishReplay==='function'",8000);
  assert.equal(await run(()=>document.querySelector('#terminal-unix-send').disabled),true);
  await run(()=>{document.querySelector('#serial-terminal textarea').focus();window.__serialTest.emit('handoff-live-1\r\nhandoff-live-2');});await page.key('Enter');
  await page.wait("document.querySelector('#serial-terminal .xterm-rows').textContent.includes('handoff-live-2')");
  assert.equal(await run(()=>window.__serialTest.posts.length),0);
  // Visible rows may be mid-paint or scrolled; byte delivery is a buffer property.
  assert.equal(await run(()=>{const b=window.__serialTest.replayTerminal.buffer.active;return Array.from({length:b.length},(_,i)=>b.getLine(i).translateToString(true)).join('\n').split('handoff-live-1').length-1;}),1);
  await run(()=>{window.Terminal.prototype.write=window.__originalReplayWrite;window.__serialTest.finishReplay();});
  await page.wait("document.querySelector('.live-console').dataset.state==='live' && !!window.__serialTest.stream",8000);
  const opens=await run(()=>window.__serialTest.liveOpens);
  await run(()=>{Object.defineProperty(document,'hidden',{configurable:true,value:true});document.dispatchEvent(new Event('visibilitychange'));});
  assert.equal(await run(()=>!!window.__serialTest.stream),true);
  await run(()=>{Object.defineProperty(document,'hidden',{configurable:true,value:false});document.dispatchEvent(new Event('visibilitychange'));document.querySelector('#serial-terminal-toggle').click();});
  await page.wait("document.querySelector('.live-console').dataset.state==='paused'");await sleep(200);
  assert.equal(await run(()=>window.__serialTest.liveOpens),opens);assert.equal(await run(()=>window.__serialTest.stream),null);
  await run(()=>document.querySelector('#serial-terminal-toggle').click());await page.wait("document.querySelector('.live-console').dataset.state==='live' && !!window.__serialTest.stream");
  assert.equal(await run(()=>window.__serialTest.posts.filter(p=>p.path==='/api/v1/serial/send').length),0);
 });
 await check('Terminal options separators and collapsed spacing match the other tools',async()=>{
  for(const [width,scale] of [[1440,1],[768,1],[360,1],[360,2]]){
   await viewport(width);await run(()=>document.documentElement.style.fontSize='');
   if(scale===2)await run(()=>document.documentElement.style.fontSize='200%');
   for(const open of [false,true]){
    await run(()=>document.querySelector('.terminal-options').open=false);
    if(open)await run(()=>document.querySelector('.terminal-options').open=true);
    const bounds=await run(()=>{
     const options=document.querySelector('.terminal-options'),next=document.querySelector('#serial-uart-details'),other=document.querySelector('#serial-tcp');
     const o=options.getBoundingClientRect(),n=next.getBoundingClientRect(),t=other.getBoundingClientRect(),summary=options.querySelector('summary').getBoundingClientRect();
     const inputs=[...options.querySelectorAll('input')].map(e=>e.getBoundingClientRect()),button=options.querySelector('button').getBoundingClientRect();
     return {left:Math.abs(o.left-n.left),right:Math.abs(o.right-n.right),gap:n.top-o.bottom,below:n.top-summary.bottom,normal:t.top-next.querySelector('summary').getBoundingClientRect().bottom,rem:parseFloat(getComputedStyle(document.documentElement).fontSize),aligned:button.left>=inputs.at(-1).right?Math.abs(button.bottom-inputs.at(-1).bottom):0};
    });
    assert(bounds.left<1&&bounds.right<1&&Math.abs(bounds.gap)<1,JSON.stringify(bounds));
    if(!open)assert(Math.abs(bounds.below-bounds.normal)<1,JSON.stringify(bounds));
    else assert(bounds.aligned<1,JSON.stringify(bounds));
    await noOverflow();await run(()=>document.querySelector('.terminal-options').scrollIntoView({block:'center'}));
    await page.screenshot(`terminal-options-${width}-${scale}-${open?'open':'closed'}.png`);
   }
  }
  await run(()=>{document.documentElement.style.fontSize='';document.querySelector('.terminal-options').open=false;});await viewport(1440);
 });
 await check('Saved output renders in xterm with historical replies muted and no second display',async()=>{
  await page.wait("document.querySelector('#serial-terminal .xterm-rows').textContent.includes('history-tail')");
  assert.equal(await run(()=>document.querySelector('#serial-recent')),null);
  assert.equal(await run(()=>window.__serialTest.posts.length),0);
  assert.equal(await run(()=>document.querySelector('#serial-terminal .xterm-rows').textContent.includes('[0x1b]')),false);
 });
 await run(()=>document.querySelector('#serial-raw-capture').open=true);
 await page.wait("document.querySelector('#serial-output')?.textContent.includes('device ready')");
 await check('Serial loads actual settings, measured endpoints and plain-text capture',async()=>{
  await run(()=>document.querySelector('#serial-raw-capture').open=true);
  assert.equal(await run(()=>document.querySelector('#serial-baud').value),'115200');
  assert.equal(await run(()=>document.querySelector('#serial-parity').value),'N');
  assert((await run(()=>document.querySelector('#serial-tcp').textContent)).includes('Serial console — 192.168.4.1:23'));
  assert((await run(()=>document.querySelector('#serial-tcp').textContent)).includes('MCU programming — 192.168.4.1:2323'));
  assert(!(await run(()=>document.querySelector('#serial-tcp').textContent)).includes('127.0.0.1'));
  assert.equal(await run(()=>document.querySelector('#serial-tcp pre').textContent),'telnet -8 192.168.4.1 23');
  assert.equal(await run(()=>window.__serialTest.posts.length),0);
  await page.wait("document.querySelector('#serial-terminal textarea')!==null");
  assert((await run(()=>document.querySelector('#serial-terminal .xterm-rows').textContent)).includes('history-tail'));
  await run(()=>window.__serialTest.bytes.push(...new TextEncoder().encode('<img src=x onerror=alert(1)>\n')));
  await page.wait("document.querySelector('#serial-output').textContent.includes('<img')");assert.equal(await run(()=>document.querySelectorAll('#view img').length),0);
 });
 await check('Capture gap, local clear and device clear have distinct semantics',async()=>{
  await run(()=>{const s=window.__serialTest;s.end+=27;s.reason='gap';s.lost=27;s.bytes.push(65);});
  await page.wait("document.querySelector('#serial-output').textContent.includes('27 bytes no longer available')");
  await run(()=>[...document.querySelectorAll('#view button')].find(b=>b.textContent==='Clear browser view').click());
  assert.equal(await run(()=>document.querySelector('#serial-output').textContent),'');assert.equal(await run(()=>window.__serialTest.posts.length),0);
  await run(()=>{window.__confirm=window.confirm;window.confirm=()=>true;[...document.querySelectorAll('#view button')].find(b=>b.textContent==='Clear device capture').click();});
  await page.wait("document.querySelector('#serial-output').textContent.includes('Device capture was cleared')");
  assert.equal(await run(()=>window.__serialTest.posts.at(-1).path),'/api/v1/serial/clear');
 });
 await check('Command Enter/Send, UTF-8 suffix and history preserve an unsent draft',async()=>{
  await run(()=>document.querySelector('#serial-raw-capture').open=true);
  await run(()=>{const input=document.querySelector('#serial-command-text');input.value='é';input.focus();});await page.key('Enter');
  await page.wait("document.querySelector('#serial-command-text').value==='' && !document.querySelector('#serial-command [type=submit]').disabled");
  assert.equal(await run(()=>window.__serialTest.posts.at(-1).v.hex),'c3a90d0a');
  await run(()=>{document.querySelector('#serial-command-text').value='second';document.querySelector('#serial-command').requestSubmit();});
  await page.wait("document.querySelector('#serial-command-text').value==='' && !document.querySelector('#serial-command [type=submit]').disabled");
  await run(()=>{const input=document.querySelector('#serial-command-text');input.value='unsent';input.dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowUp',bubbles:true}));});
  assert.equal(await run(()=>document.querySelector('#serial-command-text').value),'second');
  await run(()=>document.querySelector('#serial-command-text').dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowDown',bubbles:true})));
  assert.equal(await run(()=>document.querySelector('#serial-command-text').value),'unsent');
  await run(()=>{window.__serialTest.mode='ambiguous';document.querySelector('#serial-command').requestSubmit();});
  await page.wait("document.querySelector('#serial-command p').textContent.includes('may have been sent')");
  assert.equal(await run(()=>document.querySelector('#serial-command-text').value),'unsent');
  await run(()=>{window.__serialTest.mode='normal';document.querySelector('#serial-command-text').value='';});
  assert.equal(await run(()=>JSON.stringify({...localStorage}).includes('unsent')),false);
 });
 await check('Scrolling pauses following while capture continues; resume reaches the tail',async()=>{
  await run(()=>window.__serialTest.bytes.push(...new TextEncoder().encode('line\n'.repeat(150))));
  await page.wait("document.querySelector('#serial-output').scrollHeight>document.querySelector('#serial-output').clientHeight");
  await run(()=>{const pre=document.querySelector('#serial-output');pre.scrollTop=0;pre.dispatchEvent(new Event('scroll'));});
  await page.wait("document.querySelector('#view [aria-pressed]')?.getAttribute('aria-pressed')==='false'");
  await run(()=>window.__serialTest.bytes.push(...new TextEncoder().encode('new-tail\n')));
  await page.wait("document.querySelector('#serial-output').textContent.includes('new-tail')");
  assert.equal(await run(()=>document.querySelector('#serial-output').scrollTop),0);
  await run(()=>document.querySelector('#view [aria-pressed]').click());
  assert(await run(()=>{const p=document.querySelector('#serial-output');return p.scrollHeight-p.clientHeight-p.scrollTop<5;}));
 });
 await check('UART Save verifies readback and MCU reset names a separate target',async()=>{
  await run(()=>{document.querySelector('#serial-uart-details').open=true;document.querySelector('#serial-mcu-tools').open=true;});
  await run(()=>{document.querySelector('#serial-uart-details').open=true;const f=document.querySelector('#serial-uart');f.elements.parity.value='O';f.elements.stop_bits.value='2';f.dispatchEvent(new Event('input'));f.requestSubmit();});
  await page.wait("document.querySelector('#serial-uart').dataset.state==='saved'");
  assert.equal(await run(()=>window.__serialTest.config.parity),'O');
  await run(()=>[...document.querySelectorAll('#view button')].find(b=>b.textContent==='Reset attached MCU').click());
  await page.wait("window.__serialTest.posts.at(-1)?.path==='/api/v1/serial/reset-mcu'");
  assert.equal(await run(()=>window.__serialTest.posts.some(p=>p.path==='/log/reset')),false);
 });
 await check('Capture stops after bounded failures and resumes on explicit retry',async()=>{
  await run(()=>window.__serialTest.mode='failure');await page.wait("document.querySelector('#serial-capture-state').textContent.includes('Click Retry capture')");
  const reads=await run(()=>window.__serialTest.reads);await sleep(800);assert.equal(await run(()=>window.__serialTest.reads),reads);
  await run(()=>{window.__serialTest.mode='normal';[...document.querySelectorAll('#view button')].find(b=>b.textContent==='Retry capture').click();});
  await page.wait("document.querySelector('#serial-capture-state').textContent.includes('Capture reachable')");
 });
 await check('Programmer validates a file before reset and sends raw staged HEX with readback confirmation',async()=>{
  await run(()=>{const f=document.querySelector('#serial-program-file'),d=new DataTransfer();d.items.add(new File(['not a HEX file'],'bad.hex'));f.files=d.files;f.dispatchEvent(new Event('change'));});
  await page.wait("document.querySelector('#serial-program-file-info').textContent.includes('Invalid')");
  assert.equal(await run(()=>window.__serialTest.posts.filter(p=>p.path.startsWith('/pgm/')).length),0);
  assert.equal(await run(()=>document.querySelector('#serial-program-start').disabled),true);
  await run(()=>{const f=document.querySelector('#serial-program-file'),d=new DataTransfer();d.items.add(new File([':10000000000102030405060708090A0B0C0D0E0F78\n:00000001FF\n'],'app.hex'));f.files=d.files;f.dispatchEvent(new Event('change'));});
  await page.wait("!document.querySelector('#serial-program-start').disabled");
  assert((await run(()=>document.querySelector('#serial-program-file-info').textContent)).includes('128 bytes to write'));
  await run(()=>document.querySelector('#serial-program-start').click());
  await page.wait("document.querySelector('#serial-program-state').dataset.state==='complete'");
  const records=await run(()=>window.__serialTest.posts.filter(p=>p.path.startsWith('/pgm/')).map(p=>({path:p.path,raw:p.raw,header:p.header})));
  assert.deepEqual(records.map(p=>p.path),['/pgm/sync','/pgm/upload']);assert(records[1].raw);assert.equal(records[1].header,'1');
  assert((await run(()=>window.__serialTest.lastHex)).endsWith(':00000001FF\n'));
  assert((await run(()=>document.querySelector('#serial-program-state').textContent)).includes('read back 128'));
 });
 await check('Programming sync failure and ambiguous upload retain distinct terminal outcomes without retries',async()=>{
  await page.wait("!document.querySelector('#serial-program-start').disabled");
  let uploads=await run(()=>window.__serialTest.posts.filter(p=>p.path==='/pgm/upload').length);
  await run(()=>{window.__serialTest.mode='sync-fail';document.querySelector('#serial-program-start').click();});
  await page.wait("document.querySelector('#serial-program-state').dataset.state==='failed' && !document.querySelector('#serial-program-start').disabled");
  assert.equal(await run(()=>window.__serialTest.posts.filter(p=>p.path==='/pgm/upload').length),uploads);
  await run(()=>{window.__serialTest.mode='program-disconnect';document.querySelector('#serial-program-start').click();});
  await page.wait("document.querySelector('#serial-program-state').dataset.state==='unverified' && !document.querySelector('#serial-program-start').disabled");
  assert.equal(await run(()=>window.__serialTest.posts.filter(p=>p.path==='/pgm/upload').length),uploads+1);
  assert((await run(()=>document.querySelector('#serial-program-state').textContent)).includes('incomplete image'));
  assert.equal(await run(()=>document.querySelector('#serial-program-file').files[0].name),'app.hex');
  await run(()=>window.__serialTest.mode='normal');
 });
 await check('External programmer guide selects the target protocol and retains verification',async()=>{
  await run(()=>{document.querySelector('#serial-external-guide').open=true;const s=document.querySelector('#serial-guide-target');s.value='mega';s.dispatchEvent(new Event('change'));});
  const command=await run(()=>document.querySelector('#serial-guide-command').textContent);
  assert(command.includes('-p atmega2560 -c wiring'));assert(command.includes('-P net:192.168.4.1:2323'));assert(!command.includes('-V'));
 });

 await check('Live terminal renders ANSI/OSC safely and forwards immediate keys and terminal replies',async()=>{
  await viewport(1440);
  await page.wait("document.querySelector('#serial-terminal textarea')!==null");
  await run(()=>{const s=window.__serialTest;s.mode='normal';s.config.clients=0;s.config.programming=false;document.querySelector('#serial-command-text').value='';document.querySelector('#serial-raw-capture').open=false;});
  await page.wait("document.querySelector('.live-console').dataset.state==='live' && !!window.__serialTest.stream");
  const before=await run(()=>window.__serialTest.posts.length);
  await run(()=>document.querySelector('#serial-terminal').scrollIntoView({block:'center'}));
  await run(()=>window.__serialTest.emit('\x1b[2J\x1b[H\x1b]3008;machineid=PRIVATE_METADATA\x1b\\\x1bP+q6E616D65\x1b\\\x1b[31mhello\x1b[0m\r\n<img src=x onerror=alert(1)>\r\n\x1b[6n\x1b[0c'));
  await page.wait("document.querySelector('#serial-terminal .xterm-rows')?.textContent.includes('hello')");
  const text=await run(()=>document.querySelector('#serial-terminal .xterm-rows').textContent);
  assert(!text.includes('PRIVATE_METADATA'));assert(!text.includes('[0x1b]'));assert(text.includes('<img'));assert.equal(await run(()=>document.querySelectorAll('#serial-terminal img').length),0);
  await page.wait(`window.__serialTest.posts.length>${before}`);
  assert.match(await run(()=>window.__serialTest.posts.slice(-2).map(p=>p.v?.hex||'').join('')),/1b5b/);
  await run(()=>document.querySelector('#serial-terminal textarea').focus());await page.send('Input.dispatchKeyEvent',{type:'keyDown',key:'x',code:'KeyX',windowsVirtualKeyCode:88,text:'x'});await page.send('Input.dispatchKeyEvent',{type:'keyUp',key:'x',code:'KeyX',windowsVirtualKeyCode:88});
  await page.wait("window.__serialTest.posts.some(p=>p.v?.hex==='78')");

  for(const event of [{key:'ArrowUp',code:'ArrowUp',windowsVirtualKeyCode:38},{key:'c',code:'KeyC',windowsVirtualKeyCode:67,modifiers:2}]){
   await page.send('Input.dispatchKeyEvent',{type:'keyDown',...event});await page.send('Input.dispatchKeyEvent',{type:'keyUp',...event});
  }
  await page.wait("window.__serialTest.posts.some(p=>p.v?.hex==='1b5b41') && window.__serialTest.posts.some(p=>p.v?.hex==='03')");
  await run(()=>window.__serialTest.emit('\x1b[?2004hpaste ready'));
  await page.wait("document.querySelector('#serial-terminal .xterm-rows').textContent.includes('paste ready')");
  await run(()=>{const data=new DataTransfer();data.setData('text/plain','paste 雪');document.querySelector('#serial-terminal textarea').dispatchEvent(new ClipboardEvent('paste',{clipboardData:data,bubbles:true,cancelable:true}));});
  await page.wait("window.__serialTest.posts.some(p=>p.v?.hex==='1b5b3230307e706173746520e99baa1b5b3230317e')");
  await page.screenshot('terminal-live.png');
 });
 await check('Resize is local, Unix assistance uses applied dimensions, and live size queries receive exact replies',async()=>{
  const before=await run(()=>window.__serialTest.posts.length);
  await run(()=>{document.querySelector('.terminal-options').open=true;document.querySelector('#terminal-unix').open=true;document.querySelector('#terminal-columns').value=120;document.querySelector('#terminal-rows').value=40;document.querySelector('#terminal-columns').form.requestSubmit();});
  await page.wait("document.querySelector('#serial-terminal .xterm-rows').children.length===40");
  assert.equal(await run(()=>document.querySelector('#terminal-unix-command').textContent),'stty rows 40 cols 120');
  await run(()=>{document.querySelector('#terminal-columns').value=121;});
  assert.equal(await run(()=>document.querySelector('#terminal-unix-command').textContent),'stty rows 40 cols 120');
  await run(()=>{document.querySelector('#terminal-columns').value=0;document.querySelector('#terminal-columns').form.requestSubmit();});
  assert.equal(await run(()=>document.querySelector('#terminal-unix-command').textContent),'stty rows 40 cols 120');
  await run(()=>{document.querySelector('#terminal-columns').value=120;document.querySelector('#terminal-columns').form.requestSubmit();window.__copiedCommand=null;Object.defineProperty(navigator,'clipboard',{configurable:true,value:{writeText:async text=>{window.__copiedCommand=text;}}});document.querySelector('#terminal-unix-copy').click();});
  await page.wait("window.__copiedCommand==='stty rows 40 cols 120'");
  await sleep(100);assert.equal(await run(()=>window.__serialTest.posts.length),before);
  await page.send('Browser.grantPermissions',{origin:await run(()=>location.origin),permissions:['clipboardReadWrite','clipboardSanitizedWrite']});
  await run(async()=>{delete navigator.clipboard;window.__nativeClipboard=navigator.clipboard;await window.__nativeClipboard.writeText('clipboard sentinel');Object.defineProperty(navigator,'clipboard',{configurable:true,value:undefined});document.querySelector('#terminal-unix-copy').focus();});
  await page.key('Enter');
  assert.equal(await run(()=>window.__nativeClipboard.readText()),'stty rows 40 cols 120');
  assert.equal(await run(()=>document.activeElement.id),'terminal-unix-copy');
  assert.equal(await run(()=>window.__serialTest.posts.length),before);
  await run(()=>{delete navigator.clipboard;delete window.__nativeClipboard;});await page.send('Browser.resetPermissions');
  await run(()=>window.__serialTest.emit('\x1b[18t'));
  await page.wait(`window.__serialTest.posts.slice(${before}).some(p=>p.v?.hex==='1b5b383b34303b31323074')`);
  await sleep(100);assert.equal(await run(()=>window.__serialTest.posts.length),before+1);
  const after=before+1;
  await run(()=>window.__serialTest.emit('\x1b[14t\x1b[16t\x1b[8;12;50t'));
  await sleep(100);assert.equal(await run(()=>window.__serialTest.posts.length),after);
  assert.equal(await run(()=>document.querySelector('#terminal-unix-command').textContent),'stty rows 40 cols 120');
  for(const width of [360,1440]){await viewport(width);await noOverflow();await run(()=>document.querySelector('#terminal-unix').scrollIntoView({block:'start'}));await page.screenshot('terminal-unix-'+width+'.png');}
  await run(()=>{delete navigator.clipboard;document.querySelector('#terminal-columns').value=80;document.querySelector('#terminal-rows').value=24;document.querySelector('#terminal-columns').form.requestSubmit();document.querySelector('#terminal-unix').open=false;document.querySelector('.terminal-options').open=false;});
 });
 await check('Send to Unix shell sends the applied command plus CR once, gates busy/paused/raw modes, and restores terminal focus',async()=>{
  await run(()=>{document.querySelector('.terminal-options').open=true;document.querySelector('#terminal-unix').open=true;document.querySelector('#terminal-columns').value=120;document.querySelector('#terminal-rows').value=40;document.querySelector('#terminal-columns').form.requestSubmit();});
  await page.wait("!document.querySelector('#terminal-unix-send').disabled");
  const before=await run(()=>window.__serialTest.posts.length);
  await run(()=>{document.querySelector('#terminal-columns').value=121;document.querySelector('#terminal-unix').open=false;document.querySelector('#terminal-unix').open=true;});
  await sleep(50);assert.equal(await run(()=>window.__serialTest.posts.length),before);
  await run(()=>{window.__serialTest.holdSend=true;document.querySelector('#terminal-unix-send').click();});
  await page.wait("typeof window.__serialTest.releaseSend==='function'");
  assert.equal(await run(()=>document.activeElement===document.querySelector('#serial-terminal textarea')),true);
  assert.equal(await run(()=>document.querySelector('#terminal-unix-send').disabled),true);
  const hex=Buffer.from('stty rows 40 cols 120\r').toString('hex');
  assert.equal(await run(()=>window.__serialTest.posts.at(-1).v.hex),hex);
  await run(()=>document.querySelector('#terminal-unix-send').dispatchEvent(new Event('click')));
  assert.equal(await run(()=>window.__serialTest.posts.length),before+1);
  await run(()=>{window.__serialTest.holdSend=false;window.__serialTest.releaseSend();delete window.__serialTest.releaseSend;});
  await page.wait("!document.querySelector('#terminal-unix-send').disabled");
  for(const mode of ['pause','raw']){
   await page.evaluate(mode==='pause'?"document.querySelector('#serial-terminal-toggle').click()":"document.querySelector('#serial-raw-capture').open=true");
   await page.wait("document.querySelector('#terminal-unix-send').disabled");
   await run(()=>document.querySelector('#terminal-unix-send').dispatchEvent(new Event('click')));assert.equal(await run(()=>window.__serialTest.posts.length),before+1);
   await page.evaluate(mode==='pause'?"document.querySelector('#serial-terminal-toggle').click()":"document.querySelector('#serial-raw-capture').open=false");
   await page.wait("!document.querySelector('#terminal-unix-send').disabled");
  }
  // A lost acknowledgment pauses input and never retries a shell command.
  await run(()=>{window.__serialTest.mode='ambiguous';document.querySelector('#terminal-unix-send').click();});
  await page.wait("document.querySelector('.live-console').dataset.state==='paused'");await sleep(100);
  assert.equal(await run(()=>window.__serialTest.posts.length),before+2);
  assert.equal(await run(()=>document.querySelector('#terminal-unix-send').disabled),true);
  await run(()=>{window.__serialTest.mode='normal';document.querySelector('#serial-terminal-toggle').click();});await page.wait("!document.querySelector('#terminal-unix-send').disabled");
  await run(()=>{document.querySelector('#terminal-columns').value=80;document.querySelector('#terminal-rows').value=24;document.querySelector('#terminal-columns').form.requestSubmit();document.querySelector('#terminal-unix').open=false;document.querySelector('.terminal-options').open=false;});
 });
 await check('Navigation retains terminal screen, size and browser-only settings without sending input',async()=>{
  await run(()=>{window.__serialTest.emit('retained-screen-marker');document.querySelector('#terminal-columns').value=100;document.querySelector('#terminal-rows').value=30;document.querySelector('#terminal-columns').form.requestSubmit();});
  await page.wait("document.querySelector('#serial-terminal .xterm-rows').textContent.includes('retained-screen-marker')");
  const count=await run(()=>window.__serialTest.posts.length);
  await run(()=>location.hash='#/dashboard');await page.wait("document.title.includes('Device overview')");
  assert.equal(await run(()=>window.__serialTest.stream),null);
  await run(()=>window.__serialTest.append(new TextEncoder().encode('background-output')));
  await run(()=>location.hash='#/serial');await page.wait("document.querySelector('#serial-terminal-state')?.textContent.startsWith('Live')");
  await run(()=>document.querySelector('#serial-terminal').scrollIntoView({block:'center'}));
  await page.wait("document.querySelector('#serial-terminal .xterm-rows').textContent.includes('retained-screen-marker')");
  assert.equal(await run(()=>document.querySelector('#terminal-columns').value),'100');
  assert.equal(await run(()=>document.querySelector('#terminal-unix-command').textContent),'stty rows 30 cols 100');
  await page.wait("document.querySelector('#serial-terminal .xterm-rows').textContent.includes('background-output')");
  assert.equal(await run(()=>document.querySelectorAll('#serial-terminal .xterm-rows>div').length),30);
  assert.equal(await run(()=>window.__serialTest.posts.length),count);
  await run(()=>{document.querySelector('#terminal-columns').value=80;document.querySelector('#terminal-rows').value=24;document.querySelector('#terminal-columns').form.requestSubmit();});
 });
 await check('Expired history resumes in one terminal with a gap notice and muted historical queries',async()=>{
  const count=await run(()=>window.__serialTest.posts.length);
  await run(()=>location.hash='#/dashboard');await page.wait("document.title.includes('Device overview')");
  await run(()=>window.__serialTest.append(new TextEncoder().encode('partial-old-line'.repeat(150)+'\r\n\x1b[32mgap-tail\x1b[0m\x1b[6n\x1b[18t')));
  await run(()=>location.hash='#/serial');await page.wait("document.querySelector('.live-console')?.dataset.state==='live'");
  await run(()=>document.querySelector('#serial-terminal').scrollIntoView({block:'center'}));
  await page.wait("document.querySelector('#serial-terminal .xterm-rows').textContent.includes('gap-tail')");
  assert((await run(()=>document.querySelector('#serial-terminal .xterm-rows').textContent)).includes('Output interrupted'));
  assert.equal(await run(()=>window.__serialTest.posts.length),count);
  assert.equal(await run(()=>document.querySelector('#serial-recent')),null);
 });
 await check('Raw hex preserves binary bytes and never answers terminal queries',async()=>{
  await run(()=>{document.querySelector('#serial-raw-capture').open=true;const f=document.querySelector('#serial-raw-format');f.value='hex';f.dispatchEvent(new Event('change'));});
  await page.wait("window.__serialTest.stream===null");
  const count=await run(()=>window.__serialTest.posts.length);
  await run(()=>window.__serialTest.bytes.push(0,255,27,91,54,110,27,91,49,56,116));
  await page.wait("document.querySelector('#serial-output').textContent.includes('00 ff 1b 5b 36 6e')");
  assert.equal(await run(()=>window.__serialTest.posts.length),count);
  await run(()=>{document.querySelector('#serial-command-text').value='00 FF 1B';document.querySelector('#serial-command').requestSubmit();});
  await page.wait(`window.__serialTest.posts.length>${count}`);
  assert.equal(await run(()=>window.__serialTest.posts.at(-1).v.hex),'00ff1b');
  await page.wait("document.querySelector('#serial-command-text').value===''");
  const sent=await run(()=>window.__serialTest.posts.length);
  await run(()=>location.hash='#/dashboard');await page.wait("document.title.includes('Device overview')");
  await run(()=>location.hash='#/serial');await page.wait("document.querySelector('#serial-uart')?.dataset.state==='clean'");
  assert.equal(await run(()=>document.querySelector('#serial-raw-capture').open),true);
  assert.equal(await run(()=>document.querySelector('#serial-raw-format').value),'hex');
  assert.equal(await run(()=>window.__serialTest.stream),null);
  assert.equal(await run(()=>window.__serialTest.posts.length),sent);
  await run(()=>document.querySelector('#serial-raw-capture').open=false);
  await page.wait("document.querySelector('.live-console').dataset.state==='live'");
 });
 await check('Bottom-row terminal input stays inside one scrollback viewport at fractional DPI',async()=>{
  await run(()=>window.__serialTest.emit('\r\n'.repeat(40)+'\x1b[24;1Hbottom row'));
  await page.wait("document.querySelector('#serial-terminal .xterm-rows').textContent.includes('bottom row')");
  for(const width of [1440,360]){
   await page.send('Emulation.setDeviceMetricsOverride',{width,height:900,deviceScaleFactor:1.25,mobile:false});
   await run(()=>document.querySelector('#serial-terminal textarea').focus());
   await page.wait("Math.abs(document.querySelector('#serial-terminal .xterm').getBoundingClientRect().width-document.querySelector('#serial-terminal .xterm-screen').getBoundingClientRect().width-(document.querySelector('#serial-terminal .xterm-viewport').offsetWidth-document.querySelector('#serial-terminal .xterm-viewport').clientWidth))<2");
   const layout=await run(()=>{const host=document.querySelector('#serial-terminal'),screen=host.querySelector('.xterm-screen'),input=host.querySelector('textarea'),view=host.querySelector('.xterm-viewport'),row=host.querySelector('.xterm-rows>div'),a=input.getBoundingClientRect(),b=screen.getBoundingClientRect();return {height:a.height,rowHeight:row.getBoundingClientRect().height,inputBottom:a.bottom,screenBottom:b.bottom,outerScrollTop:host.scrollTop,viewportScrolls:view.scrollHeight>view.clientHeight,overflowY:getComputedStyle(host).overflowY};});
   assert.ok(layout.height<=layout.rowHeight+1,JSON.stringify(layout));assert.ok(layout.inputBottom<=layout.screenBottom+1,JSON.stringify(layout));assert.equal(layout.outerScrollTop,0);assert.equal(layout.viewportScrolls,true);assert.equal(layout.overflowY,'hidden');await noOverflow();
  }
  const count=await run(()=>window.__serialTest.posts.length);
  await page.send('Input.dispatchKeyEvent',{type:'keyDown',key:'g',code:'KeyG',windowsVirtualKeyCode:71,text:'g'});await page.send('Input.dispatchKeyEvent',{type:'keyUp',key:'g',code:'KeyG',windowsVirtualKeyCode:71});
  await page.wait(`window.__serialTest.posts.slice(${count}).some(p=>p.v?.hex==='67')`);await page.screenshot('terminal-bottom-row.png');await viewport(1440);
 });
 await check('Stream reading continues when terminal paint callbacks are delayed',async()=>{
  await run(()=>{const original=window.Terminal.prototype.write;window.__restoreWrite=original;window.Terminal.prototype.write=function(bytes,callback){return original.call(this,bytes,callback?()=>setTimeout(callback,200):undefined);};for(let i=0;i<20;i++)window.__serialTest.emit('\r\nrender-burst-'+i);});
  try{await page.wait("document.querySelector('#serial-terminal .xterm-rows').textContent.includes('render-burst-19')",1500);}finally{await run(()=>{window.Terminal.prototype.write=window.__restoreWrite;delete window.__restoreWrite;});}
 });
 await check('Live terminal reconnects output automatically; ambiguous input pauses without retry',async()=>{
  await run(()=>window.__serialTest.emit('partial','gap',187));
  await page.wait("document.querySelector('#serial-terminal-state').textContent.includes('Output interrupted')");
  assert.equal(await run(()=>window.__serialTest.stream),null);
  await page.wait("document.querySelector('.live-console').dataset.state==='live' && !!window.__serialTest.stream");
  const count=await run(()=>window.__serialTest.posts.length);
  await run(()=>{window.__serialTest.mode='ambiguous';document.querySelector('#serial-terminal textarea').focus();});await page.send('Input.dispatchKeyEvent',{type:'keyDown',key:'q',code:'KeyQ',windowsVirtualKeyCode:81,text:'q'});await page.send('Input.dispatchKeyEvent',{type:'keyUp',key:'q',code:'KeyQ',windowsVirtualKeyCode:81});
  await page.wait("document.querySelector('#serial-terminal-state').textContent.includes('didn’t reply')");
  await sleep(150);assert.equal(await run(()=>window.__serialTest.posts.length),count+1);
  await run(()=>window.__serialTest.mode='normal');
 });
 await check('Serial dirty navigation guard',async()=>{
  await run(()=>{document.querySelector('#serial-raw-capture').open=true;document.querySelector('#serial-command-text').value='keep-draft';window.confirm=()=>false;location.hash='#/dashboard';});
  await page.wait("location.hash==='#/serial'");assert.equal(await run(()=>document.querySelector('#serial-command-text').value),'keep-draft');
  await run(()=>{window.confirm=()=>true;location.hash='#/dashboard';});await page.wait("document.querySelector('[data-card=wifi]')!==null");
  await run(()=>{window.confirm=window.__confirm;window.fetch=window.__serialOriginalFetch;});
 });
 await check('Reload restores raw hex without opening a live terminal or answering saved queries',async()=>{
  await page.send('Page.reload',{ignoreCache:true});
  await page.wait("document.querySelector('#serial-uart')?.dataset.state==='clean' && document.querySelector('#serial-output')?.textContent.split(String.fromCharCode(10)).join(' ').includes('1b 5b 36 6e')");
  assert.equal(await run(()=>document.querySelector('#serial-raw-capture').open),true);
  assert.equal(await run(()=>document.querySelector('#serial-raw-format').value),'hex');
  assert.equal(await run(()=>window.__serialTest.liveOpens||0),0);
  assert.equal(await run(()=>window.__serialTest.posts.length),0);
  await viewport(360);await run(()=>document.querySelector('#serial-raw-format').scrollIntoView());await noOverflow();await page.screenshot('serial-raw-hex.png');
 });
 await page.send('Page.removeScriptToEvaluateOnNewDocument',{identifier:script.identifier});
}

export async function testSerialRecovery({page,run,check,fresh,hold,release,count,stage}) {
 await fresh('serial');await page.wait("document.querySelector('#serial-uart')?.dataset.state==='clean'");
 await check('Serial reset owns mutation controls across refresh and cancels queued work on departure',async()=>{
  const key='POST /api/v1/serial/reset-mcu';await hold(key);
  await run(()=>document.querySelector('#serial-mcu-tools').open=true);
  await run(()=>[...document.querySelectorAll('button')].find(b=>b.textContent==='Reset attached MCU').click());await page.wait("!!t.pending['POST /api/v1/serial/reset-mcu']");
  assert.equal(await run(()=>document.querySelector('#serial-uart [type=submit]').disabled),true);
  assert.equal(await run(()=>[...document.querySelectorAll('button')].find(b=>b.textContent==='Clear device capture').disabled),true);
  await run(()=>document.querySelector('#refresh').click());await sleep(30);
  assert.equal(await count(key),1);
  await run(()=>location.hash='#/network');await release(key);await page.wait("document.querySelector('#network-ap')?.dataset.state==='clean'");
  assert.equal(await run(()=>document.querySelector('#title').textContent),'Network');
 });
 await check('Leaving Serial cancels a queued reset before transport dispatch',async()=>{
  await fresh('serial');await page.wait("document.querySelector('#serial-uart')?.dataset.state==='clean' && !document.querySelector('#refresh').disabled");
  const key='GET /api/v1/status';await hold(key);await run(()=>document.querySelector('#refresh').click());await page.wait("!!t.pending['GET /api/v1/status']");
  await run(()=>{[...document.querySelectorAll('button')].find(b=>b.textContent==='Reset attached MCU').click();location.hash='#/dashboard';});
  await page.wait("document.querySelector('#title').textContent==='Device overview'");await release(key);await sleep(50);
  assert.equal(await count('POST /api/v1/serial/reset-mcu'),0);
 });
 // Cause one real stylesheet load failure, then retry by remounting the failed instance.
 await page.send('Fetch.enable',{patterns:[{urlPattern:'*xterm.css*',requestStage:'Request'}]});let failCSS=true;
 const offCSS=page.on('Fetch.requestPaused',p=>{page.send(failCSS?'Fetch.failRequest':'Fetch.continueRequest',failCSS?{requestId:p.requestId,errorReason:'Failed'}:{requestId:p.requestId}).catch(()=>{});failCSS=false;});
 await fresh('serial');
 await check('Failed terminal initialization recovers on remount',async()=>{
  await page.wait("document.querySelector('.live-console')?.dataset.state==='failed'");
  await run(()=>location.hash='#/dashboard');await page.wait("document.querySelector('#title').textContent==='Device overview'");
  await run(()=>location.hash='#/serial');await page.wait("!!document.querySelector('#serial-terminal textarea')");
  assert.notEqual(await run(()=>document.querySelector('.live-console').dataset.state),'failed');
 });
 await page.send('Fetch.disable');offCSS();
}
