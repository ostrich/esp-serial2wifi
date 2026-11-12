// Build application modules together; leave xterm a native lazy-loaded module.
import {build,transform} from 'esbuild';
import {minify} from 'terser';
import {mkdir,readFile,writeFile,readdir,rename} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {resolve,join} from 'node:path';
// Classic custom-page scripts expose globals used by uploaded pages.
if(process.argv[2]==='--compat'){
 const folder=resolve(process.argv[3]);
 for(const name of ['ui.js','userpage.js'])await writeFile(join(folder,name),(await minify(await readFile(join(folder,name),'utf8'),{module:false,toplevel:false,compress:{passes:3},mangle:{toplevel:false},format:{comments:/^!|@license|@preserve|Copyright|WTFPL/}})).code+'\n');
 for(const name of ['style.css','pure.css'])await writeFile(join(folder,name),(await transform(await readFile(join(folder,name),'utf8'),{loader:'css',minify:true,legalComments:'inline'})).code);
 process.exit(0);
}
const source=resolve(process.argv[2]),out=resolve(process.argv[3]),vendor=process.argv[4]?resolve(process.argv[4]):source;
await mkdir(out,{recursive:true});
await build({entryPoints:[join(source,'app.js')],outdir:out,bundle:true,format:'esm',minify:true,target:'es2020',charset:'utf8',legalComments:'inline',external:['./xterm.js']});
// Finish optimization before hashing so each asset URL identifies its bytes.
const app=join(out,'app.js');await writeFile(app,(await minify(await readFile(app,'utf8'),{module:true,compress:{passes:3},mangle:true,format:{comments:/^!|@license|@preserve/}})).code+'\n');
const terminal=await minify(await readFile(join(vendor,'xterm.js'),'utf8'),{compress:{passes:3},mangle:true,format:{comments:/^!|@license|@preserve/}});
await writeFile(join(out,'xterm.js'),terminal.code+'\n');
for(const name of ['style.css','xterm.css'])await writeFile(join(out,name),(await transform(await readFile(join(name==='xterm.css'?vendor:source,name),'utf8'),{loader:'css',minify:true,legalComments:'inline'})).code);
// Content-address the complete module/CSS set. An old tab cannot accidentally
// import a new terminal module after OTA, and new pages cannot reuse old code.
const hash=createHash('sha256');
for(const name of (await readdir(out)).sort())hash.update(name+'\0').update(await readFile(join(out,name)));
const version=hash.digest('hex').slice(0,24);await mkdir(join(out,version));
for(const name of await readdir(out))if(name!==version)await rename(join(out,name),join(out,version,name));
const html=(await readFile(join(source,'index.html'),'utf8')).replace('href="style.css"','href="'+version+'/style.css"').replace('src="app.js"','src="'+version+'/app.js"');
await writeFile(join(out,'index.html'),html);
