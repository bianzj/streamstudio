// ==========================================================================
// 文件：check-user-release-cases.mjs
// 功能：对随发行版交付的 4 个 STREAM 示例工程（MODIS-ERA5、Landsat-ERA5、MODIS-LSCT、
//       Landsat-LSCT；各选 4×4 像元区域）做有限范围的验收：真实运行 stream.exe 并核对输出摘要。
// 用法：node tools/check-user-release-cases.mjs <case-root> <新的记录目录>（记录目录必须不存在）。
// 主要步骤（对每个示例）：
//   1. 读取 project.json，确认明确选择了 4×4 区域（scene.region）
//   2. 递归检查所有输入文件引用（含 ERA5 pack 清单内部的引用）：必须是案例目录内的相对路径
//      且文件存在，并记录 SHA-256
//   3. 用 createProjectStore().buildEngineConfig 生成引擎配置并写入记录目录
//   4. 运行 release/win-unpacked/resources/engine/stream.exe run config.json（超时 60 s）
//   5. 校验 scene_summary.json：光学/热红外像元数 = 16、各类失败计数 = 0、区域与温度模式一致、
//      EB 模式下全部像元求解且收敛；并要求至少产出 2 个非空 TIFF
// 输出：<记录目录>/<案例>/{config.json,run.log,output/}，以及汇总 results.json；有失败则退出码 1。
// 注意：这只是“能跑通”的验收，不含整幅影像、GUI、全新安装或科学精度验证；
//   不会被构建/打包脚本自动调用。
// 与其他文件：依赖 server/stream/config.mjs 与已打包的 stream.exe。
// ==========================================================================

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
// 引擎取自已打包的 release/win-unpacked，与用户实际拿到的版本一致；sha 用于在报告里留证。
const engineDir=path.join(root,'release/win-unpacked/resources/engine');
const engine=path.join(engineDir,'stream.exe');
const sha=p=>crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex');
const read=p=>JSON.parse(fs.readFileSync(p,'utf8').replace(/^\uFEFF/,''));
const store=createProjectStore({normalizeHostPath:p=>p,windowsPath:p=>p.replaceAll('\\','/')});
// 待验收的 4 个示例（卫星 × 强迫数据来源）；report 记录范围声明、引擎哈希与各案例结果。
const names=['MODIS-ERA5','Landsat-ERA5','MODIS-LSCT','Landsat-LSCT'];
const report={startedAt:new Date().toISOString(),scope:'Four shipped 4x4 image configurations, actual ERA5/component inputs; no full-image, GUI, clean-install or scientific accuracy acceptance.',engineSha256:sha(engine),caseRoot,results:[]};
// 逐案例验收：任何异常都记入 result.issues，不影响后续案例。
for(const name of names){
 const folder=path.join(caseRoot,name),projectFile=path.join(folder,'project.json');
 const result={name,passed:false,issues:[],projectSha256:sha(projectFile)};
 try{
  const original=read(projectFile),p=store.readProjectFile(projectFile),r=p.scene.region;
  if(!r?.enabled||(r.endRow-r.startRow)!==4||(r.endColumn-r.startColumn)!==4)throw Error('The shipped project must explicitly select a 4x4 region.');
  // 步骤 2 的实现是下面的 scan 函数：键名为 file/path/pack/fileName 的字符串视为文件引用，
  // 绝对路径或跳出案例目录均判为错误；pack 指向的 JSON 清单会被递归展开。
  // Validate all file references, including files nested in an ERA5 manifest.
  const references=[];
  const scan=(v,base)=>{if(Array.isArray(v)){v.forEach(x=>scan(x,base));return;}if(!v||typeof v!=='object')return;
   for(const [k,x]of Object.entries(v)){if(['file','path','pack','fileName'].includes(k)&&typeof x==='string'&&x){const f=path.resolve(base,x);if(path.isAbsolute(x)||path.relative(folder,f).startsWith('..'))throw Error('Input reference leaves the case: '+x);if(!fs.statSync(f).isFile())throw Error('Missing input: '+x);references.push({path:path.relative(folder,f).replaceAll('\\','/'),sha256:sha(f)});if(k==='pack'&&f.endsWith('.json'))scan(read(f),path.dirname(f));}else scan(x,base);}
  };scan(original,folder);result.inputs=references;
  // 步骤 3：为本案例建立记录目录并生成引擎配置（dataDir 指向打包内的 stream-defined）。
  const caseRecord=path.join(record,name);fs.mkdirSync(caseRecord);
  const outputDir=path.join(caseRecord,'output');fs.mkdirSync(outputDir);
  const config=store.buildEngineConfig(p.scene,{outputDir:outputDir.replaceAll('\\','/'),dataDir:path.join(engineDir,'stream-defined').replaceAll('\\','/')});
  const configFile=path.join(caseRecord,'config.json');fs.writeFileSync(configFile,JSON.stringify(config,null,2)+'\n');
  // 步骤 4：运行引擎。PATH 仅保留引擎目录与系统目录，避免误用开发机上的其他 DLL。
  const start=Date.now();const run=cp.spawnSync(engine,['run',configFile],{cwd:caseRecord,env:{...process.env,PATH:engineDir+';C:/Windows/System32;C:/Windows'},windowsHide:true,encoding:'utf8',timeout:60000,maxBuffer:8*1024*1024});
  fs.writeFileSync(path.join(caseRecord,'run.log'),(run.stdout||'')+'\n'+(run.stderr||'')+(run.error?'\n'+run.error.message:''));
  result.seconds=(Date.now()-start)/1000;result.exitCode=run.status;
  if(run.error||run.status!==0)throw Error(run.error?.message||'Engine exit '+run.status);
  // 步骤 5：核对 scene_summary.json（像元计数、区域、温度求解状态）与输出 TIFF。
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
// 汇总：全部案例通过才算通过；失败时以退出码 1 结束。
report.completedAt=new Date().toISOString();report.passed=report.results.every(x=>x.passed);
fs.writeFileSync(path.join(record,'results.json'),JSON.stringify(report,null,2)+'\n');
if(!report.passed)process.exitCode=1;
