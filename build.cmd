@echo off
rem Builds build\SmartCollapse.exe with the Visual Studio C++ tools (Build Tools are enough).
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if not errorlevel 1 goto build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novs
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

:build
if not exist build mkdir build
rc /nologo /fo build\SmartCollapse.res res\SmartCollapse.rc || exit /b 1
cl /nologo /std:c++17 /permissive- /W4 /O1 /GL /Gy /Gw /MT /EHsc /GR- /DUNICODE /D_UNICODE ^
   /Fobuild\ /Fdbuild\ /Febuild\SmartCollapse.exe src\*.cpp build\SmartCollapse.res ^
   /link /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS /MANIFEST:NO /DELAYLOAD:comctl32.dll ^
   user32.lib gdi32.lib shell32.lib advapi32.lib dwmapi.lib ole32.lib oleaut32.lib uuid.lib comctl32.lib delayimp.lib ^
   || exit /b 1
echo Built %CD%\build\SmartCollapse.exe
exit /b 0

:novs
echo Visual Studio with the C++ build tools was not found.
exit /b 1
