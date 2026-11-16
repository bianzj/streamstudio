// Render copies of native saved states using StreamStudio's own sensor renderer.
import fs from 'node:fs';
import path from 'node:path';
import { Worker } from 'node:worker_threads';
import { pathToFileURL } from 'node:url';
import crypto from 'node:crypto';
const renderer = pathToFileURL(path.join(process.env.STREAMSTUDIO_ROOT || 'C:/work/streamstudio','server.mjs'));
const root='D:/cases/StreamStudio/case005';
for (const scenario of ['grass','pv_grass']) {
  const source=path.join(root,'runs',scenario,'output');
  const index=JSON.parse(fs.readFileSync(path.join(source,'faceteb_run.json')));
  if (index.status!=='completed'||index.completed.some(n=>!n.converged)) throw Error('Unconverged run');
  for (const group of ['nadir','angular']) {
    const out=path.join(root,'sensor',scenario,group);
    fs.mkdirSync(path.join(out,'.facet_steps'),{recursive:true});
    fs.copyFileSync(path.join(source,'faceteb.json'),path.join(out,'faceteb.json'));
    fs.copyFileSync(path.join(source,'.facet_steps/components.bin'),path.join(out,'.facet_steps/components.bin'));
    const project=JSON.parse(fs.readFileSync(path.join(root,'scenarios',scenario,'project.json')));
    Object.assign(project.configuration.sensor,{x:96,y:96,image:true,temperature:true,process:false,radiationProcess:false,energyProcess:false,principalPlane:group==='angular',hemisphere:false,vza:0,vaa:0});
    project.configuration.light.azimuth=180; // fixed N-S/E-W planes, comparable at all times
    const input=path.join(out,'sensor-project.json');fs.writeFileSync(input,JSON.stringify(project,null,2));
    const nodes=index.completed.filter(n=>group==='nadir'?n.node>=24:[33,36,39].includes(n.node));
    const products=[];
    for (const step of nodes) {
      const name='energy_T='+step.token+'.bin';
      fs.copyFileSync(path.join(source,'.facet_steps',name),path.join(out,'.facet_steps',name));
      const result=await new Promise((resolve,reject)=>{
        const worker=new Worker(renderer,{workerData:{kind:'facet-step',jsonPath:path.join(out,'faceteb.json'),inputPath:input,step},env:{...process.env,STREAM_STUDIO_SERVER_AUTOSTART:'0'}});
        let data;worker.on('message',v=>data=v);worker.on('error',reject);worker.on('exit',code=>code===0&&data?resolve(data):reject(Error('Renderer failed '+code)));
      });
      products.push({...step,...result});
      fs.writeFileSync(path.join(out,'export-manifest.json'),JSON.stringify({nativeRenderer:'C:/work/streamstudio/server.mjs',source,products},null,2));
      console.log(scenario,group,step.node,result.tifPaths.length);
    }
  }
  const nadir=path.join(root,'sensor',scenario,'nadir');
  for(const name of fs.readdirSync(nadir).filter(n=>n.endsWith('.tif'))) fs.copyFileSync(path.join(nadir,name),path.join(source,name));
  // UI product registration is separate from the unchanged native result index.
  fs.writeFileSync(path.join(source,'run-manifest.json'),JSON.stringify({runId:'case005-'+scenario,status:'completed',mode:'eFacetEB',nativeIndexSha256:crypto.createHash('sha256').update(fs.readFileSync(path.join(source,'faceteb_run.json'))).digest('hex'),rendererSha256:crypto.createHash('sha256').update(fs.readFileSync(renderer)).digest('hex'),note:'Native states plus separately rendered nadir TIFF products; native faceteb_run.json preserved'},null,2));
}
