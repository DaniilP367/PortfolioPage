$ErrorActionPreference = "Stop"

# Robust local/external link scanner for Webflow-exported static sites
# Run from the repo root (where index.html lives).

function UrlDecode([string]$s) {
    try { return [System.Uri]::UnescapeDataString($s) } catch { return $s }
}

function Strip-QueryHash([string]$s) {
    if ($null -eq $s) { return $null }
    $t = $s
    $t = ($t -split "#", 2)[0]
    $t = ($t -split "\?", 2)[0]
    return $t
}

$root = (Get-Location).Path
$index = Join-Path $root "index.html"
if (-not (Test-Path -LiteralPath $index)) {
    Write-Host "ERROR: index.html not found in: $root"
    Write-Host "Tip: cd into the folder that contains index.html and assets\"
    exit 1
}

# Collect files to scan
$scanFiles = New-Object System.Collections.Generic.List[string]
$scanFiles.Add($index) | Out-Null

if (Test-Path "assets\ext") {
    Get-ChildItem -Path "assets\ext" -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in ".css",".js",".html" } |
        ForEach-Object { $scanFiles.Add($_.FullName) | Out-Null }
}

if (Test-Path "_css_assets") {
    Get-ChildItem -Path "_css_assets" -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in ".css" } |
        ForEach-Object { $scanFiles.Add($_.FullName) | Out-Null }
}

$external = New-Object System.Collections.Generic.HashSet[string]
$localRefs = New-Object System.Collections.Generic.HashSet[string]

# Regexes
$rxHttp = [regex]'https?://[^\s"''\)\>]+'
$rxHtmlAttr = [regex]'(?i)\b(?:src|href|data-src|data-href)\s*=\s*"([^"]+)"'
$rxCssUrl = [regex]'(?i)url\(\s*([''"]?)([^)''"]+)\1\s*\)'

foreach ($file in $scanFiles) {
    $raw = $null
    try {
        $raw = Get-Content -LiteralPath $file -Raw -ErrorAction Stop
    } catch {
        Write-Host "WARN: cannot read: $file"
        continue
    }

    if ([string]::IsNullOrWhiteSpace($raw)) { continue }

    # External URLs (anywhere)
    foreach ($m in $rxHttp.Matches($raw)) {
        $u = $m.Value
        if ($u -match '^http://www\.w3\.org/2000/svg$') { continue } # SVG namespace, not a request
        $external.Add($u) | Out-Null
    }

    $ext = [System.IO.Path]::GetExtension($file).ToLowerInvariant()
    $dir = Split-Path -Parent $file

    if ($ext -eq ".html") {
        foreach ($m in $rxHtmlAttr.Matches($raw)) {
            $v = $m.Groups[1].Value
            if ([string]::IsNullOrWhiteSpace($v)) { continue }

            # ignore anchors and special schemes
            if ($v.StartsWith("#")) { continue }
            if ($v -match '^(?i)(mailto:|tel:|javascript:)') { continue }
            if ($v -match '^(?i)https?://') { continue } # already counted as external

            $p = UrlDecode (Strip-QueryHash $v)
            if ([string]::IsNullOrWhiteSpace($p)) { continue }

            # normalize slashes
            $p = $p -replace '/', '\'

            # Only track file-like paths (ignore "./" nav etc.)
            if ($p -eq ".\" -or $p -eq ".") { continue }
            if ($p -eq "\") { continue }

            # If absolute-from-root path like "\assets\..."
            if ($p.StartsWith("\")) {
                $p = $p.TrimStart("\")
                $full = Join-Path $root $p
            } else {
                $full = Join-Path $root $p
            }
            $localRefs.Add($full) | Out-Null
        }
    }

    if ($ext -eq ".css") {
        foreach ($m in $rxCssUrl.Matches($raw)) {
            $v = $m.Groups[2].Value.Trim()
            if ([string]::IsNullOrWhiteSpace($v)) { continue }

            # ignore data URIs
            if ($v -match '^(?i)data:') { continue }
            if ($v -match '^(?i)https?://') { continue } # external already counted

            $p = UrlDecode (Strip-QueryHash $v)
            if ([string]::IsNullOrWhiteSpace($p)) { continue }
            $p = $p -replace '/', '\'

            # CSS relative paths should be resolved relative to the CSS file directory
            if ($p.StartsWith("\")) {
                $p = $p.TrimStart("\")
                $full = Join-Path $root $p
            } else {
                $full = Join-Path $dir $p
            }

            $localRefs.Add($full) | Out-Null
        }
    }
}

# Check existence of local refs
$missing = New-Object System.Collections.Generic.List[string]
foreach ($p in $localRefs) {
    $clean = Strip-QueryHash $p
    if ([string]::IsNullOrWhiteSpace($clean)) { continue }
    if (-not (Test-Path -LiteralPath $clean)) {
        $missing.Add($clean) | Out-Null
    }
}

Write-Host ""
Write-Host "=== EXTERNAL URLs FOUND (resources/links) ==="
if ($external.Count -eq 0) {
    Write-Host "(none)"
} else {
    $external | Sort-Object | ForEach-Object { Write-Host $_ }
}

Write-Host ""
Write-Host "=== MISSING LOCAL REFERENCES (these will cause 404) ==="
if ($missing.Count -eq 0) {
    Write-Host "(none)"
} else {
    $missing | Sort-Object -Unique | ForEach-Object { Write-Host $_ }
}

Write-Host ""
Write-Host ("Scanned {0} file(s). Local refs: {1}. Missing: {2}. External URLs: {3}." -f $scanFiles.Count, $localRefs.Count, (($missing | Sort-Object -Unique).Count), $external.Count)
