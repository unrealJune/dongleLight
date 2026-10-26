param(
    [ValidateSet("win-x64")]
    [string]$Runtime = "win-x64",
    [string]$Destination = "$PSScriptRoot\.artifacts\app"
)
$ErrorActionPreference = "Stop"
Push-Location $PSScriptRoot
try {
    $platform = "x64"
    dotnet publish .\DongleLight\DongleLight.csproj -c Release -r $Runtime `
        -p:Platform=$platform -p:PlatformTarget=$platform --self-contained true -o $Destination --nologo
    if ($LASTEXITCODE -ne 0) { throw "Native publish failed." }
    Write-Output "Portable app: $Destination\DongleLight.exe"
    Write-Output "Copy the whole folder to each Windows devbox. Python 3.10+ is required."
    Write-Output "Install Python dependencies: python -m pip install -r `"$Destination\host\requirements.txt`""
}
finally { Pop-Location }
