# Builds the release zip: dist\paralives_upscaling-<version>.zip
#   Install.bat, setup.ps1   installer (installs BepInEx if needed, copies the mod, or removes it)
#   files\                   the mod, laid out like the game folder
#   README.md, licenses\
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

& cmd /c "`"$root\native\build.bat`""
if ($LASTEXITCODE) { throw "native build failed" }
dotnet build "$root\managed\ParalivesUpscaling.csproj" -c Release -nologo -v q
if ($LASTEXITCODE) { throw "managed build failed" }

$version = [regex]::Match((Get-Content "$root\managed\Plugin.cs" -Raw), 'Version = "([^"]+)"').Groups[1].Value
$stage = "$root\dist\stage"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }

$native = New-Item -ItemType Directory -Force "$stage\files\Paralives_Data\Plugins\x86_64"
$plugin = New-Item -ItemType Directory -Force "$stage\files\BepInEx\plugins\ParalivesUpscaling"
$licenses = New-Item -ItemType Directory -Force "$stage\licenses"
foreach ($f in "ParalivesUpscaling.dll", "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_upscaler_dx12.dll", "libxess.dll", "nvngx_dlss.dll") {
    Copy-Item "$root\native\out\$f" $native
}
Copy-Item "$root\managed\bin\Release\ParalivesUpscaling.dll" $plugin
Copy-Item "$root\installer\Install.bat", "$root\installer\setup.ps1", "$root\README.md" $stage

# AMD ships its MIT notice in the headers; the other SDKs have a license file
$amd = (Get-Content "$root\native\ffx\api\include\ffx_api.h" -TotalCount 21) -replace '^// ?', ''
Set-Content "$licenses\AMD_FidelityFX_SDK.txt" $amd
Copy-Item "$root\native\xess\LICENSE.txt" "$licenses\Intel_XeSS_SDK.txt"
Copy-Item "$root\native\dlss\LICENSE.txt" "$licenses\NVIDIA_DLSS_SDK.txt"

$zip = "$root\dist\paralives_upscaling-$version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip -CompressionLevel Optimal
Remove-Item $stage -Recurse -Force
Write-Host ("Packaged {0} ({1:N0} MB)" -f $zip, ((Get-Item $zip).Length / 1MB))
