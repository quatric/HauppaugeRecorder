; HauppaugeRecorder for Windows: hdpvr-capture and the HD PVR driver.
;   makensis -DARCH=x86_64|x86|aarch64 -DVERSION=0.1.0 -DSRC=dist\x86_64-windows-gnu \
;            -DOUTFILE=HauppaugeRecorder-x86_64-windows-setup.exe installer/windows.nsi
; The driver is installed by hdpvr-driver.exe (src/driver_win32.c): WinUSB, or
; libusbK on a Windows that has no winusb.inf.

Unicode true
!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"

!ifndef ARCH | VERSION | SRC | OUTFILE
  !error "define ARCH, VERSION, SRC and OUTFILE"
!endif

!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\HauppaugeRecorder"

Name "HauppaugeRecorder"
Caption "HauppaugeRecorder ${VERSION} Setup"
OutFile "${OUTFILE}"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
!if "${ARCH}" == "x86"
  InstallDir "$PROGRAMFILES\HauppaugeRecorder"
!else
  InstallDir "$PROGRAMFILES64\HauppaugeRecorder"
!endif
InstallDirRegKey HKLM "Software\HauppaugeRecorder" "InstallDir"
BrandingText "HauppaugeRecorder ${VERSION}"

; the icon from Hauppauge Capture, also built into hdpvr-capture.exe and hdpvr-driver.exe
!define MUI_ICON "${__FILEDIR__}/../resources/windows/app.ico"
!define MUI_UNICON "${__FILEDIR__}/../resources/windows/app.ico"
!define MUI_COMPONENTSPAGE_SMALLDESC
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
!if "${ARCH}" == "x86_64"
  ${IfNot} ${IsNativeAMD64}
    MessageBox MB_ICONSTOP "This installer is for 64-bit Intel/AMD Windows.$\nUse the x86 installer on 32-bit Windows, or the aarch64 one on Windows on Arm."
    Abort
  ${EndIf}
  SetRegView 64
!else if "${ARCH}" == "aarch64"
  ${IfNot} ${IsNativeARM64}
    MessageBox MB_ICONSTOP "This installer is for Windows on Arm. Use the x86_64 installer on Intel/AMD PCs."
    Abort
  ${EndIf}
  SetRegView 64
!else
  ${If} ${RunningX64}
    MessageBox MB_ICONSTOP "This is the 32-bit installer, which cannot install a driver on 64-bit Windows.$\nUse the x86_64 installer (or aarch64 on Windows on Arm)."
    Abort
  ${EndIf}
!endif
FunctionEnd

Function un.onInit
!if "${ARCH}" != "x86"
  SetRegView 64
!endif
FunctionEnd

Section "HauppaugeRecorder" SecApp
  SectionIn RO
  SetOutPath "$INSTDIR"
  File /r "${SRC}\*.*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  ; hdpvr-capture is a command line tool: the shortcut opens a prompt in its folder
  CreateShortcut "$SMPROGRAMS\HD PVR Recorder.lnk" "$SYSDIR\cmd.exe" '/k "cd /d "$INSTDIR" && hdpvr-capture --help"' "$INSTDIR\hdpvr-capture.exe" 0
  WriteRegStr HKLM "Software\HauppaugeRecorder" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "HauppaugeRecorder"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "HauppaugeRecorder"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\hdpvr-capture.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
SectionEnd

Section "HD PVR driver" SecDriver
  DetailPrint "Installing the HD PVR driver..."
  nsExec::ExecToLog '"$INSTDIR\hdpvr-driver.exe" install --quiet'
  Pop $0
  ${If} $0 == 0
    DetailPrint "The HD PVR driver is installed."
  ${ElseIf} $0 == 2
    DetailPrint "The driver is ready; it will be used when the HD PVR is plugged in."
  ${Else}
    MessageBox MB_ICONEXCLAMATION "The HD PVR driver could not be installed (code $0).$\nRun hdpvr-driver.exe from $INSTDIR to try again."
  ${EndIf}
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecApp} "The hdpvr-capture command line recorder, with a Start menu shortcut that opens a prompt in its folder."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDriver} "Lets hdpvr-capture use the HD PVR: WinUSB (or libusbK where Windows has no WinUSB setup file), installed as Zadig would. This replaces Hauppauge's own driver for the HD PVR."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  MessageBox MB_YESNO|MB_ICONQUESTION "Also remove the HD PVR driver?$\n(Windows then goes back to the driver it had before.)" IDNO keep_driver
    nsExec::ExecToLog '"$INSTDIR\hdpvr-driver.exe" uninstall --quiet'
    Pop $0
  keep_driver:
  Delete "$INSTDIR\hdpvr-capture.exe"
  Delete "$INSTDIR\hdpvr-driver.exe"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\libusbK.dll"
  Delete "$INSTDIR\driver\libusbK\libusbK.sys"
  Delete "$INSTDIR\driver\libusbK\WdfCoInstaller01009.dll"
  RMDir "$INSTDIR\driver\libusbK"
  RMDir "$INSTDIR\driver"
  Delete "$INSTDIR\licenses\*.txt"
  RMDir "$INSTDIR\licenses"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\HD PVR Recorder.lnk"
  DeleteRegKey HKLM "${UNINST_KEY}"
  DeleteRegValue HKLM "Software\HauppaugeRecorder" "InstallDir" ; the driver record stays if the driver was kept
SectionEnd
