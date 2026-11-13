"""Rewrites a GLB that uses EXT_meshopt_compression (as Tripo's current exports do) as a plain GLB, so slicers and Blender
that lack a meshopt decoder can read it. Pure Python, no dependencies; written from the EXT_meshopt_compression spec
(vertex codec v0, index codec v0/v1, filters NONE / EXPONENTIAL / OCTAHEDRAL / QUATERNION).

python glb_unmeshopt.py in.glb out.glb
"""
import json
import math
import struct
import sys

import numpy as np

# ------------------------------------------------------------------------------------------------ vertex codec (0xA0)


def _unzigzag8(v):
    return (-(v & 1) ^ (v >> 1)) & 0xFF


def _decode_bytes_group(data, pos, bitslog2, out):
    if bitslog2 == 0:
        out.extend(b"\0" * 16)
        return pos
    if bitslog2 == 3:
        out.extend(data[pos:pos + 16])
        return pos + 16
    bits = 1 << bitslog2  # 2 or 4
    per_byte = 8 // bits
    header_bytes = 16 // per_byte
    var = pos + header_bytes
    mask = (1 << bits) - 1
    for k in range(header_bytes):
        byte = data[pos + k]
        for j in range(per_byte):
            enc = (byte >> (8 - bits * (j + 1))) & mask
            if enc == mask:
                out.append(data[var])
                var += 1
            else:
                out.append(enc)
    return var


def _decode_bytes(data, pos, size):
    groups = size // 16
    header = pos
    pos += (groups + 3) // 4
    out = bytearray()
    for g in range(groups):
        bitslog2 = (data[header + g // 4] >> ((g % 4) * 2)) & 3
        pos = _decode_bytes_group(data, pos, bitslog2, out)
    return out, pos


def decode_vertex_buffer(data, count, stride):
    if data[0] & 0xF0 != 0xA0 or data[0] & 0x0F != 0:
        raise ValueError("unsupported vertex codec header 0x%02x" % data[0])
    block = min((8192 // stride) & ~15, 256)
    last = bytearray(data[len(data) - stride:])
    out = bytearray(count * stride)
    pos, offset = 1, 0
    while offset < count:
        n = min(block, count - offset)
        aligned = (n + 15) & ~15
        for k in range(stride):
            buf, pos = _decode_bytes(data, pos, aligned)
            p = last[k]
            for i in range(n):
                p = (_unzigzag8(buf[i]) + p) & 0xFF
                out[(offset + i) * stride + k] = p
            last[k] = p
        offset += n
    tail = max(stride, 32)
    if len(data) - pos != tail:
        raise ValueError("vertex stream: %d bytes left, expected %d" % (len(data) - pos, tail))
    return bytes(out)


# ------------------------------------------------------------------------------------------------ index codec (0xE0/1)

def _vbyte(data, pos):
    lead = data[pos]
    pos += 1
    if lead < 128:
        return lead, pos
    result, shift = lead & 127, 7
    for _ in range(4):
        g = data[pos]
        pos += 1
        result |= (g & 127) << shift
        shift += 7
        if g < 128:
            break
    return result, pos


def _index(data, pos, last):
    v, pos = _vbyte(data, pos)
    d = (v >> 1) ^ (-(v & 1) & 0xFFFFFFFF)
    return (last + d) & 0xFFFFFFFF, pos


def decode_index_buffer(data, count):
    if data[0] & 0xF0 != 0xE0 or data[0] & 0x0F > 1:
        raise ValueError("unsupported index codec header 0x%02x" % data[0])
    version = data[0] & 0x0F
    fecmax = 13 if version >= 1 else 15
    edge = [[0xFFFFFFFF, 0xFFFFFFFF] for _ in range(16)]
    vert = [0xFFFFFFFF] * 16
    eo = vo = 0
    nxt = last = 0
    code = 1
    pos = code + count // 3
    aux = data[len(data) - 16:]
    out = [0] * count

    def push_v(v, cond=1):
        nonlocal vo
        vert[vo] = v
        vo = (vo + cond) & 15

    def push_e(a, b):
        nonlocal eo
        edge[eo] = [a, b]
        eo = (eo + 1) & 15

    for i in range(0, count, 3):
        codetri = data[code]
        code += 1
        if codetri < 0xF0:
            fe = codetri >> 4
            a, b = edge[(eo - 1 - fe) & 15]
            fec = codetri & 15
            if fec < fecmax:
                c = nxt if fec == 0 else vert[(vo - 1 - fec) & 15]
                fec0 = 1 if fec == 0 else 0
                nxt += fec0
                out[i:i + 3] = (a, b, c)
                push_v(c, fec0)
            else:
                if fec != 15:
                    c = (last + (fec - (fec ^ 3))) & 0xFFFFFFFF
                else:
                    c, pos = _index(data, pos, last)
                last = c
                out[i:i + 3] = (a, b, c)
                push_v(c)
            push_e(c, b)
            push_e(a, c)
        elif codetri < 0xFE:
            codeaux = aux[codetri & 15]
            feb, fec = codeaux >> 4, codeaux & 15
            a = nxt
            nxt += 1
            b = nxt if feb == 0 else vert[(vo - feb) & 15]
            feb0 = 1 if feb == 0 else 0
            nxt += feb0
            c = nxt if fec == 0 else vert[(vo - fec) & 15]
            fec0 = 1 if fec == 0 else 0
            nxt += fec0
            out[i:i + 3] = (a, b, c)
            push_v(a)
            push_v(b, feb0)
            push_v(c, fec0)
            push_e(b, a)
            push_e(c, b)
            push_e(a, c)
        else:
            codeaux = data[pos]
            pos += 1
            fea = 0 if codetri == 0xFE else 15
            feb, fec = codeaux >> 4, codeaux & 15
            if codeaux == 0:
                nxt = 0
            if fea == 0:
                a = nxt
                nxt += 1
            else:
                a = 0
            if feb == 0:
                b = nxt
                nxt += 1
            else:
                b = vert[(vo - feb) & 15]
            if fec == 0:
                c = nxt
                nxt += 1
            else:
                c = vert[(vo - fec) & 15]
            if fea == 15:
                a, pos = _index(data, pos, last)
                last = a
            if feb == 15:
                b, pos = _index(data, pos, last)
                last = b
            if fec == 15:
                c, pos = _index(data, pos, last)
                last = c
            out[i:i + 3] = (a, b, c)
            push_v(a)
            push_v(b, 1 if feb in (0, 15) else 0)
            push_v(c, 1 if fec in (0, 15) else 0)
            push_e(b, a)
            push_e(c, b)
            push_e(a, c)
    if pos != len(data) - 16:
        raise ValueError("index stream did not end at the codeaux table")
    return out


# ------------------------------------------------------------------------------------------------ filters

def filter_exponential(raw):
    v = np.frombuffer(raw, dtype="<u4").astype(np.int64)
    m = ((v & 0xFFFFFF) ^ 0x800000) - 0x800000
    e = ((v >> 24) ^ 0x80) - 0x80
    return (m.astype(np.float64) * np.exp2(e.astype(np.float64))).astype("<f4").tobytes()


def filter_octahedral(raw, stride):
    if stride == 4:
        a = np.frombuffer(raw, dtype="i1").reshape(-1, 4).astype(np.float64)
        maxv = 127.0
    else:
        a = np.frombuffer(raw, dtype="<i2").reshape(-1, 4).astype(np.float64)
        maxv = 32767.0
    one = a[:, 2]
    x, y = a[:, 0] / one, a[:, 1] / one
    z = 1 - np.abs(x) - np.abs(y)
    t = np.maximum(-z, 0)
    x = x - np.where(x >= 0, t, -t)
    y = y - np.where(y >= 0, t, -t)
    n = np.sqrt(x * x + y * y + z * z)
    res = np.stack([x / n, y / n, z / n], 1) * maxv
    out = a.copy()
    out[:, :3] = np.round(res)
    return out.astype("i1" if stride == 4 else "<i2").tobytes()


def filter_quaternion(raw, stride):
    raise ValueError("quaternion filter not supported (rotations are not used by meshes)")


# ------------------------------------------------------------------------------------------------ GLB rewrite

def main():
    src, dst = sys.argv[1], sys.argv[2]
    b = open(src, "rb").read()
    jl = struct.unpack("<I", b[12:16])[0]
    j = json.loads(b[20:20 + jl])
    at = 20 + jl
    bl = struct.unpack("<I", b[at:at + 4])[0]
    binary = b[at + 8:at + 8 + bl]

    def buffer_bytes(index):
        if index != 0 or "uri" in j["buffers"][index]:
            raise ValueError("only the GLB's own buffer is supported")
        return binary

    out_bin = bytearray()
    views = []
    for v in j["bufferViews"]:
        ext = v.get("extensions", {}).get("EXT_meshopt_compression")
        if ext:
            data = buffer_bytes(ext["buffer"])[ext.get("byteOffset", 0):ext.get("byteOffset", 0) + ext["byteLength"]]
            count, stride, mode = ext["count"], ext["byteStride"], ext["mode"]
            if mode == "ATTRIBUTES":
                raw = decode_vertex_buffer(data, count, stride)
                flt = ext.get("filter", "NONE")
                if flt == "EXPONENTIAL":
                    raw = filter_exponential(raw)
                elif flt == "OCTAHEDRAL":
                    raw = filter_octahedral(raw, stride)
                elif flt == "QUATERNION":
                    raw = filter_quaternion(raw, stride)
            elif mode == "TRIANGLES":
                idx = decode_index_buffer(data, count)
                raw = np.array(idx, dtype="<u2" if stride == 2 else "<u4").tobytes()
            else:
                raise ValueError("unsupported meshopt mode " + mode)
            nv = {"buffer": 0, "byteLength": len(raw)}
            if mode == "ATTRIBUTES":
                nv["byteStride"] = stride
            if "target" in v:
                nv["target"] = v["target"]
        else:
            raw = buffer_bytes(v.get("buffer", 0))[v.get("byteOffset", 0):v.get("byteOffset", 0) + v["byteLength"]]
            nv = {k: val for k, val in v.items() if k not in ("buffer", "byteOffset", "extensions")}
            nv["buffer"] = 0
            nv["byteLength"] = len(raw)
        while len(out_bin) % 4:
            out_bin.append(0)
        nv["byteOffset"] = len(out_bin)
        out_bin.extend(raw)
        views.append(nv)
    while len(out_bin) % 4:
        out_bin.append(0)
    j["bufferViews"] = views
    j["buffers"] = [{"byteLength": len(out_bin)}]
    for key in ("extensionsUsed", "extensionsRequired"):
        if key in j:
            j[key] = [e for e in j[key] if e != "EXT_meshopt_compression"]
            if not j[key]:
                del j[key]
    js = json.dumps(j, separators=(",", ":")).encode()
    js += b" " * ((4 - len(js) % 4) % 4)
    total = 12 + 8 + len(js) + 8 + len(out_bin)
    with open(dst, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
        f.write(struct.pack("<II", len(out_bin), 0x004E4942) + out_bin)
    print(f"{dst}: {len(views)} buffer views, {total / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
