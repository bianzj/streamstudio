// ==========================================================================
// 文件：export-faceteb-frames.mjs
// 功能：把已保存的 FacetEB 各节点结果，通过现有的 Stream Studio 影像渲染器导出为 512×512 温度 TIFF。
// 用法：node tools/export-faceteb-frames.mjs <RUN_DIR> <OUTPUT_DIR>（输出目录必须不存在）。
// 主要步骤：
//   1. 读 <RUN_DIR>/output/faceteb_run.json，要求节点非空且全部已收敛，按节点序号排序
//   2. 在 <OUTPUT_DIR>/tiff 下复制几何 faceteb.json 与组分 components.bin
//   3. 取运行的 project.json，改成只出影像和温度（关闭各类过程输出），另存 export-project.json
//   4. 逐节点：复制 energy_T=<token>.bin 并记录其 SHA-256，起一个 server.mjs 的 worker
//      （kind: facet-step）渲染，校验输出为 1 张 512×512 的 TIFF
//   5. 每完成一个节点即重写 export-manifest.json（含来源哈希、TIFF 路径、可见像元数、温度单位 K）
// 约定：只使用复制出的 step 二进制，原始仿真结果不被修改。
// 与其他文件：依赖 server.mjs 中 worker 的 'facet-step' 任务；STREAM_STUDIO_SERVER_AUTOSTART=0
//   使其只作为 worker 加载而不启动服务。
// ==========================================================================

// Export saved FacetEB nodes through the existing Stream Studio image renderer.
// Only copied step binaries are consumed; original simulation results remain intact.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { Worker } from 'node:worker_threads';
const [runArg, outArg] = process.argv.slice(2);
if (!runArg || !outArg) throw new Error('Usage: node tools/export-faceteb-frames.mjs RUN_DIR OUTPUT_DIR');
const run = path.resolve(runArg), out = path.resolve(outArg);
if (fs.existsSync(out)) throw new Error('Refusing to overwrite output: ' + out);
const source = path.join(run, 'output');
const index = JSON.parse(fs.readFileSync(path.join(source, 'faceteb_run.json')));
const nodes = [...index.completed].sort((a, b) => a.node - b.node);
if (!nodes.length || nodes.some(n => !n.converged)) throw new Error('Missing or unconverged nodes');
const rasterDir = path.join(out, 'tiff');
fs.mkdirSync(path.join(rasterDir, '.facet_steps'), { recursive: true });
fs.copyFileSync(path.join(source, 'faceteb.json'), path.join(rasterDir, 'faceteb.json'));
// control.facetOutputFormat = binary: the geometry arrays live in faceteb.bin next to the JSON.
{ const { copyFacetSidecars } = await import(new URL('../server/facet-result.mjs', import.meta.url)); copyFacetSidecars(path.join(source, 'faceteb.json'), rasterDir); }
fs.copyFileSync(path.join(source, '.facet_steps', 'components.bin'), path.join(rasterDir, '.facet_steps', 'components.bin'));
const project = JSON.parse(fs.readFileSync(path.join(run, 'project.json')));
Object.assign(project.configuration.sensor, { image: true, temperature: true, process: false, radiationProcess: false, energyProcess: false, principalPlane: false, hemisphere: false });
const inputPath = path.join(out, 'export-project.json');
fs.writeFileSync(inputPath, JSON.stringify(project, null, 2));
// 步骤 4-5：逐节点渲染并写清单；任何一步失败即抛错，不会留下“看似完整”的清单（最后检查数量）。
const products = [];
for (const step of nodes) {
  const name = 'energy_T=' + step.token + '.bin';
  const original = fs.readFileSync(path.join(source, '.facet_steps', name));
  const sha256 = crypto.createHash('sha256').update(original).digest('hex');
  fs.writeFileSync(path.join(rasterDir, '.facet_steps', name), original);
  const result = await new Promise((resolve, reject) => {
    const worker = new Worker(new URL('../server.mjs', import.meta.url), {
      workerData: { kind: 'facet-step', jsonPath: path.join(rasterDir, 'faceteb.json'), inputPath, step },
      env: { ...process.env, STREAM_STUDIO_SERVER_AUTOSTART: '0' }
    });
    let message;
    worker.on('message', v => { message = v; });
    worker.on('error', reject);
    worker.on('exit', code => code === 0 && message ? resolve(message) : reject(new Error('Export worker failed: ' + code)));
  });
  if (result.tifPaths.length !== 1 || result.width !== 512 || result.height !== 512) throw new Error('Unexpected raster layout');
  products.push({ ...step, sourceStepSha256: sha256, tiff: path.relative(out, result.tifPaths[0]).replaceAll('\\', '/'), visiblePixels: result.visiblePixels });
  fs.writeFileSync(path.join(out, 'export-manifest.json'), JSON.stringify({ sourceRun: run, nodes: products, temperatureUnits: 'K', atmosphereEnabled: project.configuration.atmosphere.enabled, width: 512, height: 512 }, null, 2));
  console.log(`${products.length}/${nodes.length} ${step.token}`);
}
if (products.length !== nodes.length) throw new Error('Export incomplete');
console.log('Completed: ' + out);
