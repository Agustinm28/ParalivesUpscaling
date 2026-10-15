# Builds ParalivesUpscaling and copies it into the game.
param([string]$Game = "D:\SteamLibrary\steamapps\common\Paralives")
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

& cmd /c "`"$root\native\build.bat`""
if ($LASTEXITCODE) { throw "native build failed" }
dotnet build "$root\managed\ParalivesUpscaling.csproj" -c Release -nologo -v q
if ($LASTEXITCODE) { throw "managed build failed" }

$plugins = "$Game\Paralives_Data\Plugins\x86_64"
Copy-Item "$root\native\out\ParalivesUpscaling.dll", "$root\native\out\amd_fidelityfx_loader_dx12.dll", "$root\native\out\amd_fidelityfx_upscaler_dx12.dll", "$root\native\out\libxess.dll" $plugins
New-Item -ItemType Directory -Force "$Game\BepInEx\plugins\ParalivesUpscaling" | Out-Null
Copy-Item "$root\managed\bin\Release\ParalivesUpscaling.dll" "$Game\BepInEx\plugins\ParalivesUpscaling\"
Write-Host "Installed. Launch with -force-d3d12. Logs: BepInEx\LogOutput.log and $plugins\ParalivesUpscaling.log"
