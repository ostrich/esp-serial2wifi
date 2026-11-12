/* Versioned, redacted telemetry. */
const states = new Set(['disabled','ready','connecting','connected','degraded','failed','unknown']);
const object = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const string = value => typeof value === 'string';
const nullableString = value => value === null || string(value);
const bool = value => typeof value === 'boolean';
const integer = value => Number.isSafeInteger(value) && value >= 0;
const nullableInteger = value => value === null || integer(value);

export function validCapabilities(value) {
  return object(value) && value.schema === 1 && object(value.features) && object(value.limits) &&
    ['mqtt','syslog','custom_pages','rest','socket','ota','optiboot_upload'].every(key=>bool(value.features[key])) &&
    bool(value.features.optiboot_readback) &&
    (!value.features.optiboot_readback||value.features.optiboot_upload) &&
    ['console_buffer_bytes','max_ota_image_bytes','flash_slot_bytes'].every(key=>integer(value.limits[key])) &&
    value.limits.max_ota_image_bytes <= value.limits.flash_slot_bytes &&
    (!value.features.ota || value.limits.max_ota_image_bytes > 0) &&
    Array.isArray(value.serial_tcp_ports) && value.serial_tcp_ports.every(port=>Number.isInteger(port)&&port>0&&port<65536) &&
    new Set(value.serial_tcp_ports).size === value.serial_tcp_ports.length;
}

export function validStatus(value) {
  if (!object(value) || value.schema !== 1 || !string(value.hostname) || !string(value.description) ||
      !['firmware','system','wifi','serial'].every(key=>object(value[key]))) return false;
  const {firmware,system,wifi,serial,mqtt} = value;
  return string(firmware.version) && string(firmware.sdk) && nullableString(firmware.slot) &&
    string(firmware.image_abi) && integer(firmware.flash_bytes) && bool(firmware.ota_eligible) &&
    nullableInteger(system.uptime_ms) && integer(system.heap_free_bytes) && string(system.reset_reason) && nullableInteger(system.reset_code) &&
    ['ap','station','station_ap','disabled'].includes(wifi.mode) && states.has(wifi.state) &&
    ['reason','ssid','ip','ap_ip','mac'].every(key=>nullableString(wifi[key])) &&
    (wifi.rssi_dbm === null || Number.isInteger(wifi.rssi_dbm) && wifi.rssi_dbm >= -128 && wifi.rssi_dbm <= 0) &&
    nullableInteger(wifi.channel) && nullableInteger(wifi.ap_clients) &&
    states.has(serial.state) && bool(serial.listener_ready) && integer(serial.clients) && nullableInteger(serial.baud) &&
    (serial.configured_format === null || string(serial.configured_format) && /^[5-8][NEO](?:[12]|1\.5)$/.test(serial.configured_format)) &&
    (serial.target_connected === null || bool(serial.target_connected)) && integer(serial.rx_bytes) && bool(serial.loss_detected) &&
    (integer(serial.boot_id)&&serial.boot_id<=4294967295) &&
    (object(serial.errors)&&serialErrorFields.every(([key])=>integer(serial.errors[key])&&serial.errors[key]<=4294967295)) &&
    (mqtt === undefined || object(mqtt) && bool(mqtt.enabled) && states.has(mqtt.state));
}

const number = new Intl.NumberFormat(undefined,{maximumFractionDigits:0});
export const count = value => number.format(value);
export function signalText(wifi){
  return wifi.rssi_dbm===null?(wifi.mode==='ap'?'Not applicable in standalone Wi-Fi mode':'Not reported'):count(wifi.rssi_dbm)+' dBm';
}
export function uptime(milliseconds) {
  if (milliseconds === null) return 'Not reported';
  const seconds = Math.floor(milliseconds/1000), days = Math.floor(seconds/86400), hours = Math.floor(seconds/3600)%24,
    minutes = Math.floor(seconds/60)%60;
  if (days) return `${count(days)}d ${count(hours)}h ${count(minutes)}m`;
  if (hours) return `${count(hours)}h ${count(minutes)}m`;
  if (minutes) return `${count(minutes)}m ${count(seconds%60)}s`;
  return `${count(seconds)}s`;
}
export function bytes(value) { return count(value)+' bytes'; }

const labels = {disabled:'Disabled',ready:'Ready',connecting:'Connecting',connected:'Connected',degraded:'Degraded',failed:'Failed',unknown:'Unknown'};
const reasons = {wrong_password:'The network rejected the password.',ap_not_found:'The station network was not found.',
  association_failed:'Station association failed.',dhcp_pending:'Waiting for an IP address.',dhcp_timeout:'DHCP timed out without an IP address.'};

export const serialErrorFields=[
 ['uart_ring_bytes','UART buffer bytes dropped'],['uart_frame_bytes','UART bytes discarded after framing errors'],
 ['uart_frames','UART framing errors'],['uart_overruns','UART FIFO overruns (lost byte count unknown)'],
 ['tcp_output_bytes','TCP output bytes dropped'],['tcp_rejections','TCP input batches rejected'],
 ['tcp_input_alloc','TCP input allocation failures'],['tcp_output_alloc','TCP output allocation failures'],
 ['tcp_send','TCP send failures'],['tcp_hold','TCP flow-control failures'],['web_output_bytes','Web terminal bytes dropped']
];
export function serialErrorSummary(status,previous=null,incomplete=false){
 const s=status.serial,totals=Object.fromEntries(serialErrorFields.map(([key])=>[key,s.errors[key]]));
 const comparable=!!(previous&&previous.boot===s.boot_id&&integer(status.system.uptime_ms)&&integer(previous.uptime)&&status.system.uptime_ms>=previous.uptime&&serialErrorFields.every(([key])=>totals[key]>=previous.totals[key]));
 const changes=comparable?Object.fromEntries(serialErrorFields.map(([key])=>[key,totals[key]-previous.totals[key]])):null;
 const increased=changes&&Object.values(changes).some(Boolean),recorded=Object.values(totals).some(Boolean);
 let label=increased?'New serial errors':recorded?(comparable?'Earlier serial errors':'Serial errors recorded'):incomplete?'Counters reset or wrapped':'TCP listener ready';
 let detail=serialErrorFields.filter(([key])=>totals[key]).map(([key,name])=>name+': '+count(totals[key])).join('; ');
 if(detail)detail='Since restart: '+detail+'.';
 else detail=incomplete?'Counters decreased or wrapped; totals may be incomplete.':'No serial errors reported since restart.';
 if(incomplete&&recorded)detail+=' Counters decreased or wrapped; totals may be incomplete.';
 if(recorded||incomplete)detail+=' '+(comparable?(increased?'New errors since the previous reading: '+serialErrorFields.filter(([key])=>changes[key]).map(([key,name])=>name+': +'+count(changes[key])).join('; ')+'.':'No new errors since the previous reading.'):'Waiting for the next reading to check for new errors.');
 const state=!s.listener_ready?'failed':increased||recorded&&!comparable?'degraded':incomplete?'unknown':'ready';
 if(!s.listener_ready){label='TCP listener failed';detail='Serial TCP access is unavailable. '+detail;}
 return {label,state,detail,totals,changes,interval_ms:comparable?status.system.uptime_ms-previous.uptime:null,hasErrors:recorded||incomplete,incomplete};
}
export class SerialErrorTracker{
 constructor(){this.previous=null;this.boot=null;this.incomplete=false;}
 reset(){this.previous=null;}
 observe(status){
  const s=status.serial;
  if(s.boot_id!==this.boot){this.previous=null;this.incomplete=false;this.boot=s.boot_id;}
  if(this.previous&&serialErrorFields.some(([key])=>s.errors[key]<this.previous.totals[key]))this.incomplete=true;
  const result=serialErrorSummary(status,this.previous,this.incomplete);
  this.previous={boot:s.boot_id,uptime:status.system.uptime_ms,totals:result.totals};
  return result;
 }
}

export function health(status, capabilities, stale = false, serialReport=serialErrorSummary(status)) {
  const {wifi,serial,firmware,mqtt} = status;
  const ap = wifi.mode === 'ap', recovery = wifi.mode === 'station_ap' && wifi.ap_ip !== null;
  let wifiState = wifi.state;
  let wifiLabel = ap && wifi.state === 'ready' ? 'Access point ready' :
    wifi.state === 'connected' ? 'Joined network' : labels[wifi.state];
  if (recovery && wifi.state === 'failed') { wifiState = 'degraded'; wifiLabel = 'Recovery AP available'; }
  const wifiMeta = [wifi.ip ? 'IP '+wifi.ip : 'No station IP address',
    recovery ? 'Recovery AP '+wifi.ap_ip : null,
    wifi.channel === null ? null : 'Channel '+count(wifi.channel),
    ap && wifi.ap_clients !== null ? count(wifi.ap_clients)+' Wi-Fi '+(wifi.ap_clients===1?'client':'clients') : null].filter(Boolean).join(' · ');
  const cards = [
    {id:'wifi',title:'Wi-Fi',state:wifiState,label:wifiLabel,
      detail:reasons[wifi.reason] || (wifi.mode==='disabled'?'Wi-Fi is turned off.':ap ? 'Standalone access point.' : recovery ? 'Station and recovery AP are active.' : 'Station network status.'),
      primary:wifi.ssid || 'Network name not reported',meta:wifiMeta},
    {id:'serial',title:'Serial bridge',state:serialReport.state,label:serialReport.label,
      detail:serialReport.hasErrors||!serial.listener_ready?serialReport.detail:serial.target_connected===null?'Check the console for output from the attached device.':serial.target_connected?'Attached device detected.':'Attached device not detected.',
      primary:(serial.baud===null?'Baud not reported':count(serial.baud)+' baud')+(serial.configured_format?' · '+serial.configured_format+'':''),
      meta:count(serial.clients)+' serial TCP '+(serial.clients===1?'client':'clients')+' · '+bytes(serial.rx_bytes)+' received since restart'},
    {id:'firmware',title:'Firmware',state:'ready',label:'Running',primary:firmware.version,
      detail:firmware.ota_eligible && capabilities?.features.ota?'Wi-Fi updates enabled.':'OTA is unavailable on this build.',
      meta:firmware.slot?'Active slot '+firmware.slot:'Active slot not reported'}
  ];
  if (capabilities?.features.mqtt) {
    const state = mqtt?.state || 'unknown';
    const mqttLabel = state==='connected'?'Broker connected':state==='connecting'?'Connecting to broker':labels[state];
    cards.push({id:'mqtt',title:'MQTT',state,label:mqttLabel,
      primary:!mqtt?'Not reported':mqtt.enabled?'Enabled':'',
      detail:!mqtt?'No broker state was reported.':!mqtt.enabled?'MQTT is turned off.':state==='connected'?'Connected to the broker.':
        state==='failed'?'The broker rejected the connection.':'Waiting for a broker connection.',
      meta:''});
  }
  return cards.map(card=>({...card,badgeState:stale?'unknown':card.state,badgeLabel:stale?'Out of date · '+card.label:card.label}));
}

// Displays and connection examples use device-reported IPv4 addresses, never an unrelated
// browser host. Canonical octets also keep copied shell commands unambiguous.
export function reportedAddress(wifi,browserHost=''){
 const candidates=wifi?.mode==='ap'?[wifi.ap_ip,wifi.ip]:[wifi?.ip,wifi?.ap_ip];
 const addresses=candidates.map(ip=>{
  if(typeof ip!=='string'||!/^(?:\d{1,3}\.){3}\d{1,3}$/.test(ip))return null;
  const octets=ip.split('.').map(Number);
  return octets.every(n=>n<256)&&octets.some(Boolean)?octets.join('.'):null;
 }).filter(Boolean);
 return addresses.find(ip=>ip===browserHost)||addresses[0]||null;
}
