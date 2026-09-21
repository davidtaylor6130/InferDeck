param()
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$vc = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$python = Join-Path $root 'build\runtime-radiance-probe\toolchain\python312-standalone\python'
$tempDir = Join-Path $env:TEMP 'inferdeck-vllm-stub'
New-Item -ItemType Directory -Force $tempDir | Out-Null
$out = Join-Path $tempDir 'inferdeck-vllm-stub.exe'
$cmd = 'call "{0}" >nul && cl /nologo /std:c++latest /EHsc /W4 /utf-8 libs\vllm_radiance_wrapper\tests\test_stub_bridge.cpp libs\vllm_radiance_wrapper\src\vllm_radiance_model.cpp /Ilibs\vllm_radiance_wrapper\include /Ilibs\model\include /Ilibs\foundation\include /Ilibs\inference\include /Ibuild\runtime-radiance-probe\toolchain\python312-standalone\python\include /link /LIBPATH:build\runtime-radiance-probe\toolchain\python312-standalone\python\libs python312.lib /OUT:"{1}"' -f $vc,$out
Push-Location $root
try { & cmd.exe /d /s /c $cmd; if ($LASTEXITCODE) { exit $LASTEXITCODE }; $env:PATH = $python + ';' + $env:PATH; $p = Start-Process -FilePath $out -PassThru -NoNewWindow; if (-not $p.WaitForExit(30000)) { $p.Kill(); throw 'stub test timed out after 30 seconds' }; exit $p.ExitCode } finally { Remove-Item -LiteralPath (Join-Path $root "test_stub_bridge.obj"),(Join-Path $root "vllm_radiance_model.obj") -Force -ErrorAction SilentlyContinue; Pop-Location }
