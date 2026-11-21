import hashlib
import shutil
import struct
import subprocess
from pathlib import Path

PKG009 = Path(r"E:\全新AI工作区\跨卡NR\参考发布包\0.0.9")
FROZEN_DIR = Path(r"E:\AI\跨卡NR\2077FSR\03_版本冻结\双AFSR跨卡NR双形态流畅版-20261121\放入游戏exe所在目录")
CORE_DLL = Path(r"E:\全新AI工作区\跨卡NR\阶段1-009核心构建\out\dlssnr_core.dll")
STAGE = Path(r"E:\全新AI工作区\跨卡NR\阶段1-009核心构建\stage1-deploy")

def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()

def apply_delta(pack_path: Path, delta_path: Path, out_path: Path):
    pack = bytearray(pack_path.read_bytes())
    assert pack[:8] == b"NRMODEL1", f"Not NRMODEL1: {pack_path}"
    count_pack = struct.unpack_from("<I", pack, 8)[0]
    index = {}
    at = 16
    for _ in range(count_pack):
        n = struct.unpack_from("<I", pack, at)[0]
        name = pack[at + 4 : at + 4 + n].decode("utf-8")
        off, sz = struct.unpack_from("<QQ", pack, at + 4 + n)
        index[name] = (off, sz)
        at += 4 + n + 16
    index_end = at

    d = delta_path.read_bytes()
    assert d[:8] == b"NRXDIFF1", f"Not NRXDIFF1: {delta_path}"
    count = struct.unpack_from("<I", d, 8)[0]

    # Scan to see if any entry is added
    has_added = False
    p = 12
    for _ in range(count):
        n = struct.unpack_from("<I", d, p)[0]
        name = d[p + 4 : p + 4 + n].decode("utf-8")
        sz = struct.unpack_from("<Q", d, p + 4 + n)[0]
        if name not in index:
            has_added = True
            break
        p += 4 + n + 8 + sz
    orig = bytes(pack) if has_added else None

    added = []
    p = 12
    for _ in range(count):
        n = struct.unpack_from("<I", d, p)[0]
        name = d[p + 4 : p + 4 + n].decode("utf-8")
        sz = struct.unpack_from("<Q", d, p + 4 + n)[0]
        p += 4 + n + 8
        if name not in index:
            base = name.split("/", 1)[1]
            assert base in index and index[base][1] == sz, f"Missing base {base}"
            boff = index[base][0]
            b = bytes(orig[boff + i] ^ d[p + i] for i in range(sz))
            added.append((name, b))
        else:
            assert index[name][1] == sz, f"Size mismatch for {name}"
            off = index[name][0]
            for i in range(sz):
                pack[off + i] ^= d[p + i]
        p += sz
    assert p == len(d), f"Delta trailing bytes: {len(d) - p}"

    if added:
        grow = sum(4 + len(name.encode("utf-8")) + 16 for name, _ in added)
        out = bytearray()
        out += pack[:8]
        out += struct.pack("<I", count_pack + len(added))
        out += pack[12:16]
        at = 16
        for _ in range(count_pack):
            n = struct.unpack_from("<I", pack, at)[0]
            off, sz = struct.unpack_from("<QQ", pack, at + 4 + n)
            out += pack[at : at + 4 + n]
            out += struct.pack("<QQ", off + grow, sz)
            at += 4 + n + 16
        end = len(pack) + grow
        for name, b in added:
            nb = name.encode("utf-8")
            out += struct.pack("<I", len(nb)) + nb + struct.pack("<QQ", end, len(b))
            end += len(b)
        out += pack[index_end:]
        for _, b in added:
            out += b
        out_path.write_bytes(out)
    else:
        out_path.write_bytes(pack)

if STAGE.exists():
    shutil.rmtree(STAGE)
STAGE.mkdir(parents=True)

# 1. Copy clean 0.0.9 dlssnr_core.dll
shutil.copy2(CORE_DLL, STAGE / "dlssnr_core.dll")

# 2. Copy 0.0.9 dlssnr-amd assets
dst_assets = STAGE / "dlssnr-amd"
shutil.copytree(PKG009 / "dlssnr-amd", dst_assets)

# 3. Copy & verify base models from frozen dir
silver_src = FROZEN_DIR / "dlssnr-amd" / "dlssnr.bin"
small_src = FROZEN_DIR / "dlssnr-amd" / "dlssnr-small.bin.off"
silver_dst = dst_assets / "CC_SILVER_AARDWOLD.bin"
small_dst = dst_assets / "CC_SMALL_FP8.bin"
shutil.copy2(silver_src, silver_dst)
shutil.copy2(small_src, small_dst)

assert sha256(silver_dst).upper() == "2B41C888CF4155B8958C665BA64018AB0BD25C85FC71A2B6DB86D0D04D1F7FBD"
assert sha256(small_dst).upper() == "7E966E2639DBEC6596AA0BAC3B8761137D2C40CA8CB4ACA30DF7CA10043C6E33"
print("Base models verified OK.")

# 4. Generate CC_SMALL_FP8_EXPERIMENTAL.bin
exp_dst = dst_assets / "CC_SMALL_FP8_EXPERIMENTAL.bin"
apply_delta(small_dst, PKG009 / "experimental/dlssnr-small-experimental.delta", exp_dst)
exp_want = (PKG009 / "experimental/CC_SMALL_FP8_EXPERIMENTAL.bin.sha256").read_text().strip().split()[0].lower()
assert sha256(exp_dst).lower() == exp_want, f"exp sha mismatch: {sha256(exp_dst)} != {exp_want}"
print("CC_SMALL_FP8_EXPERIMENTAL.bin generated & verified OK.")

# 5. Generate CC_SILVER_AARDWOLD_OPTIMIZED.bin
opt_dst = dst_assets / "CC_SILVER_AARDWOLD_OPTIMIZED.bin"
apply_delta(silver_dst, PKG009 / "optimized/dlssnr-optimized.delta", opt_dst)
opt_want = (PKG009 / "optimized/CC_SILVER_AARDWOLD_OPTIMIZED.bin.sha256").read_text().strip().split()[0].lower()
assert sha256(opt_dst).lower() == opt_want, f"opt sha mismatch: {sha256(opt_dst)} != {opt_want}"
print("CC_SILVER_AARDWOLD_OPTIMIZED.bin generated & verified OK.")

# 6. Generate CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin
int4_dst = dst_assets / "CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin"
subprocess.run([
    str(PKG009 / "model-tools/dlssnr-int4-weights.exe"),
    str(opt_dst), str(dst_assets / "int4-ultra"), str(int4_dst)
], check=True)
int4_want = (PKG009 / "model-tools/dlssnr-int4.sha256").read_text().strip().split()[0].lower()
assert sha256(int4_dst).lower() == int4_want, f"int4 sha mismatch: {sha256(int4_dst)} != {int4_want}"
print("CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin generated & verified OK.")

# 7. Prepare dlssnr-amd.ini (0.0.9 base, matching 2077FSR's Preprocess defaults and Model=CC_SMALL_FP8_EXPERIMENTAL)
ini_text = (PKG009 / "ini/dlssnr-amd-optiscaler.ini").read_text(encoding="utf-8")
ini_text = ini_text.replace("Model = CC_SMALL_FP8\n", "Model = CC_SMALL_FP8_EXPERIMENTAL\n", 1)
ini_text = ini_text.replace("Model = CC_SMALL_FP8\r\n", "Model = CC_SMALL_FP8_EXPERIMENTAL\r\n", 1)
ini_text = ini_text.replace("OSD = switch", "OSD = off", 1)
ini_text = ini_text.replace("Enabled = 0\n; 0 = off, no preprocessing runs (default)", "Enabled = 1\n; 0 = off, no preprocessing runs (default)", 1)
ini_text = ini_text.replace("Enabled = 0\r\n; 0 = off, no preprocessing runs (default)", "Enabled = 1\r\n; 0 = off, no preprocessing runs (default)", 1)
ini_text = ini_text.replace("Exposure = off", "Exposure = auto", 1)
ini_text = ini_text.replace("Curve = filmic", "Curve = aces", 1)
ini_text = ini_text.replace("Saturation = 1.00", "Saturation = 1.01", 1)
(STAGE / "dlssnr-amd.ini").write_bytes(ini_text.encode("utf-8"))
print("Stage 1 deployment tree ready at:", STAGE)
