import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {validNetwork,validScan,validateNetwork,draftFrom,matchesNetwork,bodyFor,NetworkTransaction} from '../web/app/network-model.js';
const fixtures=JSON.parse(await readFile(new URL('./fixtures/api-v1.json',import.meta.url)));
const initial=fixtures['/api/v1/network'],baseStatus=fixtures['/api/v1/status'];
assert(validNetwork(initial));assert(!validNetwork({...initial,operation:{id:-1,state:'applied'}}));
let draft=draftFrom('station',initial);draft.ssid='new&+%雪';draft.password_action='replace';draft.password='correct horse';
assert.deepEqual(validateNetwork('station',draft),{});
assert.equal(bodyFor('station',draft).get('password'),'correct horse');assert(bodyFor('station',draft).toString().includes('new%26%2B%25'));
assert(validateNetwork('station',{...draft,ssid:'雪'.repeat(11)}).ssid);
assert(validateNetwork('station',{...draft,dhcp:false,address:'256.1.2.3',mask:'255.0.255.0',gateway:'1.1.1.1',dns1:'8.8.8.8',dns2:'0.0.0.0'}).address);
assert(validateNetwork('station',{...draft,dhcp:false,address:'10.1.1.3',mask:'255.255.255.0',gateway:'10.1.2.1',dns1:'8.8.8.8',dns2:'0.0.0.0'}).gateway);
assert.deepEqual(validateNetwork('station',{...draft,dhcp:false,address:'10.1.1.3',mask:'255.255.255.0',gateway:'10.1.1.1',dns1:'8.8.8.8',dns2:'0.0.0.0'}),{});
assert.deepEqual(validateNetwork('station',{...draft,dhcp:false,address:'10.1.1.3',mask:'255.255.255.0',gateway:'0.0.0.0',dns1:'0.0.0.0',dns2:'0.0.0.0'}),{});
assert(validateNetwork('ap',{...draft,channel:'14',hidden:false,security:'wpa2'}).channel);
for(const address of ['0.0.0.0','127.0.0.1','224.1.1.1','192.168.4.0','192.168.4.255','300.1.1.1','010.030.007.254'])assert(validateNetwork('ap',{...draftFrom('ap',initial),address}).address);
assert(validScan({schema:1,state:'complete',total:0,retained:0,page:0,truncated:false,networks:[]}));
async function run(mode,kind='station',values=draft){
 let clock=0,posts=0,reads=0,hidden=mode==='hidden',config=structuredClone(initial),status=structuredClone(baseStatus);
 const stages=[];
 const client={async request(path,options={}){
  if(options.method==='POST'){
   posts++;assert(path.startsWith('/api/v1/network/'));assert(!path.includes('?'));assert.equal(options.retries,0);
   if(mode==='ambiguous'){const e=new Error('Timeout');e.ambiguous=true;throw e;}
   config.operation={id:77,state:'applied',error:null};
   if(kind==='station'){
    config.station={ssid:values.ssid,password_configured:values.security!=='open',bssid:values.bssid||null};
    config.ip=Object.fromEntries(['dhcp','address','mask','gateway','dns1','dns2'].map(k=>[k,values[k]]));config.mode='station_ap';
    status.wifi={...status.wifi,mode:'station_ap',state:'connected',reason:null,ssid:values.ssid,ip:values.dhcp?'10.1.1.3':values.address};
   }else if(kind==='ap'){
    config.ap={ssid:values.ssid,password_configured:true,security:values.security,channel:Number(values.channel),hidden:values.hidden,address:values.address};config.mode='ap';
    status.wifi={...status.wifi,mode:'ap',state:'ready',ssid:values.ssid,ip:values.address,ap_ip:values.address};
   }else config.identity={...values};
   return {id:77,state:'accepted',error:null};
  }
  reads++;
  if(mode==='unreachable')throw new Error('Offline');
  if(mode==='wrong-password')status.wifi={...status.wifi,state:'failed',reason:'wrong_password'};
  if(mode==='dhcp-timeout')status.wifi={...status.wifi,state:'failed',reason:'dhcp_timeout'};
  if(mode==='no-ap')status.wifi={...status.wifi,state:'failed',reason:'ap_not_found'};
  if(mode==='dhcp-delay'&&reads<7)status.wifi={...status.wifi,state:'connecting',reason:'dhcp_pending'};
  else if(mode==='dhcp-delay')status.wifi={...status.wifi,state:'connected',reason:null};
  if(mode==='ap-address-mismatch')status.wifi.ap_ip='192.168.4.1';
  if(mode==='mismatch')config.station.ssid='different';
  if(mode==='receipt-lost')config.operation.id=0;
  if(mode==='save-failure')config.operation={id:77,state:'failed',error:'sdk_change_failed'};
  return path==='/api/v1/status'?status:config;
 }};
 const transaction=new NetworkTransaction(client,{now:()=>clock,pause:async ms=>{clock+=ms;if(clock>4000)hidden=false;},visible:()=>!hidden,budget:15000});
 transaction.addEventListener('stage',e=>stages.push(e.detail));
 let error;try{await transaction.run(kind,values);}catch(e){error=e;}
 assert.equal(posts,1);assert(stages.every(s=>!s.message.includes('correct horse')));
 if(['normal','dhcp-delay','hidden'].includes(mode)){assert(!error);assert.equal(transaction.state,'connected');assert(matchesNetwork(kind,values,transaction.latest));}
 else {assert(error);assert.notEqual(transaction.state,'connected');}
 if(['wrong-password','no-ap','dhcp-timeout'].includes(mode))assert.equal(transaction.state,'recovery');
 if(mode==='dhcp-timeout')assert(stages.some(s=>s.message.includes('DHCP address')));
 if(mode==='save-failure')assert.equal(transaction.state,'rejected');
 if(mode==='ambiguous')assert.equal(reads,0);
 if(mode==='hidden')assert(reads>0);
 if(mode==='dhcp-delay')assert(stages.some(s=>s.message.includes('Waiting for an IP address')));
 return transaction;
}
for(const mode of ['normal','wrong-password','no-ap','dhcp-timeout','dhcp-delay','unreachable','mismatch','receipt-lost','save-failure','ambiguous','hidden'])await run(mode);
await run('normal','ap',draftFrom('ap',initial));await run('normal','ap',{...draftFrom('ap',initial),address:'10.30.7.254'});await run('normal','identity',draftFrom('identity',initial));
await run('ap-address-mismatch','ap',{...draftFrom('ap',initial),address:'10.30.7.254'});
console.log('PASS network validation, encoded credentials, matching receipts, actual-state verification, DHCP delay, wrong password/no AP, disconnect, readback mismatch, ambiguity and bounded hidden checks');
// Every cancellation/semantic-receipt failure leaves a terminal outcome.
for(const receipt of [{id:0,state:'idle',error:null},{id:7,state:'failed',error:'failed'}]){
 const t=new NetworkTransaction({request:async()=>receipt});await assert.rejects(t.run('identity',{}));assert.equal(t.state,'unverified');
}
{
 const c=new AbortController();c.abort();let posts=0;
 const t=new NetworkTransaction({request:async()=>{posts++;}});await assert.rejects(t.run('identity',{},c.signal));assert.equal(posts,0);assert.equal(t.state,'cancelled');
}
{
 const c=new AbortController();let posts=0;
 const t=new NetworkTransaction({request:async()=>{posts++;return {id:1,state:'accepted',error:null};}},{pause:async()=>{c.abort();throw Error('cancelled');}});
 await assert.rejects(t.run('identity',{},c.signal));assert.equal(posts,1);assert.equal(t.state,'unverified');
}
console.log('PASS invalid change receipts, pre-dispatch cancellation and cancelled post-write verification have terminal states');
