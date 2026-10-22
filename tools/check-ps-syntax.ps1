# Fails if a PowerShell script has syntax errors.
# Usage: pwsh -NoProfile -File tools/check-ps-syntax.ps1 <script.ps1>
param([Parameter(Mandatory = $true)][string]$Path)

$errors = $null
[System.Management.Automation.Language.Parser]::ParseFile((Resolve-Path $Path), [ref]$null, [ref]$errors) | Out-Null

if ($errors.Count -gt 0) {
    $errors | ForEach-Object { Write-Error $_.Message }
    exit 1
}
Write-Host "PowerShell syntax OK: $Path"
