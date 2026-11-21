# Third-Party Notices & Acknowledgments

This repository incorporates or builds upon several open-source components and community projects. This document lists each dependency, its author/repository, and its applicable license terms.

---

## 1. Upstream Inspirations & Core Runtime Reference

### DLSSNR-AMD (v0.0.9)
- **Author / Repository**: `mochizuki0323` — <https://gitlab.com/mochizuki0323/DLSSNR-AMD/-/releases>
- **License**: **MIT License** (`Copyright (c) 2026 mochizuki0323`)
- **Usage in This Project**:
  - Provides the AMD secondary-GPU neural rendering runtime (`dlssnr_core.dll`, built from `v0.0.9` with single-GPU present hooks and passthrough paths stubbed out via `src/stage1_dlssnr_core/build_dlssnr_core_009.py` & `nr_clean_ngx_stubs.cpp`).
  - Default configuration `configs/dlssnr-amd.ini` originates from `DLSSNR-AMD`.
  - Model weight packs (`dlssnr-amd/*.bin`, `aco/`, `shaders/`) are **not bundled** in this Git repository due to GitHub file size limits (>100 MB); obtain them directly from the official `mochizuki0323/DLSSNR-AMD` v0.0.9 release.

### Neural-coprocessor (MGPU Bridge)
- **Author / Repository**: `maohgad-web` — <https://github.com/maohgad-web/Neural-coprocessor>
- **License**: **MIT License**
- **Usage in This Project**:
  - Provided the foundational architectural inspiration: splitting DX12 rendering and terminal neural post-processing across two GPUs (Render GPU -> Coprocessor GPU -> composite back to display/swapchain), adapted here into a native AMD dual-GPU pipeline driven by FSR/OptiScaler capture and shared-handle Vulkan/D3D12 interop.

### dlss-5-amd-project
- **Author / Repository**: `TheAutomatic` — <https://github.com/TheAutomatic/dlss-5-amd-project>
- **License**: Hybrid (**PolyForm Noncommercial License 1.0.0** for project scripts/tooling; MIT / GPL-3.0 for respective upstream components)
- **Collaboration**: Reference point for community licensing and `OptiScaler` + `DLSSNR-AMD` integration etiquette; explicitly covered by the Additional Upstream Collaboration Grant in `LICENSE`.

---

## 2. Bundled Source Dependencies (`third_party/`)

### Dear ImGui (v1.90.9)
- **Repository**: <https://github.com/ocornut/imgui>
- **License**: **MIT License** (see `third_party/imgui-1.90.9/LICENSE.txt`)
- **Usage**: In-game DX12 parameter overlay (`Del` menu).

### MinHook
- **Repository**: <https://github.com/TsudaKageyu/minhook>
- **License**: **BSD 2-Clause License** (see `third_party/minhook/LICENSE.txt`)
- **Usage**: Runtime hooking of OptiScaler NGX D3D12 entry points and DXGI SwapChain `Present/Present1`.

---

## 3. External Runtime Host (Not Bundled in Repository)

### OptiScaler (`v10.0.0-pre1` / Nightly `20261030`, commit `e7e3872d`)
- **Repository**: <https://github.com/optiscaler/OptiScaler> (Tested build from <https://github.com/optiscaler/OptiScaler-nightly/releases/tag/nightly-20261030>, also distributed unmodified inside `mochizuki0323/DLSSNR-AMD` v0.0.9)
- **License**: **GNU General Public License v3.0 (GPL-3.0)**
- **Integration Boundary**:
  - `CoGPU-For-FSRtoAMD` does **not** include, compile, or statically link OptiScaler GPL-3.0 source code, nor does this repository redistribute OptiScaler binaries (`dxgi.dll`, XeSS/FFX SDK DLLs).
  - Instead, `cogpu_nr.asi` implements the generic `InitializeASI` plugin entry and runtime NGX/DXGI interface, loaded dynamically by the user's own OptiScaler installation (`LoadAsiPlugins=true`).
  - Please respect OptiScaler's GPL-3.0 terms when obtaining and using OptiScaler separately.
