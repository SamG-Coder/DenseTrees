$ErrorActionPreference = 'Stop'
$toolRoot = 'C:\msys64\ucrt64\bin'
$cmake = Join-Path $toolRoot 'cmake.exe'
$ninja = Join-Path $toolRoot 'ninja.exe'
$compiler = Join-Path $toolRoot 'g++.exe'
if (-not (Test-Path -LiteralPath $cmake)) { throw 'MSYS2 UCRT64 CMake is missing. See README.md.' }
$env:Path = "$toolRoot;$env:Path"
$dxc = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter dxc.exe -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match '\\x64\\' } | Select-Object -First 1 -ExpandProperty FullName
if (-not $dxc) { throw 'DirectX Shader Compiler is missing. Install Microsoft.DirectX.ShaderCompiler with winget.' }
New-Item -ItemType Directory -Force -Path 'build\shaders' | Out-Null
& $dxc -T lib_6_6 -HV 2021 -O3 -Fo 'build\shaders\raytracing.dxil' 'shaders\raytracing.hlsl'
if ($LASTEXITCODE -ne 0) { throw 'DXR shader compilation failed.' }
& $cmake -S . -B build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_CXX_COMPILER=$compiler" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $cmake --build build --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
& (Join-Path $toolRoot 'ctest.exe') --test-dir build --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
Write-Host 'Built: build\bin\DenseTrees.exe'
