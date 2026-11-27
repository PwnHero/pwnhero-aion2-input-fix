#Requires -RunAsAdministrator
$ErrorActionPreference = "Stop"

$driverDir = Join-Path $PSScriptRoot "driver"
$setupExe  = Join-Path $driverDir "pwnhid_setup.exe"
$bridgeExe = Join-Path $PSScriptRoot "aion2_input_bridge.exe"

Write-Host "[1/2] Installing PwnHero Virtual HID driver..." -ForegroundColor Cyan
& $setupExe install $driverDir
if ($LASTEXITCODE -ne 0) {
    throw "Driver installation failed (exit code $LASTEXITCODE)."
}

$dev = Get-PnpDevice | Where-Object { ($_.HardwareID -contains "ROOT\PWNHID") -and $_.Status -ne "OK" } | Select-Object -First 1
if ($dev) {
    Disable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false -ErrorAction SilentlyContinue | Out-Null
    Enable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false -ErrorAction SilentlyContinue | Out-Null
    Start-Sleep -Seconds 1
}

Write-Host "[2/2] Registering PwnHeroAion2Bridge scheduled task..." -ForegroundColor Cyan
$action    = New-ScheduledTaskAction -Execute $bridgeExe
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Highest
$settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Hours 0)
Register-ScheduledTask -TaskName "PwnHeroAion2Bridge" -Action $action -Principal $principal -Settings $settings -Force | Out-Null

Write-Host ""
Write-Host "Installation complete!" -ForegroundColor Green
Write-Host "Add these prep-cmd lines to your Aion 2 app in Apollo (apps.json or Web UI):"
Write-Host "  do:   schtasks /Run /TN PwnHeroAion2Bridge"
Write-Host "  undo: cmd /c taskkill /IM aion2_input_bridge.exe /F >nul 2>&1"
