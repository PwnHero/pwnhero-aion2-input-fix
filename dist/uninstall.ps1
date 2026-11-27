#Requires -RunAsAdministrator
$ErrorActionPreference = "SilentlyContinue"

$setupExe = Join-Path $PSScriptRoot "driver\pwnhid_setup.exe"

Write-Host "[1/3] Stopping aion2_input_bridge.exe..." -ForegroundColor Cyan
taskkill /IM aion2_input_bridge.exe /F 2>$null | Out-Null

Write-Host "[2/3] Removing PwnHeroAion2Bridge scheduled task..." -ForegroundColor Cyan
Unregister-ScheduledTask -TaskName "PwnHeroAion2Bridge" -Confirm:$false

Write-Host "[3/3] Uninstalling PwnHero Virtual HID driver & certificate..." -ForegroundColor Cyan
if (Test-Path $setupExe) {
    & $setupExe uninstall
}
$curOem = $null
foreach ($line in (pnputil /enum-drivers)) {
    if ($line -match "Published Name:\s+(oem\d+\.inf)") { $curOem = $matches[1] }
    if ($line -match "Original Name:\s+pwnhid\.inf" -and $curOem) {
        pnputil /delete-driver $curOem /uninstall /force 2>$null | Out-Null
        $curOem = $null
    }
}
Remove-Item "$env:SystemRoot\System32\drivers\UMDF\pwnhid_umdf.dll" -Force -ErrorAction SilentlyContinue

Write-Host "Uninstall complete." -ForegroundColor Green
