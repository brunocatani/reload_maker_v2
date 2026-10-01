@echo off
"%~dp0runtime\python\python.exe" -B -I "%~dp0tools\product_cli.py" %*
exit /b %ERRORLEVEL%
