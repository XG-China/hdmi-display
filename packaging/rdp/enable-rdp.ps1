#Requires -RunAsAdministrator
<#
    Enable Microsoft RDP at 60 fps for HDMI Display.
    Run as administrator, then reboot (or sign out and back in) once.
#>
$ErrorActionPreference = "Stop"

$ts = "HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server"
$ws = "$ts\WinStations"

# Allow RDP connections.
Set-ItemProperty -Path $ts -Name fDenyTSConnections -Value 0 -Type DWord

# Add an English (US) keyboard and make it the default input method. Without
# this a zh-CN-only install keeps the Pinyin IME active and RDP letter keys
# look "random" (they are being composed as Chinese).
$langs = Get-WinUserLanguageList
if (-not ($langs | Where-Object { $_.LanguageTag -eq "en-US" })) {
    $langs.Add("en-US")
    Set-WinUserLanguageList -LanguageList $langs -Force
}
Set-WinDefaultInputMethodOverride -InputTip "0409:00000409"

# RDP is capped at 30 fps by default; 15 ms interval raises the cap to 60.
New-ItemProperty -Path $ws -Name DWMFRAMEINTERVAL -PropertyType DWord `
    -Value 15 -Force | Out-Null

# Firewall group (localized Windows uses the group name, not a rule name).
Enable-NetFirewallRule -DisplayGroup "Remote Desktop" -ErrorAction SilentlyContinue

$ip = (Get-NetIPAddress -AddressFamily IPv4 |
    Where-Object { $_.IPAddress -notlike "127.*" -and $_.IPAddress -notlike "169.254.*" } |
    Select-Object -First 1).IPAddress

Write-Host ""
Write-Host "RDP enabled, frame cap set to 60 fps."
Write-Host "Reboot (or sign out and back in) once for the 60 fps setting to apply."
Write-Host "In the HDMI Display panel choose '远程桌面（RDP）' and enter:"
Write-Host "    用户名:密码@$ip:3389"
Write-Host "Example: Administrator:yourpassword@$ip:3389"
