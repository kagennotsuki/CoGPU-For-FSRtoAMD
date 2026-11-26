# CoGPU-For-FSRtoAMD

**使用 FSR/OptiScaler 注入游戏采集帧数据，交由 AMD 副卡进行神经渲染（NR）后再跨卡回传合成的双 A 卡协同方案。**

本仓库对应已验收冻结定版 **`基于mochizuki的FSR跨卡NR-20261121`**（OptiScaler ASI 插件 + `DLSSNR-AMD v0.0.9` 纯净核心 + 无 ReShade 直连 DXGI Present 抓帧），在 **Cyberpunk 2077（主卡 RX 7900 XT + 副卡 RX 9070 XT）** 上完成双形态实机验收。

---

## 致谢与项目起源 / Acknowledgments & Inspiration

- **核心神经渲染运行时参考**：[**mochizuki0323 / DLSSNR-AMD**](https://gitlab.com/mochizuki0323/DLSSNR-AMD/-/releases)（MIT License）
  - 本项目副卡端神经网络推理核心基于 `mochizuki0323/DLSSNR-AMD` **v0.0.9**，并通过 `src/stage1_dlssnr_core/` 中的剥离脚本在编译期禁用单卡 `NR Control` DXGI Hook 与 `D3D12Passthrough`，生成专供跨卡调度的无头 `dlssnr_core.dll`（保留 ACO 模式、4 套模型热切换、INT4 优化档位、16/32-bit 历史精度与 Style Grade）。
- **最初架构思路启发**：[**maohgad-web / Neural-coprocessor**](https://github.com/maohgad-web/Neural-coprocessor)（MIT License）
  - 本项目“渲染主卡负责常规光栅/超分、第二张 GPU 作为协处理器执行终端神经后处理再合回展示”的跨卡协同分工思想最初受 `Neural-coprocessor (MGPU Bridge)` 启发，并在此基础上彻底重写为面向 AMD 副卡、D3D12↔Vulkan 共享句柄零拷贝中转及 FSR/OptiScaler 帧生命周期同步的原生实现。
- **社区协议与协作参考**：[**TheAutomatic / dlss-5-amd-project**](https://github.com/TheAutomatic/dlss-5-amd-project)
  - 参见下方“开源协议与上游回馈授权”。

---

## 核心特性 / Features

- **无需 ReShade / `finalimg.addon64`**：
  - 通过 **OptiScaler ASI 插件 (`OptiScaler/plugins/cogpu_nr.asi`)** 接管游戏 NGX / FSR3.1+ 输入并挂载 DXGI Factory / SwapChain `Present` / `Present1` 直接抓帧。
  - 同时保留不依赖 OptiScaler 的独立 **`amd_fidelityfx_upscaler_dx12.dll`** FFX 代理构建路径。
- **A / B 双运行形态实时热切换（快捷键 `F3`）**：
  - **A 形态（由【游戏卡】负责呈现最终画面 · A3 同帧合成 + 超分）**：主卡采集 Color / MV / Depth 传给副卡推理，答案回传后在同帧资源与真实超分器内合成并由游戏主卡直接呈现（副卡来不及出新帧时在缓存窗内平滑复用最近完成帧）。
  - **B 形态（由【神经卡】负责呈现最终画面 · 4K 后置 DComp 覆盖 + 直连抓帧）**：主 SwapChain 提交前直连抓取 `PRESENT` 后缓冲送副卡完成合成，最终画面由神经卡经独立 DComp 透明覆盖层呈现（`Del` 菜单顶部实时显示当前运行形态与呈现显卡，支持一键切换）。
- **可热切换 NR 推理分辨率倍率（NR Scale · 移植自 mochizuki）**：
  - 支持在 `Del` 菜单内直接切换 `固定 1080p`、`50% / 67% / 75% / 100% 渲染分辨率`、`固定 720p / 900p / 1440p / 4K (3840x2160)` 共 9 档（默认保持经典 `1080p`），自动安全排空双卡队列并重建跨卡共享资源与 0.0.9 会话（`F7 / F8` 跑马灯与三横带诊断已在 A/B 双形态统一关闭）。
- **GPU 预处理闭环实装（`[Preprocess]`）**：
  - 在 `DownCS` / `kBformDS` 与 `ResidCS` / `kBformResid` 计算着色器内直接实现前向/逆向预处理闭环，支持预处理总开关、逆色调映射开关（`Inverse`）、曝光模式/EV 补偿、色调曲线（`none / neutral / reinhard / filmic / gt / aces / agx`）、中灰对比度与饱和度调节（HDR 采用 `PrepSafeRatio` + `log2` 亮度/色度限幅 + BT.2020 宽色域零偏恒等保护），并双向同步 `dlssnr-amd.ini` 与 `xess_proxy.saved.ini`。
- **完整 HDR10 PQ / scRGB 管线支持（A & B 双形态）**：
  - 支持游戏内 `HDR10 PQ` (`R10G10B10A2_UNORM` / ST.2084) 与 `scRGB` (`RGBA16F`) 自动识别、线性归一化喂图及双形态无损彩色/高亮峰值还原。
- **多遍快速叠层与深度全链路接入 (`Passes fast` · `DLSSNR-AMD v0.0.9`)**：
  - 在神经卡单次 `dlssnr_core` 推理下等效 `2 ~ 8` 遍 DLSS5 叠层：支持 `Interleaved`（每槽位独立 NGX 历史句柄、完整 `R32F` 深度打包接入 `DLSSNR.Depth` 与 Depth 3 件套抗重影、6 级 Push-Pull 与末帧直出保护）、`Extrapolation`、`Feedback`、`Hybrid` 与 `Edit strength` 共 5 种模式（开启时核心统一使用 `RGBA16F`，A/B 双形态与 SDR/HDR 全共用）。
- **完整菜单控制与配置持久化（快捷键 `Del`）**：
  - 0.0.9 网络模型热切换（`CC_SMALL_FP8_EXPERIMENTAL` / `CC_SMALL_FP8` / `CC_SILVER_AARDWOLD_OPTIMIZED` / `CC_SILVER_AARDWOLD`）、优化档位、16/32-bit 历史精度、Style Grade、NR Scale、Intensity/Style/LocalTone/LocalStruct/SkinStruct 控制与保存/撤销/恢复默认按钮。

---

## 仓库目录结构 / Repository Layout

```text
CoGPU-For-FSRtoAMD/
├── src/                         # 核心源码（C++/HLSL/NGX Feature18 适配/A2-A3 环形同步）
│   └── stage1_dlssnr_core/      # DLSSNR-AMD v0.0.9 无头 dlssnr_core.dll 剥离与构建脚本
├── third_party/                 # Dear ImGui v1.90.9 (MIT) & MinHook (BSD-2-Clause)
├── scripts/                     # 便携式本地一键编译脚本（ASI 插件 & 独立 FFX 代理）
├── configs/                     # xess_proxy.* 开关配置、Add4K HLSL 与 dlssnr-amd.ini
├── prebuilt/                    # 自研编译成品（cogpu_nr.asi / dlssnr_core.dll / 备选 FFX DLL）
├── verification/                # SHA256 指纹清单 (fingerprints.tsv) 与定版实机验收日志快照
├── LICENSE                      # PolyForm Noncommercial 1.0.0 + 上游项目 MIT 协作回馈授权
└── THIRD_PARTY_NOTICES.md       # 第三方组件与上游致谢声明
```

> **为什么仓库里不包含 `dxgi.dll` 与 `dlssnr-amd/*.bin`？**
> 1. `DLSSNR-AMD v0.0.9` 的神经网络权重包（`CC_SILVER_AARDWOLD*.bin`、`CC_SMALL_FP8*.bin`）单个体积达 `114 ~ 148 MB`，超过 GitHub 单文件 `100 MB` 的 Git 硬限制；
> 2. **OptiScaler** 采用 **GPL-3.0** 协议，本仓库坚持**仅提供自研代码及自身构建产物，不捆绑分发上游闭源 SDK 或 GPL 二进制文件**。请按下述步骤从各自官方页面获取。

---

## 部署方法 / Installation (Cyberpunk 2077 / DX12 FSR Games)

前置准备：**清理旧残留** —— 确认游戏 `.exe` 目录（如 `Cyberpunk 2077\bin\x64\`）内**没有**旧版 ReShade `dxgi.dll`、`finalimg.addon64` 或此前手工替换过的 `amd_fidelityfx_upscaler_dx12.dll`（如有请先还原游戏原版），并在 **Windows 图形设置** 中将游戏主程序指定给**主渲染显卡**（如 RX 7900 XT）。

### 方式一：基于 `mochizuki DLSSNR-AMD 0.0.9` 一键安装（推荐，无需单独下载 OptiScaler）

> `DLSSNR-AMD 0.0.9` 官方发行压缩包内已自带适配验证过的 `OptiScaler`（`v10.0.0-pre1 / nightly-20261030`）、配置与 `install.bat` 安装器。

1. **用 mochizuki 安装器打底**：从 [**mochizuki0323 / DLSSNR-AMD v0.0.9 Releases**](https://gitlab.com/mochizuki0323/DLSSNR-AMD/-/releases) 下载 Windows 包并解压，运行 `install.bat`，按提示选择 **OptiScaler** 路线并导入你自备的 `nvngx_dlssnr.dll`，将游戏目标指向游戏 `.exe` 目录（会自动配齐 `dxgi.dll`、`OptiScaler\`、`dlssnr-amd\` 权重模型目录并启用 `LoadAsiPlugins=true`）。
2. **删除单卡版插件（关键）**：进入 `<游戏目录>\OptiScaler\plugins\`，**删除单卡版 `dlssnr-amd.asi`**（必须删除，避免与本项目的跨卡插件冲突）。
3. **覆盖本项目压缩包**：从本仓库 [**Releases**](https://github.com/kagennotsuki/CoGPU-For-FSRtoAMD/releases) 下载 `CoGPU-For-FSRtoAMD-*.zip`，解压覆盖到游戏 `.exe` 同目录（含 `dlssnr_core.dll`、`OptiScaler/plugins/cogpu_nr.asi` 及全套 `xess_proxy.*` / `dlssnr-amd.ini`）。
4. **核对双卡与 OptiScaler 参数**：
   - `xess_proxy.gpu.txt` **默认已全部注释（纯自动识别）**：插件会自动排除**游戏主卡 LUID** 与微软软件渲染器，**同型号双卡**也能直接选中另一张副卡，一般无需修改；若机箱内有 3 张及以上独显想人工钉死某一张，再取消对应行注释（支持 `nr=dev:` / `nr=luid:<hi>:<lo>` / `nr=sub:<SubSysId>` / `nr=idx:<序号>`）；
   - 打开 `OptiScaler.ini` 确认以下三项（`LoadAsiPlugins=true` 已由安装器设置，请重点检查后两项）：
     ```ini
     [Plugins]
     LoadAsiPlugins=true
     [Spoofing]
     Dxgi=false
     [FrameGen]
     Enabled=false
     ```

### 方式二：从 OptiScaler 仓库单独手工安装（仅当你想使用其它版本 OptiScaler 时）

1. 从 [**OptiScaler Nightly (`v10.0.0-pre1` · 实测 `nightly-20261030` / commit `e7e3872d`)**](https://github.com/optiscaler/OptiScaler-nightly/releases/tag/nightly-20261030)（[上游主仓库](https://github.com/optiscaler/OptiScaler)；旧 `0.7.x` 稳定版菜单样式不同且缺少配套 ASI 宿主支持）下载后解压至游戏 `.exe` 目录，运行 `setup_windows.bat` 选择以 **`dxgi.dll`** 注入。
2. 从 [**mochizuki0323 / DLSSNR-AMD v0.0.9 Releases**](https://gitlab.com/mochizuki0323/DLSSNR-AMD/-/releases) 提取或放入 `dlssnr-amd\` 模型与着色器目录（含 `CC_*.bin`、`aco/`、`shaders/`）。
3. 解压覆盖本仓库 Release 压缩包，并按方式一步骤 4 核对 `xess_proxy.gpu.txt` 与 `OptiScaler.ini` 三项参数。

---

## 快捷键与菜单 / Hotkeys

| 按键 | 功能 | 说明 |
|---|---|---|
| **`Insert`** | OptiScaler 菜单 | 切换超分后端、查看输入与性能叠加 |
| **`Del`** | 跨卡 NR 参数菜单 | 模型热切换、强度/风格控制、GPU 预处理与存档持久化 |
| **`F3`** | 切换 **A 形态 ↔ B 形态** | A3 同帧合成 与 B-4K 独立窗口叠加层热切换 |
| **`F10`** | 开启 / 关闭跨卡 NR | 运行时总开关 |

---

## 本地从源码编译 / Building from Source

前置要求：**Windows + Visual Studio 2022 (Build Tools / Community with C++ x64) + Windows SDK**。

```powershell
# 编译 OptiScaler ASI 插件 (输出 out/asi/cogpu_nr.asi)
powershell -ExecutionPolicy Bypass -File .\scripts\build_cogpu_asi.ps1

# 编译备选独立 FFX 代理 DLL (输出 out/ffx/amd_fidelityfx_upscaler_dx12.dll)
powershell -ExecutionPolicy Bypass -File .\scripts\build_ffx_proxy.ps1
```

若需从 `mochizuki0323/DLSSNR-AMD v0.0.9` 上游源码重新剥离编译无头 `dlssnr_core.dll`，请参考 `src/stage1_dlssnr_core/build_dlssnr_core_009.py` 与 `nr_clean_ngx_stubs.cpp`（需对应 MinGW-w64 GCC 工具链与 Vulkan Headers）。

---

## 已验证产物指纹 / Verified Fingerprints

详见 [`verification/fingerprints.tsv`](verification/fingerprints.tsv)：

| 文件 | 大小 (Bytes) | SHA256 |
|---|---:|---|
| `prebuilt/OptiScaler/plugins/cogpu_nr.asi` | `1,263,104` | `F6471EC8DF232C23FB47294B04B6BCC69B821E49F964EF66F573DCA8739D6418` |
| `prebuilt/dlssnr_core.dll` | `6,035,547` | `89CBA727B4440EA99F74354DB866285C3498044DFB43EF8E905EF4ECAF617642` |
| `prebuilt/standalone_ffx_proxy/amd_fidelityfx_upscaler_dx12.dll` | `1,231,360` | `7484B17E583C7FC2AE63E296E559AD9081C1DFD0E2F02271553159B5C46460D9` |

---

## 开源协议与协作授权 / License & Upstream Collaboration Grant

- 本仓库原创源码、适配层与配置默认采用 **[PolyForm Noncommercial License 1.0.0](LICENSE)** 发布：**允许个人学习、研究、修改与非商业使用；禁止任何形式的商业倒卖、付费整合包或收费代装。**
- **上游双向协作特别授权**：为便于技术交流与上游回馈，作者 `kagennotsuki` 在 [`LICENSE`](LICENSE) 中明确向 **[mochizuki0323 / DLSSNR-AMD](https://gitlab.com/mochizuki0323/DLSSNR-AMD)** 与 **[TheAutomatic / dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)** 的维护者与官方仓库额外授予 **MIT License** 权限，可直接复用、适配或向上游合入本仓库中的原创代码与补丁。
- 仓库内包含的第三方代码（`Dear ImGui`、`MinHook`）及外部运行依赖（`OptiScaler`、`DLSSNR-AMD`、`Neural-coprocessor`）均遵循其原有协议，详见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。
