param(
    [Parameter(Mandatory = $true)][string]$SdkRoot,
    [string]$ZigCompiler = 'zig'
)
$ErrorActionPreference = 'Stop'
$kit = (Resolve-Path -LiteralPath $SdkRoot).Path.Replace('\', '/')
$output = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $output 'zig-global'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $output 'zig-cache'
& $ZigCompiler cc -std=gnu11 -O0 -g -Wall -Wextra -Werror "-I$kit/libounce/include" "$PSScriptRoot/protocol.test.c" -o "$output/protocol.test.exe"
if ($LASTEXITCODE) { throw 'Protocol host-test compilation failed' }
& "$output/protocol.test.exe"
if ($LASTEXITCODE) { throw 'Protocol host tests failed' }
& node --test "$PSScriptRoot/client.test.cjs" "$PSScriptRoot/dump.test.cjs"
if ($LASTEXITCODE) { throw 'PC client tests failed' }
