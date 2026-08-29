; ZComms installer. Per-user on purpose: the app is unsigned until the
; Microsoft developer account lands, and a per-user install ($LOCALAPPDATA)
; needs no UAC elevation -- SmartScreen still warns on first run, but the
; install itself never asks for admin. Re-running the installer over an
; existing install is an upgrade.
;
; Built by tools/release.ps1 (makensis /DVERSION=... /DSOURCE_DIR=...).
Unicode true
ManifestSupportedOS all
RequestExecutionLevel user
SetCompressor /SOLID lzma

!include "MUI2.nsh"

!ifndef VERSION
  !define VERSION "0.0.0-dev"
!endif
!ifndef SOURCE_DIR
  !error "SOURCE_DIR must point to the staged ZComms folder"
!endif
!ifndef OUT_FILE
  !define OUT_FILE "ZComms-Setup-${VERSION}.exe"
!endif

; The files whose absence means the package is broken, not merely lean.
; The Zoom SDK loader pulls its DLL tree lazily, so the whole staged bin
; directory ships; these named checks catch a mis-staged build at PACKAGE
; time instead of as a support incident.
!macro RequireSourceFile RELPATH
  !if /FileExists "${SOURCE_DIR}\${RELPATH}"
  !else
    !error "SOURCE_DIR is missing required file: ${RELPATH}"
  !endif
!macroend
!insertmacro RequireSourceFile "zcomms.exe"
!insertmacro RequireSourceFile "sdk.dll"
!insertmacro RequireSourceFile "WebView2Loader.dll"
!insertmacro RequireSourceFile "QUICKSTART.md"

Name "ZComms"
OutFile "${OUT_FILE}"
InstallDir "$LOCALAPPDATA\ZComms\app"
BrandingText "ZComms ${VERSION}"

!define MUI_ICON "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"
!define MUI_ABORTWARNING

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\zcomms.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start ZComms"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "ZComms"
  SetOutPath "$INSTDIR"
  File /r "${SOURCE_DIR}\*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateDirectory "$SMPROGRAMS\ZComms"
  CreateShortcut "$SMPROGRAMS\ZComms\ZComms.lnk" "$INSTDIR\zcomms.exe"
  CreateShortcut "$SMPROGRAMS\ZComms\Uninstall ZComms.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortcut "$DESKTOP\ZComms.lnk" "$INSTDIR\zcomms.exe"

  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZComms" \
    "DisplayName" "ZComms"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZComms" \
    "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZComms" \
    "Publisher" "ZComms"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZComms" \
    "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZComms" \
    "InstallLocation" "$INSTDIR"
SectionEnd

Section "Uninstall"
  ; The app must not be running -- sdk.dll and the exe would be locked.
  ; NSIS deletes what it can and leaves the rest; a clean stop first is the
  ; documented path (QUICKSTART).
  Delete "$SMPROGRAMS\ZComms\ZComms.lnk"
  Delete "$SMPROGRAMS\ZComms\Uninstall ZComms.lnk"
  RMDir "$SMPROGRAMS\ZComms"
  Delete "$DESKTOP\ZComms.lnk"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZComms"
  ; %APPDATA%\ZComms (tokens, config) is deliberately left in place: an
  ; uninstall/reinstall cycle should not force a fresh Zoom sign-in.
SectionEnd
