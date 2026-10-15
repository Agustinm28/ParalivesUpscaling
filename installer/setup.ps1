# Paralives Upscaling - installer / uninstaller (run it through Install.bat).
#
# Finds the Paralives folder, installs BepInEx 5 if it is missing (pinned version, checked against its SHA-256),
# copies the mod from .\files, and reminds about -force-d3d12. If the mod is already installed it offers to update
# or remove it; removing never touches BepInEx, which other mods may use.
param([string]$Game)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$BepInExVersion = '5.4.23.5'
$BepInExUrl = "https://github.com/BepInEx/BepInEx/releases/download/v$BepInExVersion/BepInEx_win_x64_$BepInExVersion.zip"
$BepInExSha256 = '82F9878551030F54657792C0740D9D51A09500EEAE1FBA21106B0C441E6732C4'

# everything the mod puts in the game (uninstall removes exactly this)
$NativeFiles = 'ParalivesUpscaling.dll', 'amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll',
               'libxess.dll', 'nvngx_dlss.dll', 'ParalivesUpscaling.log'
$PluginDir = 'BepInEx\plugins\ParalivesUpscaling'
$CacheDir = 'BepInEx\cache\ParalivesUpscaling'
$ConfigFile = 'BepInEx\config\paralives.upscaling.cfg'

function Say([string]$Text, [string]$Color = 'Gray') { Write-Host $Text -ForegroundColor $Color }

function Ask([string]$Question, [string[]]$Keys) {
    while ($true) {
        $a = (Read-Host $Question).Trim().ToLowerInvariant()
        if ($a.Length -gt 0 -and $Keys -contains $a.Substring(0, 1)) { return $a.Substring(0, 1) }
    }
}

function Find-Game {
    if ($Game) { return $Game }
    # 1. the zip was extracted inside (or right below) the game folder
    foreach ($d in @($PSScriptRoot, (Split-Path $PSScriptRoot -Parent))) {
        if ($d -and (Test-Path (Join-Path $d 'Paralives.exe'))) { return $d }
    }
    # 2. Steam libraries
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if ($steam) {
        $libraries = @($steam)
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) { $libraries += $m.Groups[1].Value -replace '\\\\', '\' }
        }
        foreach ($l in $libraries) {
            $p = Join-Path $l 'steamapps\common\Paralives'
            if (Test-Path (Join-Path $p 'Paralives.exe')) { return $p }
        }
    }
    # 3. ask
    Say 'Paralives was not found automatically. Pick its folder (the one with Paralives.exe).' Yellow
    return Select-GameFolder
}

# Folder picker; returns a folder containing Paralives.exe, or $null if cancelled.
function Select-GameFolder {
    Add-Type -AssemblyName System.Windows.Forms
    while ($true) {
        $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
        $dialog.Description = 'Select the Paralives folder (the one with Paralives.exe)'
        if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { return $null }
        if (Test-Path (Join-Path $dialog.SelectedPath 'Paralives.exe')) { return $dialog.SelectedPath }
        Say "That folder has no Paralives.exe: $($dialog.SelectedPath)" Yellow
    }
}

# Shows the folder and lets the user confirm or change it before anything is touched.
function Confirm-GameFolder([string]$Root) {
    while ($true) {
        Say ''
        Say "Paralives folder: $Root" White
        switch (Ask 'Is this the right folder? [Y]es / [C]hange / [Q]uit' @('y', 'c', 'q')) {
            'y' { return $Root }
            'q' { return $null }
            'c' {
                $picked = Select-GameFolder
                if ($picked) { $Root = $picked }
            }
        }
    }
}

# .NET instead of Get-FileHash / Expand-Archive / Set-Clipboard: those cmdlets are missing when Windows PowerShell
# inherits a PowerShell 7 module path.
function Get-Sha256([string]$Path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($Path)
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '') } finally { $stream.Dispose(); $sha.Dispose() }
}

function Expand-Zip([string]$Zip, [string]$Destination) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        foreach ($e in $archive.Entries) {
            $target = Join-Path $Destination $e.FullName
            if ($e.FullName.EndsWith('/')) { [void][System.IO.Directory]::CreateDirectory($target); continue }
            [void][System.IO.Directory]::CreateDirectory((Split-Path $target -Parent))
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, $target, $true)
        }
    } finally { $archive.Dispose() }
}

function Install-BepInEx([string]$Root) {
    Say "BepInEx not found: downloading BepInEx $BepInExVersion (x64) from GitHub..." Yellow
    $zip = Join-Path $env:TEMP "BepInEx_win_x64_$BepInExVersion.zip"
    (New-Object System.Net.WebClient).DownloadFile($BepInExUrl, $zip)
    $hash = Get-Sha256 $zip
    if ($hash -ne $BepInExSha256) {
        Remove-Item $zip -Force
        throw "The downloaded BepInEx file does not match the expected checksum ($hash). Nothing was installed."
    }
    Expand-Zip $zip $Root
    Remove-Item $zip -Force
    Say 'BepInEx installed.' Green
}

function Install-Mod([string]$Root) {
    $files = Join-Path $PSScriptRoot 'files'
    if (-not (Test-Path $files)) {
        # double-clicking Install.bat inside the zip in Explorer copies only that file and setup.ps1 to a temp folder
        if ($PSScriptRoot -like "$([System.IO.Path]::GetTempPath().TrimEnd('\'))*") {
            throw "It looks like Install.bat was run from inside the zip. Extract the zip first (right-click > Extract All), then run Install.bat from the extracted folder."
        }
        throw "The 'files' folder is missing next to this script ($PSScriptRoot). Extract the whole zip, then run Install.bat from the extracted folder."
    }
    if (Test-Path (Join-Path $Root 'BepInEx\core\BepInEx.dll')) { Say 'BepInEx found.' } else { Install-BepInEx $Root }
    Copy-Item (Join-Path $files '*') $Root -Recurse -Force
    Say 'Paralives Upscaling installed.' Green
    Say ''
    try { Add-Type -AssemblyName System.Windows.Forms; [System.Windows.Forms.Clipboard]::SetText('-force-d3d12') } catch { }
    Say 'IMPORTANT: the mod only works on DirectX 12. In Steam:' Yellow
    Say '  right-click Paralives > Properties > General > Launch Options, and add:  -force-d3d12' Yellow
    Say '  (-force-d3d12 has been copied to the clipboard: just paste it there)' Yellow
    Say ''
    Say 'Then start the game and choose Options > Video > Upscaling Technology.'
}

function Uninstall-Mod([string]$Root) {
    $native = Join-Path $Root 'Paralives_Data\Plugins\x86_64'
    foreach ($f in $NativeFiles) {
        $p = Join-Path $native $f
        if (Test-Path $p) { Remove-Item $p -Force }
    }
    foreach ($d in $PluginDir, $CacheDir) {
        $p = Join-Path $Root $d
        if (Test-Path $p) { Remove-Item $p -Recurse -Force }
    }
    $cfg = Join-Path $Root $ConfigFile
    if ((Test-Path $cfg) -and (Ask 'Also delete your Paralives Upscaling settings? [Y]es / [N]o' @('y', 'n')) -eq 'y') { Remove-Item $cfg -Force }
    Say 'Paralives Upscaling removed. BepInEx was left installed (other mods may use it).' Green
    Say 'You can also remove -force-d3d12 from the Steam launch options if nothing else needs it.'
}

try {
    Say 'Paralives Upscaling (FSR / XeSS / DLSS)' Cyan
    Say ''
    $root = Find-Game
    if (-not $root) { throw 'Paralives was not found. Run Install.bat again and pick the folder that contains Paralives.exe.' }
    $root = Confirm-GameFolder $root
    if (-not $root) { Say 'Nothing changed.'; Say ''; Read-Host 'Press Enter to close' | Out-Null; exit }
    while (Get-Process -Name 'Paralives' -ErrorAction SilentlyContinue) {
        Read-Host 'Paralives is running. Close the game, then press Enter' | Out-Null
    }

    $installed = (Test-Path (Join-Path $root 'Paralives_Data\Plugins\x86_64\ParalivesUpscaling.dll')) -or (Test-Path (Join-Path $root $PluginDir))
    if ($installed) {
        Say 'Paralives Upscaling is already installed.'
        switch (Ask '[U]pdate / reinstall, [R]emove, or [C]ancel?' @('u', 'r', 'c')) {
            'u' { Install-Mod $root }
            'r' { Uninstall-Mod $root }
            'c' { Say 'Nothing changed.' }
        }
    } else {
        Install-Mod $root
    }
} catch {
    Say ''
    Say "Error: $($_.Exception.Message)" Red
}
Say ''
Read-Host 'Press Enter to close' | Out-Null
