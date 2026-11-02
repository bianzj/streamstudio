// Explicit, bounded release-case acceptance. Never invoked by build/package scripts.
// Run: node tools/check-user-release-cases.mjs <case-root> <new-record-directory>
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import cp from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {createProjectStore} from '../server/stream/config.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const [caseArg,recordArg]=process.argv.slice(2);
if(!caseArg||!recordArg)throw Error('Usage: node tools/check-user-release-cases.mjs <case-root> <new-record-directory>');
const caseRoot=path.resolve(caseArg),record=path.resolve(recordArg);
if(fs.existsSync(record))throw Error('Record directory already exists; choose a new directory.');
fs.mkdirSync(record,{recursive:true});
const engineDir=path.join(root,'release/win-unpacked/resources/engine');
const engine=path.join(engineDir,'stream.exe');
const sha=p=>crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex');
const read=p=>JSON.parse(fs.readFileSync(p,'utf8').replace(/^\uFEFF/,''));
const store=createProjectStore({normalizeHostPath:p=>p,windowsPath:p=>p.replaceAll('\\','/')});
const names=['MODIS-ERA5','Landsat-ERA5','MODIS-LSCT','Landsat-LSCT'];
const report={startedAt:new Date().toISOString(),scope:'Four shipped 4x4 image configurations, actual ERA5/component inputs; no full-image, GUI, clean-install or scientific accuracy acceptance.',engineSha256:sha(engine),caseRoot,results:[]};
for(const name of names){
 const folder=path.join(caseRoot,name),projectFile=path.join(folder,'project.json');
 const result={name,passed:false,issues:[],projectSha256:sha(projectFile)};
 try{
  const original=read(projectFile),p=store.readProjectFile(projectFile),r=p.scene.region;
  if(!r?.enabled||(r.endRow-r.startRow)!==4||(r.endColumn-r.startColumn)!==4)throw Error('The shipped project must explicitly select a 4x4 region.');
  // Validate all file references, including files nested in an ERA5 manifest.
  const references=[];
  const scan=(v,base)=>{if(Array.isArray(v)){v.forEach(x=>scan(x,base));return;}if(!v||typeof v!=='object')return;
   for(const [k,x]of Object.entries(v)){if(['file','path','pack','fileName'].includes(k)&&typeof x==='string'&&x){const f=path.resolve(base,x);if(path.isAbsolute(x)||path.relative(folder,f).startsWith('..'))throw Error('Input reference leaves the case: '+x);if(!fs.statSync(f).isFile())throw Error('Missing input: '+x);references.push({path:path.relative(folder,f).replaceAll('\\','/'),sha256:sha(f)});if(k==='pack'&&f.endsWith('.json'))scan(read(f),path.dirname(f));}else scan(x,base);}
  };scan(original,folder);result.inputs=references;
  const caseRecord=path.join(record,name);fs.mkdirSync(caseRecord);
  const outputDir=path.join(caseRecord,'output');fs.mkdirSync(outputDir);
  const config=store.buildEngineConfig(p.scene,{outputDir:outputDir.replaceAll('\\','/'),dataDir:path.join(engineDir,'stream-defined').replaceAll('\\','/')});
  const configFile=path.join(caseRecord,'config.json');fs.writeFileSync(configFile,JSON.stringify(config,null,2)+'\n');
  const start=Date.now();const run=cp.spawnSync(engine,['run',configFile],{cwd:caseRecord,env:{...process.env,PATH:engineDir+';C:/Windows/System32;C:/Windows'},windowsHide:true,encoding:'utf8',timeout:60000,maxBuffer:8*1024*1024});
  fs.writeFileSync(path.join(caseRecord,'run.log'),(run.stdout||'')+'\n'+(run.stderr||'')+(run.error?'\n'+run.error.message:''));
  result.seconds=(Date.now()-start)/1000;result.exitCode=run.status;
  if(run.error||run.status!==0)throw Error(run.error?.message||'Engine exit '+run.status);
  const s=read(path.join(outputDir,'scene_summary.json'));result.counts=s.counts;result.grid=s.grid;result.temperature=s.temperature;
  for(const key of ['opticalPixels','thermalPixels'])if(s.counts?.[key]!==16)result.issues.push(key+' must equal 16');
  for(const key of ['invalidInputPixels','unknownTypePixels','solarFailedPixels','thermalFailedPixels'])if(s.counts?.[key]!==0)result.issues.push(key+' must equal zero');
  for(const key of ['startRow','endRow','startColumn','endColumn'])if(s.grid?.[key]!==r[key])result.issues.push('Region mismatch: '+key);
  if(s.temperature?.mode!==p.scene.temperature.mode)result.issues.push('Temperature mode mismatch');
  if(p.scene.temperature.mode==='eb'){
   for(const key of ['solvedPixels','convergedPixels'])if(s.temperature[key]!==16)result.issues.push(key+' must equal 16');
   for(const key of ['failedPixels','noForcingPixels'])if(s.temperature[key]!==0)result.issues.push(key+' must equal zero');
  }
  result.outputs=fs.readdirSync(outputDir).filter(n=>/\.tiff?$/i.test(n)).map(n=>({name:n,bytes:fs.statSync(path.join(outputDir,n)).size,sha256:sha(path.join(outputDir,n))}));
  if(result.outputs.length<2||result.outputs.some(x=>!x.bytes))result.issues.push('Expected nonempty optical and thermal TIFF outputs');
  result.passed=result.issues.length===0;
 }catch(e){result.issues.push(e.message);}
 report.results.push(result);fs.writeFileSync(path.join(record,'results.json'),JSON.stringify(report,null,2)+'\n');
 console.log(JSON.stringify({name,passed:result.passed,seconds:result.seconds,issues:result.issues}));
}
report.completedAt=new Date().toISOString();report.passed=report.results.every(x=>x.passed);
fs.writeFileSync(path.join(record,'results.json'),JSON.stringify(report,null,2)+'\n');
if(!report.passed)process.exitCode=1;
