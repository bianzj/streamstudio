"""Minimal GeoTIFF reader (numpy only) for HiStream outputs: strips/tiles, compression 1/8/32946(deflate),
optional LZW(5), predictor 1/2/3, planar config 1/2, float32/float64/int types. Also ENVI reader."""
import struct, zlib, json, re, os
import numpy as np

TAGS = {256: 'width', 257: 'height', 258: 'bps', 259: 'compression', 262: 'photometric', 273: 'strip_offsets',
        277: 'spp', 278: 'rows_per_strip', 279: 'strip_counts', 284: 'planar', 317: 'predictor', 322: 'tile_w',
        323: 'tile_h', 324: 'tile_offsets', 325: 'tile_counts', 339: 'sample_format', 42112: 'gdal_metadata',
        42113: 'gdal_nodata', 270: 'description'}
TYPES = {1: ('B', 1), 2: ('s', 1), 3: ('H', 2), 4: ('I', 4), 5: ('II', 8), 6: ('b', 1), 7: ('B', 1), 8: ('h', 2),
         9: ('i', 4), 10: ('ii', 8), 11: ('f', 4), 12: ('d', 8), 16: ('Q', 8), 17: ('q', 8), 18: ('Q', 8)}


def _lzw_decode(data):
    # TIFF LZW (MSB-first, early change)
    out = bytearray(); dictionary = [bytes([i]) for i in range(256)] + [b'', b'']
    bitpos = 0; nbits = 9; prev = None; n = len(data) * 8
    def read():
        nonlocal bitpos
        if bitpos + nbits > n: return 257
        byte = bitpos >> 3; val = int.from_bytes(data[byte:byte + 4].ljust(4, b'\0'), 'big')
        val = (val >> (32 - nbits - (bitpos & 7))) & ((1 << nbits) - 1); bitpos += nbits; return val
    while True:
        code = read()
        if code == 257: break
        if code == 256:
            dictionary = dictionary[:258]; nbits = 9; code = read()
            if code == 257: break
            out += dictionary[code]; prev = dictionary[code]; continue
        if code < len(dictionary): entry = dictionary[code]; dictionary.append(prev + entry[:1])
        else: entry = prev + prev[:1]; dictionary.append(entry)
        out += entry; prev = entry
        if len(dictionary) + 1 >= (1 << nbits) and nbits < 12: nbits += 1
    return bytes(out)


def read_tiff(path):
    with open(path, 'rb') as f: buf = f.read()
    bo = '<' if buf[:2] == b'II' else '>'
    magic = struct.unpack(bo + 'H', buf[2:4])[0]
    big = magic == 43
    if big: off = struct.unpack(bo + 'Q', buf[8:16])[0]
    else: off = struct.unpack(bo + 'I', buf[4:8])[0]
    if big:
        n = struct.unpack(bo + 'Q', buf[off:off + 8])[0]; p = off + 8; esz = 20
    else:
        n = struct.unpack(bo + 'H', buf[off:off + 2])[0]; p = off + 2; esz = 12
    tags = {}
    for i in range(n):
        e = buf[p + i * esz: p + (i + 1) * esz]
        if big: tag, typ, cnt = struct.unpack(bo + 'HHQ', e[:12]); valraw = e[12:20]; inline = 8
        else: tag, typ, cnt = struct.unpack(bo + 'HHI', e[:8]); valraw = e[8:12]; inline = 4
        fmt, size = TYPES.get(typ, ('B', 1))
        total = size * cnt
        if total <= inline: data = valraw[:total]
        else:
            vo = struct.unpack(bo + ('Q' if big else 'I'), valraw[:8 if big else 4])[0]; data = buf[vo:vo + total]
        if typ == 2: val = data.rstrip(b'\0').decode('latin1')
        elif typ in (5, 10): val = struct.unpack(bo + fmt[0] * (2 * cnt), data)
        else: val = struct.unpack(bo + fmt * cnt, data)
        tags[TAGS.get(tag, tag)] = val
    W = tags['width'][0]; H = tags['height'][0]; spp = tags.get('spp', (1,))[0]
    bps = tags['bps'][0]; sf = tags.get('sample_format', (1,))[0]; comp = tags.get('compression', (1,))[0]
    pred = tags.get('predictor', (1,))[0]; planar = tags.get('planar', (1,))[0]
    dt = {(3, 32): 'f4', (3, 64): 'f8', (1, 8): 'u1', (1, 16): 'u2', (1, 32): 'u4', (2, 8): 'i1', (2, 16): 'i2', (2, 32): 'i4'}[(sf, bps)]
    dtype = np.dtype(bo + dt)

    def decomp(b):
        if comp == 1: return b
        if comp in (8, 32946): return zlib.decompress(b)
        if comp == 5: return _lzw_decode(b)
        raise ValueError('unsupported compression %d' % comp)

    def unpredict(arr, w, nsamp):
        # arr: raw bytes of a block rows x w x nsamp
        if pred == 1: return np.frombuffer(arr, dtype=dtype).copy()
        if pred == 2:
            a = np.frombuffer(arr, dtype=dtype).copy().reshape(-1, w, nsamp)
            return np.cumsum(a, axis=1, dtype=dtype).reshape(-1)
        if pred == 3:
            bs = dtype.itemsize
            a = np.frombuffer(arr, dtype='u1').copy().reshape(-1, w * nsamp * bs)
            a = np.cumsum(a, axis=1, dtype='u1')
            a = a.reshape(-1, bs, w * nsamp).transpose(0, 2, 1)  # bytes stored MSB-first planes
            if bo == '<': a = a[:, :, ::-1]
            return np.ascontiguousarray(a).view(np.dtype(bo + dt)).reshape(-1)
        raise ValueError('pred')

    nsamp_block = spp if planar == 1 else 1
    nplanes = 1 if planar == 1 else spp
    out = np.zeros((nplanes, H, W, nsamp_block), dtype=dtype.newbyteorder('='))
    if 'tile_w' in tags:
        tw = tags['tile_w'][0]; th = tags['tile_h'][0]
        offs = tags['tile_offsets']; cnts = tags['tile_counts']
        tx = (W + tw - 1) // tw; ty = (H + th - 1) // th
        for i, (o, c) in enumerate(zip(offs, cnts)):
            plane = i // (tx * ty); j = i % (tx * ty); r0 = (j // tx) * th; c0 = (j % tx) * tw
            a = unpredict(decomp(buf[o:o + c]), tw, nsamp_block).reshape(th, tw, nsamp_block)
            hh = min(th, H - r0); ww = min(tw, W - c0)
            out[plane, r0:r0 + hh, c0:c0 + ww, :] = a[:hh, :ww, :]
    else:
        rps = tags.get('rows_per_strip', (H,))[0]
        offs = tags['strip_offsets']; cnts = tags['strip_counts']
        spplane = (H + rps - 1) // rps
        for i, (o, c) in enumerate(zip(offs, cnts)):
            plane = i // spplane; r0 = (i % spplane) * rps; hh = min(rps, H - r0)
            a = unpredict(decomp(buf[o:o + c]), W, nsamp_block)[: hh * W * nsamp_block].reshape(hh, W, nsamp_block)
            out[plane, r0:r0 + hh, :, :] = a
    if planar == 1: bands = np.moveaxis(out[0], 2, 0)
    else: bands = out[:, :, :, 0]
    meta = {k: v for k, v in tags.items() if k in ('gdal_metadata', 'gdal_nodata', 'description')}
    names = []
    md = tags.get('gdal_metadata')
    if md:
        for m in re.finditer(r'<Item name="DESCRIPTION" sample="(\d+)"[^>]*>([^<]*)</Item>', md):
            names.append((int(m.group(1)), m.group(2)))
    names = [n for _, n in sorted(names)]
    return bands.astype(np.float64), names, meta


def read_envi(hdr_path):
    txt = open(hdr_path, 'r', encoding='utf-8', errors='replace').read()
    def get(key, default=None):
        m = re.search(r'^\s*' + key + r'\s*=\s*(\{[^}]*\}|[^\n]*)', txt, re.M | re.I)
        return m.group(1).strip() if m else default
    samples = int(get('samples')); lines = int(get('lines')); bands = int(get('bands'))
    dt = {'1': 'u1', '2': 'i2', '3': 'i4', '4': 'f4', '5': 'f8', '12': 'u2'}[get('data type')]
    bo = '>' if get('byte order', '0') == '1' else '<'
    il = get('interleave', 'bsq').lower()
    base = os.path.splitext(hdr_path)[0]
    data_path = next((p for p in (base, base + '.dat', base + '.img', base + '.raw', base + '.bin') if os.path.isfile(p)), None)
    a = np.fromfile(data_path, dtype=bo + dt)
    if il == 'bsq': a = a.reshape(bands, lines, samples)
    elif il == 'bil': a = a.reshape(lines, bands, samples).transpose(1, 0, 2)
    else: a = a.reshape(lines, samples, bands).transpose(2, 0, 1)
    bn = get('band names')
    names = [s.strip() for s in bn.strip('{}').split(',')] if bn else []
    return a.astype(np.float64), names, {'header': txt}
