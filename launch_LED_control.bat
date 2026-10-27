@echo off
REM Template for a Windows launcher for the LED control GUI
REM
REM Uses a conda environment (created from environment.yml, name: sfrmatrix-led).
REM Adapt the path below if conda (e.g. Miniforge) is installed elsewhere than
REM the default location. 
REM Adapt the conda environment name (sfrmatrix-led) if you chose another one.

REM Run from the directory of this batch file, wherever it is started from
cd /d "%~dp0"

call "%USERPROFILE%\miniforge3\Scripts\activate.bat" sfrmatrix-led
python LED_control_gui.py
pause
