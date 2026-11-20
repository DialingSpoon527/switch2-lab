param([string]$ZigCompiler = 'zig')
$ErrorActionPreference = 'Stop'
$output = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $output 'zig-global'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $output 'zig-cache'
& $ZigCompiler cc -std=c11 -O0 -g -Wall -Wextra -Werror "$PSScriptRoot/core.c" "$PSScriptRoot/core.test.c" -o "$output/core.test.exe"
if ($LASTEXITCODE) { throw 'Cleaner host-test compilation failed' }
& "$output/core.test.exe"
if ($LASTEXITCODE) { throw 'Cleaner host tests failed' }
