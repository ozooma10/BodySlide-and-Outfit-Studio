# Runs the actual headless Outfit Studio copy/apply/export paths against a
# generated Starfield fixture. Build OutfitStudio and BSOSTests first.
# Example: ./tests/Run-ClothWeightCopy.ps1 -BinaryDirectory build/RelWithDebInfo
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BinaryDirectory,
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$binaries = (Resolve-Path -LiteralPath $BinaryDirectory).Path
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path $repo ('build/cloth-weight-copy/' + [guid]::NewGuid().ToString('N'))
}
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new output directory to exclude stale results.' }
$run = New-Item -ItemType Directory -Path $OutputDirectory
$app = New-Item -ItemType Directory -Path (Join-Path $run.FullName 'app')
Copy-Item -LiteralPath (Join-Path $binaries 'OutfitStudio.exe') -Destination $app.FullName
Get-ChildItem -LiteralPath $binaries -Filter '*.dll' | Copy-Item -Destination $app.FullName
$resources = New-Item -ItemType Directory -Path (Join-Path $app.FullName 'res')
foreach ($folder in @('images', 'shaders', 'xrc')) {
    Copy-Item -LiteralPath (Join-Path $repo "res/$folder") -Destination $resources.FullName -Recurse
}
function Xml([string]$value) { [System.Security.SecurityElement]::Escape($value) }
$oldDirectory = $env:BSOS_CLOTH_COPY_DIR
$failed = $false
try {
    foreach ($scenario in @('CopyBoneWeights', 'TransferWeights', 'CopyToUnskinned')) {
        $method = if ($scenario -eq 'CopyToUnskinned') { 'CopyBoneWeights' } else { $scenario }
        $inputName = if ($scenario -eq 'CopyToUnskinned') { 'input-unskinned.nif' } else { 'input.nif' }
        $case = New-Item -ItemType Directory -Path (Join-Path $run.FullName $scenario)
        $scripts = New-Item -ItemType Directory -Path (Join-Path $case.FullName 'Automations')
        $env:BSOS_CLOTH_COPY_DIR = $case.FullName
        & (Join-Path $binaries 'BSOSTests.exe') '[.cloth-copy-setup]'
        if ($LASTEXITCODE -ne 0) { throw 'Fixture creation failed.' }
        Copy-Item -LiteralPath (Join-Path $case.FullName $inputName) -Destination (Join-Path $case.FullName 'workflow-input.nif')
        @"
<Config>
 <TargetGame>9</TargetGame><WarnMissingGamePath>false</WarnMissingGamePath>
 <BSATextureScan>false</BSATextureScan><LogLevel>4</LogLevel><Language>0</Language>
 <GameDataPath>$(Xml $case.FullName)</GameDataPath><ProjectPath>$(Xml $case.FullName)</ProjectPath>
 <Anim><DefaultSkeletonReference>$(Xml (Join-Path $case.FullName 'skeleton.nif'))</DefaultSkeletonReference><SkeletonRootName>Root</SkeletonRootName></Anim>
</Config>
"@ | Set-Content -LiteralPath (Join-Path $app.FullName 'Config.xml') -Encoding utf8
        @"
<AutomationScript name="ClothCopy">
 <Step type="ImportFile" active="true"><FilePath>$(Xml (Join-Path $case.FullName 'workflow-input.nif'))</FilePath></Step>
 <Step type="SetReferenceShape" active="true"><ShapeName>Reference</ShapeName></Step>
 <Step type="$method" active="true"><ProximityRadius>2</ProximityRadius><MaxResults>1</MaxResults><UseMask>false</UseMask></Step>
 <Step type="$method" active="true"><ProximityRadius>2</ProximityRadius><MaxResults>1</MaxResults><UseMask>false</UseMask></Step>
 <Step type="ExportFile" active="true"><FilePath>$(Xml (Join-Path $case.FullName 'output.nif'))</FilePath><WithRef>true</WithRef></Step>
</AutomationScript>
"@ | Set-Content -LiteralPath (Join-Path $scripts.FullName 'ClothCopy.xml') -Encoding utf8
        $process = Start-Process -FilePath (Join-Path $app.FullName 'OutfitStudio.exe') -ArgumentList '-a', 'ClothCopy' -WorkingDirectory $app.FullName -WindowStyle Hidden -PassThru
        if (!$process.WaitForExit(60000)) {
            $process.Kill()
            throw "$method timed out; see $($app.FullName)."
        }
        if ($process.ExitCode -ne 0) { throw "$method failed with exit code $($process.ExitCode)." }
        Copy-Item -LiteralPath (Join-Path $app.FullName 'Log_OS.txt') -Destination $case.FullName
        Write-Host "Verifying $scenario in $($case.FullName)"
        & (Join-Path $binaries 'BSOSTests.exe') '[.cloth-copy-verify]'
        if ($LASTEXITCODE -ne 0) { $failed = $true }
    }
}
finally { $env:BSOS_CLOTH_COPY_DIR = $oldDirectory }
Write-Host "Workflow artifacts: $($run.FullName)"
if ($failed) { throw 'Cloth weight copy regression checks failed.' }
