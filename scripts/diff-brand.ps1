<#
.SYNOPSIS
  List brand-branch changes relative to upstream-pin (read-only).
.EXAMPLE
  .\scripts\diff-brand.ps1            # commits + file status summary
  .\scripts\diff-brand.ps1 -Stat      # with diffstat
#>
param(
  [string]$Base = 'upstream-pin',
  [string]$Brand = 'brand',
  [switch]$Stat
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$mb = (& git -C $repo merge-base $Base $Brand).Trim()
if ($LASTEXITCODE) { throw "no merge-base between $Base and $Brand" }
$behind = (& git -C $repo rev-list --count "$Brand..$Base").Trim()
$ahead  = (& git -C $repo rev-list --count "$Base..$Brand").Trim()
Write-Host "$Brand is $ahead ahead / $behind behind $Base (merge-base $mb)"
Write-Host "--- commits on $Brand not in $Base"
& git -C $repo log --oneline "$Base..$Brand"
Write-Host "--- files changed ($Base...$Brand)"
& git -C $repo -c core.quotepath=false diff --name-status "$Base...$Brand"
if ($Stat) { & git -C $repo diff --stat "$Base...$Brand" }