// ==========================================================================
// 文件：png.mjs
// 功能：最小化 PNG 解码器，供 runtime-scene.mjs / surface-textures.mjs 等读取分类纹理与开孔率纹理
// 主要步骤（decodePng）：
//   1. 校验 PNG 签名，遍历数据块，读 IHDR / PLTE / IDAT，遇 IEND 结束
//   2. 合并 IDAT 并 zlib 解压，逐行还原 5 种行滤波（None/Sub/Up/Average/Paeth）
//   3. 把灰度、灰度+透明度、调色板、RGB、RGBA 统一转成紧凑 RGB（丢弃 alpha）
// 输入/输出：Buffer → { width, height, rgb: Uint8Array(width*height*3) }
// 约定：仅支持 8 位、非隔行 PNG；不校验 CRC；颜色不做伽马/色彩空间转换。
// ==========================================================================

// Minimal PNG decoder (8-bit, non-interlaced; grey, RGB, palette, grey+alpha,
// RGBA) returning packed RGB. Used for class and openness textures, whose
// texels must keep exact colours (JPEG blurs class boundaries).
import { inflateSync } from 'node:zlib'

const SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a])
// PNG 颜色类型 → 每像素通道数：0 灰度、2 RGB、3 调色板、4 灰度+alpha、6 RGBA。
const CHANNELS = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 }

// 作用：解码 PNG；label 仅用于错误信息。返回行优先（自上而下）的 RGB 数据。
// 失败（签名、头、位深、隔行、数据不完整、滤波类型）时抛出中文错误。
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
  // 步骤 2：每行前有 1 字节滤波类型；left/up/upLeft 为已还原的相邻字节（边界处按 0 处理）。
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
      // Paeth 预测：选与 p = left + up - upLeft 最接近的邻居。
      else if (filter === 4) {
        const p = left + up - upLeft
        const pa = Math.abs(p - left), pb = Math.abs(p - up), pc = Math.abs(p - upLeft)
        value += pa <= pb && pa <= pc ? left : pb <= pc ? up : upLeft
      } else if (filter !== 0) throw new Error(`${label} 包含未知的行滤波类型 ${filter}`)
      row[x] = value & 0xff
    }
  }
  // 步骤 3：统一输出 RGB；channels ≤ 2（灰度）时三个通道取同一值，alpha 被忽略。
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
