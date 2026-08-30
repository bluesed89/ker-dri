@echo off
setlocal EnableExtensions EnableDelayedExpansion
title EAC Test Mode Fixer

net session >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo.
    echo [!] Yonetici Administrator olarak calistirin! 
    echo [!] Sag tik -^> Yonetici olarak calistir
    echo.
    pause
    exit /b 1
)

echo ============================================================
echo   EAC TEST SIGNING BLOCKER FIXER - LO and ENI
echo ============================================================
echo.

echo [1/4] Test Signing Mode ve Integrity Checks kapatiliyor...
bcdedit /set testsigning off >nul 2>&1
bcdedit /set nointegritychecks off >nul 2>&1
bcdedit /set disable_integrity_checks off >nul 2>&1
echo [OK] Test Signing KAPATILDI. EAC artik engellemeyecek!
echo.

echo [2/4] Vulnerable Driver Blocklist kapatiliyor...
reg add "HKLM\SYSTEM\CurrentControlSet\Control\CI\Config" /v "VulnerableDriverBlocklistEnable" /t REG_DWORD /d 0 /f >nul 2>&1
echo [OK] Vulnerable Driver Blocklist kapatildi.
echo.

echo [3/4] Hypervisor / Memory Integrity Kayit Defteri Ayarlari...
reg add "HKLM\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity" /v "Enabled" /t REG_DWORD /d 0 /f >nul 2>&1
echo [OK] HVCI kayit degeri kapatildi.
echo.

echo [4/4] Eski vulnerable driver servisleri temizleniyor...
sc stop iqvw64e >nul 2>&1
sc delete iqvw64e >nul 2>&1
echo [OK] Temizlendi.
echo.

echo ============================================================
echo   ISLEM TAMAMLANDI!
echo ============================================================
echo.
echo ONEMLI HUSUSLAR:
echo 1. Bilgisayarinizi YENIDEN BASLATIN - Restart.
echo 2. Yeniden baslattiktan sonra Windows normal modda baslayacak.
echo 3. EAC oyunu baslatmaniza ENGEL OLMAYACAK!
echo 4. Driver yuklemek icin load_driver.bat calistirin.
echo.
pause
