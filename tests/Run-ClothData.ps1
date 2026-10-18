# Exercises real reference/import/export and project-save paths without GUI prompts.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BinaryDirectory,
    [string]$OutputDirectory,
    [string]$OutfitStudioExecutable
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$binaries = (Resolve-Path -LiteralPath $BinaryDirectory).Path
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repo ('build/cloth-data/' + [guid]::NewGuid().ToString('N')) }
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new output directory to exclude stale exports.' }
$run = New-Item -ItemType Directory -Path $OutputDirectory
$app = New-Item -ItemType Directory -Path (Join-Path $run.FullName 'app')
if (!$OutfitStudioExecutable) { $OutfitStudioExecutable = Join-Path $binaries 'OutfitStudio.exe' }
Copy-Item -LiteralPath $OutfitStudioExecutable -Destination (Join-Path $app.FullName 'OutfitStudio.exe')
Get-ChildItem -LiteralPath $binaries -Filter '*.dll' | Copy-Item -Destination $app.FullName
$res = New-Item -ItemType Directory -Path (Join-Path $app.FullName 'res')
foreach ($folder in @('images', 'shaders', 'xrc')) { Copy-Item -LiteralPath (Join-Path $repo "res/$folder") -Destination $res.FullName -Recurse }
$scripts = New-Item -ItemType Directory -Path (Join-Path $run.FullName 'Automations')
function Xml([string]$value) { [System.Security.SecurityElement]::Escape($value) }
$oldDirectory = $env:BSOS_CLOTH_DATA_DIR
try {
    $env:BSOS_CLOTH_DATA_DIR = $run.FullName
    & (Join-Path $binaries 'BSOSTests.exe') '[.cloth-data-setup]'
    if ($LASTEXITCODE -ne 0) { throw 'Fixture generation failed.' }
    @"
<Config>
 <TargetGame>9</TargetGame><WarnMissingGamePath>false</WarnMissingGamePath>
 <BSATextureScan>false</BSATextureScan><LogLevel>4</LogLevel><Language>0</Language>
 <GameDataPath>$(Xml $run.FullName)</GameDataPath><ProjectPath>$(Xml $run.FullName)</ProjectPath>
 <Anim><DefaultSkeletonReference>$(Xml (Join-Path $run.FullName 'skeleton.nif'))</DefaultSkeletonReference><SkeletonRootName>Root</SkeletonRootName></Anim>
</Config>
"@ | Set-Content -LiteralPath (Join-Path $app.FullName 'Config.xml') -Encoding utf8
    @"
<AutomationScript name="ClothData">
 <Step type="LoadReference" active="true"><SourceFile>$(Xml (Join-Path $run.FullName 'reference.nif'))</SourceFile><Shape>Reference</Shape><MergeSliders>false</MergeSliders><MergeZaps>false</MergeZaps></Step>
 <Step type="ImportFile" active="true"><FilePath>$(Xml (Join-Path $run.FullName 'outfit.nif'))</FilePath></Step>
 <Step type="ExportFile" active="true"><FilePath>$(Xml (Join-Path $run.FullName 'outfit-only.nif'))</FilePath><WithRef>false</WithRef></Step>
 <Step type="ExportFile" active="true"><FilePath>$(Xml (Join-Path $run.FullName 'with-reference.nif'))</FilePath><WithRef>true</WithRef></Step>
 <Step type="SaveProject" active="true"><DisplayName>Cloth</DisplayName><OutputFileName>Cloth</OutputFileName><SliderSetFile>$(Xml (Join-Path $run.FullName 'SliderSets/Cloth.osp'))</SliderSetFile><ShapeDataFolder>Cloth</ShapeDataFolder><ShapeDataFile>Cloth.nif</ShapeDataFile><AutoCopyRef>false</AutoCopyRef><GenWeights>false</GenWeights></Step>
 <Step type="SaveProject" active="true"><DisplayName>Combined</DisplayName><OutputFileName>Combined</OutputFileName><SliderSetFile>$(Xml (Join-Path $run.FullName 'SliderSets/Combined.osp'))</SliderSetFile><ShapeDataFolder>Combined</ShapeDataFolder><ShapeDataFile>Combined.nif</ShapeDataFile><AutoCopyRef>true</AutoCopyRef><GenWeights>false</GenWeights></Step>
 <Step type="RenameShape" active="true"><OldName>Coat</OldName><NewName>Renamed</NewName></Step>
 <Step type="DuplicateShape" active="true"><TargetMeshes>Renamed</TargetMeshes><NewName>Duplicate</NewName></Step>
 <Step type="DeleteShape" active="true"><TargetMeshes>Renamed</TargetMeshes></Step>
 <Step type="ExportFile" active="true"><FilePath>$(Xml (Join-Path $run.FullName 'edited.nif'))</FilePath><WithRef>false</WithRef></Step>
 <Step type="MergeGeometry" active="true"><SourceShape>Reference</SourceShape><TargetShape>Duplicate</TargetShape><DeleteSource>true</DeleteSource></Step>
 <Step type="ExportFile" active="true"><FilePath>$(Xml (Join-Path $run.FullName 'merged.nif'))</FilePath><WithRef>true</WithRef></Step>
 <Step type="ClearProject" active="true" />
 <Step type="ImportFile" active="true"><FilePath>$(Xml (Join-Path $run.FullName 'ShapeData/Cloth/Cloth.nif'))</FilePath></Step>
 <Step type="ExportFile" active="true"><FilePath>$(Xml (Join-Path $run.FullName 'reopened.nif'))</FilePath><WithRef>true</WithRef></Step>
 <Step type="ClearProject" active="true" />
 <Step type="ImportFile" active="true"><FilePath>$(Xml (Join-Path $run.FullName 'with-reference.nif'))</FilePath></Step>
 <Step type="SetReferenceShape" active="true"><ShapeName>Reference</ShapeName></Step>
 <Step type="ExportFile" active="true"><FilePath>$(Xml (Join-Path $run.FullName 'combined-reopened-outfit.nif'))</FilePath><WithRef>false</WithRef></Step>
 <Step type="ClearProject" active="true" />
 <Step type="ImportFile" active="true"><FilePath>$(Xml (Join-Path $run.FullName 'ShapeData/Combined/Combined.nif'))</FilePath></Step>
 <Step type="SetReferenceShape" active="true"><ShapeName>Reference</ShapeName></Step>
 <Step type="ExportFile" active="true"><FilePath>$(Xml (Join-Path $run.FullName 'project-reopened-outfit.nif'))</FilePath><WithRef>false</WithRef></Step>
</AutomationScript>
"@ | Set-Content -LiteralPath (Join-Path $scripts.FullName 'ClothData.xml') -Encoding utf8
    $process = Start-Process -FilePath (Join-Path $app.FullName 'OutfitStudio.exe') -ArgumentList '-a', 'ClothData' -WorkingDirectory $app.FullName -WindowStyle Hidden -PassThru
    if (!$process.WaitForExit(60000)) { $process.Kill(); throw "Workflow timed out. See $($app.FullName)." }
    if ($process.ExitCode -ne 0) { throw "Workflow failed with exit code $($process.ExitCode)." }
    Copy-Item -LiteralPath (Join-Path $app.FullName 'Log_OS.txt') -Destination $run.FullName
    & (Join-Path $binaries 'BSOSTests.exe') '[.cloth-data-verify]'
    if ($LASTEXITCODE -ne 0) { throw 'Cloth preservation regression failed.' }
}
finally { $env:BSOS_CLOTH_DATA_DIR = $oldDirectory }
Write-Host "Verified cloth workflow: $($run.FullName)"
