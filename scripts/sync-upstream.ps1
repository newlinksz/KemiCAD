<#
.SYNOPSIS
  Sync the upstream-pin branch with official KiCad (gitlab upstream).
  Default: fetch upstream and PRINT what would change. Nothing is moved or merged.
  -Apply : fast-forward upstream-pin to -Target (fast-forward only, never merge, never touches brand).
.EXAMPLE
  .\scripts\sync-upstream.ps1                        # fetch + report vs upstream/master
  .\scripts\sync-upstream.ps1 -Target 1a2b3c4d       # report for a specific commit
  .\scripts\sync-upstream.ps1 -Target 1a2b3c4d -Apply
#>
param(
  [string]$Target = 'upstream/master',
  [string]$Branch = 'upstream-pin',
  [switch]$Apply,
  [switch]$NoFetch
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
function G { & git -C $repo @args; if ($LASTEXITCODE) { throw "git $args failed ($LASTEXITCODE)" } }

if (-not $NoFetch) { Write-Host "fetching upstream ..."; G fetch upstream --no-tags }
$old = (G rev-parse "refs/heads/$Branch").Trim()
$new = (G rev-parse "$Target^{commit}").Trim()
Write-Host "$Branch : $old"
Write-Host "target : $new ($Target)"
if ($old -eq $new) { Write-Host "already at target"; exit 0 }
& git -C $repo merge-base --is-ancestor $old $new
if ($LASTEXITCODE -ne 0) { Write-Warning "target is NOT a descendant of $Branch; fast-forward impossible, refusing."; exit 2 }
$n = (G rev-list --count "$old..$new").Trim()
Write-Host "$n new upstream commits; last 10:"
G log --oneline -10 "$old..$new"
if (-not $Apply) { Write-Host "dry run only. Re-run with -Apply to fast-forward $Branch."; exit 0 }
$cur = (G rev-parse --abbrev-ref HEAD).Trim()
if ($cur -eq $Branch) { G merge --ff-only $new } else { G update-ref "refs/heads/$Branch" $new $old }
Write-Host "$Branch fast-forwarded to $new. brand was not touched; rebase/merge brand manually after review."