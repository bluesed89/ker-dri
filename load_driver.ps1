# ──────────────────────────────────────────────────────────────────────
# load_driver.ps1 — Hızlı driver yükleme (PowerShell)
# Admin PowerShell'de çalıştır: .\load_driver.ps1
# ──────────────────────────────────────────────────────────────────────

# Admin kontrolü
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "[!] Admin olarak calistirin!" -ForegroundColor Red
    exit 1
}

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$KdmapperPath = Join-Path $ProjectRoot "kernel_radar\driver\build\bin\kdmapper.exe"
$DriverPath = Join-Path $ProjectRoot "kernel_radar\driver\build\bin\LoRadarDriver.sys"

# Fallback paths
if (-not (Test-Path $KdmapperPath)) {
    $KdmapperPath = Join-Path $ProjectRoot "kdmapper\x64\Release\kdmapper_Release.exe"
}

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  LO RADAR - Driver Loader" -ForegroundColor Cyan  
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""

# 0) EAC Test Signing Mode Kontrolü & Vulnerable Driver Blocklist
Write-Host "[0/4] EAC Uyumlu Sistem Ayarlari Yapiliyor..." -ForegroundColor Yellow
try {
    bcdedit /set testsigning off | Out-Null
    bcdedit /set nointegritychecks off | Out-Null
    Set-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\CI\Config" -Name "VulnerableDriverBlocklistEnable" -Value 0 -ErrorAction SilentlyContinue
    Write-Host "[OK] Test Signing KAPATILDI (EAC engellemeyecek) & Blocklist devredisi." -ForegroundColor Green
} catch {
    Write-Host "[!] bcdedit/registry ayari yapilamadi." -ForegroundColor Red
}

# 1) Defender kapatma
Write-Host "[1/4] Defender kapatiliyor..." -ForegroundColor Yellow
try {
    Set-MpPreference -DisableRealtimeMonitoring $true -ErrorAction SilentlyContinue
    Add-MpPreference -ExclusionPath $ProjectRoot -ErrorAction SilentlyContinue
    Write-Host "[OK] Defender kapatildi." -ForegroundColor Green
} catch {
    Write-Host "[!] Defender kapatilamadi - Tamper Protection kontrol edin." -ForegroundColor Red
}

# 2) Eski iqvw64e servisini temizle
Write-Host "[2/4] Eski intel servisi temizleniyor..." -ForegroundColor Yellow
sc.exe stop iqvw64e 2>$null | Out-Null
sc.exe delete iqvw64e 2>$null | Out-Null
Write-Host "[OK] Temizlendi." -ForegroundColor Green

# 3) Dosya kontrolleri
Write-Host "[3/4] Dosyalar kontrol ediliyor..." -ForegroundColor Yellow
if (-not (Test-Path $KdmapperPath)) {
    Write-Host "[HATA] kdmapper bulunamadi: $KdmapperPath" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $DriverPath)) {
    Write-Host "[HATA] Driver bulunamadi: $DriverPath" -ForegroundColor Red
    exit 1
}
Write-Host "[OK] kdmapper: $KdmapperPath" -ForegroundColor Green
Write-Host "[OK] driver:   $DriverPath" -ForegroundColor Green

# 4) Kdmapper çalıştır
Write-Host ""
Write-Host "[4/4] Driver yukleniyor..." -ForegroundColor Yellow
Write-Host "--------------------------------------------" -ForegroundColor DarkGray

$process = Start-Process -FilePath $KdmapperPath -ArgumentList "`"$DriverPath`"" -Wait -PassThru -NoNewWindow
$exitCode = $process.ExitCode

Write-Host "--------------------------------------------" -ForegroundColor DarkGray
Write-Host ""

if ($exitCode -eq 0) {
    Write-Host "============================================" -ForegroundColor Green
    Write-Host "  Driver basariyla yuklendi!" -ForegroundColor Green
    Write-Host "  Device: \\.\LoRadarDriver2" -ForegroundColor Green
    Write-Host "  Radar'i calistirabilirsiniz." -ForegroundColor Green
    Write-Host "============================================" -ForegroundColor Green
} else {
    Write-Host "[HATA] Yukleme basarisiz! Kod: $exitCode" -ForegroundColor Red
    Write-Host ""
    Write-Host "Cozum onerileri:" -ForegroundColor Yellow
    Write-Host "  1. Shift+Restart > Troubleshoot > Advanced > Startup Settings" -ForegroundColor White
    Write-Host "     > Restart > 7 (Disable Driver Signature Enforcement)" -ForegroundColor White
    Write-Host "  2. BIOS'tan Secure Boot kapatin" -ForegroundColor White
    Write-Host "  3. Tamper Protection kapatin" -ForegroundColor White
    Write-Host "  4. HVCI/VBS kapatin" -ForegroundColor White
}

Write-Host ""
Read-Host "Devam etmek icin Enter'a basin"
