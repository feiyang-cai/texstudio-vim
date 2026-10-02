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
