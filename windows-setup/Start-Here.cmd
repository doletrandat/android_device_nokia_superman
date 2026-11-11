@echo off
title Lumia 730 Android setup
rem Double-click this file. It starts the guided setup and asks Windows for Administrator rights.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Superman-Setup.ps1"
if errorlevel 1 pause
