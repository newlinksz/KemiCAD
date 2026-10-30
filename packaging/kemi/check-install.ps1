# KemiCAD install-dir regression check: run after an install (default: C:\Program Files\KemiCAD).
# Exits non-zero if a shipped file is missing or kemicad-cli writes known bogus errors to stderr.
param( [string] $InstallDir = 'C:\Program Files\KemiCAD' )

$failed = 0
function Check( [string] $name, [bool] $ok ) {
    if( $ok ) { Write-Output "PASS $name" } else { Write-Output "FAIL $name"; $script:failed++ }
}

# 1.0.1 renamed the main program kicad.exe -> kemicad.exe; shortcuts must follow
Check 'bin\kemicad.exe exists' ( Test-Path ( Join-Path $InstallDir 'bin\kemicad.exe' ) )
Check 'bin\kicad.exe (pre-1.0.1 name) absent' ( -not ( Test-Path ( Join-Path $InstallDir 'bin\kicad.exe' ) ) )
$wsh = New-Object -ComObject WScript.Shell
foreach( $lnk in @( ( Join-Path $env:PUBLIC 'Desktop\KemiCAD.lnk' ), ( Join-Path $env:USERPROFILE 'Desktop\KemiCAD.lnk' ),
                    ( Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\KemiCAD\KemiCAD.lnk' ),
                    ( Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\KemiCAD\KemiCAD.lnk' ) ) ) {
    if( Test-Path $lnk ) {
        $target = $wsh.CreateShortcut( $lnk ).TargetPath
        Check "shortcut $lnk -> bin\kemicad.exe" ( $target -like '*\bin\kemicad.exe' )
    }
}

$cli = Join-Path $InstallDir 'bin\kemicad-cli.exe'
Check 'bin\kemicad-cli.exe exists' ( Test-Path $cli )
Check 'bin\kicad-cli.exe (pre-1.0.1 name) absent' ( -not ( Test-Path ( Join-Path $InstallDir 'bin\kicad-cli.exe' ) ) )
# Without fontconfig's config every text render logs "Fontconfig error: Cannot load default config file"
Check 'etc\fonts\fonts.conf exists' ( Test-Path ( Join-Path $InstallDir 'etc\fonts\fonts.conf' ) )
Check 'share\kicad\schemas\api.v1.schema.json exists' `
      ( Test-Path ( Join-Path $InstallDir 'share\kicad\schemas\api.v1.schema.json' ) )

if( Test-Path $cli ) {
    # A fresh config dir is the state right after install; run twice to cover first and later runs
    $cfg = Join-Path ( [IO.Path]::GetTempPath() ) ( 'kemi-check-' + [Guid]::NewGuid() )
    foreach( $run in 1, 2 ) {
        $psi = New-Object Diagnostics.ProcessStartInfo $cli, 'version'
        $psi.UseShellExecute = $false
        $psi.RedirectStandardError = $true
        $psi.RedirectStandardOutput = $true
        $psi.EnvironmentVariables['KICAD_CONFIG_HOME'] = $cfg
        $p = [Diagnostics.Process]::Start( $psi )
        $out = $p.StandardOutput.ReadToEnd(); $err = $p.StandardError.ReadToEnd(); $p.WaitForExit()
        Check "kemicad-cli version run $run exit 0" ( $p.ExitCode -eq 0 )
        Check "kemicad-cli version run $run stderr clean" `
              ( $err -notmatch "couldn't be created|Directory ''|Fontconfig|schema file .* not found" )
        if( $err ) { Write-Output "  stderr: $($err.Trim())" }
    }
    Remove-Item $cfg -Recurse -Force -ErrorAction SilentlyContinue
}

if( $failed ) { Write-Output "$failed check(s) failed"; exit 1 }
Write-Output 'all checks passed'