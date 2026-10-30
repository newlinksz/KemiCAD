Unicode true
ManifestDPIAware true
!define APPNAME "KemiCAD"
!define VERSION "1.0.1"
!ifndef DIST
!define DIST "D:\KiCad\build\kemi-eda-dist"
!endif
!ifndef OUTDIR
!define OUTDIR "D:\KiCad\build\installer"
!endif
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\KemiCAD"
SetCompressor /SOLID lzma
Name "${APPNAME} ${VERSION}"
!ifdef PERUSER
OutFile "${OUTDIR}\KemiCAD-${VERSION}-x64-peruser-test.exe"
!else
OutFile "${OUTDIR}\KemiCAD-${VERSION}-x64-setup.exe"
!endif
InstallDir "$PROGRAMFILES64\KemiCAD"
!ifdef PERUSER
RequestExecutionLevel user
!define REGROOT HKCU
!define SVC current
!else
RequestExecutionLevel admin
!define REGROOT HKLM
!define SVC all
!endif
InstallDirRegKey ${REGROOT} "${UNINSTKEY}" "InstallLocation"
!include "MUI2.nsh"
!include "x64.nsh"
!define MUI_ICON "..\..\resources\bitmaps_png\icons\icon_kicad.ico"
!define MUI_UNICON "..\..\resources\bitmaps_png\icons\icon_kicad.ico"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"
VIProductVersion "1.0.1.0"
VIAddVersionKey /LANG=${LANG_SIMPCHINESE} "ProductName" "KemiCAD"
VIAddVersionKey /LANG=${LANG_SIMPCHINESE} "CompanyName" "深圳新智联软件有限公司"
VIAddVersionKey /LANG=${LANG_SIMPCHINESE} "FileVersion" "1.0.1"
VIAddVersionKey /LANG=${LANG_SIMPCHINESE} "ProductVersion" "1.0.1"
VIAddVersionKey /LANG=${LANG_SIMPCHINESE} "FileDescription" "KemiCAD 1.0.1 安装程序 (基于 KiCad 开源软件, GPL-3.0)"
VIAddVersionKey /LANG=${LANG_SIMPCHINESE} "LegalCopyright" "Copyright (C) 深圳新智联软件有限公司; based on KiCad, GPL-3.0-or-later"
Function .onInit
  ${If} ${RunningX64}
    SetRegView 64
  ${Else}
    MessageBox MB_OK|MB_ICONSTOP "KemiCAD 需要 64 位 Windows。" /SD IDOK
    Abort
  ${EndIf}
FunctionEnd
Function un.onInit
  SetRegView 64
FunctionEnd
Section "KemiCAD" SecMain
  SectionIn RO
  SetShellVarContext ${SVC}
  SetOutPath "$INSTDIR"
  ; 1.0.1 renamed the CLI; drop the pre-1.0.1 binary on upgrade
  Delete "$INSTDIR\bin\kicad-cli.exe"
  ; 1.0.1 renamed the main program to kemicad.exe; drop the pre-1.0.1 kicad.exe on upgrade
  Delete "$INSTDIR\bin\kicad.exe"
  File /r "${DIST}\bin"
  File /r "${DIST}\share"
  ; fontconfig resolves ..\etc\fonts\fonts.conf from bin; without it text metrics differ from the build
  File /r "${DIST}\etc"
  File "/oname=LICENSE.txt" "..\..\LICENSE"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateDirectory "$SMPROGRAMS\KemiCAD"
  CreateShortcut "$SMPROGRAMS\KemiCAD\KemiCAD.lnk" "$INSTDIR\bin\kemicad.exe"
  CreateShortcut "$SMPROGRAMS\KemiCAD\卸载 KemiCAD.lnk" "$INSTDIR\uninstall.exe"
  CreateShortcut "$DESKTOP\KemiCAD.lnk" "$INSTDIR\bin\kemicad.exe"
  WriteRegStr ${REGROOT} "${UNINSTKEY}" "DisplayName" "KemiCAD 1.0.1"
  WriteRegStr ${REGROOT} "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr ${REGROOT} "${UNINSTKEY}" "Publisher" "深圳新智联软件有限公司"
  WriteRegStr ${REGROOT} "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\bin\kemicad.exe"
  WriteRegStr ${REGROOT} "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr ${REGROOT} "${UNINSTKEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr ${REGROOT} "${UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD ${REGROOT} "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD ${REGROOT} "${UNINSTKEY}" "NoRepair" 1
  ; App Paths: lets Win+R / Start run "kemicad"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\App Paths\kemicad.exe" "" "$INSTDIR\bin\kemicad.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\App Paths\kemicad.exe" "Path" "$INSTDIR\bin"
SectionEnd
Section "Uninstall"
  SetShellVarContext ${SVC}
  Delete "$DESKTOP\KemiCAD.lnk"
  RMDir /r "$SMPROGRAMS\KemiCAD"
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\share"
  RMDir /r "$INSTDIR\etc"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey ${REGROOT} "${UNINSTKEY}"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\App Paths\kemicad.exe"
SectionEnd
