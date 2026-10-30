[CmdletBinding()]
param(
    [string]$Version = '',
    [string]$WorkRoot = '',
    [string]$BuildDirectory = '',
    [switch]$SkipNativeBuild,
    [switch]$SkipPortableZip,
    [switch]$SkipInstaller,
    [switch]$Offline
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path -Parent $PSScriptRoot
$sourceVersion = (Get-Content -LiteralPath (Join-Path $repo 'VERSION') -Raw).Trim()
if (!$Version) { $Version = $sourceVersion }
if ($Version -ne $sourceVersion) { throw 'Package version must match VERSION. Update VERSION before building.' }
$allowedRoot = [IO.Path]::GetFullPath((Join-Path $repo '_verify/distribution'))
if (!$WorkRoot) { $WorkRoot = $allowedRoot }
$WorkRoot = [IO.Path]::GetFullPath($WorkRoot)
if ($WorkRoot -ne $allowedRoot -and !$WorkRoot.StartsWith($allowedRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'WorkRoot must remain inside this repository''s _verify/distribution directory.'
}
if ($Version -notmatch '^\d+\.\d+\.\d+(?:[-.][A-Za-z0-9]+)*$') { throw 'Invalid release version.' }
if (!$BuildDirectory) { $BuildDirectory = Join-Path $WorkRoot 'build' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
function Assert-ScratchPath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if (!$full.StartsWith($allowedRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing operation outside distribution scratch directory: $full"
    }
    return $full
}
$null = Assert-ScratchPath $BuildDirectory
function New-Directory([string]$Path) { $null = New-Item -ItemType Directory -Path $Path -Force }
function Invoke-Checked([string]$File, [string[]]$Arguments) {
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$File failed with exit code $LASTEXITCODE" }
}
# Optional Authenticode signing. Configure it with environment variables:
#   ANIMBRIDGE_SIGN_CERT_THUMBPRINT              certificate in the Windows certificate store, or
#   ANIMBRIDGE_SIGN_PFX + ANIMBRIDGE_SIGN_PFX_PASSWORD   a .pfx file and its password
#   ANIMBRIDGE_SIGN_TIMESTAMP_URL                RFC 3161 server (default http://timestamp.digicert.com)
# Without a certificate the build is unchanged and unsigned.
$signThumbprint = $env:ANIMBRIDGE_SIGN_CERT_THUMBPRINT
$signPfx = $env:ANIMBRIDGE_SIGN_PFX
$signingEnabled = [bool]$signThumbprint -or [bool]$signPfx
$signTimestampUrl = $env:ANIMBRIDGE_SIGN_TIMESTAMP_URL
if (!$signTimestampUrl) { $signTimestampUrl = 'http://timestamp.digicert.com' }
function Get-SignTool {
    $found = Get-ChildItem -Path "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
        Sort-Object { [version]($_.Directory.Parent.Name) } -Descending | Select-Object -First 1
    if (!$found) { throw 'Code signing is configured but signtool.exe was not found. Install the Windows 10/11 SDK.' }
    return $found.FullName
}
function Invoke-CodeSigning([string[]]$Files) {
    if (!$signingEnabled) { Write-Host 'Code signing skipped (no certificate configured)'; return }
    $signtool = Get-SignTool
    $arguments = @('sign', '/fd', 'SHA256', '/tr', $signTimestampUrl, '/td', 'SHA256')
    if ($signThumbprint) { $arguments += @('/sha1', $signThumbprint) }
    else {
        if (!(Test-Path -LiteralPath $signPfx)) { throw 'ANIMBRIDGE_SIGN_PFX does not point to an existing file.' }
        $arguments += @('/f', $signPfx)
        # The password is passed to signtool but never printed.
        if ($env:ANIMBRIDGE_SIGN_PFX_PASSWORD) { $arguments += @('/p', $env:ANIMBRIDGE_SIGN_PFX_PASSWORD) }
    }
    foreach ($file in $Files) {
        Write-Host "Signing $(Split-Path -Leaf $file)"
        & $signtool @arguments $file | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "signtool failed for $(Split-Path -Leaf $file) with exit code $LASTEXITCODE" }
    }
}
$downloads = Join-Path $WorkRoot 'downloads'
$expanded = Join-Path $WorkRoot 'expanded'
$out = Join-Path $WorkRoot 'out'
foreach ($dir in @($WorkRoot, $downloads, $expanded, $out)) { New-Directory $dir }
$temporary = Join-Path $WorkRoot 'tmp'
New-Directory $temporary
$previousTemp = $env:TEMP
$previousTmp = $env:TMP
$previousPythonEnvironment = @{}
foreach ($entry in Get-ChildItem Env:) {
    if (($entry.Name.StartsWith('PYTHON', [StringComparison]::OrdinalIgnoreCase) -or $entry.Name.StartsWith('BLENDER_', [StringComparison]::OrdinalIgnoreCase))) {
        $previousPythonEnvironment[$entry.Name] = $entry.Value
        [Environment]::SetEnvironmentVariable($entry.Name, $null, 'Process')
    }
}
$env:PYTHONDONTWRITEBYTECODE = '1'
foreach ($part in @('config', 'scripts', 'data')) { New-Directory (Join-Path $temporary ('blender-' + $part)) }
$env:BLENDER_USER_CONFIG = Join-Path $temporary 'blender-config'
$env:BLENDER_USER_SCRIPTS = Join-Path $temporary 'blender-scripts'
$env:BLENDER_USER_DATAFILES = Join-Path $temporary 'blender-data'
$env:TEMP = $temporary
$env:TMP = $temporary
try {
$lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'dependencies.lock.json') -Raw | ConvertFrom-Json
$assets = @{}
foreach ($asset in $lock.assets) {
    $path = Join-Path $downloads $asset.filename
    if (!(Test-Path -LiteralPath $path)) {
        if ($Offline) { throw "Offline cache is missing $($asset.filename)" }
        Write-Host "Downloading $($asset.filename)"
        $partial = $path + '.partial'
        Invoke-Checked 'curl.exe' @('--fail', '--location', '--retry', '3', '--silent', '--show-error', '--output', $partial, $asset.url)
        $actual = (Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $asset.sha256) { throw "SHA256 mismatch for downloaded $($asset.filename)" }
        Move-Item -LiteralPath $partial -Destination $path -Force
    }
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $asset.sha256) { throw "Cached SHA256 mismatch: $($asset.filename). Restore the pinned original." }
    $assets[$asset.name] = $path
}
if (!$SkipNativeBuild) {
    $configureArgs = @('-S', $repo, '-B', $BuildDirectory, '-A', 'x64', '-DBUILD_TESTING=OFF', '-DANIMBRIDGE_ENABLE_SOURCE_FALLBACK=OFF', '-DANIMBRIDGE_BUILD_DESKTOP=ON', '-DANIMBRIDGE_ENABLE_UI_TEST_HOOKS=OFF')
    if ($Offline) { $configureArgs += '-DFETCHCONTENT_FULLY_DISCONNECTED=ON' }
    else { $configureArgs += '-DFETCHCONTENT_FULLY_DISCONNECTED=OFF' }
    Invoke-Checked 'cmake.exe' $configureArgs
    Invoke-Checked 'cmake.exe' @('--build', $BuildDirectory, '--config', 'Release', '--parallel')
}
$cacheFile = Join-Path $BuildDirectory 'CMakeCache.txt'
if (!(Test-Path -LiteralPath $cacheFile)) { throw 'Native CMake build cache is missing.' }
$cache = Get-Content -LiteralPath $cacheFile -Raw
if ($cache -notmatch '(?m)^ANIMBRIDGE_ENABLE_SOURCE_FALLBACK:BOOL=OFF\r?$') { throw 'Release build must disable source-directory fallback.' }
if ($cache -notmatch '(?m)^ANIMBRIDGE_ENABLE_UI_TEST_HOOKS:BOOL=OFF\r?$') { throw 'Release build must compile out UI test hooks.' }
if ($cache -notmatch '(?m)^ANIMBRIDGE_BUILD_DESKTOP:BOOL=ON\r?$') { throw 'Release build must include the desktop shell.' }
if ($cache -notmatch ('(?m)^CMAKE_PROJECT_VERSION:STATIC=' + [regex]::Escape($Version) + '\r?$')) { throw 'Native build version does not match VERSION. Reconfigure and rebuild before packaging.' }
$nativeDir = Join-Path $BuildDirectory 'Release'
foreach ($name in @('AnimBridge.exe', 'AnimBridgeCli.exe', 'AnimBridgeProcessSupervisor.exe')) {
    if (!(Test-Path -LiteralPath (Join-Path $nativeDir $name))) { throw "Missing native executable $name" }
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
function Reset-ScratchDirectory([string]$Destination) {
    $Destination = Assert-ScratchPath $Destination
    if (Test-Path -LiteralPath $Destination) {
        Remove-Item -LiteralPath $Destination -Recurse -Force
    }
    New-Directory $Destination
}
function Expand-Zip([string]$Archive, [string]$Destination) {
    # Always extract from the verified archive. Expanded runtime caches can
    # contain partial files or bytecode generated by a previous smoke run.
    Reset-ScratchDirectory $Destination
    [IO.Compression.ZipFile]::ExtractToDirectory($Archive, $Destination, $true)
}
$blenderParent = Join-Path $expanded 'blender'
Expand-Zip $assets.blender $blenderParent
$blenderDir = Join-Path $blenderParent 'blender-5.2.3-windows-x64'
$webviewParent = Join-Path $expanded 'webview2'
$webviewDir = Join-Path $webviewParent 'Microsoft.WebView2.FixedVersionRuntime.156.0.4314.50.x64'
Reset-ScratchDirectory $webviewParent
Invoke-Checked 'expand.exe' @($assets.webview2, '-F:*', $webviewParent) *> (Join-Path $WorkRoot 'webview-expand.log')
$sourceInfo = Join-Path $expanded 'blender-source-info'
Reset-ScratchDirectory $sourceInfo
Invoke-Checked 'tar.exe' @('-xf', $assets.'blender-source', '-C', $sourceInfo,
    'blender-5.2.3/COPYING', 'blender-5.2.3/doc/license/GPL3-license.txt',
    'blender-5.2.3/build_files/build_environment/cmake/versions.cmake')

$nativeNotices = Join-Path $expanded 'native-notices'
Reset-ScratchDirectory $nativeNotices
Invoke-Checked 'tar.exe' @('-xf', $assets.'nlohmann-json', '-C', $nativeNotices, 'json/LICENSE.MIT')
Invoke-Checked 'tar.exe' @('-xf', $assets.zlib, '-C', $nativeNotices, 'zlib-1.3.1/LICENSE')
$jsonDir = Join-Path $nativeNotices 'json'
$zlibDir = Join-Path $nativeNotices 'zlib-1.3.1'
$sdkDir = Join-Path $expanded 'webview2-sdk'
Expand-Zip $assets.'webview2-sdk' $sdkDir

$stage = Assert-ScratchPath (Join-Path $WorkRoot 'stage/AnimBridge')
if (Test-Path -LiteralPath $stage) {
    # Only validated generated staging/extraction directories are reset.
    # Installed applications and user data are never deletion targets.
    Remove-Item -LiteralPath $stage -Recurse -Force
}
foreach ($relative in @('', 'backend/src/unityanimbridge', 'runtime/python/Lib/site-packages', 'licenses', 'sources')) {
    New-Directory (Join-Path $stage $relative)
}
foreach ($name in @('AnimBridge.exe', 'AnimBridgeCli.exe', 'AnimBridgeProcessSupervisor.exe')) { Copy-Item -LiteralPath (Join-Path $nativeDir $name) -Destination $stage }
Copy-Item -LiteralPath (Join-Path $repo 'native/ui') -Destination (Join-Path $stage 'ui') -Recurse
# Backend inputs are deliberately allowlisted. Never copy the repository,
# _verify, user libraries, profiles, exports, or Python bytecode caches.
foreach ($file in Get-ChildItem -LiteralPath (Join-Path $repo 'src/unityanimbridge') -File -Filter '*.py') {
    Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $stage 'backend/src/unityanimbridge')
}
foreach ($folder in @('blender', 'unity', 'unreal')) {
    $sourceRoot = Join-Path $repo $folder
    foreach ($file in Get-ChildItem -LiteralPath $sourceRoot -Recurse -File) {
        if ($file.Extension -notin @('.py', '.cs')) { continue }
        $relative = [IO.Path]::GetRelativePath($sourceRoot, $file.FullName)
        $target = Join-Path $stage ("backend/$folder/" + $relative)
        New-Directory (Split-Path -Parent $target)
        Copy-Item -LiteralPath $file.FullName -Destination $target
    }
}
# The shipped default preview character (backend/assets/characters/*.fbx).
$charactersTarget = Join-Path $stage 'backend/assets/characters'
New-Directory $charactersTarget
foreach ($file in Get-ChildItem -LiteralPath (Join-Path $repo 'assets/characters') -File -Filter '*.fbx') {
    Copy-Item -LiteralPath $file.FullName -Destination $charactersTarget
}
if (!(Test-Path -LiteralPath (Join-Path $charactersTarget 'Default_Humanoid.fbx'))) { throw 'Default_Humanoid.fbx is missing from the backend payload.' }
Copy-Item -LiteralPath (Join-Path $repo 'pyproject.toml') -Destination (Join-Path $stage 'backend')
Copy-Item -LiteralPath (Join-Path $repo 'VERSION') -Destination (Join-Path $stage 'backend')
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination (Join-Path $stage 'backend')
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination $stage
$pythonDir = Join-Path $stage 'runtime/python'
Expand-Zip $assets.python $pythonDir
$sitePackages = Join-Path $pythonDir 'Lib/site-packages'
New-Directory $sitePackages
foreach ($name in @('numpy', 'pyyaml')) {
    [IO.Compression.ZipFile]::ExtractToDirectory($assets[$name], $sitePackages, $true)
}
# Embeddable CPython deliberately ignores external Python installations and
# PYTHONPATH. All paths below are relative to this movable python.exe.
@('python312.zip', '.', 'Lib/site-packages', '../../backend/src', 'import site') |
    Set-Content -LiteralPath (Join-Path $pythonDir 'python312._pth') -Encoding ascii
Copy-Item -LiteralPath $blenderDir -Destination (Join-Path $stage 'runtime/blender') -Recurse
Copy-Item -LiteralPath $webviewDir -Destination (Join-Path $stage 'runtime/webview2') -Recurse
foreach ($name in @('INSTALL.txt', 'Prepare-Portable.cmd')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $stage
}
$licenses = Join-Path $stage 'licenses'
foreach ($name in @('THIRD_PARTY_NOTICES.txt', 'SOURCE-AVAILABILITY.txt')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $licenses
}
Copy-Item -Path (Join-Path $PSScriptRoot 'licenses/*') -Destination $licenses
Copy-Item -LiteralPath (Join-Path $pythonDir 'LICENSE.txt') -Destination (Join-Path $licenses 'Python-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $sourceInfo 'blender-5.2.3/COPYING') -Destination (Join-Path $licenses 'Blender-COPYING.txt')
Copy-Item -LiteralPath (Join-Path $sourceInfo 'blender-5.2.3/doc/license/GPL3-license.txt') -Destination (Join-Path $licenses 'Blender-GPL-3.0.txt')
Copy-Item -LiteralPath (Join-Path $sourceInfo 'blender-5.2.3/build_files/build_environment/cmake/versions.cmake') -Destination (Join-Path $licenses 'Blender-dependency-sources.cmake')
Copy-Item -LiteralPath (Join-Path $jsonDir 'LICENSE.MIT') -Destination (Join-Path $licenses 'nlohmann-json-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $zlibDir 'LICENSE') -Destination (Join-Path $licenses 'zlib-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $sdkDir 'LICENSE.txt') -Destination (Join-Path $licenses 'WebView2-SDK-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $sdkDir 'NOTICE.txt') -Destination (Join-Path $licenses 'WebView2-SDK-NOTICE.txt')
$termsHeader = @'
AnimBridge includes independently licensed third-party components.
By proceeding, you accept the Microsoft WebView2 runtime terms below for
that component. Other components retain their own licenses and rights,
including the separately supplied Blender executable under GPL 3.0 or later.
See THIRD_PARTY_NOTICES.txt and SOURCE-AVAILABILITY.txt in this directory.

'@
$termsHeader + (Get-Content -LiteralPath (Join-Path $licenses 'WebView2-Fixed-LICENSE.txt') -Raw) |
    Set-Content -LiteralPath (Join-Path $licenses 'INSTALL-TERMS.txt') -Encoding utf8BOM
$termsBytes = [IO.File]::ReadAllBytes((Join-Path $licenses 'INSTALL-TERMS.txt'))
if ($termsBytes.Length -lt 3 -or $termsBytes[0] -ne 239 -or $termsBytes[1] -ne 187 -or $termsBytes[2] -ne 191) {
    throw 'The Inno Setup license text must be UTF-8 with BOM.'
}
foreach ($name in @('blender-source', 'nlohmann-json', 'zlib')) {
    Copy-Item -LiteralPath $assets[$name] -Destination (Join-Path $stage 'sources')
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'dependencies.lock.json') -Destination $stage
# Sign before the manifest hashes the files and before Inno Setup packs them.
Invoke-CodeSigning @(foreach ($name in @('AnimBridge.exe', 'AnimBridgeCli.exe', 'AnimBridgeProcessSupervisor.exe')) { Join-Path $stage $name })
# Finalize the native runtime contract BEFORE launching any staged executable.
# The common tool verifies the frozen source fingerprint, production cache flags,
# supervisor, bootstrap and every pinned input archive.
$stageTool = Join-Path $PSScriptRoot 'stage.py'
$privatePython = Join-Path $pythonDir 'python.exe'
Invoke-Checked $privatePython @('-I', '-B', '-X', 'utf8', $stageTool, 'finalize', '--target', 'windows',
    '--root', $stage, '--repo', $repo, '--build-dir', $BuildDirectory, '--asset-cache', $downloads)
Write-Host 'Checking the embedded interpreter and backend without user Python paths...'
Invoke-Checked $privatePython @('-I', '-B', '-X', 'utf8', (Join-Path $stage 'backend/runtime_probe.py'),
    '--backend', (Join-Path $stage 'backend/src'))
Invoke-Checked (Join-Path $stage 'runtime/blender/blender.exe') @('--background', '--factory-startup', '--python-use-system-env',
    '--python-expr', 'import sys,bpy; assert sys.dont_write_bytecode; print("ANIMBRIDGE_BUNDLED_BLENDER", bpy.app.version_string)')
# Fake-evaluator contract tests intentionally require the separate development
# test build. Production/harness artifacts are validated with their actual private
# runtimes by Verify-Distribution.py; they must not enable source fallback merely
# to run development fixtures.
Invoke-Checked $privatePython @('-I', '-B', '-X', 'utf8', $stageTool, 'manifest', '--target', 'windows', '--root', $stage, '--repo', $repo)
Invoke-Checked $privatePython @('-I', '-B', '-X', 'utf8', $stageTool, 'verify', '--target', 'windows', '--root', $stage)
Copy-Item -LiteralPath (Join-Path $stage 'distribution-manifest.json') -Destination (Join-Path $out "AnimBridge-$Version-manifest.json") -Force
Write-Host "Staging finalized: $stage"
if (!$SkipPortableZip) {
    $zip = Assert-ScratchPath (Join-Path $out "AnimBridge-$Version-Portable-x64.zip")
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Fastest, $true)
}
if (!$SkipInstaller) {
    $innoDir = Join-Path $WorkRoot 'tools/InnoSetup'
    $iscc = Join-Path $innoDir 'ISCC.exe'
    if (!(Test-Path -LiteralPath $iscc)) {
        New-Directory (Split-Path -Parent $innoDir)
        $signature = Get-AuthenticodeSignature -LiteralPath $assets.'inno-setup-build-tool'
        if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notlike '*Pyrsys B.V.*') { throw 'Inno Setup publisher verification failed.' }
        $process = Start-Process -FilePath $assets.'inno-setup-build-tool' -ArgumentList @('/PORTABLE=1', '/CURRENTUSER', '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/NOICONS', '/TASKS=', ('/DIR="' + $innoDir + '"')) -WindowStyle Hidden -Wait -PassThru
        if ($process.ExitCode -ne 0 -or !(Test-Path -LiteralPath $iscc)) { throw 'Portable Inno Setup extraction failed.' }
    }
    $innoLock = $lock.assets | Where-Object name -EQ 'inno-setup-build-tool'
    if ((Get-FileHash -LiteralPath $iscc -Algorithm SHA256).Hash.ToLowerInvariant() -ne $innoLock.compiler_sha256) {
        throw 'Inno Setup compiler cache does not match the compiler extracted from the pinned official installer.'
    }
    Write-Host 'Compiling the offline setup executable...'
    Invoke-Checked $iscc @("/DAppVersion=$Version", "/DPayloadDir=$stage", "/DOutputDir=$out", (Join-Path $PSScriptRoot 'AnimBridge.iss')) *> (Join-Path $WorkRoot 'inno-build.log')
}
# Sign the setup executable before checksums are computed.
if (!$SkipInstaller) { Invoke-CodeSigning @(Get-ChildItem -LiteralPath $out -File -Filter "AnimBridge-$Version-Setup*.exe" | ForEach-Object FullName) }
$resultFiles = @(Get-ChildItem -LiteralPath $out -File | Where-Object { $_.Name -like "AnimBridge-$Version-*" })
$resultFiles | Sort-Object Name | ForEach-Object { '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.Name } |
    Set-Content -LiteralPath (Join-Path $out 'SHA256SUMS.txt') -Encoding ascii
Write-Host "Distribution assembled: $out (G2/G4 release gates require separate evidence)"
$resultFiles | Select-Object Name, Length
} finally {
    $env:TEMP = $previousTemp
    $env:TMP = $previousTmp
    foreach ($entry in Get-ChildItem Env:) {
        if (($entry.Name.StartsWith('PYTHON', [StringComparison]::OrdinalIgnoreCase) -or $entry.Name.StartsWith('BLENDER_', [StringComparison]::OrdinalIgnoreCase))) { [Environment]::SetEnvironmentVariable($entry.Name, $null, 'Process') }
    }
    foreach ($name in $previousPythonEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $previousPythonEnvironment[$name], 'Process') }
}
