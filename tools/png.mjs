// Minimal PNG decoder (8-bit, non-interlaced; grey, RGB, palette, grey+alpha,
// RGBA) returning packed RGB. Used for class and openness textures, whose
// texels must keep exact colours (JPEG blurs class boundaries).
import { inflateSync } from 'node:zlib'

const SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a])
const CHANNELS = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 }

export function decodePng(buffer, label = 'PNG') {
  if (buffer.length < 33 || !buffer.subarray(0, 8).equals(SIGNATURE)) throw new Error(`${label} 不是 PNG 文件`)
  let offset = 8, width = 0, height = 0, bitDepth = 0, colorType = 0, interlace = 0, palette = null
  const data = []
  while (offset + 8 <= buffer.length) {
    const length = buffer.readUInt32BE(offset)
    const type = buffer.toString('ascii', offset + 4, offset + 8)
    const chunk = buffer.subarray(offset + 8, offset + 8 + length)
    if (type === 'IHDR') {
      width = chunk.readUInt32BE(0); height = chunk.readUInt32BE(4)
      bitDepth = chunk[8]; colorType = chunk[9]; interlace = chunk[12]
    } else if (type === 'PLTE') palette = chunk
    else if (type === 'IDAT') data.push(chunk)
    else if (type === 'IEND') break
    offset += 12 + length
  }
  const channels = CHANNELS[colorType]
  if (!width || !height || !channels) throw new Error(`${label} 缺少有效的图像头`)
  if (bitDepth !== 8 || interlace !== 0) throw new Error(`${label} 需为 8 位、非隔行 PNG`)
  if (colorType === 3 && !palette) throw new Error(`${label} 缺少调色板`)
  const raw = inflateSync(Buffer.concat(data))
  const stride = width * channels
  if (raw.length < height * (stride + 1)) throw new Error(`${label} 数据不完整`)
  const pixels = Buffer.alloc(height * stride)
  for (let y = 0; y < height; y += 1) {
    const filter = raw[y * (stride + 1)]
    const source = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1))
    const row = pixels.subarray(y * stride, (y + 1) * stride)
    const previous = y > 0 ? pixels.subarray((y - 1) * stride, y * stride) : null
    for (let x = 0; x < stride; x += 1) {
      const left = x >= channels ? row[x - channels] : 0
      const up = previous ? previous[x] : 0
      const upLeft = previous && x >= channels ? previous[x - channels] : 0
      let value = source[x]
      if (filter === 1) value += left
      else if (filter === 2) value += up
      else if (filter === 3) value += (left + up) >> 1
      else if (filter === 4) {
        const p = left + up - upLeft
        const pa = Math.abs(p - left), pb = Math.abs(p - up), pc = Math.abs(p - upLeft)
        value += pa <= pb && pa <= pc ? left : pb <= pc ? up : upLeft
      } else if (filter !== 0) throw new Error(`${label} 包含未知的行滤波类型 ${filter}`)
      row[x] = value & 0xff
    }
  }
  const rgb = new Uint8Array(width * height * 3)
  for (let i = 0; i < width * height; i += 1) {
    const base = i * channels
    if (colorType === 3) {
      const entry = pixels[base] * 3
      rgb[i * 3] = palette[entry]; rgb[i * 3 + 1] = palette[entry + 1]; rgb[i * 3 + 2] = palette[entry + 2]
    } else if (channels <= 2) {
      rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = pixels[base]
    } else {
      rgb[i * 3] = pixels[base]; rgb[i * 3 + 1] = pixels[base + 1]; rgb[i * 3 + 2] = pixels[base + 2]
    }
  }
  return { width, height, rgb }
}
