param(
    [Parameter(Mandatory = $true)][string]$SdkRoot,
    [Parameter(Mandatory = $true)][string]$DevkitProRoot
)
$ErrorActionPreference = 'Stop'
$kit = (Resolve-Path -LiteralPath $SdkRoot).Path.Replace('\', '/')
$toolchain = (Resolve-Path -LiteralPath $DevkitProRoot).Path.Replace('\', '/')
$env:DEVKITPRO = $kit
$output = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$compiler = "$toolchain/devkitA64/bin/aarch64-none-elf-gcc.exe"
$arch = @('-mcpu=cortex-a78c+crypto', '-mtp=soft', '-fPIE')
& $compiler @arch -std=gnu11 -O2 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections `
    -D__SWITCH__ -D__SWITCH2__ "-I$kit/libounce/include" "-I$kit/portlibs/switch2/include" `
    -c "$PSScriptRoot/main.c" -o "$output/main.o"
if ($LASTEXITCODE) { throw 'Command bridge compilation failed' }
& $compiler @arch "-specs=$kit/libounce/switch.specs" '-Wl,--gc-sections' "$output/main.o" `
    "-L$kit/portlibs/switch2/lib" "-L$kit/libounce/lib" '-Wl,--start-group' -lkaihen -lnx '-Wl,--end-group' `
    -o "$output/hen-command-bridge.elf"
if ($LASTEXITCODE) { throw 'Command bridge link failed' }
& "$toolchain/tools/bin/nacptool.exe" --create 'HEN command bridge' 'Local research tool' '0.2.1' "$output/hen-command-bridge.nacp"
if ($LASTEXITCODE) { throw 'Command bridge metadata failed' }
& "$toolchain/tools/bin/elf2nro.exe" "$output/hen-command-bridge.elf" "$output/hen-command-bridge.nro" "--nacp=$output/hen-command-bridge.nacp" "--icon=$PSScriptRoot/assets/icon.jpg"
if ($LASTEXITCODE) { throw 'Command bridge NRO conversion failed' }
$sourcePaths = @('main.c', 'protocol.h', 'client.cjs', 'cli.cjs', 'build.ps1', 'test-local.ps1', 'client.test.cjs', 'protocol.test.c', 'examples/smoke.cjs', 'examples/hen-status.json', 'assets/icon.jpg')
$sources = @($sourcePaths | ForEach-Object {
    [ordered]@{path=$_; sha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $_) -Algorithm SHA256).Hash.ToLowerInvariant()}
})
$dependencyPaths = @('libounce/include/switch.h', 'portlibs/switch2/include/kaihen.h', 'libounce/lib/libnx.a', 'portlibs/switch2/lib/libkaihen.a')
$dependencies = @($dependencyPaths | ForEach-Object {
    [ordered]@{path=$_; sha256=(Get-FileHash -LiteralPath "$kit/$_" -Algorithm SHA256).Hash.ToLowerInvariant()}
})
$manifest = [ordered]@{
    preparedUtc=[DateTime]::UtcNow.ToString('o')
    version='0.2.1'
    hardwareTested=$false
    consoleRequestsMade=$false
    compiler=(& $compiler --version | Select-Object -First 1)
    inputFiles=$sources
    sdkFiles=$dependencies
    nro=[ordered]@{path='build/hen-command-bridge.nro'; bytes=(Get-Item -LiteralPath "$output/hen-command-bridge.nro").Length; sha256=(Get-FileHash -LiteralPath "$output/hen-command-bridge.nro" -Algorithm SHA256).Hash.ToLowerInvariant()}
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$output/manifest.json" -Encoding UTF8
Get-FileHash -LiteralPath "$output/hen-command-bridge.nro" -Algorithm SHA256
