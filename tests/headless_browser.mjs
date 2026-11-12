/* Isolated Chromium/CDP helper for local UI tests. Never attaches to a user profile. */
import {spawn} from 'node:child_process';
import {mkdtemp, readFile, rm, writeFile} from 'node:fs/promises';
import {join, resolve} from 'node:path';
export const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

export async function launch(browser, output) {
  const profile = await mkdtemp(join(resolve(output), 'profile-'));
  const process = spawn(browser, ['--headless', '--disable-gpu', '--no-first-run',
    '--no-default-browser-check', '--disable-background-networking', '--disable-extensions',
    '--disable-component-update', '--remote-debugging-address=127.0.0.1',
    '--remote-debugging-port=0', '--user-data-dir='+profile, 'about:blank'], {stdio:['ignore','ignore','pipe']});
  let stopped=false, launchError;
  const closed=new Promise(resolve=>{
    process.once('close',()=>{stopped=true;resolve();});
    process.once('error',error=>{launchError=error;});
  });
  let diagnostics = ''; process.stderr.on('data', chunk => { diagnostics = (diagnostics+chunk).slice(-65536); });
  let socket, sequence = 0; const pending = new Map(), handlers = new Map();
  const close = async () => {
    socket?.close();
    for (const entry of pending.values()) { clearTimeout(entry.timer); entry.reject(new Error('Browser closed')); }
    pending.clear();
    if (!stopped) {
      process.kill('SIGTERM');
      await Promise.race([closed,sleep(2000)]);
      if (!stopped) {
        process.kill('SIGKILL');
        await Promise.race([closed,sleep(2000)]);
      }
    }
    await writeFile(join(output,'browser.stderr'), diagnostics);
    if (!stopped) throw new Error('Browser shutdown timed out; retained profile: '+profile);
    await rm(profile,{recursive:true,force:true,maxRetries:5,retryDelay:100});
  };
  try {
    let port;
    for (let i=0;i<200;i++) {
      if (launchError) throw launchError;
      if (stopped || process.exitCode !== null || process.signalCode !== null) throw new Error('Isolated browser exited before readiness');
      try { port = Number((await readFile(join(profile,'DevToolsActivePort'),'utf8')).split('\n')[0]); break; } catch (_) { await sleep(50); }
    }
    if (!port) throw new Error('Browser debugging endpoint did not become ready');
    const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    const target = targets.find(target=>target.type==='page');
    if (!target) throw new Error('Isolated browser has no test page');
    socket = new WebSocket(target.webSocketDebuggerUrl);
    await new Promise((resolve,reject)=>{socket.onopen=resolve;socket.onerror=reject;});
    socket.onmessage = event => {
      const message = JSON.parse(event.data);
      if (message.id) {
        const entry = pending.get(message.id); if (!entry) return;
        clearTimeout(entry.timer);pending.delete(message.id);
        if (message.error) entry.reject(new Error('CDP: '+message.error.message)); else entry.resolve(message.result);
      } else for (const handler of handlers.get(message.method)||[]) handler(message.params);
    };
    const send = (method, params={}) => new Promise((resolve,reject)=>{
      const id=++sequence;
      const timer=setTimeout(()=>{pending.delete(id);reject(new Error('Timed out: '+method));},15000);
      pending.set(id,{resolve,reject,timer}); socket.send(JSON.stringify({id,method,params}));
    });
    const evaluate = async expression => {
      const result=await send('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});
      if (result.exceptionDetails) throw new Error('Page script: '+(result.exceptionDetails.exception?.description||result.exceptionDetails.text));
      return result.result.value;
    };
    const wait = async (expression, timeout=6000) => {
      const end=Date.now()+timeout;
      while (Date.now()<end) { if (await evaluate(expression)) return; await sleep(30); }
      throw new Error('Browser readiness check timed out: '+expression);
    };
    const api={send,evaluate,wait,close,
      on(event, handler) { if (!handlers.has(event)) handlers.set(event,new Set());handlers.get(event).add(handler);return()=>handlers.get(event).delete(handler); },
      async key(key) {
        const windowsVirtualKeyCode = {Tab:9,Enter:13,Escape:27}[key] || 0;
        await send('Input.dispatchKeyEvent',{type:'keyDown',key,code:key,windowsVirtualKeyCode,
          nativeVirtualKeyCode:windowsVirtualKeyCode,text:key==='Enter'?'\r':undefined});
        await send('Input.dispatchKeyEvent',{type:'keyUp',key,code:key,windowsVirtualKeyCode,nativeVirtualKeyCode:windowsVirtualKeyCode});
      },
      async screenshot(name) { const data=await send('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});await writeFile(join(output,name),Buffer.from(data.data,'base64')); }
    };
    await send('Page.enable');await send('Runtime.enable');return api;
  } catch(error) { await close();throw error; }
}
