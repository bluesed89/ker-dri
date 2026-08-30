@echo off
setlocal EnableExtensions EnableDelayedExpansion
title LO Radar Driver Loader

net session >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo.
    echo [!] HATA: Bu script Yonetici Administrator olarak calistirilmalidir!
    echo [!] Lutfen dosyaya SAG TIKLAYIP Yonetici olarak calistir secin.
    echo.
    pause
    exit /b 1
)

echo ============================================================
echo   LO RADAR DRIVER LOADER
echo ============================================================
echo.

echo [1/5] Test Signing Mode kontrol ediliyor...
bcdedit /enum {current} | findstr /i "testsigning" | findstr /i "Yes" >nul 2>&1
if %ERRORLEVEL% equ 0 (
    echo [!] Test Signing AKTIF gorunuyor! EAC bunu engeller.
    echo [!] Test Signing KAPATILIYOR...
    bcdedit /set testsigning off >nul 2>&1
    bcdedit /set nointegritychecks off >nul 2>&1
    echo [OK] Test Signing kapatildi. Bilgisayari yeniden baslatmaniz gerekebilir!
) else (
    echo [OK] Test Signing zaten KAPALI - EAC ile uyumlu.
)
echo.

echo [2/5] Vulnerable Driver Blocklist kontrol ediliyor...
reg add "HKLM\SYSTEM\CurrentControlSet\Control\CI\Config" /v "VulnerableDriverBlocklistEnable" /t REG_DWORD /d 0 /f >nul 2>&1
echo [OK] Vulnerable Driver Blocklist devredisi birakildi.
echo.

echo [3/5] Intel vulnerable driver servisi temizleniyor...
sc stop iqvw64e >nul 2>&1
sc delete iqvw64e >nul 2>&1
echo [OK] Eski iqvw64e servisi temizlendi.
echo.

echo [4/5] Defender klasor istisnasi ekleniyor...
powershell -Command "Add-MpPreference -ExclusionPath '%~dp0' -ErrorAction SilentlyContinue" >nul 2>&1
echo [OK] Proje klasoru Defender exclusion'a eklendi.
echo.

echo [5/5] Kdmapper ile driver yukleniyor...
echo.

set "KDMAPPER=%~dp0kernel_radar\driver\build\bin\kdmapper.exe"
set "DRIVER=%~dp0kernel_radar\driver\build\bin\LoRadarDriver.sys"

if not exist "!KDMAPPER!" (
    set "KDMAPPER=%~dp0kdmapper\x64\Release\kdmapper_Release.exe"
)

if not exist "!KDMAPPER!" (
    echo [HATA] kdmapper.exe bulunamadi!
    echo.
    pause
    exit /b 1
)

if not exist "!DRIVER!" (
    echo [HATA] Driver dosyasi bulunamadi: !DRIVER!
    echo [!] Lutfen once driver'i build edin.
    echo.
    pause
    exit /b 1
)

echo [*] Kdmapper: "!KDMAPPER!"
echo [*] Driver:   "!DRIVER!"
echo.
echo [*] Yukleme basliyor...
echo ------------------------------------------------------------
"!KDMAPPER!" "!DRIVER!"
set "RESULT=%ERRORLEVEL%"
echo ------------------------------------------------------------
echo.

if %RESULT% equ 0 (
    echo ============================================================
    echo   [OK] DRIVER BASARIYLA YUKLENDI!
    echo   Device: \\.\LoRadarDriver2
    echo   Artik radar uygulamasini calistirabilirsiniz.
    echo ============================================================
) else (
    echo [HATA] Driver yukleme basarisiz! Hata kodu: %RESULT%
    echo.
    echo Olasi cozumler:
    echo   1. Windows Security -> Core Isolation -> Memory Integrity = OFF yapin.
    echo   2. BIOS'tan Secure Boot'u kapatin.
    echo   3. Antivirus/Defender devredisi birakin.
)

echo.
echo Devam etmek ve pencereyi kapatmak icin bir tusa basin...
pause
