@echo off
rem Builds ParalivesUpscaling.dll (x64) into .\out together with the FidelityFX, XeSS and DLSS runtime DLLs.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /LD /O2 /EHsc /W3 /std:c++17 /I. ParalivesUpscaling.cpp /Fo:out\ /Fe:out\ParalivesUpscaling.dll || exit /b 1
copy /y ffx\bin\amd_fidelityfx_loader_dx12.dll out\ >nul
copy /y ffx\bin\amd_fidelityfx_upscaler_dx12.dll out\ >nul
copy /y xess\bin\libxess.dll out\ >nul
copy /y dlss\bin\nvngx_dlss.dll out\ >nul
