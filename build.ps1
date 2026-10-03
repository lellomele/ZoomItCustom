param([ValidateSet('Release','Debug')][string]$Configuration='Release',[switch]$RunTests)
$ErrorActionPreference='Stop'
$vswhere=Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs=(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
if(!$vs){throw 'Installare gli strumenti C++ di Visual Studio.'}
$vcvars=Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$command='call "'+$vcvars+'" >nul && set'
$environment=& $env:ComSpec /d /s /c $command
if($LASTEXITCODE -ne 0){throw 'Impossibile inizializzare gli strumenti C++.'}
$seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach($line in $environment){
 if(($line -match '^([^=]+)=(.*)$') -and $seen.Add($matches[1])){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}
}
$cmake=Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$linker=(Get-Command link.exe -ErrorAction Stop).Source
$ninja=Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
& $cmake -S $PSScriptRoot -B (Join-Path $PSScriptRoot "build-$Configuration") -G Ninja "-DCMAKE_BUILD_TYPE=$Configuration" -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl "-DCMAKE_LINKER=$linker" "-DCMAKE_MAKE_PROGRAM=$ninja"
if($LASTEXITCODE -ne 0){throw 'Configurazione fallita.'}
& $cmake --build (Join-Path $PSScriptRoot "build-$Configuration")
if($LASTEXITCODE -ne 0){throw 'Compilazione fallita.'}




if($RunTests) {
 $ctest=Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
 & $ctest --test-dir (Join-Path $PSScriptRoot "build-$Configuration") --output-on-failure
 if($LASTEXITCODE -ne 0){throw 'Prove di regressione fallite.'}
}