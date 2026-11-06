# A portable server avoids installing a Windows service or waiting for Features
# on Demand. Only the loopback fixture starts this process, with temporary keys.
$ErrorActionPreference = 'Stop'
$server = "$env:WINDIR\System32\OpenSSH\sshd.exe"
if (!(Test-Path $server)) {
    $architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
    if ($architecture -eq 'Arm64') {
        $asset = 'OpenSSH-ARM64.zip'
        $expected = 'd2b1928225479fec9e0c938f527fdb5ed676446cca1bbcc5af84aca1f272842e'
    } else {
        $asset = 'OpenSSH-Win64.zip'
        $expected = 'b67ff8becd203502190ec42ac7787c24c2cd3325ed7700485fe7a1c83d2a7957'
    }
    $folder = Join-Path $env:RUNNER_TEMP 'remote-test-openssh'
    New-Item -ItemType Directory -Force $folder | Out-Null
    $archive = Join-Path $folder $asset
    Invoke-WebRequest "https://github.com/PowerShell/Win32-OpenSSH/releases/download/10.4.0.0p1-Preview/$asset" -OutFile $archive
    if ((Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
        throw 'OpenSSH test archive checksum mismatch'
    }
    Expand-Archive $archive -DestinationPath $folder -Force
    $server = (Get-ChildItem $folder -Recurse -Filter sshd.exe | Select-Object -First 1).FullName
    if (!$server) { throw 'Portable OpenSSH server executable is missing' }
}
"TXS_TEST_SSHD=$server" >> $env:GITHUB_ENV
