import {delay as wait,hex} from './core.js';
import {validSerial} from './serial-model.js';
import {validCapabilities} from './status.js';
const encoder=new TextEncoder();
function hexRecord(address,data){
 const values=[data.length,address>>8,address&255,0,...data];values.push((-values.reduce((sum,n)=>sum+n,0))&255);
 return ':'+hex(values).toUpperCase();
}
/* The existing HTTP programmer identifies ATmega328P and uses 128-byte pages.
 * Normalize supported input into complete aligned application pages so odd,
 * sparse or unusually partitioned records cannot change word/page semantics. */
export function prepareHex(text){
 if(typeof text!=='string'||text.length>262144||!/^[\x09\x0a\x0d\x20-\x7e]*$/.test(text))throw new Error('Choose an ASCII Intel HEX file of at most 256 KiB.');
 const image=new Uint8Array(32256).fill(255),present=new Uint8Array(32256),pages=new Set();
 let eof=false,payloadBytes=0,first=32256,last=0;
 for(const raw of text.split(/\r\n|\n|\r/)){
  const line=raw.trim();if(!line)continue;
  if(eof)throw new Error('The EOF record must be last.');
  if(!/^:[0-9a-f]+$/i.test(line)||line.length<11||(line.length&1)===0)throw new Error('Invalid Intel HEX record.');
  const bytes=Uint8Array.from(line.slice(1).match(/../g),h=>parseInt(h,16));
  const length=bytes[0],address=(bytes[1]<<8)|bytes[2],type=bytes[3];
  if(bytes.length!==length+5)throw new Error('HEX record length does not match its payload.');
  if((bytes.reduce((sum,n)=>sum+n,0)&255)!==0)throw new Error('HEX record checksum does not match.');
  if(type===0){
   if(address+length>32256)throw new Error('The image exceeds the ATmega328P application area (0x0000–0x7DFF).');
   for(let i=0;i<length;i++){
    const at=address+i;if(present[at])throw new Error('Overlapping HEX data records are not supported.');
    present[at]=1;image[at]=bytes[4+i];pages.add(Math.floor(at/128));payloadBytes++;first=Math.min(first,at);last=Math.max(last,at+1);
   }
  }else if(type===1){if(length!==0||address!==0)throw new Error('Invalid EOF record.');eof=true;}
  else if(type===2||type===4){
   if(length!==2||address!==0||bytes[4]!==0||bytes[5]!==0)throw new Error('This programmer supports only ATmega328P application addresses; use the target-specific external tool.');
  }else if(type===5){if(length!==4||address!==0)throw new Error('Invalid start-address record.');}
  else throw new Error('This HEX record type is not supported by the ATmega328P programmer.');
 }
 if(!eof||!payloadBytes)throw new Error('The HEX file needs application data and a final EOF record.');
 const records=[];
 for(const page of [...pages].sort((a,b)=>a-b))for(let offset=0;offset<128;offset+=16){const at=page*128+offset;records.push(hexRecord(at,image.slice(at,at+16)));}
 records.push(':00000001FF');const hex=encoder.encode(records.join('\n')+'\n');
 return Object.freeze({hex,payloadBytes,programBytes:pages.size*128,pages:pages.size,first,last,fileBytes:text.length});
}
export class ProgramTransaction extends EventTarget {
 constructor(client,{pause=wait,now=()=>Date.now()}={}){super();this.client=client;this.pause=pause;this.now=now;this.busy=false;this.state='idle';}
 stage(state,message){this.state=state;this.dispatchEvent(new CustomEvent('stage',{detail:{state,message}}));}
 async run(prepared,signal){
  if(this.busy)throw new Error('Programming is already in progress.');
  if(!prepared?.hex||!prepared.programBytes)throw new Error('Select a valid HEX file first.');
  // Revalidate staged bytes independently before any reset: callers cannot
  // substitute an arbitrary object or mutate the staged typed array unnoticed.
  const checked=prepareHex(new TextDecoder('utf-8',{fatal:true}).decode(prepared.hex));
  if(checked.programBytes!==prepared.programBytes||checked.hex.length!==prepared.hex.length||checked.hex.some((n,i)=>n!==prepared.hex[i]))throw new Error('Staged HEX changed; select the file again.');
  this.busy=true;let resetRequested=false,uploadStarted=false;
  try{
   this.stage('checking','Checking the target and serial connections…');
   const capabilities=await this.client.request('/api/v1/capabilities',{signal,retries:0,validate:validCapabilities});
   if(!capabilities.features.optiboot_upload||!capabilities.features.optiboot_readback)throw new Error('This firmware doesn’t include the ATmega328P programmer. Use the external-tool guide below.');
   const serial=await this.client.request('/api/v1/serial',{signal,retries:0,validate:validSerial});
   if(serial.reset_pin===null||serial.programming||serial.queued_bytes||serial.clients)throw new Error('Configure the MCU reset pin and close serial/programming clients before programming.');
   this.stage('syncing','Resetting the MCU and connecting to Optiboot…');resetRequested=true;
   await this.client.request('/pgm/sync',{method:'POST',body:new URLSearchParams({verify:'readback'}),signal,retries:0,responseType:'text',timeout:5000});
   const deadline=this.now()+10000;let sync;
   for(let attempt=0;attempt<40&&this.now()<deadline;attempt++){
    await this.pause(200,signal);
    const value=await this.client.request('/pgm/sync',{signal,retries:0,responseType:'text',timeout:2000});
    const match=/^SYNC at (\d+) baud: bootloader v\d+\.\d+; readback enabled$/.exec(value.trim());
    if(match){sync=Number(match[1]);break;}
    if(value.startsWith('SYNC'))throw new Error('Readback mode was not acknowledged; programming was not started.');
    if(value!=='NOT READY'&&value!=='')throw new Error('Optiboot synchronization failed. Check the MCU, reset wiring and bootloader.');
   }
   if(!sync)throw new Error('Optiboot synchronization timed out. Check the MCU and reset wiring.');
   this.stage('transferring',`Transferring ${prepared.programBytes.toLocaleString()} bytes at ${sync.toLocaleString()} baud. Each page is read back before continuing.`);uploadStarted=true;
   const result=await this.client.request('/pgm/upload',{method:'POST',body:checked.hex,signal,retries:0,responseType:'text',timeout:120000});
   this.stage('verifying','Checking the written data…');
   const match=/^Success\. (\d+) bytes at (\d+) baud; readback verified (\d+) bytes\.$/.exec(result.trim());
   if(!match||Number(match[1])!==prepared.programBytes||Number(match[2])!==sync||Number(match[3])!==prepared.programBytes)throw new Error('The programmer did not confirm complete flash readback. Inspect the target before retrying.');
   this.stage('complete',`Programmed and read back ${prepared.programBytes.toLocaleString()} bytes at ${sync.toLocaleString()} baud. Check the console for the MCU’s output.`);
   return {bytes:prepared.programBytes,baud:sync};
  }catch(error){
   const ambiguous=error.ambiguous||signal?.aborted;
   this.stage(ambiguous?'unverified':'failed',ambiguous?(uploadStarted?'The connection closed during programming. The MCU may have an incomplete image; check it before trying again.':resetRequested?'The MCU may have reset. Check it before trying again.':'The connection closed before programming started.'):(uploadStarted?'Programming failed. The MCU may have an incomplete image; check it before trying again.':error.message));
   throw error;
  }finally{this.busy=false;}
 }
}
