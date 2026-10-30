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
fs.copyFileSync(path.join(source, '.facet_steps', 'components.bin'), path.join(rasterDir, '.facet_steps', 'components.bin'));
const project = JSON.parse(fs.readFileSync(path.join(run, 'project.json')));
Object.assign(project.configuration.sensor, { image: true, temperature: true, process: false, radiationProcess: false, energyProcess: false, principalPlane: false, hemisphere: false });
const inputPath = path.join(out, 'export-project.json');
fs.writeFileSync(inputPath, JSON.stringify(project, null, 2));
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
