@echo off
rem Paralives Upscaling: install, update or remove the mod (see setup.ps1).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1" %*
