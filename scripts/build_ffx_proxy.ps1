#build scripts - portable standalone FFX upscaler proxy DLL
param(
    [string]$Out = (Join-Path $PSScriptRoot '..\out\ffx')
)
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..')
$src  = Join-Path $repo 'src'
$imgui = Join-Path $repo 'third_party\imgui-1.90.9'

$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) {
    $vcvars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
}
if (-not (Test-Path -LiteralPath $vcvars)) { throw "vcvars64.bat not found; run from VS x64 Developer Prompt or adjust path." }
if (-not (Test-Path -LiteralPath $Out)) { New-Item -ItemType Directory -Path $Out -Force | Out-Null }
$Out = (Resolve-Path $Out).Path

$incs = @(
    $imgui,
    (Join-Path $imgui 'backends'),
    $src
) | ForEach-Object { "/I`"$_`"" }

$cppSrcs = @(
    (Join-Path $src 'ffx_core_reuse.cpp'),
    (Join-Path $src 'ffx_adapter_on_core.cpp'),
    (Join-Path $src 'ngx_f18_backend.cpp'),
    (Join-Path $imgui 'imgui.cpp'),
    (Join-Path $imgui 'imgui_draw.cpp'),
    (Join-Path $imgui 'imgui_tables.cpp'),
    (Join-Path $imgui 'imgui_widgets.cpp'),
    (Join-Path $imgui 'backends\imgui_impl_dx12.cpp')
) | ForEach-Object { "`"$_`"" }

$defFile = Join-Path $src 'ffx_on_core.def'
$cl = @(
    'cl /nologo /LD /O2 /Ob2 /MT /GR /EHsc /std:c++20 /utf-8',
    '/DWIN32 /D_WINDOWS /DNDEBUG /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN',
    '/Dffx_on_core_EXPORTS /DCMAKE_INTDIR="Release"',
    ($incs -join ' '),
    ($cppSrcs -join ' '),
    "/Fo`"$Out\\`" /Fd`"$Out\vc.pdb`"",
    '/link',
    "/DEF:`"$defFile`"",
    "/IMPLIB:`"$Out\amd_fidelityfx_upscaler_dx12.lib`"",
    "/OUT:`"$Out\amd_fidelityfx_upscaler_dx12.dll`"",
    '/MACHINE:x64 d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib',
    'kernel32.lib user32.lib gdi32.lib winspool.lib shell32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib'
) -join ' '

$cmd = "`"$vcvars`" >nul 2>&1 && cd /d `"$src`" && $cl"
& cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) { throw "compile failed exit=$LASTEXITCODE" }
$dll = Join-Path $Out 'amd_fidelityfx_upscaler_dx12.dll'
Write-Host "[build-ffx] OK $((Get-Item $dll).Length) bytes -> $dll"
