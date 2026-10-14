param([switch]$SelfTest)
$ErrorActionPreference = 'Stop'
function Test-AllowedPath([string]$Path) {
    if ($Path -match '(^|/)(game|dumps|secrets|upstream|bloodborne_portPC|local|out|build[^/]*)(/|$)') { return $false }
    if ($Path -in @('.gitignore','.gitattributes','CMakeLists.txt')) { return $true }
    if ($Path -match '^third_party/[^/]+/LICENSE$') { return $true }
    return $Path -match '\.(c|cpp|h|mm|md|json|csv|hex|yml|plist|ps1|cmake|txt)$'
}
if ($SelfTest) {
    foreach ($path in @('eboot.bin','game/readme.md','dump.prx','secret.p12','out/boot.hex','build/a.cpp','local/account.json','third_party/LibAtrac9/payload.bin','game/core/x.c')) {
        if (Test-AllowedPath $path) { throw "Should reject: $path" }
    }
    foreach ($path in @('core/alu.h','tests/test_memory.cpp','.github/workflows/host.yml','fixtures/add.hex','docs/PROGRESS.md','CMakeLists.txt','core/miniz.c','third_party/LibAtrac9/LICENSE','third_party/LibAtrac9/src/decoder.c')) {
        if (-not (Test-AllowedPath $path)) { throw "Should accept: $path" }
    }
    Write-Output 'Source path policy tests passed.'
    exit 0
}
$root = Split-Path $PSScriptRoot -Parent
$files = @(git -C $root -c core.quotepath=false ls-files --cached)
if ($LASTEXITCODE -ne 0 -or $files.Count -eq 0) { throw 'No tracked files to inspect. Stage the intended sources first.' }
foreach ($path in $files) {
    if (-not (Test-AllowedPath $path)) { throw "Blocked publication path: $path" }
    $full = Join-Path $root $path
    $item = Get-Item -LiteralPath $full -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Links not allowed: $path" }
    if ($item.Length -gt 1MB) { throw "Review oversized file: $path" }
    $bytes = [IO.File]::ReadAllBytes($full)
    if ($bytes -contains 0) { throw "Binary content: $path" }
    $text = [Text.Encoding]::UTF8.GetString($bytes)
    $keyMarker = '-----BEGIN ' + '(?:RSA |EC |OPENSSH )?PRIVATE KEY-----'
    $tokenMarker = '(?:gh[pousr]_' + '[A-Za-z0-9]{30,}|github_pat_' + '[A-Za-z0-9_]{30,})'
    if ($text -match $keyMarker -or $text -match $tokenMarker) { throw "Possible credential in: $path (content withheld)" }
}
Write-Output "Publication policy passed for $($files.Count) tracked files. This is a guardrail, not a complete secret/content audit."
