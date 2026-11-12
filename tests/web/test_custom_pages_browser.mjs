import assert from 'node:assert/strict';

export async function testCustomPagesReload({page,run,check,base}) {
 const callback={message:'<b>Ready</b>',items:['one','two'],rows:[['Heading'],['Value']],value:'sample',enabled:true};let buttonId=null;
 await page.send('Fetch.enable',{patterns:[{urlPattern:'*/user/probe.html.json*',requestStage:'Request'}]});
 page.on('Fetch.requestPaused',p=>{const url=new URL(p.request.url);if(p.request.method==='POST')buttonId=url.searchParams.get('id');page.send('Fetch.fulfillRequest',{requestId:p.requestId,responseCode:200,responseHeaders:[{name:'Content-Type',value:'application/json'}],body:Buffer.from(JSON.stringify(callback)).toString('base64')}).catch(()=>{});});
 await page.send('Page.navigate',{url:base+'/user/probe.html'});
 await check('Minified custom-page helpers preserve rendering, form values, public globals and exact button IDs',async()=>{
  await page.wait("document.querySelector('#message b')?.textContent==='Ready'");
  assert.equal(await run(()=>document.querySelectorAll('#items li').length),2);assert.equal(await run(()=>document.querySelectorAll('#rows tr').length),2);assert.equal(await run(()=>document.querySelector('[name=value]').value),'sample');assert.equal(await run(()=>document.querySelector('[name=enabled]').checked),true);
  assert.equal(await run(()=>['ajaxJson','notifyResponse','notifyButtonPressed','showWarning','onLoad'].every(k=>typeof window[k]==='function')),true);
  await run(()=>document.getElementById('pump&fan"quoted').click());await page.wait("document.querySelector('#message b')?.textContent==='Ready'");
  for(let i=0;i<100&&buttonId===null;i++)await page.evaluate('new Promise(r=>setTimeout(r,20))');assert.equal(buttonId,'pump&fan"quoted');
  for(const width of [1440,360]){await page.send('Emulation.setDeviceMetricsOverride',{width,height:900,deviceScaleFactor:1,mobile:false});await page.wait('document.documentElement.scrollWidth<=innerWidth');await page.screenshot('custom-page-'+width+'.png');}
 });
}
