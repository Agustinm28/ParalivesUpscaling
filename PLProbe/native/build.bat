@echo off
rem Builds PLProbe.dll (x64). Needs the Unity plugin headers in .\Unity\
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /LD /O2 /EHsc /W3 /I. PLProbe.cpp /Fe:PLProbe.dll || exit /b 1
