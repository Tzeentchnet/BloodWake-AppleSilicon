@echo off
rem Play Blood Wake (the port).
rem With no options: the launcher opens (settings and a Play button; tools\launcher.pyw).
rem With options it plays straight away, as before:
rem   play.cmd --edition remastered    play.cmd --keyboard    play.cmd --cpu
cd /d "%~dp0"
if "%~1"=="" (
  start "" pyw -3 tools\launcher.pyw
) else (
  py -3 tools\play.py %*
)
