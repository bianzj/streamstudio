// ==========================================================================
// 文件：export_case005.mjs
// 功能：案例005（光伏草场）用 StreamStudio 自身的传感器渲染器（server.mjs 的 facet-step 工作线程），
//       对 FacetEB 已保存的原生状态补渲染热红外 TIFF（天底图与主平面多角度图），不重跑能量平衡。
// 主要步骤（对 grass、pv_grass 两个场景各做一遍）：
//   1. 读 runs/<场景>/output/faceteb_run.json，要求运行完成且所有节点收敛，否则报错；
//   2. 对 nadir、angular 两组：复制 faceteb.json 与 components.bin，生成传感器工程 sensor-project.json；
//   3. 对选定节点复制 energy_T=*.bin，在 worker 中调用渲染器，记录产物到 export-manifest.json；
//   4. 把天底 TIFF 复制回 output，并写 run-manifest.json 以便界面登记产物。
// 数据流：D:/cases/StreamStudio/case005/{runs,scenarios} → sensor/<场景>/{nadir,angular}。
// 与其他文件的关系：pv_case005.py 生成工程与原生运行；本脚本的 TIFF 由 pv_media005.py、
//       finalize_case005.py 等读取。渲染器路径取环境变量 STREAMSTUDIO_ROOT（缺省 C:/work/streamstudio）。
// 约定：节点编号 node 从 0 起、每节点 1 h；前 24 个节点预热，故天底图取 node>=24（第二天），
//       多角度图取 node 33、36、39（即 09、12、15 时）。
// ==========================================================================

// Render copies of native saved states using StreamStudio's own sensor renderer.
import fs from 'node:fs';
import path from 'node:path';
import { Worker } from 'node:worker_threads';
import { pathToFileURL } from 'node:url';
import crypto from 'node:crypto';
const renderer = pathToFileURL(path.join(process.env.STREAMSTUDIO_ROOT || 'C:/work/streamstudio','server.mjs'));
const root='D:/cases/StreamStudio/case005';
// 逐场景处理：无光伏草场（grass）与光伏草场（pv_grass）。
for (const scenario of ['grass','pv_grass']) {
  const source=path.join(root,'runs',scenario,'output');
  const index=JSON.parse(fs.readFileSync(path.join(source,'faceteb_run.json')));
  // 未完成或有节点未收敛时拒绝渲染，避免把不可靠的状态当成结果。
  if (index.status!=='completed'||index.completed.some(n=>!n.converged)) throw Error('Unconverged run');
  // 两组产品：nadir 为天底单视角逐时图；angular 为主平面多视角（principalPlane）图。
  for (const group of ['nadir','angular']) {
    const out=path.join(root,'sensor',scenario,group);
    fs.mkdirSync(path.join(out,'.facet_steps'),{recursive:true});
    fs.copyFileSync(path.join(source,'faceteb.json'),path.join(out,'faceteb.json'));
    fs.copyFileSync(path.join(source,'.facet_steps/components.bin'),path.join(out,'.facet_steps/components.bin'));
    const project=JSON.parse(fs.readFileSync(path.join(root,'scenarios',scenario,'project.json')));
    // 传感器设置：96 x 96 像元，只出温度图（image/temperature），关闭过程量输出；
    // angular 组开启主平面、关闭半球；vza/vaa 置 0，多视角由 principalPlane 展开（待核对）。
    Object.assign(project.configuration.sensor,{x:96,y:96,image:true,temperature:true,process:false,radiationProcess:false,energyProcess:false,principalPlane:group==='angular',hemisphere:false,vza:0,vaa:0});
    project.configuration.light.azimuth=180; // fixed N-S/E-W planes, comparable at all times
    const input=path.join(out,'sensor-project.json');fs.writeFileSync(input,JSON.stringify(project,null,2));
    // 选节点：天底取预热之后的第二天（node>=24）；多角度只取 09、12、15 时三个节点。
    const nodes=index.completed.filter(n=>group==='nadir'?n.node>=24:[33,36,39].includes(n.node));
    const products=[];
    for (const step of nodes) {
      const name='energy_T='+step.token+'.bin';
      fs.copyFileSync(path.join(source,'.facet_steps',name),path.join(out,'.facet_steps',name));
      // 在 worker 线程中调用 server.mjs 的 facet-step 渲染；关闭服务自启动（STREAM_STUDIO_SERVER_AUTOSTART=0）。
      const result=await new Promise((resolve,reject)=>{
        const worker=new Worker(renderer,{workerData:{kind:'facet-step',jsonPath:path.join(out,'faceteb.json'),inputPath:input,step},env:{...process.env,STREAM_STUDIO_SERVER_AUTOSTART:'0'}});
        let data;worker.on('message',v=>data=v);worker.on('error',reject);worker.on('exit',code=>code===0&&data?resolve(data):reject(Error('Renderer failed '+code)));
      });
      products.push({...step,...result});
      fs.writeFileSync(path.join(out,'export-manifest.json'),JSON.stringify({nativeRenderer:'C:/work/streamstudio/server.mjs',source,products},null,2));
      console.log(scenario,group,step.node,result.tifPaths.length);
    }
  }
  // 把天底 TIFF 复制到原生输出目录，供界面在原结果目录下直接读取。
  const nadir=path.join(root,'sensor',scenario,'nadir');
  for(const name of fs.readdirSync(nadir).filter(n=>n.endsWith('.tif'))) fs.copyFileSync(path.join(nadir,name),path.join(source,name));
  // 写 run-manifest.json 登记产物，并记录原生索引与渲染器的 SHA-256；原生 faceteb_run.json 保持不变。
  // UI product registration is separate from the unchanged native result index.
  fs.writeFileSync(path.join(source,'run-manifest.json'),JSON.stringify({runId:'case005-'+scenario,status:'completed',mode:'eFacetEB',nativeIndexSha256:crypto.createHash('sha256').update(fs.readFileSync(path.join(source,'faceteb_run.json'))).digest('hex'),rendererSha256:crypto.createHash('sha256').update(fs.readFileSync(renderer)).digest('hex'),note:'Native states plus separately rendered nadir TIFF products; native faceteb_run.json preserved'},null,2));
}
