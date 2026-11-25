# This helper only selects a Linux distribution and starts the local builder.
param([switch]$CheckOnly)
$ErrorActionPreference = 'Stop'
try {
    $distributions = @(& wsl.exe --list --quiet | ForEach-Object {
        ($_ -replace '\x00', '').Trim()
    } | Where-Object { $_ -and $_ -notmatch '^docker-desktop' })
    if ($LASTEXITCODE -ne 0 -or $distributions.Count -eq 0) {
        throw 'Install and open a WSL Linux distribution such as Ubuntu first.'
    }
    if ($env:HEN_BUILD_DISTRO) {
        $selected = $env:HEN_BUILD_DISTRO
        if ($distributions -notcontains $selected) {
            throw "The requested HEN_BUILD_DISTRO '$selected' is not installed."
        }
    } elseif ($distributions -contains 'Ubuntu') {
        $selected = 'Ubuntu'
    } elseif ($distributions.Count -eq 1) {
        $selected = $distributions[0]
    } else {
        for ($i = 0; $i -lt $distributions.Count; $i++) {
            Write-Host ('{0}. {1}' -f ($i + 1), $distributions[$i])
        }
        $choice = 0
        if (-not [int]::TryParse((Read-Host 'Choose your build distribution'), [ref]$choice) -or
            $choice -lt 1 -or $choice -gt $distributions.Count) {
            throw 'Choose one of the listed distribution numbers.'
        }
        $selected = $distributions[$choice - 1]
    }
    Write-Host "Using WSL distribution: $selected"
    if ($CheckOnly) {
        & wsl.exe -d $selected --exec python3 --version
    } else {
        & wsl.exe -d $selected --cd $PSScriptRoot --exec python3 tools/release.py wizard
    }
    if ($LASTEXITCODE -ne 0) {
        throw 'The Linux helper stopped. Read the error above; no console installation was attempted.'
    }
} catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
}
