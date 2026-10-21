<#
 aplicar.ps1 - applies the wol-volfog patches (volumetric fog and light shafts for the wowdecomp client) in order.

 usage:  .\aplicar.ps1 -Destino C:\path\to\wowdecomp                 all the patches (.\patches and .\patches\propios, 26 in total) with "git apply", in order
         .\aplicar.ps1 -Destino C:\path\to\wowdecomp -Consolidado    copy the module folders and apply cambios-cliente.diff with --reject
         .\aplicar.ps1 -Destino C:\path\to\wowdecomp -Revertir       "git apply -R" of all the patches, in reverse order
         add -Comprobar to only check (git apply --check): in the default mode only the first patch can be checked, because
         each one needs the previous ones.

 If a patch fails, the ones applied before are undone (add -NoDeshacer to keep them). Exit code 0 = everything done.
 The patches are LF text (what git diff gives with core.autocrlf=true); if your tree is CRLF without autocrlf the script retries
 with --ignore-whitespace.
#>
param(
    [Parameter(Mandatory = $true)][string]$Destino,
    [switch]$Revertir,
    [switch]$Consolidado,
    [switch]$Comprobar,
    [switch]$NoDeshacer
)

$ErrorActionPreference = 'Continue'
$Aqui = Split-Path -Parent $MyInvocation.MyCommand.Path
$Orden = @(
    # Prerrequisitos de World of Legends (el árbol de desarrollo ya los llevaba cuando se hicieron B1-B10):
    '00a-recordar-contrasena.patch', '00b-topes-2008.patch', '00c-sombras-4096.patch', '00d-ui-depth.patch', '00e-sin-consola.patch',
    '01-B1-formatos.patch', '02-B2-shaders-pase.patch', '03-B3-depth-intz.patch', '04-B3b-correcciones.patch',
    '05-B4B5-cvars-modelo.patch', '06-B6-marcha-composicion.patch', '07-B7-temporal.patch', '08-B6b-correcciones.patch',
    '09-B8-luces.patch', '10-B9-haces.patch', '11-B10-interiores-ajustes.patch', '12-B13-msaa-resz.patch', '13-B13b-correcciones.patch',
    # Cambios propios del cliente de World of Legends (carpeta patches\propios; 00e-sin-consola ya es la "sin-consola" de esa carpeta):
    'propios\00f-filelog.patch', 'propios\E1-post-fxaa-tonemap.patch', 'propios\hd-cache.patch', 'propios\E3-sombras-pcf.patch',
    'propios\E2-hbao.patch', 'propios\E2b-hbao-correccion.patch', 'propios\E4-gradacion-zona.patch', 'propios\cliente64.patch'
)
$Carpetas = @('Engine\Source\VolFog', 'Shaders\VolFog', 'Tools\VolFog')
$Ficheros = @('WoW\Source\WorldClient\VolFogWorld.h', 'WoW\Source\WorldClient\VolFogWorld.cpp')

if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Write-Host 'git is not in the PATH.'; exit 2 }
$Destino = (Resolve-Path -LiteralPath $Destino).Path
if (-not ((Test-Path (Join-Path $Destino 'CMakeLists.txt')) -and (Test-Path (Join-Path $Destino 'Engine\Source\Gx')))) {
    Write-Host "$Destino does not look like a wowdecomp checkout (CMakeLists.txt and Engine\Source\Gx expected)."
    exit 2
}

# git apply with the arguments given; on failure retries with --ignore-whitespace. Returns $true when it worked.
function Invoke-Apply([string]$Patch, [string[]]$Extra) {
    & git -C $Destino apply --whitespace=nowarn @Extra $Patch
    if ($LASTEXITCODE -eq 0) { return $true }
    & git -C $Destino apply --whitespace=nowarn --ignore-whitespace @Extra $Patch
    return ($LASTEXITCODE -eq 0)
}

function Undo-Applied([string[]]$Done) {
    for ($i = $Done.Count - 1; $i -ge 0; $i--) {
        Write-Host "  undoing $($Done[$i])"
        if (-not (Invoke-Apply (Join-Path $Aqui "patches\$($Done[$i])") @('-R'))) { Write-Host "  could not undo $($Done[$i]); check git status" }
    }
}

if ($Consolidado) {
    $diff = Join-Path $Aqui 'cambios-cliente.diff'
    if ($Comprobar) {
        & git -C $Destino apply --check --whitespace=nowarn $diff
        exit $LASTEXITCODE
    }
    foreach ($c in $Carpetas) {
        $dst = Join-Path $Destino $c
        New-Item -ItemType Directory -Force -Path $dst | Out-Null
        Copy-Item -Path (Join-Path $Aqui "$c\*") -Destination $dst -Recurse -Force
    }
    foreach ($f in $Ficheros) { Copy-Item -LiteralPath (Join-Path $Aqui $f) -Destination (Join-Path $Destino $f) -Force }
    Write-Host 'module folders copied; applying cambios-cliente.diff (--reject)'
    & git -C $Destino apply --reject --whitespace=nowarn $diff
    $rc = $LASTEXITCODE
    if ($rc -ne 0) {
        Write-Host 'Some hunks did not apply (see the .rej files below). Put them by hand next to the equivalent lines of your tree:'
        Get-ChildItem -Path $Destino -Recurse -Filter '*.rej' | ForEach-Object { Write-Host "  $($_.FullName)" }
    }
    exit $rc
}

if ($Revertir) {
    $rev = @($Orden)
    [array]::Reverse($rev)
    foreach ($p in $rev) {
        Write-Host "reverting $p"
        if (-not (Invoke-Apply (Join-Path $Aqui "patches\$p") @('-R'))) { Write-Host "FAILED reverting $p"; exit 1 }
    }
    Write-Host 'all patches reverted'
    exit 0
}

if ($Comprobar) {
    $primero = Join-Path $Aqui "patches\$($Orden[0])"
    & git -C $Destino apply --check --whitespace=nowarn $primero
    if ($LASTEXITCODE -ne 0) { & git -C $Destino apply --check --whitespace=nowarn --ignore-whitespace $primero }
    exit $LASTEXITCODE
}

$hechos = @()
foreach ($p in $Orden) {
    Write-Host "applying $p"
    if (-not (Invoke-Apply (Join-Path $Aqui "patches\$p") @())) {
        Write-Host "FAILED applying $p. These patches were made on wowdecomp 0e4e9fe plus other local patches of the World of Legends client;"
        Write-Host 'on another tree use -Consolidado (module + cambios-cliente.diff) and place the rejected hunks by hand.'
        if (-not $NoDeshacer) { Undo-Applied $hechos }
        exit 1
    }
    $hechos += $p
}
Write-Host 'all 26 patches applied; now: cmake -S . -B build/x64 -G "Visual Studio 17 2022" -A x64 -DWOWDECOMP_DISABLE_SIGNATURE_CHECKS=ON'
exit 0
