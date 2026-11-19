param(
    [Parameter(Mandatory = $true)][string]$SdkRoot,
    [Parameter(Mandatory = $true)][string]$DevkitProRoot
)
$ErrorActionPreference = 'Stop'
$kit = (Resolve-Path -LiteralPath $SdkRoot).Path.Replace('\', '/')
$toolchain = (Resolve-Path -LiteralPath $DevkitProRoot).Path.Replace('\', '/')
$sourceRoot = $PSScriptRoot.Replace('\', '/')
$env:DEVKITPRO = $kit
$output = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$compiler = "$toolchain/devkitA64/bin/aarch64-none-elf-gcc.exe"
$arch = @('-mcpu=cortex-a78c+crypto', '-mtp=soft', '-fPIE', "-ffile-prefix-map=$sourceRoot=source", "-fdebug-prefix-map=$sourceRoot=source", "-ffile-prefix-map=$kit=sdk", "-fdebug-prefix-map=$kit=sdk")
$objects = @()
foreach ($source in @('main.c', 'native.c', 'core.c')) {
    $object = Join-Path $output ($source.Replace('.c', '.o'))
    & $compiler @arch -std=gnu11 -O2 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections `
        -D__SWITCH__ -D__SWITCH2__ "-I$kit/libounce/include" "-I$kit/portlibs/switch2/include" `
        -c "$PSScriptRoot/$source" -o $object
    if ($LASTEXITCODE) { throw "Compilation failed: $source" }
    $objects += $object
}
& $compiler @arch "-specs=$kit/libounce/switch.specs" '-Wl,--gc-sections' @objects `
    "-L$kit/portlibs/switch2/lib" "-L$kit/libounce/lib" '-Wl,--start-group' -lkaihen -lnx '-Wl,--end-group' `
    -o "$output/seedsow-fs-cleaner.elf"
if ($LASTEXITCODE) { throw 'Cleaner link failed' }
& "$toolchain/tools/bin/nacptool.exe" --create 'Seedsow FS cleanup' 'Local research tool' '0.1.2' "$output/seedsow-fs-cleaner.nacp"
if ($LASTEXITCODE) { throw 'Cleaner metadata failed' }
& "$toolchain/tools/bin/elf2nro.exe" "$output/seedsow-fs-cleaner.elf" "$output/seedsow-fs-cleaner.nro" "--nacp=$output/seedsow-fs-cleaner.nacp" "--icon=$PSScriptRoot/assets/icon.jpg"
if ($LASTEXITCODE) { throw 'Cleaner NRO conversion failed' }
$sourcePaths = @('main.c', 'native.c', 'native.h', 'core.c', 'core.h', 'core.test.c', 'build.ps1', 'assets/icon.svg', 'assets/icon.jpg', 'render-icon.py')
$sources = @($sourcePaths | ForEach-Object {
    $file = Join-Path $PSScriptRoot $_
    [ordered]@{ path=$_; sha256=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() }
})
$manifest = [ordered]@{
    preparedUtc=[DateTime]::UtcNow.ToString('o')
    version='0.1.2'
    hardwareTested=$false
    consoleRequestsMade=$false
    sdkKaihenHeaderSha256=(Get-FileHash -LiteralPath "$kit/portlibs/switch2/include/kaihen.h" -Algorithm SHA256).Hash.ToLowerInvariant()
    compiler=(& $compiler --version | Select-Object -First 1)
    inputFiles=$sources
    nro=[ordered]@{path='build/seedsow-fs-cleaner.nro'; bytes=(Get-Item -LiteralPath "$output/seedsow-fs-cleaner.nro").Length; sha256=(Get-FileHash -LiteralPath "$output/seedsow-fs-cleaner.nro" -Algorithm SHA256).Hash.ToLowerInvariant()}
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$output/manifest.json" -Encoding UTF8
Get-FileHash -LiteralPath "$output/seedsow-fs-cleaner.nro" -Algorithm SHA256
