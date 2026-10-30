import * as THREE from 'three'
import { OBJLoader } from 'three/addons/loaders/OBJLoader.js'
import { groundedPlacementHeight, isGroundedObject } from './project-schema.js'

const PREVIEW_INSTANCE_LIMIT = 5000

function positionsFromText(content) {
  return String(content).replace(/^\uFEFF/, '').split(/\r?\n/)
    .map((line) => line.trim())
    .filter((line) => line && !line.startsWith('#') && !line.startsWith('//'))
    .map((line, sourceIndex) => {
      const values = line.split(/[\s,]+/).map(Number)
      return { x: values[0] || 0, y: values[1] || 0, z: values[2] || 0, scale: Number.isFinite(values[3]) ? values[3] : 1, rotation: Number.isFinite(values[4]) ? values[4] : 0, sourceIndex }
    })
}

function samplePlacements(placements, limit = PREVIEW_INSTANCE_LIMIT) {
  if (placements.length <= limit) return placements
  const step = placements.length / limit
  return Array.from({ length: limit }, (_, index) =>
    placements[Math.min(placements.length - 1, Math.floor((index + 0.5) * step))])
}

function dispose(root) {
  root.traverse((child) => {
    child.geometry?.dispose?.()
    if (Array.isArray(child.material)) child.material.forEach((material) => material.dispose?.())
    else child.material?.dispose?.()
  })
}

function colorFor(type) {
  const value = String(type || '').toLowerCase()
  if (value.includes('fire')) return 0xff6a18
  if (value.includes('fog')) return 0xc8d3dc
  if (value.includes('human')) return 0x3d6fd6
  if (value.includes('vehicle')) return 0xd69b32
  if (value.includes('ship')) return 0x547a94
  if (value.includes('water')) return 0x3d8eaa
  if (value.includes('build')) return 0x7e8580
  if (value.includes('soil')) return 0x756247
  return 0x3f9568
}

const previewTextureCache = new Map()

function seededTextureRandom(seed) {
  let value = seed >>> 0
  return () => {
    value += 0x6D2B79F5
    let result = value
    result = Math.imul(result ^ result >>> 15, result | 1)
    result ^= result + Math.imul(result ^ result >>> 7, result | 61)
    return ((result ^ result >>> 14) >>> 0) / 4294967296
  }
}

function drawNoise(ctx, random, count, colors, radius = 2) {
  for (let index = 0; index < count; index += 1) {
    ctx.fillStyle = colors[Math.floor(random() * colors.length)]
    const size = Math.max(.4, random() * radius)
    ctx.beginPath()
    ctx.ellipse(random() * 512, random() * 512, size, size * (.5 + random()), random() * Math.PI, 0, Math.PI * 2)
    ctx.fill()
  }
}

function createPreviewTexture(preset) {
  if (previewTextureCache.has(preset)) return previewTextureCache.get(preset)
  const canvas = document.createElement('canvas')
  canvas.width = canvas.height = 512
  const ctx = canvas.getContext('2d')
  const random = seededTextureRandom([...preset].reduce((sum, char) => sum * 31 + char.charCodeAt(0), 2166136261))

  if (preset === 'solar-panel') {
    const gradient = ctx.createLinearGradient(0, 0, 512, 512)
    gradient.addColorStop(0, '#071a32'); gradient.addColorStop(.5, '#0d3159'); gradient.addColorStop(1, '#061528')
    ctx.fillStyle = gradient; ctx.fillRect(0, 0, 512, 512)
    ctx.strokeStyle = 'rgba(188,220,242,.72)'; ctx.lineWidth = 4
    for (let x = 0; x <= 512; x += 64) { ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, 512); ctx.stroke() }
    for (let y = 0; y <= 512; y += 128) { ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(512, y); ctx.stroke() }
    ctx.strokeStyle = 'rgba(235,247,255,.28)'; ctx.lineWidth = 2
    for (let x = 8; x < 512; x += 16) { ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, 512); ctx.stroke() }
    drawNoise(ctx, random, 500, ['rgba(95,166,215,.10)', 'rgba(255,255,255,.08)'], 1.2)
  } else if (preset === 'red-brick') {
    ctx.fillStyle = '#d7c8b8'; ctx.fillRect(0, 0, 512, 512)
    const rowHeight = 64, mortar = 6, brickWidth = 128
    for (let row = -1; row < 9; row += 1) {
      const offset = row % 2 ? -brickWidth / 2 : 0
      for (let column = -1; column < 6; column += 1) {
        const x = offset + column * brickWidth + mortar / 2
        const y = row * rowHeight + mortar / 2
        const red = 116 + Math.round(random() * 48)
        const green = 43 + Math.round(random() * 28)
        const blue = 30 + Math.round(random() * 20)
        ctx.fillStyle = `rgb(${red},${green},${blue})`
        ctx.fillRect(x, y, brickWidth - mortar, rowHeight - mortar)
        ctx.fillStyle = 'rgba(255,210,174,.07)'
        ctx.fillRect(x + 4, y + 4, brickWidth - mortar - 8, 5)
      }
    }
    drawNoise(ctx, random, 1500, ['rgba(35,12,8,.11)', 'rgba(255,225,190,.08)'], 1.7)
  } else if (preset === 'gravel') {
    ctx.fillStyle = '#77756f'; ctx.fillRect(0, 0, 512, 512)
    drawNoise(ctx, random, 7000, ['#4d4d4a', '#60605c', '#85827a', '#aaa69c', '#6e675e', '#928a7e'], 4.2)
  } else if (preset === 'stone-wall') {
    ctx.fillStyle = '#b7aa94'; ctx.fillRect(0, 0, 512, 512)
    for (let row = 0; row < 7; row += 1) {
      let x = row % 2 ? -45 : -5
      const y = row * 78
      while (x < 512) {
        const width = 70 + random() * 80
        ctx.fillStyle = ['#8e897b', '#aaa08d', '#79776e', '#b8aa91'][Math.floor(random() * 4)]
        ctx.strokeStyle = '#d0c4b1'; ctx.lineWidth = 7
        ctx.beginPath()
        ctx.roundRect(x, y + random() * 7, width, 67 + random() * 8, 9)
        ctx.fill(); ctx.stroke(); x += width - 1
      }
    }
    drawNoise(ctx, random, 1000, ['rgba(35,31,26,.12)', 'rgba(255,255,255,.1)'], 1.8)
  } else if (preset === 'roof-tile') {
    ctx.fillStyle = '#6f2d23'; ctx.fillRect(0, 0, 512, 512)
    for (let row = -1; row < 9; row += 1) {
      const y = row * 68
      for (let column = -1; column < 10; column += 1) {
        const x = column * 60 + (row % 2 ? 30 : 0)
        ctx.fillStyle = ['#9f4934', '#ad553b', '#87402f', '#b85b3d'][Math.floor(random() * 4)]
        ctx.strokeStyle = '#57251d'; ctx.lineWidth = 4
        ctx.beginPath(); ctx.roundRect(x, y, 64, 76, [5, 5, 24, 24]); ctx.fill(); ctx.stroke()
      }
    }
  } else if (preset === 'wood') {
    ctx.fillStyle = '#855a38'; ctx.fillRect(0, 0, 512, 512)
    for (let x = 0; x < 512; x += 32) {
      ctx.fillStyle = x % 64 ? 'rgba(58,31,15,.18)' : 'rgba(226,175,112,.12)'
      ctx.fillRect(x, 0, 5 + random() * 9, 512)
    }
    for (let index = 0; index < 90; index += 1) {
      ctx.strokeStyle = 'rgba(48,25,13,.18)'; ctx.lineWidth = 1 + random() * 2
      const y = random() * 512
      ctx.beginPath(); ctx.moveTo(0, y); ctx.bezierCurveTo(150, y + random() * 18, 360, y - random() * 18, 512, y + random() * 8); ctx.stroke()
    }
  } else if (preset === 'grass') {
    ctx.fillStyle = '#567343'; ctx.fillRect(0, 0, 512, 512)
    drawNoise(ctx, random, 9000, ['#344f2f', '#66834c', '#789459', '#455f36', '#92a867'], 2.4)
  } else if (preset === 'canvas') {
    ctx.fillStyle = '#58613d'; ctx.fillRect(0, 0, 512, 512)
    for (let y = 0; y < 512; y += 4) { ctx.fillStyle = y % 8 ? 'rgba(20,24,10,.10)' : 'rgba(230,235,200,.06)'; ctx.fillRect(0, y, 512, 2) }
    for (let x = 0; x < 512; x += 4) { ctx.fillStyle = x % 8 ? 'rgba(20,24,10,.08)' : 'rgba(230,235,200,.05)'; ctx.fillRect(x, 0, 2, 512) }
    drawNoise(ctx, random, 900, ['rgba(30,34,18,.10)', 'rgba(210,214,170,.07)'], 6)
  } else if (preset === 'camouflage-woodland' || preset === 'camouflage-desert') {
    const palette = preset === 'camouflage-woodland'
      ? ['#5a6234', '#33401f', '#6e5232', '#1e1f19']
      : ['#c8b183', '#a88d5f', '#83704c', '#dccaa0']
    ctx.fillStyle = palette[0]; ctx.fillRect(0, 0, 512, 512)
    for (const color of palette.slice(1)) {
      ctx.fillStyle = color
      for (let blob = 0; blob < 7; blob += 1) {
        let x = random() * 512, y = random() * 512
        for (let step = 0; step < 14; step += 1) {
          const radius = 14 + random() * 26
          x += (random() - .5) * 46; y += (random() - .5) * 30
          // Draw wrapped copies so the pattern tiles without seams.
          for (const dx of [-512, 0, 512]) for (const dy of [-512, 0, 512]) {
            ctx.beginPath(); ctx.ellipse(x + dx, y + dy, radius * 1.4, radius, random() * Math.PI, 0, Math.PI * 2); ctx.fill()
          }
        }
      }
    }
  } else if (preset === 'camouflage-digital') {
    const palette = ['#5c683e', '#3a4628', '#7c7050', '#24261e']
    for (let y = 0; y < 512; y += 32) {
      for (let x = 0; x < 512; x += 32) {
        const cluster = Math.sin(x * 0.021 + y * 0.013) + Math.cos(x * 0.009 - y * 0.024) + random() * 1.4
        ctx.fillStyle = palette[Math.min(3, Math.max(0, Math.floor(cluster + 1.6)))]
        ctx.fillRect(x, y, 32, 32)
      }
    }
  } else if (preset === 'stripes') {
    for (let x = 0; x < 512; x += 64) {
      ctx.fillStyle = (x / 64) % 2 ? '#e8e8e4' : '#5a5f66'
      ctx.fillRect(x, 0, 64, 512)
    }
    drawNoise(ctx, random, 1200, ['rgba(0,0,0,.05)', 'rgba(255,255,255,.06)'], 1.4)
  } else if (preset === 'shade-net') {
    // Semi-opaque weave behind the strands keeps the net visible once mipmapping
    // averages the 5 mm strands away at viewing distance.
    ctx.clearRect(0, 0, 512, 512)
    ctx.fillStyle = 'rgba(29,33,31,.55)'; ctx.fillRect(0, 0, 512, 512)
    ctx.strokeStyle = '#1d211f'; ctx.lineWidth = 9
    for (let x = 0; x <= 512; x += 32) { ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, 512); ctx.stroke() }
    ctx.lineWidth = 6
    for (let y = 0; y <= 512; y += 16) { ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(512, y); ctx.stroke() }
  } else if (preset === 'metal') {
    const gradient = ctx.createLinearGradient(0, 0, 512, 0)
    gradient.addColorStop(0, '#515b61'); gradient.addColorStop(.48, '#98a2a6'); gradient.addColorStop(.55, '#626d72'); gradient.addColorStop(1, '#394349')
    ctx.fillStyle = gradient; ctx.fillRect(0, 0, 512, 512)
    for (let y = 0; y < 512; y += 16) { ctx.fillStyle = 'rgba(255,255,255,.055)'; ctx.fillRect(0, y, 512, 2) }
    drawNoise(ctx, random, 650, ['rgba(18,24,27,.12)', 'rgba(255,255,255,.11)'], 1.5)
  } else {
    ctx.fillStyle = preset === 'plaster' ? '#ddd8ca' : '#aaa79e'; ctx.fillRect(0, 0, 512, 512)
    drawNoise(ctx, random, 4200, preset === 'plaster'
      ? ['rgba(115,105,90,.08)', 'rgba(255,255,255,.15)']
      : ['rgba(65,63,59,.09)', 'rgba(255,255,255,.13)'], 2.2)
    for (let index = 0; index < 20; index += 1) {
      ctx.strokeStyle = 'rgba(70,66,59,.05)'; ctx.lineWidth = 1 + random() * 3
      ctx.beginPath(); ctx.moveTo(random() * 512, random() * 512); ctx.lineTo(random() * 512, random() * 512); ctx.stroke()
    }
  }

  const texture = new THREE.CanvasTexture(canvas)
  texture.colorSpace = THREE.SRGBColorSpace
  texture.wrapS = texture.wrapT = THREE.RepeatWrapping
  texture.anisotropy = 4
  texture.needsUpdate = true
  previewTextureCache.set(preset, texture)
  return texture
}

const imageTextureCache = new Map()

// Keys are project-relative paths, so the cache must be dropped whenever the
// project changes or a texture file is re-imported under the same name.
export function invalidateImageTextures(fileName) {
  for (const [key, texture] of [...imageTextureCache]) {
    if (fileName !== undefined && key !== fileName) continue
    imageTextureCache.delete(key)
    texture.dispose()
  }
}

// Class-texture preview: the same PNG and the same projection as the solver
// (row 0 at v = 0, hence flipY = false; nearest texels keep class edges).
function loadImageTexture(fileName) {
  if (!imageTextureCache.has(fileName)) {
    const texture = new THREE.TextureLoader().load(
      `/api/project/image?path=${encodeURIComponent(fileName)}`,
      undefined,
      undefined,
      () => { if (imageTextureCache.get(fileName) === texture) imageTextureCache.delete(fileName) }
    )
    texture.colorSpace = THREE.SRGBColorSpace
    texture.wrapS = texture.wrapT = THREE.RepeatWrapping
    texture.flipY = false
    texture.magFilter = THREE.NearestFilter
    imageTextureCache.set(fileName, texture)
  }
  return imageTextureCache.get(fileName)
}

function previewSpectrum(item, mesh, config) {
  const names = [mesh.name, mesh.parent?.name, mesh.material?.name].filter(Boolean).map((name) => String(name).toLowerCase())
  const binding = (item.meshes || []).find((candidate) => names.includes(String(candidate.name || '').toLowerCase()))
  const spectralName = binding?.spectralName || item.spectralName
  return (config?.spectra || []).find((spectrum) => spectrum.name === spectralName)
}

function applyProjectedUvs(geometry, repeatSize) {
  if (!geometry?.attributes?.position) return geometry
  let result = geometry
  if (geometry.index) {
    result = geometry.toNonIndexed()
    geometry.dispose()
  }
  const position = result.attributes.position
  const uv = new Float32Array(position.count * 2)
  const scale = 1 / Math.max(.05, Number(repeatSize) || 2)
  for (let index = 0; index + 2 < position.count; index += 3) {
    const ax = position.getX(index), ay = position.getY(index), az = position.getZ(index)
    const bx = position.getX(index + 1), by = position.getY(index + 1), bz = position.getZ(index + 1)
    const cx = position.getX(index + 2), cy = position.getY(index + 2), cz = position.getZ(index + 2)
    const abx = bx - ax, aby = by - ay, abz = bz - az
    const acx = cx - ax, acy = cy - ay, acz = cz - az
    const nx = Math.abs(aby * acz - abz * acy)
    const ny = Math.abs(abz * acx - abx * acz)
    const nz = Math.abs(abx * acy - aby * acx)
    for (let vertex = index; vertex < index + 3; vertex += 1) {
      const x = position.getX(vertex), y = position.getY(vertex), z = position.getZ(vertex)
      if (nx >= ny && nx >= nz) { uv[vertex * 2] = z * scale; uv[vertex * 2 + 1] = y * scale }
      else if (ny >= nz) { uv[vertex * 2] = x * scale; uv[vertex * 2 + 1] = z * scale }
      else { uv[vertex * 2] = x * scale; uv[vertex * 2 + 1] = y * scale }
    }
  }
  result.setAttribute('uv', new THREE.BufferAttribute(uv, 2))
  return result
}

function opaqueMaterial(parameters = {}) {
  return new THREE.MeshStandardMaterial({
    ...parameters,
    transparent: false,
    opacity: 1,
    depthTest: true,
    depthWrite: true,
    alphaTest: parameters.alphaTest ?? 0,
    alphaToCoverage: false
  })
}

// Nets take their holes from the texture alpha, so they always blend; an alpha
// cutoff would discard them once mipmaps average the strands below it.
function sceneMaterial(item, parameters = {}, openness = 0, net = false) {
  if (item.type === 'Fog') return new THREE.MeshStandardMaterial({
    ...parameters, color: colorFor(item.type), transparent: true, opacity: 0.22,
    depthTest: true, depthWrite: false, side: THREE.DoubleSide
  })
  if (openness > 0 || net) return new THREE.MeshStandardMaterial({
    ...parameters, transparent: true, opacity: Math.max(0.25, 1 - openness),
    depthTest: true, depthWrite: false, side: THREE.DoubleSide
  })
  return opaqueMaterial(parameters)
}

function waterMaterial(item, config) {
  const [sizeX = 1, , sizeZ = 1] = item.dimensions || []
  const waterProperties = (config?.materials || []).find((entry) => entry.name === item.materialName)?.params || {}
  const windSpeed = Math.max(0, Number(config?.fluid?.windSpeed) || 2)
  const windAngle = THREE.MathUtils.degToRad(Number(config?.fluid?.windDirection) || 0)
  const wavelength = THREE.MathUtils.clamp(Math.min(sizeX, sizeZ) * .18, 1.2, 12)
  const slopeVariance = Math.max(0, Number(waterProperties.slopeVariance) || 0)
  const amplitude = slopeVariance > 0
    ? THREE.MathUtils.clamp(Math.sqrt(slopeVariance) * wavelength * .12, .015, .28)
    : THREE.MathUtils.clamp(.025 + windSpeed * .012, .025, .22)
  return new THREE.ShaderMaterial({
    glslVersion: THREE.GLSL3,
    transparent: true,
    depthTest: true,
    depthWrite: false,
    side: THREE.DoubleSide,
    toneMapped: false,
    uniforms: {
      uTime: { value: 0 },
      uAmplitude: { value: amplitude },
      uFrequency: { value: Math.PI * 2 / wavelength },
      uSpeed: { value: .65 + Math.min(windSpeed, 15) * .08 },
      uWind: { value: new THREE.Vector2(Math.sin(windAngle), Math.cos(windAngle)) }
    },
    vertexShader: `
      uniform float uTime;
      uniform float uAmplitude;
      uniform float uFrequency;
      uniform float uSpeed;
      uniform vec2 uWind;
      out vec3 vWorldPosition;
      out vec3 vWaveNormal;
      out float vWaveHeight;

      void main() {
        vec2 direction1 = normalize(uWind + vec2(0.0001, 0.0));
        vec2 direction2 = vec2(-direction1.y, direction1.x);
        float phase1 = dot(position.xz, direction1) * uFrequency + uTime * uSpeed;
        float phase2 = dot(position.xz, normalize(direction1 + direction2 * 0.72)) * uFrequency * 1.83 + uTime * uSpeed * 1.31;
        float phase3 = dot(position.xz, normalize(direction1 - direction2 * 1.15)) * uFrequency * 3.10 - uTime * uSpeed * 1.72;
        float wave = sin(phase1) * 0.56 + sin(phase2) * 0.29 + sin(phase3) * 0.15;
        float dh1 = cos(phase1) * uAmplitude * 0.56 * uFrequency;
        float dh2 = cos(phase2) * uAmplitude * 0.29 * uFrequency * 1.83;
        float dh3 = cos(phase3) * uAmplitude * 0.15 * uFrequency * 3.10;
        vec2 d2 = normalize(direction1 + direction2 * 0.72);
        vec2 d3 = normalize(direction1 - direction2 * 1.15);
        vec2 gradient = direction1 * dh1 + d2 * dh2 + d3 * dh3;
        vec3 transformed = position;
        transformed.y += wave * uAmplitude;
        vWaveHeight = wave;
        vWaveNormal = normalize(normalMatrix * vec3(-gradient.x, 1.0, -gradient.y));
        vec4 worldPosition = modelMatrix * vec4(transformed, 1.0);
        vWorldPosition = worldPosition.xyz;
        gl_Position = projectionMatrix * modelViewMatrix * vec4(transformed, 1.0);
      }
    `,
    fragmentShader: `
      precision highp float;
      in vec3 vWorldPosition;
      in vec3 vWaveNormal;
      in float vWaveHeight;
      out vec4 outColor;

      void main() {
        vec3 normal = normalize(gl_FrontFacing ? vWaveNormal : -vWaveNormal);
        vec3 viewDirection = normalize(cameraPosition - vWorldPosition);
        vec3 lightDirection = normalize(vec3(0.38, 0.88, 0.27));
        vec3 halfDirection = normalize(lightDirection + viewDirection);
        float diffuse = max(dot(normal, lightDirection), 0.0);
        float specular = pow(max(dot(normal, halfDirection), 0.0), 96.0);
        float fresnel = pow(1.0 - max(dot(normal, viewDirection), 0.0), 4.0);
        vec3 deepWater = vec3(0.018, 0.20, 0.31);
        vec3 crestWater = vec3(0.10, 0.48, 0.61);
        vec3 color = mix(deepWater, crestWater, clamp(0.42 + vWaveHeight * 0.16 + diffuse * 0.28, 0.0, 1.0));
        color = mix(color, vec3(0.34, 0.67, 0.78), fresnel * 0.42);
        color += vec3(1.0, 0.94, 0.78) * specular * 1.35;
        outColor = vec4(pow(color, vec3(1.0 / 2.2)), 0.86);
      }
    `
  })
}

function ellipseWaterGeometry(sizeX, sizeZ, radialSegments, angularSegments) {
  const positions = [0, 0, 0]
  const indices = []
  for (let ring = 1; ring <= radialSegments; ring += 1) {
    const radius = ring / radialSegments
    for (let segment = 0; segment < angularSegments; segment += 1) {
      const angle = segment / angularSegments * Math.PI * 2
      positions.push(Math.cos(angle) * sizeX * .5 * radius, 0, Math.sin(angle) * sizeZ * .5 * radius)
    }
  }
  for (let segment = 0; segment < angularSegments; segment += 1) {
    indices.push(0, 1 + segment, 1 + (segment + 1) % angularSegments)
  }
  for (let ring = 1; ring < radialSegments; ring += 1) {
    const inner = 1 + (ring - 1) * angularSegments
    const outer = inner + angularSegments
    for (let segment = 0; segment < angularSegments; segment += 1) {
      const next = (segment + 1) % angularSegments
      indices.push(inner + segment, outer + segment, outer + next)
      indices.push(inner + segment, outer + next, inner + next)
    }
  }
  const geometry = new THREE.BufferGeometry()
  geometry.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3))
  geometry.setIndex(indices)
  geometry.computeVertexNormals()
  return geometry
}

function generatedWaterPreview(item, config) {
  const [sizeX = 1, , sizeZ = 1] = item.dimensions || []
  const segmentsX = Math.max(16, Math.min(96, Math.round(sizeX * 1.5)))
  const segmentsZ = Math.max(16, Math.min(96, Math.round(sizeZ * 1.5)))
  const ellipse = String(item.shape || '').toLowerCase().includes('ellipsoid')
  const geometry = ellipse
    ? ellipseWaterGeometry(
        sizeX,
        sizeZ,
        Math.max(6, Math.min(24, Math.round(Math.min(sizeX, sizeZ) / 2))),
        Math.max(48, Math.min(192, segmentsX + segmentsZ))
      )
    : new THREE.PlaneGeometry(sizeX, sizeZ, segmentsX, segmentsZ)
  if (!ellipse) geometry.rotateX(-Math.PI / 2)
  geometry.computeVertexNormals()
  const mesh = new THREE.Mesh(geometry, waterMaterial(item, config))
  mesh.userData.waterSurface = true
  mesh.renderOrder = 2
  const group = new THREE.Group()
  group.add(mesh)
  return group
}

function fireVolume(item, config) {
  const parameters = item.generation?.parameters || {}
  const seed = Number(parameters.seed ?? item.medium?.seed ?? 1) || 1
  const windAngle = THREE.MathUtils.degToRad(Number(config?.fluid?.windDirection) || 0)
  const windResponse = THREE.MathUtils.clamp(Number(parameters.windResponse ?? item.medium?.windResponse ?? .35), 0, 2)
  const material = new THREE.ShaderMaterial({
    glslVersion: THREE.GLSL3,
    transparent: true,
    depthTest: true,
    depthWrite: false,
    side: THREE.BackSide,
    toneMapped: false,
    uniforms: {
      uTime: { value: seed * .137 },
      uSeed: { value: seed },
      uWind: { value: new THREE.Vector2(Math.sin(windAngle), Math.cos(windAngle)).multiplyScalar(windResponse) }
    },
    vertexShader: `
      out vec3 vLocalPosition;
      flat out vec3 vRayOrigin;
      void main() {
        vLocalPosition = position;
        vRayOrigin = (inverse(modelMatrix) * vec4(cameraPosition, 1.0)).xyz;
        gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
      }
    `,
    fragmentShader: `
      precision highp float;
      in vec3 vLocalPosition;
      flat in vec3 vRayOrigin;
      out vec4 outColor;
      uniform float uTime;
      uniform float uSeed;
      uniform vec2 uWind;

      float hash31(vec3 p) {
        p = fract(p * 0.1031);
        p += dot(p, p.yzx + 33.33 + uSeed * 0.001);
        return fract((p.x + p.y) * p.z);
      }

      float noise3(vec3 p) {
        vec3 cell = floor(p);
        vec3 f = fract(p);
        f = f * f * (3.0 - 2.0 * f);
        float n000 = hash31(cell + vec3(0, 0, 0));
        float n100 = hash31(cell + vec3(1, 0, 0));
        float n010 = hash31(cell + vec3(0, 1, 0));
        float n110 = hash31(cell + vec3(1, 1, 0));
        float n001 = hash31(cell + vec3(0, 0, 1));
        float n101 = hash31(cell + vec3(1, 0, 1));
        float n011 = hash31(cell + vec3(0, 1, 1));
        float n111 = hash31(cell + vec3(1, 1, 1));
        return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
                   mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
      }

      float fbm(vec3 p) {
        float value = 0.0;
        float amplitude = 0.55;
        for (int octave = 0; octave < 4; ++octave) {
          value += amplitude * noise3(p);
          p = p * 2.03 + vec3(13.1, 7.7, 5.3);
          amplitude *= 0.48;
        }
        return value;
      }

      vec2 rayBox(vec3 origin, vec3 direction) {
        vec3 safeDirection = vec3(
          abs(direction.x) < 0.00001 ? (direction.x < 0.0 ? -0.00001 : 0.00001) : direction.x,
          abs(direction.y) < 0.00001 ? (direction.y < 0.0 ? -0.00001 : 0.00001) : direction.y,
          abs(direction.z) < 0.00001 ? (direction.z < 0.0 ? -0.00001 : 0.00001) : direction.z
        );
        vec3 nearPlane = (-vec3(0.5) - origin) / safeDirection;
        vec3 farPlane = ( vec3(0.5) - origin) / safeDirection;
        vec3 lower = min(nearPlane, farPlane);
        vec3 upper = max(nearPlane, farPlane);
        return vec2(max(max(lower.x, lower.y), lower.z), min(min(upper.x, upper.y), upper.z));
      }

      void main() {
        vec3 rayOrigin = vRayOrigin;
        vec3 rayDirection = normalize(vLocalPosition - rayOrigin);
        vec2 hit = rayBox(rayOrigin, rayDirection);
        float rayStart = max(hit.x, 0.0);
        if (hit.y <= rayStart) discard;

        const int STEPS = 56;
        float stepLength = (hit.y - rayStart) / float(STEPS);
        float jitter = hash31(vec3(gl_FragCoord.xy, uTime)) * stepLength;
        vec3 accumulatedColor = vec3(0.0);
        float accumulatedAlpha = 0.0;

        for (int sampleIndex = 0; sampleIndex < STEPS; ++sampleIndex) {
          float travel = rayStart + jitter + (float(sampleIndex) + 0.5) * stepLength;
          if (travel > hit.y || accumulatedAlpha > 0.985) break;
          vec3 p = rayOrigin + rayDirection * travel;
          float height = clamp(p.y + 0.5, 0.0, 1.0);
          float rise = uTime * 1.7;
          float coarse = fbm(vec3(p.xz * 3.2, height * 2.7 - rise));
          float fine = noise3(vec3(p.xz * 8.0 + coarse, height * 7.0 - rise * 1.9));
          vec2 flutter = vec2(
            noise3(vec3(height * 3.0, rise * 0.55, uSeed)) - 0.5,
            noise3(vec3(uSeed, height * 3.2, rise * 0.48)) - 0.5
          );
          vec2 center = uWind * height * height * 0.22 + flutter * height * 0.16;
          float radius = mix(0.36, 0.035, pow(height, 0.72));
          float radial = length(p.xz - center) / max(radius, 0.01);
          float distortedEdge = 0.88 + (coarse - 0.5) * 0.52 + (fine - 0.5) * 0.20;
          float body = 1.0 - smoothstep(distortedEdge, distortedEdge + 0.28, radial);
          float vertical = smoothstep(0.0, 0.055, height) * (1.0 - smoothstep(0.88, 1.0, height));
          float tongues = smoothstep(0.28, 0.72, coarse + body * 0.30);
          float density = body * vertical * mix(0.42, 1.0, tongues);
          if (density < 0.004) continue;

          float core = (1.0 - smoothstep(0.0, 0.72, radial)) * (1.0 - height * 0.58);
          float heat = clamp(core + density * 0.48, 0.0, 1.0);
          vec3 edgeColor = vec3(1.0, 0.075, 0.004);
          vec3 middleColor = vec3(1.0, 0.48, 0.015);
          vec3 coreColor = vec3(1.0, 0.92, 0.48);
          vec3 flameColor = mix(edgeColor, middleColor, smoothstep(0.08, 0.52, heat));
          flameColor = mix(flameColor, coreColor, smoothstep(0.58, 0.94, heat));
          float sampleAlpha = 1.0 - exp(-density * stepLength * 8.5);
          accumulatedColor += (1.0 - accumulatedAlpha) * flameColor * sampleAlpha;
          accumulatedAlpha += (1.0 - accumulatedAlpha) * sampleAlpha;
        }

        if (accumulatedAlpha < 0.008) discard;
        outColor = vec4(accumulatedColor / max(accumulatedAlpha, 0.001), accumulatedAlpha * 0.92);
      }
    `
  })

  const group = new THREE.Group()
  const volume = new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1), material)
  volume.renderOrder = 4
  volume.frustumCulled = true
  volume.userData.fireVolume = true
  group.add(volume)

  const boundsSource = new THREE.BoxGeometry(1, 1, 1)
  const boundsGeometry = new THREE.EdgesGeometry(boundsSource)
  boundsSource.dispose()
  const bounds = new THREE.LineSegments(
    boundsGeometry,
    new THREE.LineBasicMaterial({ color: 0xff8b32, transparent: true, opacity: .14, depthWrite: false })
  )
  bounds.renderOrder = 3
  group.add(bounds)
  return group
}

function primitive(item, config) {
  const [sizeX = 1, sizeY = 1, sizeZ = 1] = item.dimensions
  if (item.type === 'Fire') {
    const group = fireVolume(item, config)
    const standardAxes = item.medium?.coordinateOrder === 'XYZ'
    group.scale.set(sizeX, standardAxes ? sizeZ : sizeY, standardAxes ? sizeY : sizeZ)
    return group
  }
  if (item.type === 'Fog') {
    const geometry = item.shape.toLowerCase().includes('ellipsoid')
      ? new THREE.SphereGeometry(.5, 24, 14)
      : new THREE.BoxGeometry(1, 1, 1)
    const mesh = new THREE.Mesh(geometry, sceneMaterial(item, { color: colorFor(item.type), roughness: 1 }))
    const standardAxes = item.medium?.coordinateOrder === 'XYZ'
    mesh.scale.set(sizeX, standardAxes ? sizeZ : sizeY, standardAxes ? sizeY : sizeZ)
    mesh.renderOrder = 3
    return mesh
  }
  const geometry = item.shape.toLowerCase().includes('ellipsoid')
    ? new THREE.SphereGeometry(.5, 16, 10)
    : new THREE.BoxGeometry(1, 1, 1)
  const mesh = new THREE.Mesh(geometry, sceneMaterial(item, { color: colorFor(item.type), roughness: .82 }))
  // Project dimensions already use Three.js order: X, Y-up, Z.
  mesh.scale.set(sizeX, sizeY, sizeZ)
  return mesh
}

function groundedPreviewPlacement(item, placement, footprint, config) {
  if (!isGroundedObject(item)) return placement
  const z = groundedPlacementHeight(footprint,
    { north: placement.x, east: placement.y, scale: placement.scale, rotation: placement.rotation },
    (north, east) => terrainHeight(config, north, east))
  return { ...placement, z }
}

function terrainHeight(config, x, z) {
  if (!config.scene.terrain) return 0
  const dem = config.scene.demInfo
  const width = Math.max(2, Math.round(Number(dem?.terrainWidth) || 0))
  const height = Math.max(2, Math.round(Number(dem?.terrainHeight) || 0))
  const values = Array.isArray(dem?.terrainValues) ? dem.terrainValues : []
  if (values.length !== width * height) return 0
  // GeoTIFF columns grow eastward (+Z); row zero is the northern edge (+X).
  const u = THREE.MathUtils.clamp(Number(z) / Math.max(1, Number(config.scene.y)), 0, 1) * (width - 1)
  const v = (1 - THREE.MathUtils.clamp(Number(x) / Math.max(1, Number(config.scene.x)), 0, 1)) * (height - 1)
  const x0 = Math.floor(u), y0 = Math.floor(v)
  const x1 = Math.min(width - 1, x0 + 1), y1 = Math.min(height - 1, y0 + 1)
  const fallback = Number.isFinite(Number(dem.minimum)) ? Number(dem.minimum) : 0
  const sample = (column, row) => {
    const value = Number(values[row * width + column])
    return Number.isFinite(value) ? value : fallback
  }
  const top = THREE.MathUtils.lerp(sample(x0, y0), sample(x1, y0), u - x0)
  const bottom = THREE.MathUtils.lerp(sample(x0, y1), sample(x1, y1), u - x0)
  return Math.max(0, THREE.MathUtils.lerp(top, bottom, v - y0) - fallback)
}

function nextMovementRandom(movement) {
  movement.randomState = (Math.imul(movement.randomState, 1664525) + 1013904223) >>> 0
  return movement.randomState / 4294967296
}

function chooseMovementDirection(movement) {
  const angle = nextMovementRandom(movement) * Math.PI * 2
  movement.directionX = Math.cos(angle)
  movement.directionZ = Math.sin(angle)
  movement.currentSpeed = Math.max(0, movement.speed + (nextMovementRandom(movement) * 2 - 1) * movement.speedVariation)
  movement.turnRemaining = movement.turnInterval * (.5 + nextMovementRandom(movement))
}

function attachRandomMovement(object, item, placementIndex, config, sceneScale, worldWidth, worldDepth) {
  if ((item.type !== 'Human' && item.type !== 'Vehicle' && item.type !== 'Ship') || !item.movement?.enabled) return
  const movement = {
    enabled: true,
    speed: Math.max(0, Number(item.movement.speed) || 0),
    speedVariation: Math.max(0, Number(item.movement.speedVariation) || 0),
    range: Math.max(.1, Number(item.movement.range) || .1) * sceneScale,
    turnInterval: Math.max(.1, Number(item.movement.turnInterval) || .1),
    randomState: ((Math.round(Number(item.movement.seed) || 1) + Math.imul(placementIndex + 1, 2654435761)) >>> 0),
    originX: object.position.x,
    originZ: object.position.z,
    minX: -worldWidth / 2,
    maxX: worldWidth / 2,
    minZ: -worldDepth / 2,
    maxZ: worldDepth / 2,
    heightOffset: object.position.y - terrainHeight(config, (object.position.x + worldWidth / 2) / sceneScale, (object.position.z + worldDepth / 2) / sceneScale) * sceneScale,
    sceneScale
  }
  chooseMovementDirection(movement)
  object.userData.randomMovement = movement
}

export function updateSceneDynamics(world, deltaSeconds, config) {
  const elapsed = Math.min(.1, Math.max(0, Number(deltaSeconds) || 0))
  if (!elapsed || !config?.scene) return
  const sceneScale = Math.min(1, 45 / Math.max(config.scene.x, config.scene.y, 1))
  const worldWidth = config.scene.x * sceneScale
  const worldDepth = config.scene.y * sceneScale
  const animatedWaterMaterials = new Set()
  world.traverse((object) => {
    if (object.userData.fireVolume && object.material?.uniforms?.uTime) {
      object.material.uniforms.uTime.value += elapsed
    }
    if (object.userData.waterSurface && object.material?.uniforms?.uTime && !animatedWaterMaterials.has(object.material)) {
      object.material.uniforms.uTime.value += elapsed
      animatedWaterMaterials.add(object.material)
    }
    const movement = object.userData.randomMovement
    if (!movement?.enabled) return
    movement.turnRemaining -= elapsed
    if (movement.turnRemaining <= 0) chooseMovementDirection(movement)

    const distance = movement.currentSpeed * movement.sceneScale * elapsed
    let nextX = object.position.x + movement.directionX * distance
    let nextZ = object.position.z + movement.directionZ * distance
    const offsetX = nextX - movement.originX
    const offsetZ = nextZ - movement.originZ
    if (offsetX * offsetX + offsetZ * offsetZ > movement.range * movement.range) {
      const inwardAngle = Math.atan2(movement.originX - object.position.x, movement.originZ - object.position.z)
      const jitter = (nextMovementRandom(movement) - .5) * Math.PI / 3
      movement.directionX = Math.sin(inwardAngle + jitter)
      movement.directionZ = Math.cos(inwardAngle + jitter)
      nextX = object.position.x + movement.directionX * distance
      nextZ = object.position.z + movement.directionZ * distance
    }
    if (nextX < movement.minX || nextX > movement.maxX) movement.directionX *= -1
    if (nextZ < movement.minZ || nextZ > movement.maxZ) movement.directionZ *= -1
    object.position.x = THREE.MathUtils.clamp(object.position.x + movement.directionX * distance, movement.minX, movement.maxX)
    object.position.z = THREE.MathUtils.clamp(object.position.z + movement.directionZ * distance, movement.minZ, movement.maxZ)
    object.rotation.y = -Math.atan2(movement.directionZ, movement.directionX)
    const sceneX = (object.position.x + worldWidth / 2) / sceneScale
    const sceneY = (object.position.z + worldDepth / 2) / sceneScale
    const footprint = object.userData.projectGroundFootprint
    const lift = footprint
      ? groundedPlacementHeight(footprint, { north: sceneX, east: sceneY, scale: object.scale.x / sceneScale, rotation: -THREE.MathUtils.radToDeg(object.rotation.y) },
        (north, east) => terrainHeight(config, north, east)) * sceneScale
      : movement.heightOffset
    object.position.y = terrainHeight(config, sceneX, sceneY) * sceneScale + lift
  })
}

// isCurrent() turns false when a newer load has started; that load owns the scene.
export async function loadXmlScene({ api, world, config, log, isCurrent = () => true }) {
  for (const child of [...world.children]) {
    if (child.userData.projectObject || child.userData.kind === 'vegetation' || child.userData.kind === 'building') {
      world.remove(child)
      dispose(child)
    }
  }

  const items = config.objects.items || []
  const sceneScale = Math.min(1, 45 / Math.max(config.scene.x, config.scene.y, 1))
  const worldWidth = config.scene.x * sceneScale
  const worldDepth = config.scene.y * sceneScale
  const offsetX = Number(config.scene.offsetX) || 0
  const offsetY = Number(config.scene.offsetY) || 0
  const offsetZ = Number(config.scene.offsetZ) || 0
  let instanceCount = 0
  let sourceInstanceCount = 0
  let previewTextureMeshCount = 0
  const objSourceCache = new Map()

  for (const [itemIndex, item] of items.entries()) {
    try {
      let placements = [{ x: config.scene.x / 2, y: config.scene.y / 2, z: 0, scale: 1, rotation: 0 }]
      if (item.positionFile) {
        const result = await api.readText(item.positionFile)
        if (!isCurrent()) return null
        placements = positionsFromText(result.content)
        sourceInstanceCount += placements.length
        // Preview the complete source scene. Offsets only move the calculation
        // domain origin; clipping is applied to the temporary runtime project.
        placements = placements.map((placement) => ({
          ...placement,
          x: placement.x - offsetX,
          y: placement.y - offsetZ,
          z: placement.z - offsetY
        }))
      } else {
        sourceInstanceCount += placements.length
      }
      placements = samplePlacements(placements)

      const participatingMedium = item.type === 'Fire' || item.type === 'Fog' || Boolean(item.medium)
      if (item.fileName && !participatingMedium) {
        let source
        if (item.type === 'Water' && item.sourceKind === 'prim') source = generatedWaterPreview(item, config)
        else {
          // FIXED[中] 性能: 整个 OBJ 以文本读入后在主线程同步 parse，没有体积上限也没有按 fileName 缓存；大模型会长时间阻塞 UI，多个对象引用同一 OBJ 时重复读取解析；已修复：单次加载内按 fileName 缓存解析结果，复用时 clone 并克隆各 Mesh 的 geometry（UV 处理会 dispose 几何体，直接共享会破坏缓存）；体积上限与 Worker 化仍 DEFERRED
          let cached = objSourceCache.get(item.fileName)
          if (!cached) {
            const result = await api.readText(item.fileName)
            if (!isCurrent()) return null
            cached = new OBJLoader().parse(result.content)
            objSourceCache.set(item.fileName, cached)
          }
          source = cached.clone(true)
          source.traverse((child) => { if (child.geometry) child.geometry = child.geometry.clone() })
        }
        const sourceBounds = new THREE.Box3().setFromObject(source)
        if (Number.isFinite(sourceBounds.min.y)) source.position.y -= sourceBounds.min.y
        source.traverse((child) => {
          if (!child.isMesh) return
          child.castShadow = item.type !== 'Water'
          child.receiveShadow = true
          // Imported GIS/City OBJ files frequently contain clockwise roof
          // rings. Render both sides in the editor so valid faces are not
          // hidden solely because their winding is reversed.
          if (item.type === 'Water') {
            if (!child.userData.waterSurface) child.material = waterMaterial(item, config)
            child.userData.waterSurface = true
            child.renderOrder = 2
          } else {
            const hasVertexColors = child.geometry?.hasAttribute('color')
            const spectrum = previewSpectrum(item, child, config)
            const preview = spectrum?.previewTexture
            const classTexture = spectrum?.classTexture?.enabled && spectrum.classTexture.fileName ? spectrum.classTexture : null
            const imageMap = classTexture ? loadImageTexture(classTexture.fileName) : null
            const previewEnabled = !imageMap && Boolean(preview?.enabled)
            if (imageMap || previewEnabled) {
              child.geometry = applyProjectedUvs(child.geometry, imageMap ? classTexture.repeatSize : preview.repeatSize)
              previewTextureMeshCount += 1
            }
            const opennessTexture = spectrum?.opennessTexture?.enabled ? spectrum.opennessTexture : null
            const openness = opennessTexture
              ? (Number(opennessTexture.minOpenness) + Number(opennessTexture.maxOpenness)) / 2
              : Math.max(0, Number(spectrum?.openness) || 0)
            const net = previewEnabled && preview.preset === 'shade-net'
            child.material = sceneMaterial(item, {
              // Realistic library OBJ files embed per-vertex material colors so
              // bark, leaves, glass, tyres and sails remain visually distinct
              // without requiring sidecar texture loading in the editor.
              color: imageMap || previewEnabled || hasVertexColors ? 0xffffff : colorFor(item.type),
              vertexColors: imageMap || previewEnabled ? false : hasVertexColors,
              map: imageMap || (previewEnabled ? createPreviewTexture(String(preview.preset || 'concrete')) : null),
              roughness: .78,
              side: THREE.DoubleSide
            }, openness, net)
          }
        })
        const groundFootprint = item.groundFootprint || { minX: sourceBounds.min.x, maxX: sourceBounds.max.x, minZ: sourceBounds.min.z, maxZ: sourceBounds.max.z }
        for (const [placementIndex, sourcePlacement] of placements.entries()) {
          const placement = groundedPreviewPlacement(item, sourcePlacement, groundFootprint, config)
          const object = source.clone(true)
          object.userData.projectObject = true
          object.userData.projectGroundFootprint = isGroundedObject(item) ? groundFootprint : null
          object.scale.setScalar(sceneScale * placement.scale)
          const groundHeight = terrainHeight(config, placement.x, placement.y)
          object.position.set(placement.x * sceneScale - worldWidth / 2, (placement.z + groundHeight) * sceneScale, placement.y * sceneScale - worldDepth / 2)
          object.rotation.y = -THREE.MathUtils.degToRad(placement.rotation)
          object.userData.projectObjectIndex = itemIndex
          object.userData.projectPlacementIndex = placement.sourceIndex ?? placementIndex
          object.userData.projectPlacement = { ...placement }
          object.userData.projectInitialScale = object.scale.clone()
          object.userData.projectVerticalAnchor = 0
          attachRandomMovement(object, item, placementIndex, config, sceneScale, worldWidth, worldDepth)
          world.add(object)
          instanceCount += 1
        }
      } else {
        const dimensions = Array.isArray(item.dimensions) ? item.dimensions.map(Number) : [1, 1, 1]
        const groundFootprint = item.groundFootprint || { minX: -dimensions[0] / 2, maxX: dimensions[0] / 2, minZ: -dimensions[2] / 2, maxZ: dimensions[2] / 2 }
        for (const [placementIndex, sourcePlacement] of placements.entries()) {
          const placement = groundedPreviewPlacement(item, sourcePlacement, groundFootprint, config)
          const object = primitive(item, config)
          object.userData.projectObject = true
          object.userData.projectGroundFootprint = isGroundedObject(item) ? groundFootprint : null
          object.userData.kind = String(item.type).toLowerCase().includes('build') ? 'building' : 'vegetation'
          object.scale.multiplyScalar(sceneScale * placement.scale)
          const groundHeight = terrainHeight(config, placement.x, placement.y)
          object.position.set(placement.x * sceneScale - worldWidth / 2, (placement.z + groundHeight) * sceneScale + object.scale.y / 2, placement.y * sceneScale - worldDepth / 2)
          object.rotation.y = -THREE.MathUtils.degToRad(placement.rotation)
          object.userData.projectObjectIndex = itemIndex
          object.userData.projectPlacementIndex = placement.sourceIndex ?? placementIndex
          object.userData.projectPlacement = { ...placement }
          object.userData.projectInitialScale = object.scale.clone()
          object.userData.projectVerticalAnchor = object.scale.y / 2
          attachRandomMovement(object, item, placementIndex, config, sceneScale, worldWidth, worldDepth)
          object.castShadow = object.receiveShadow = true
          world.add(object)
          instanceCount += 1
        }
      }
    } catch (error) {
      if (!isCurrent()) return null
      log(`预览对象 ${item.name} 失败：${error.message}`, 'error')
    }
  }
  const sampleLabel = instanceCount < sourceInstanceCount
    ? `，从 ${sourceInstanceCount} 个实际实例中抽样预览 ${instanceCount} 个`
    : `、${instanceCount} 个场景对象`
  const textureLabel = previewTextureMeshCount ? `，${previewTextureMeshCount} 个 Mesh 使用预览纹理` : ''
  log(`Three.js 已按 project.json 加载 ${items.length} 类${sampleLabel}${textureLabel}`, 'success')
  return instanceCount
}
