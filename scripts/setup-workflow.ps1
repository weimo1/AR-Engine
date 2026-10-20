param()
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location -LiteralPath $projectRoot
try {
    $env:OPENSPEC_TELEMETRY = '0'
    $env:OPEN_SPEC_INTERACTIVE = '0'
    $env:OCR_ENABLE_TELEMETRY = '0'
    $env:OCR_NO_UPDATE = '1'
    & npm.cmd ci --ignore-scripts --no-audit --no-fund
    if ($LASTEXITCODE -ne 0) { throw 'npm ci failed' }
    $toolLock = Get-Content -LiteralPath 'workflow/tool-lock.json' -Raw -Encoding UTF8 | ConvertFrom-Json
    $toolRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot '.tools/ocr'))
    $toolDirectory = [IO.Path]::GetFullPath((Join-Path $toolRoot $toolLock.ocr.version))
    if (-not $toolDirectory.StartsWith($toolRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'OCR directory escapes project tool root'
    }
    New-Item -ItemType Directory -Path $toolDirectory -Force | Out-Null
    $binaryPath = Join-Path $toolDirectory 'ocr.exe'
    $downloadNeeded = -not (Test-Path -LiteralPath $binaryPath)
    if (-not $downloadNeeded) {
        $downloadNeeded = (Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $toolLock.ocr.sha256
    }
    if ($downloadNeeded) {
        $downloadPath = Join-Path $toolDirectory ('ocr-download-' + [guid]::NewGuid().ToString('N') + '.tmp')
        try {
            # Bounded wait: a stalled CDN must fail the job instead of hanging CI until cancellation.
            Invoke-WebRequest -Uri $toolLock.ocr.url -OutFile $downloadPath -TimeoutSec 300
            $downloadHash = (Get-FileHash -LiteralPath $downloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($downloadHash -ne $toolLock.ocr.sha256) { throw 'OCR download checksum mismatch' }
            foreach ($ownedPath in @($downloadPath, $binaryPath)) {
                if (-not ([IO.Path]::GetFullPath($ownedPath)).StartsWith($toolDirectory + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
                    throw 'OCR file move escapes owned tool directory'
                }
            }
            Move-Item -LiteralPath $downloadPath -Destination $binaryPath -Force
        } finally {
            if (Test-Path -LiteralPath $downloadPath) { Remove-Item -LiteralPath $downloadPath }
        }
    }
    $actualHash = (Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $toolLock.ocr.sha256) { throw 'OCR checksum mismatch; binary was not executed' }
    & $binaryPath version
    if ($LASTEXITCODE -ne 0) { throw 'OCR version check failed' }
    Write-Output 'Project-local tools ready. Start with README.md.'
} finally {
    Pop-Location
}
