# Builds the two release zips in dist\:
#   paralives_upscaling-<version>.zip            mod only, laid out like the game folder (extract into it); no scripts,
#                                                so it passes automated scans (Nexus quarantines downloader scripts)
#                                                README.md and licenses\ go in BepInEx\plugins\ParalivesUpscaling\
#   paralives_upscaling-<version>-installer.zip  Install.bat + setup.ps1 (installs BepInEx if needed, copies the mod,
#                                                or removes it), the mod under files\, README.md, licenses\
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

& cmd /c "`"$root\native\build.bat`""
if ($LASTEXITCODE) { throw "native build failed" }
dotnet build "$root\managed\ParalivesUpscaling.csproj" -c Release -nologo -v q
if ($LASTEXITCODE) { throw "managed build failed" }
dotnet build "$root\patcher\ParalivesUpscaling.Patcher.csproj" -c Release -nologo -v q
if ($LASTEXITCODE) { throw "patcher build failed" }

$version = [regex]::Match((Get-Content "$root\managed\Plugin.cs" -Raw), 'Version = "([^"]+)"').Groups[1].Value
$stage = "$root\dist\stage"

# Copies the mod (game-folder layout) into $Dest; README and licenses into $Docs.
function Stage-Mod([string]$Dest, [string]$Docs) {
    $native = New-Item -ItemType Directory -Force "$Dest\Paralives_Data\Plugins\x86_64"
    $plugin = New-Item -ItemType Directory -Force "$Dest\BepInEx\plugins\ParalivesUpscaling"
    foreach ($f in "ParalivesUpscaling.dll", "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_upscaler_dx12.dll", "amd_fidelityfx_framegeneration_dx12.dll",
                   "libxess.dll", "libxess_fg.dll", "libxell.dll", "nvngx_dlss.dll") {
        Copy-Item "$root\native\out\$f" $native
    }
    Copy-Item "$root\managed\bin\Release\ParalivesUpscaling.dll" $plugin
    # preloader patcher: sets up the frame generation swapchain before Unity creates its own
    $patcher = New-Item -ItemType Directory -Force "$Dest\BepInEx\patchers\ParalivesUpscaling"
    Copy-Item "$root\patcher\bin\Release\ParalivesUpscaling.Patcher.dll" $patcher

    $licenses = New-Item -ItemType Directory -Force "$Docs\licenses"
    Copy-Item "$root\README.md" $Docs
    # AMD ships its MIT notice in the headers; the other SDKs have a license file
    $amd = (Get-Content "$root\native\ffx\api\include\ffx_api.h" -TotalCount 21) -replace '^// ?', ''
    Set-Content "$licenses\AMD_FidelityFX_SDK.txt" $amd
    Copy-Item "$root\native\xess\LICENSE.txt" "$licenses\Intel_XeSS_SDK.txt"
    Copy-Item "$root\native\dlss\LICENSE.txt" "$licenses\NVIDIA_DLSS_SDK.txt"
    Copy-Item "$root\LICENSE" "$licenses\Paralives_Upscaling_MIT.txt"
}

function Pack([string]$Name) {
    $zip = "$root\dist\$Name"
    if (Test-Path $zip) { Remove-Item $zip -Force }
    Compress-Archive -Path "$stage\*" -DestinationPath $zip -CompressionLevel Optimal
    Remove-Item $stage -Recurse -Force
    Write-Host ("Packaged {0} ({1:N0} MB)" -f $zip, ((Get-Item $zip).Length / 1MB))
}

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
Stage-Mod $stage "$stage\BepInEx\plugins\ParalivesUpscaling"
Pack "paralives_upscaling-$version.zip"

Stage-Mod "$stage\files" $stage
Copy-Item "$root\installer\Install.bat", "$root\installer\setup.ps1" $stage
Pack "paralives_upscaling-$version-installer.zip"
