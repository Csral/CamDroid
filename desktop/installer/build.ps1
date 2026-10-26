param(
    [string]$BuildDirectory = "$PSScriptRoot/../build-installer",
    [string]$Version = '1.0',
    [string]$InstallerVersion = '1.0.0',
    [string]$VcpkgInstalled = "$PSScriptRoot/../build-vcpkg/vcpkg_installed/x64-windows",
    [string]$RuntimeDirectory = '',
    [string]$Configuration = 'Release',
    [string]$OutputDirectory = "$PSScriptRoot/../dist"
)
$ErrorActionPreference = 'Stop'
function Relative-Path([string]$root, [string]$path) {
    $prefix = [IO.Path]::GetFullPath($root).TrimEnd([char[]]'\/') + [IO.Path]::DirectorySeparatorChar
    $absolute = [IO.Path]::GetFullPath($path)
    if (!$absolute.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'File is outside the packaging directory.'
    }
    $absolute.Substring($prefix.Length)
}

if ($Configuration -eq 'Debug') {
    Write-Output 'Debug builds are for local development. Build Release to generate distributable installers.'
    return
}
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
if (!$RuntimeDirectory) {
    $RuntimeDirectory = Join-Path $build $Configuration
}
$runtime = (Resolve-Path -LiteralPath $RuntimeDirectory).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
$stage = Join-Path $build "msi-payload-$Configuration"
$wix = [IO.Path]::GetFullPath("$PSScriptRoot/../build-tools/wix4/wix.exe")
if (!(Test-Path -LiteralPath $wix)) {
    dotnet tool install wix --version 4.0.6 --tool-path (Split-Path $wix)
    if ($LASTEXITCODE) {
        throw 'Could not install WiX.'
    }
}

foreach ($extension in 'UI', 'Util', 'Firewall', 'Bal') {
    & $wix extension add "WixToolset.$extension.wixext/4.0.6"
    if ($LASTEXITCODE) {
        throw "Could not load WiX $extension extension."
    }
}
# Only a generated staging directory inside this build tree may be cleared.
if (![IO.Path]::GetFullPath($stage).StartsWith($build + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Invalid staging path.'
}

if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $stage, $output | Out-Null
foreach ($file in Get-ChildItem -LiteralPath $runtime -File) {
    if (($file.Extension -eq '.dll' -and $file.Name -ne 'camdroid_setup_actions.dll') -or
        $file.Name -in 'camdroid_desktop.exe', 'camdroid_desktop.com', 'camdroid_uninstall.exe') {
        Copy-Item -LiteralPath $file.FullName -Destination $stage
    }
}

foreach ($name in 'platforms', 'styles', 'imageformats', 'iconengines', 'networkinformation', 'tls', 'generic') {
    $source = Join-Path $runtime $name
    if (Test-Path -LiteralPath $source) {
        Copy-Item -LiteralPath $source -Destination $stage -Recurse
    }
}
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$redist = Get-ChildItem -LiteralPath "$visualStudio/VC/Redist/MSVC" -Directory |
Where-Object Name -Match '^\d+\.' | Sort-Object {
    [version]$_.Name
} -Descending | Select-Object -First 1
$crt = Get-ChildItem -LiteralPath "$($redist.FullName)/x64" -Directory -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1
if (!$crt) {
    throw 'Visual C++ redistribution files were not found.'
}
Get-ChildItem -LiteralPath $crt.FullName -File -Filter '*.dll' | Copy-Item -Destination $stage

$projectRoot = [IO.Path]::GetFullPath("$PSScriptRoot/../..")
foreach ($relative in 'LICENSE', 'app/LICENSE', 'desktop/LICENSE', 'protocol/LICENSE', 'protocol/COPYING') {
    $source = Join-Path $projectRoot $relative
    $target = Join-Path "$stage/licenses/CamDroid" $relative
    New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
    Copy-Item -LiteralPath $source -Destination $target
}

# Redistributed dependency notices travel with the binaries.
foreach ($notice in Get-ChildItem -LiteralPath "$VcpkgInstalled/share" -Recurse -File -Filter copyright) {
    $relative = Relative-Path "$VcpkgInstalled/share" $notice.FullName
    $target = Join-Path "$stage/licenses" $relative
    New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
    Copy-Item -LiteralPath $notice.FullName -Destination $target
}

function Escape-Xml([string]$text) {
    [Security.SecurityElement]::Escape($text)
}

function Identifier([string]$text) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $hash = $algorithm.ComputeHash([Text.Encoding]::UTF8.GetBytes($text.ToLowerInvariant()))
    }
    finally {
        $algorithm.Dispose()
    }
    'I' + [BitConverter]::ToString($hash).Replace('-', '').Substring(0, 30)
}
$xml = [Text.StringBuilder]::new()
[void]$xml.AppendLine('<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs" xmlns:fire="http://wixtoolset.org/schemas/v4/wxs/firewall"><Fragment>')
$directories = @{
    '.' = 'INSTALLFOLDER'
}

foreach ($directory in Get-ChildItem -LiteralPath $stage -Directory -Recurse | Sort-Object FullName) {
    $relative = Relative-Path $stage $directory.FullName
    $parent = Split-Path $relative
    if (!$parent) {
        $parent = '.'
    }
    $id = Identifier $relative
    $directories[$relative] = $id
    [void]$xml.AppendLine('<DirectoryRef Id="' + $directories[$parent] + '"><Directory Id="' + $id + '" Name="' + (Escape-Xml $directory.Name) + '" /></DirectoryRef>')
}
$components = [Collections.Generic.List[string]]::new()
foreach ($file in Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName) {
    $relative = Relative-Path $stage $file.FullName
    $parent = Split-Path $relative
    if (!$parent) {
        $parent = '.'
    }
    $id = Identifier $relative
    $fileId = switch ($relative) {
        'camdroid_desktop.exe' {
            'MainExe'
        }
        'camdroid_camera.dll' {
            'CameraDll'
        }
        'camdroid_uninstall.exe' {
            'UninstallExe'
        }
        default {
            'F' + $id
        }
    }
    $components.Add($id)
    [void]$xml.AppendLine('<DirectoryRef Id="' + $directories[$parent] + '"><Component Id="' + $id + '" Guid="*"><File Id="' + $fileId + '" Source="' + (Escape-Xml $file.FullName) + '" KeyPath="yes" />')
    if ($fileId -eq 'MainExe') {
        [void]$xml.AppendLine('<fire:FirewallException Id="CamDroidTCP" Name="CamDroid video (private networks)" Program="[#MainExe]" Protocol="tcp" Scope="localSubnet" Profile="private" />')
        [void]$xml.AppendLine('<fire:FirewallException Id="CamDroidUDP" Name="CamDroid discovery and video (private networks)" Program="[#MainExe]" Protocol="udp" Scope="localSubnet" Profile="private" />')
    }
    [void]$xml.AppendLine('</Component></DirectoryRef>')
}
[void]$xml.AppendLine('<ComponentGroup Id="Payload">')
foreach ($id in $components) {
    [void]$xml.AppendLine('<ComponentRef Id="' + $id + '" />')
}
[void]$xml.AppendLine('</ComponentGroup></Fragment></Wix>')
$payload = Join-Path $build 'Payload.wxs'
[IO.File]::WriteAllText($payload, $xml.ToString())
Add-Type -AssemblyName System.Drawing
$brandImage = [System.Drawing.Image]::FromFile([IO.Path]::GetFullPath("$PSScriptRoot/../assets/camdroid.png"))
$bannerBitmap = Join-Path $build 'camdroid-banner.bmp'
$dialogBitmap = Join-Path $build 'camdroid-dialog.bmp'

try {
    foreach ($layout in @(
        @{ Path = $bannerBitmap; Width = 493; Height = 58; X = 437; Y = 8; Size = 42 },
        @{ Path = $dialogBitmap; Width = 493; Height = 312; X = 34; Y = 92; Size = 96 }
    )) {
        $bitmap = [System.Drawing.Bitmap]::new($layout.Width, $layout.Height, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
        $drawing = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            $drawing.Clear([System.Drawing.Color]::White)
            $drawing.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $drawing.DrawImage($brandImage, $layout.X, $layout.Y, $layout.Size, $layout.Size)
            $bitmap.Save($layout.Path, [System.Drawing.Imaging.ImageFormat]::Bmp)
        }
        finally {
            $drawing.Dispose()
            $bitmap.Dispose()
        }
    }
}
finally {
    $brandImage.Dispose()
}

$msi = Join-Path $output "CamDroid-$Version-x64.msi"
$msiArguments = @(
    'build', "$PSScriptRoot/Package.wxs", $payload,
    '-arch', 'x64',
    '-ext', 'WixToolset.UI.wixext',
    '-ext', 'WixToolset.Util.wixext',
    '-ext', 'WixToolset.Firewall.wixext',

    '-d', "BannerBitmap=$bannerBitmap",
    '-d', "DialogBitmap=$dialogBitmap",
    '-d', "AppIcon=$PSScriptRoot/../assets/camdroid.ico",
    '-d', "ProductVersion=$InstallerVersion",
    '-d', "ReleaseVersion=$Version",

    '-d', "SetupActions=$runtime/camdroid_setup_actions.dll",
    '-out', $msi
)

& $wix @msiArguments
if ($LASTEXITCODE) {
    throw 'MSI packaging failed.'
}

Copy-Item -LiteralPath "$runtime/camdroid_uninstall.exe" -Destination "$output/CamDroid-Uninstall.exe" -Force
$bundleArguments = @(
    'build', "$PSScriptRoot/Bundle.wxs",
    '-arch', 'x64',
    '-ext', 'WixToolset.Bal.wixext',
    '-ext', 'WixToolset.Util.wixext',

    '-d', "AppIcon=$PSScriptRoot/../assets/camdroid.ico",
    '-d', "AppLogo=$PSScriptRoot/../assets/camdroid.png",
    '-d', "ThemeFile=$PSScriptRoot/Theme.xml",
    '-d', "ProductVersion=$InstallerVersion",
    '-d', "ReleaseVersion=$Version",

    '-d', "MsiPackage=$msi",
    '-out', "$output/CamDroid-Setup.exe"
)

& $wix @bundleArguments
if ($LASTEXITCODE) {
    throw 'Setup executable packaging failed.'
}

Write-Output "Installer: $msi"
Write-Output "Setup: $output/CamDroid-Setup.exe"
