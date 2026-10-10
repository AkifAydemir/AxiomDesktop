$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    function Invoke-Checked {
        param([string]$Program, [string[]]$Arguments)
        & $Program @Arguments
        if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
    }
    Invoke-Checked -Program cmake -Arguments @('-S', '.', '-B', 'build', '-A', 'x64')
    Invoke-Checked -Program cmake -Arguments @('--build', 'build', '--config', 'Release')
    Invoke-Checked -Program ctest -Arguments @('--test-dir', 'build', '-C', 'Release', '--output-on-failure')
    & (Join-Path $PSScriptRoot 'check-icon.ps1') -IconPath 'src/icons/axiom.ico' -ExecutablePath 'build/Release/Axiom.exe'
    Write-Host 'Build and automated checks completed. Interactive UI is a separate check.'
} finally {
    Pop-Location
}
