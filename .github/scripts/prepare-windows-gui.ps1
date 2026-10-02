# Prepare a disposable hosted CI desktop; never run this on a user's machine.
# Windows ARM runners can open a first-login privacy wizard over the test app.
$ErrorActionPreference = 'Stop'
$oobePolicy = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\OOBE'
New-Item -Path $oobePolicy -Force | Out-Null
New-ItemProperty -Path $oobePolicy -Name DisablePrivacyExperience -PropertyType DWord -Value 1 -Force | Out-Null
$engagement = 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\UserProfileEngagement'
New-Item -Path $engagement -Force | Out-Null
New-ItemProperty -Path $engagement -Name ScoobeSystemSettingEnabled -PropertyType DWord -Value 0 -Force | Out-Null
Get-Process -Name UserOOBEBroker,CloudExperienceHostBroker,CloudExperienceHost,msoobe -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
# Some images use OOBE.exe/oobesettings.exe instead of the older broker names.
Get-Process | Where-Object { $_.ProcessName -like '*oobe*' } | ForEach-Object {
    Write-Host "Closing first-login setup process $($_.ProcessName)"
    Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
}
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class DesktopForeground {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
}
'@
[uint32]$foregroundProcessId = 0
[DesktopForeground]::GetWindowThreadProcessId([DesktopForeground]::GetForegroundWindow(), [ref]$foregroundProcessId) | Out-Null
if ($foregroundProcessId) {
    $foregroundProcess = Get-Process -Id $foregroundProcessId -ErrorAction SilentlyContinue
    Write-Host "Desktop foreground process: $($foregroundProcess.ProcessName); window: $($foregroundProcess.MainWindowTitle)"
}
