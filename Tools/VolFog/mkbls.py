"""Packs compiled D3D9 shader bytecode (fxc /Fo output) into the GXSH ".bls" container of the WoW 3.3.5a client.

Not in the original client (World of Legends): native volumetric fog (wolVolFog), brief B2. GPL-3.0-only (same licence
as Shaders/VolFog); standard library only.

usage: python -I mkbls.py <out.bls> <permutation0.pso> [<permutation1.pso> ...]
       python -I mkbls.py --check <file.bls> [<file.bls> ...]

Container (little-endian, CGxDevice::IShaderLoad + CGxShader::Load of the client):
    u32 magic 'GXSH' (on disk 48 53 58 47)   u32 version 0x00010003   u32 N (number of permutations)
    N entries: u32 unk_003C, u32 unk_0040, u16 unk_0044, u16 unk_0046, u32 codeSize, bytecode, padding to 4 bytes.

The unk fields are read and kept by the client but nothing in it consumes them; the original files do not leave them at 0
(checked against Terrain1, Combiners_Opaque, FFXBox4, FFXGlow, TerrainSM and the vs_3_0 Terrain, Diffuse_T1, ShadowMap
of patch.MPQ / patch-2.MPQ), so they are reproduced from the bytecode:
    unk_003C  bit mask of the inputs the shader declares (vertex shader: vertex attributes; pixel shader: interpolants)
    unk_0040  bit mask of the outputs (vertex shader: interpolants written; pixel shader: 0x200 = colour 0)
    unk_0044  pixel shader: bit mask of the samplers used (bit n = sN); vertex shader: 0
    unk_0046  always 0
    bit of a usage: position 0, blend weight 1, blend indices 2, normal 3, fog 7, colour n 9 + n, texcoord n 11 + n.
"""
import os
import struct
import sys

MAGIC = 0x47585348      # 'GXSH'
VERSION = 0x00010003

D3DDECLUSAGE = {0: 'position', 1: 'blendweight', 2: 'blendindices', 3: 'normal', 4: 'psize', 5: 'texcoord',
                6: 'tangent', 7: 'binormal', 8: 'tessfactor', 9: 'positiont', 10: 'color', 11: 'fog',
                12: 'depth', 13: 'sample'}
FIXED_BITS = {'position': 0, 'blendweight': 1, 'blendindices': 2, 'normal': 3, 'fog': 7}
COLOR_BASE = 9
TEXCOORD_BASE = 11

REG_INPUT, REG_TEXTURE, REG_OUTPUT, REG_SAMPLER = 1, 3, 6, 10
OP_DCL, OP_COMMENT, OP_END = 31, 0xFFFE, 0xFFFF


def usage_bit(usage, index, where):
    name = D3DDECLUSAGE.get(usage, 'usage %d' % usage)
    if name in FIXED_BITS and index == 0:
        return FIXED_BITS[name]
    if name == 'color' and index < 2:
        return COLOR_BASE + index
    if name == 'texcoord' and index < 8:
        return TEXCOORD_BASE + index
    raise SystemExit('mkbls: %s %s%d has no known unk bit (not seen in the original .bls files); extend usage_bit()'
                     % (where, name, index))


def derive_unk(code):
    """Returns (unk_003C, unk_0040, unk_0044, unk_0046, is_pixel) from D3D9 shader bytecode."""
    if len(code) < 8 or len(code) % 4:
        raise SystemExit('mkbls: bytecode size %d is not a multiple of 4 (is this an fxc /Fo output?)' % len(code))
    tokens = struct.unpack('<%dI' % (len(code) // 4), code)
    kind = tokens[0] >> 16
    if kind not in (0xFFFF, 0xFFFE):
        raise SystemExit('mkbls: bytecode does not start with a D3D9 version token (0x%08x)' % tokens[0])
    is_pixel = kind == 0xFFFF
    major = (tokens[0] >> 8) & 0xFF
    inputs = outputs = samplers = 0
    pos = 1
    ended = False
    while pos < len(tokens):
        token = tokens[pos]
        op = token & 0xFFFF
        if op == OP_END:
            ended = True
            break
        if op == OP_COMMENT:
            pos += 1 + ((token >> 16) & 0x7FFF)
            continue
        length = (token >> 24) & 0xF
        if op == OP_DCL:
            decl, dest = tokens[pos + 1], tokens[pos + 2]
            reg_type = ((dest >> 28) & 7) | ((dest >> 8) & 0x18)
            reg_num = dest & 0x7FF
            usage, index = decl & 0x1F, (decl >> 16) & 0xF
            if reg_type == REG_SAMPLER:
                samplers |= 1 << reg_num
            elif reg_type == REG_INPUT:
                if is_pixel and major < 3:
                    inputs |= 1 << (COLOR_BASE + reg_num)       # ps_2_x: v# are the colour registers
                else:
                    inputs |= 1 << usage_bit(usage, index, 'input')
            elif reg_type == REG_TEXTURE and is_pixel:
                inputs |= 1 << (TEXCOORD_BASE + reg_num)        # ps_2_x: t# are the texcoord registers
            elif reg_type == REG_OUTPUT and not is_pixel:
                outputs |= 1 << usage_bit(usage, index, 'output')
        pos += 1 + length
    if not ended:
        raise SystemExit('mkbls: bytecode has no end token')
    if is_pixel:
        outputs = 1 << COLOR_BASE   # colour 0; every original pixel shader writes exactly this
    else:
        samplers = 0
    return inputs, outputs, samplers, 0, is_pixel


def build(out_path, pso_paths):
    entries = []
    for path in pso_paths:
        with open(path, 'rb') as f:
            code = f.read()
        in_mask, out_mask, sampler_mask, unk46, is_pixel = derive_unk(code)
        entries.append(struct.pack('<IIHHI', in_mask, out_mask, sampler_mask, unk46, len(code)) + code
                       + b'\0' * (-len(code) % 4))
    data = struct.pack('<III', MAGIC, VERSION, len(entries)) + b''.join(entries)
    with open(out_path, 'wb') as f:
        f.write(data)
    print('mkbls: %s: %d permutation(s), %d bytes' % (out_path, len(entries), len(data)))


def check(path):
    """Opens a .bls with the same steps as CGxDevice::IShaderLoad and compares the unk fields with the bytecode."""
    with open(path, 'rb') as f:
        data = f.read()
    path = os.path.basename(path)
    if len(data) < 12:
        print('%s: %d bytes, not a GXSH file (the client treats it as a missing shader)' % (path, len(data)))
        return False
    ok = True
    magic, version, count = struct.unpack_from('<III', data, 0)
    if magic != MAGIC or version != VERSION:
        print('%s: BAD header magic=%08x version=%08x' % (path, magic, version))
        return False
    pos = 12
    mismatches = 0
    for i in range(count):
        if pos + 16 > len(data):
            print('%s: entry %d truncated' % (path, i))
            return False
        u3c, u40, u44, u46, size = struct.unpack_from('<IIHHI', data, pos)
        pos += 16
        code = data[pos:pos + size]
        if len(code) != size:
            print('%s: entry %d bytecode truncated' % (path, i))
            return False
        pos += (size + 3) & ~3
        try:
            d3c, d40, d44, d46, _ = derive_unk(code)
        except SystemExit as exc:
            print('%s: entry %d: %s' % (path, i, exc))
            ok = False
            continue
        if (u3c, u40, u44, u46) != (d3c, d40, d44, d46):
            mismatches += 1
            print('%s: entry %d unk stored %05x/%05x/%04x/%04x derived %05x/%05x/%04x/%04x'
                  % (path, i, u3c, u40, u44, u46, d3c, d40, d44, d46))
    if pos != len(data):
        print('%s: %d unread bytes after %d entries' % (path, len(data) - pos, count))
        ok = False
    print('%s: GXSH v%08x, %d permutation(s), %d bytes, unk fields %s' % (
        path, version, count, len(data), 'all match the bytecode' if not mismatches else '%d MISMATCH' % mismatches))
    return ok and not mismatches


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == '--check':
        results = [check(p) for p in sys.argv[2:]]
        sys.exit(0 if all(results) else 1)
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    build(sys.argv[1], sys.argv[2:])
