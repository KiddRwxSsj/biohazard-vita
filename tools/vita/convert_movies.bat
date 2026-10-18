@echo off
rem Drag a mod folder (or a downloaded movie pack) onto this file.
rem The Vita-ready copy is written next to it as <folder>_vita.
python "%~dp0convert_movies.py" %*
pause
