param([Parameter(Mandatory=$true)][string]$UpstreamPath)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath $UpstreamPath).Path
$destination = Join-Path (Split-Path $PSScriptRoot -Parent) 'docs'
$revision = (git -C $repo rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot read upstream revision' }
$files = @(git -C $repo ls-files src gpu tests tools scripts)
if ($LASTEXITCODE -ne 0) { throw 'Cannot list upstream sources' }
$patterns = [ordered]@{
    x86 = '__x86_64__|__cpuid|\bcpuid\b|__asm__|\bxbyak\b|sysv_abi|_mm256_'
    windows = '\b_WIN32\b|windows\.h|VirtualAlloc|VirtualProtect|CreateThread|AddVectoredExceptionHandler'
    posix_vm = '\bmmap\b|\bmprotect\b|\bsigaction\b|\bfutex\b|\bX11\b'
    executable_memory = 'PROT_EXEC|PAGE_EXECUTE|FlushInstructionCache'
}
$rows = foreach ($relative in $files) {
    $full = Join-Path $repo $relative
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { continue }
    if ($relative -notmatch '\.(c|cpp|h|hpp|py|sh|txt|cmake)$') { continue }
    $text = [IO.File]::ReadAllText($full)
    $tags = @()
    foreach ($pattern in $patterns.GetEnumerator()) {
        if ($text -match $pattern.Value) { $tags += $pattern.Key }
    }
    [pscustomobject]@{
        revision = $revision
        path = $relative
        bytes = (Get-Item -LiteralPath $full).Length
        sha256 = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLowerInvariant()
        host_assumptions = $tags -join '|'
        review = 'manual_review_required'
    }
}
$rows | Export-Csv -LiteralPath (Join-Path $destination 'upstream-files.csv') -NoTypeInformation -Encoding UTF8
Write-Output "Inventoried $(@($rows).Count) source files at $revision. Pattern tags do not prove portability."
