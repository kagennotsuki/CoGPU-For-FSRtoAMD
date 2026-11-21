#build scripts - portable standalone repo builds
param(
    [string]$Out = (Join-Path $PSScriptRoot '..\out\asi')
)
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..')
$src  = Join-Path $repo 'src'
$imgui = Join-Path $repo 'third_party\imgui-1.90.9'
$minhook = Join-Path $repo 'third_party\minhook'

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
    $src,
    (Join-Path $minhook 'include')
) | ForEach-Object { "/I`"$_`"" }

$cSrcs = @(
    (Join-Path $minhook 'src\buffer.c'),
    (Join-Path $minhook 'src\hook.c'),
    (Join-Path $minhook 'src\trampoline.c'),
    (Join-Path $minhook 'src\hde\hde64.c')
) | ForEach-Object { "`"$_`"" }

$cppSrcs = @(
    (Join-Path $src 'ffx_core_reuse.cpp'),
    (Join-Path $src 'optiscaler_adapter_on_core.cpp'),
    (Join-Path $src 'ngx_f18_backend.cpp'),
    (Join-Path $imgui 'imgui.cpp'),
    (Join-Path $imgui 'imgui_draw.cpp'),
    (Join-Path $imgui 'imgui_tables.cpp'),
    (Join-Path $imgui 'imgui_widgets.cpp'),
    (Join-Path $imgui 'backends\imgui_impl_dx12.cpp')
) | ForEach-Object { "`"$_`"" }

$defFile = Join-Path $src 'cogpu_nr_asi.def'
$cl = @(
    'cl /nologo /LD /O2 /Ob2 /MT /GR /EHsc /std:c++20 /utf-8',
    '/DWIN32 /D_WINDOWS /DNDEBUG /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN',
    '/DCOGPU_ASI_BUILD /Dffx_on_core_EXPORTS /DCMAKE_INTDIR="Release"',
    ($incs -join ' '),
    ($cSrcs -join ' '),
    ($cppSrcs -join ' '),
    "/Fo`"$Out\\`" /Fd`"$Out\vc.pdb`"",
    '/link',
    "/DEF:`"$defFile`"",
    "/IMPLIB:`"$Out\cogpu_nr.lib`"",
    "/OUT:`"$Out\cogpu_nr.asi`"",
    '/MACHINE:x64 d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib',
    'kernel32.lib user32.lib gdi32.lib winspool.lib shell32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib'
) -join ' '

$cmd = "`"$vcvars`" >nul 2>&1 && cd /d `"$src`" && $cl"
& cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) { throw "compile failed exit=$LASTEXITCODE" }
$asi = Join-Path $Out 'cogpu_nr.asi'
Write-Host "[build-asi] OK $((Get-Item $asi).Length) bytes -> $asi"
