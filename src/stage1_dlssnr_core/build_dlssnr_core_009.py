import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(r"E:\全新AI工作区\跨卡NR\社区项目\DLSSNR-AMD")
MINGW_BIN = Path(r"E:\全新AI工作区\跨卡NR\工具链\mingw64\bin")
OUT = Path(r"E:\全新AI工作区\跨卡NR\阶段1-009核心构建\out")
STUBS = Path(r"E:\全新AI工作区\跨卡NR\阶段1-009核心构建\nr_clean_ngx_stubs.cpp")

OUT.mkdir(parents=True, exist_ok=True)
CXX = str(MINGW_BIN / "g++.exe")
CC = str(MINGW_BIN / "gcc.exe")
OBJDUMP = str(MINGW_BIN / "objdump.exe")

env = os.environ.copy()
env["PATH"] = str(MINGW_BIN) + os.pathsep + env.get("PATH", "")

def run(cmd):
    p = subprocess.run(cmd, env=env, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if p.returncode != 0:
        print("CMD FAILED:", " ".join(cmd))
        print(p.stdout)
        print(p.stderr)
        sys.exit(p.returncode)
    return p

# 1. Generate nr_device_chain_sizes.hpp
run([sys.executable, str(REPO / "windows/build/generate_device_chain.py"),
     str(REPO / "toolchain/Vulkan-Headers"), str(OUT / "nr_device_chain_sizes.hpp")])

stamp = "0.0.9-clean-ngx"
minhook = REPO / "artifacts/ref/DLSS5-Feeder/external/minhook"
common = [
    "-std=c++17", "-O2", "-DNDEBUG",
    f'-DNR_BUILD_STAMP="{stamp}"',
    "-DNR_INT4=1",
    f"-I{REPO / 'toolchain/Vulkan-Headers/include'}",
    f"-I{REPO / 'windows/src'}",
    f"-I{REPO / 'windows/src/core'}",
    f"-I{REPO / 'windows/src/pe'}",
    f"-I{OUT}",
]
rdna4_defines = [
    "-DNR_GEMM_WIDE_MT=64", "-DNR_GEMM_WIDE_NT=256",
    "-DNR_GEMM_PROJ_MT=32", "-DNR_GEMM_PROJ_NT=128",
    "-DNR_PROJW_MIN_TOKENS=768", "-DNR_GEMM_REMAP_HOST=1",
    "-DNR_FFWD_WGW=4", "-DNR_FFWD_GMAJOR=1", "-DNR_UPSVIEW_VEC=1", "-DNR_REPACK_VEC=1",
    "-DNR_WIDE_UPS_MASK=7", "-DNR_PERSIST_DF=1", "-DNR_DS_FUSE=15",
    "-DNR_DECUPS_VEC=16", "-DNR_DECQ_SMALL=1",
    "-DNR_PERSIST_DS_MASK=7", "-DNR_PERSIST_UPS_MASK=7", "-DNR_REPACK_FOLD=1", "-DNR_STRAG_PCT=25",
    "-DNR_PERSIST_ONE_MASK=6", "-DNR_NOISE_FIELD=1", "-DNR_FFWD_FM2_MIN_TOKENS=2560", "-DNR_POST_ALPHA=1",
    "-DNR_TCHAIN_TILES=1",
]

cpp_units = [
    ("nr_runtime", REPO / "windows/src/core/nr_runtime.cpp", rdna4_defines),
    ("nr_native_plan", REPO / "windows/src/core/nr_native_plan.cpp", []),
    ("nr_renodx", REPO / "windows/src/core/nr_renodx.cpp", []),
    ("nr_wilsjo2", REPO / "windows/src/core/nr_wilsjo2.cpp", []),
    ("nr_pe_log", REPO / "windows/src/pe/nr_pe_log.cpp", []),
    ("nr_pe_crash", REPO / "windows/src/pe/nr_pe_crash.cpp", []),
    ("nr_pe_interop", REPO / "windows/src/pe/nr_pe_interop.cpp", []),
    ("nr_vram_ledger", REPO / "windows/src/pe/nr_vram_ledger.cpp", []),
    ("nr_pe_vkdevice", REPO / "windows/src/pe/nr_pe_vkdevice.cpp", [f"-I{minhook / 'include'}"]),
    ("nr_pe_session", REPO / "windows/src/pe/nr_pe_session.cpp", []),
    ("nr_pe_bridge", REPO / "windows/src/pe/nr_pe_bridge.cpp", []),
    ("nr_pe_d3d12split", REPO / "windows/src/pe/nr_pe_d3d12split.cpp", []),
    ("nr_pe_config", REPO / "windows/src/pe/nr_pe_config.cpp", []),
    ("nr_dlssnr_model", REPO / "windows/src/pe/nr_dlssnr_model.cpp", []),
    ("nr_pe_renodx_route", REPO / "windows/src/pe/nr_pe_renodx_route.cpp", []),
    ("nr_screenshot", REPO / "windows/src/pe/nr_screenshot.cpp", []),
    ("nr_ngx_core", REPO / "windows/src/pe/nr_ngx_core.cpp", []),
    ("nr_clean_ngx_stubs", STUBS, []),
]

objs = []
for name, src, extra in cpp_units:
    obj = OUT / f"{name}.o"
    if not obj.exists() or src.stat().st_mtime > obj.stat().st_mtime:
        print(f"Compiling {name}...")
        run([CXX, *common, *extra, "-c", str(src), "-o", str(obj)])
    objs.append(str(obj))

c_units = [
    ("mh_hook", minhook / "src/hook.c", []),
    ("mh_buffer", minhook / "src/buffer.c", []),
    ("mh_trampoline", minhook / "src/trampoline.c", []),
    ("mh_hde", minhook / "src/hde/hde64.c", []),
]
for name, src, extra in c_units:
    obj = OUT / f"{name}.o"
    if not obj.exists() or src.stat().st_mtime > obj.stat().st_mtime:
        print(f"Compiling {name}...")
        run([CC, "-O2", f"-I{minhook / 'include'}", f"-I{minhook / 'src'}", *extra, "-c", str(src), "-o", str(obj)])
    objs.append(str(obj))

ldflags = ["-ld3d12", "-ldxgi", "-lole32", "-lversion", "-static", "-static-libgcc", "-static-libstdc++"]

# 2. Probe undefined vk* symbols for lazy loading
probe = subprocess.run([CXX, "-shared", "-o", "NUL", *objs, *ldflags],
                       env=env, capture_output=True, text=True, encoding="utf-8", errors="replace")
probe_text = (probe.stdout or "") + "\n" + (probe.stderr or "")
vk_syms = sorted(set(re.findall(r"(?:undefined reference|more undefined references) to `_*?(vk[A-Za-z0-9_]+)'", probe_text))
                 | set(re.findall(r"(?:undefined reference|more undefined references) to `nr_vk_p_(vk[A-Za-z0-9_]+)'", probe_text)))
non_vk_errors = [line for line in probe_text.splitlines()
                 if "undefined reference" in line
                 and not re.search(r"to `_*?(?:vk|nr_vk_)[A-Za-z0-9_]+'", line)]
if non_vk_errors:
    print("NON-VK UNDEFINED REFERENCES:")
    print("\n".join(non_vk_errors))
    sys.exit(1)

print(f"Found {len(vk_syms)} lazy Vulkan symbols")

# Generate vulkan_lazy.s and vulkan_lazy.c (matching windows/build/vulkan_lazy.sh)
s_lines = ["// Generated for clean 0.0.9 dlssnr_core.dll", "    .text"]
for sym in vk_syms:
    s_lines.append(f"    .globl {sym}\n{sym}:\n    jmp *nr_vk_p_{sym}(%rip)")
s_lines.append("    .data")
for sym in vk_syms:
    s_lines.append(f"    .globl nr_vk_p_{sym}\n    .align 8\nnr_vk_p_{sym}:\n    .quad nr_vk_unresolved")
(OUT / "vulkan_lazy.s").write_text("\n".join(s_lines) + "\n", encoding="utf-8")

inst_re = re.compile(
    r"^vk(CreateInstance|DestroyInstance|EnumerateInstance.*|EnumeratePhysicalDevice.*|"
    r"GetPhysicalDevice.*|CreateDevice|EnumerateDevice.*|GetInstanceProcAddr|GetDeviceProcAddr|"
    r"Create.*SurfaceKHR|DestroySurfaceKHR|.*DebugUtils.*|.*DebugReport.*)$"
)
c_head = [
    "// Generated for clean 0.0.9 dlssnr_core.dll",
    "#include <windows.h>",
    "long nr_vk_unresolved(void) { return -3; }",
]
for sym in vk_syms:
    c_head.append(f"extern void* nr_vk_p_{sym};")
c_head.append("static const char* const nr_vk_names[] = {" + ", ".join(f'"{s}"' for s in vk_syms) + "};")
c_head.append("static void** const nr_vk_slots[] = {" + ", ".join(f"&nr_vk_p_{s}" for s in vk_syms) + "};")
c_head.append("static const unsigned char nr_vk_device_level[] = {" +
              ", ".join("0" if inst_re.match(s) else "1" for s in vk_syms) + "};")

vulkan_lazy_sh = (REPO / "windows/build/vulkan_lazy.sh").read_text(encoding="utf-8")
c_tail = vulkan_lazy_sh.split("cat <<'C'\n", 1)[1].rsplit("\nC\n", 1)[0]
(OUT / "vulkan_lazy.c").write_text("\n".join(c_head) + "\n" + c_tail + "\n", encoding="utf-8")

run([CC, "-c", str(OUT / "vulkan_lazy.s"), "-o", str(OUT / "vulkan_lazy_s.o")])
run([CC, "-O2", "-c", str(OUT / "vulkan_lazy.c"), "-o", str(OUT / "vulkan_lazy_c.o")])

dll_out = OUT / "dlssnr_core.dll"
print("Linking dlssnr_core.dll...")
run([CXX, "-shared", "-o", str(dll_out), *objs, str(OUT / "vulkan_lazy_s.o"), str(OUT / "vulkan_lazy_c.o"), *ldflags])

p_exp = run([OBJDUMP, "-p", str(dll_out)])
exports = sorted(set(re.findall(r"\bNVSDK_NGX_[A-Za-z0-9_]+", p_exp.stdout)))
dll_imports = re.findall(r"DLL Name:\s+(\S+)", p_exp.stdout)
print(f"SUCCESS: {dll_out} ({dll_out.stat().st_size} bytes)")
print(f"NGX exports ({len(exports)}): {', '.join(exports[:10])} ...")
print(f"DLL imports: {', '.join(dll_imports)}")
