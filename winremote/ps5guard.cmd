@echo off
rem ps5guard: temperature watchdog for Windows on the PS5 (runs as SYSTEM from the task "ps5-guard", started at boot).
rem
rem Nothing under Windows lowers the GPU clock when the console gets hot (Linux does that itself), so this is the
rem second line behind the controller's own automatic fan: every 5 s it reads the SoC temperature with ps5therm and
rem   - at FANMAX degrees or more switches the fan to full speed (back to automatic after two minutes below COOL),
rem   - at KILL degrees or more ends every program that has the GPU open (ps5gpu2 clients; the GPU then idles),
rem   - when six readings in a row fail, switches the fan to full speed (it cannot see, so it plays safe),
rem   - gives the screen back to the desktop when a full-screen GPU program has died (ps5gpu2 housekeeping),
rem   - restarts the GPU when it is stuck (ps5gpu2 health, C:\ps5\gpu\gpu-recover.cmd).
rem Usage: ps5guard.cmd [FANMAX [KILL [COOL]]]   (defaults 80 88 65; lower numbers are for testing it)
rem Log: C:\ps5\ps5guard.log (a line at every action and one reading every five minutes).
setlocal
set FANMAX=%1
set KILL=%2
set COOL=%3
if "%FANMAX%"=="" set FANMAX=80
if "%KILL%"=="" set KILL=88
if "%COOL%"=="" set COOL=65
set LOG=C:\ps5\ps5guard.log
set HOT=0
set BAD=0
set CALM=0
set N=0
set STUCK=0
set LASTF=
echo ==== guard start %date% %time% fan-max %FANMAX% kill %KILL% cool %COOL% >> %LOG%

:loop
set T=
for /f "tokens=2" %%t in ('C:\ps5\ps5therm.exe 2^>nul ^| findstr /b MainSoC') do set T=%%t
set TI=
for /f "delims=." %%i in ("%T%") do set TI=%%i
echo %TI%| findstr /r /x "[0-9][0-9]*" >nul || set TI=
if "%TI%"=="" goto noreading
set BAD=0
set /a N+=1
if %N% geq 60 echo %date% %time% MainSoC %T% C, fan forced %HOT% >> %LOG%
if %N% geq 60 set N=0
if %TI% geq %KILL% goto kill
if %TI% geq %FANMAX% goto fanmax
if %TI% lss %COOL% goto calm
set CALM=0
goto wait

:noreading
set /a BAD+=1
if %BAD% lss 6 goto wait
if %HOT%==1 goto wait
echo %date% %time% no temperature reading six times in a row: fan to full speed >> %LOG%
C:\ps5\ps5therm.exe set max >> %LOG% 2>&1
set HOT=1
set CALM=0
goto wait

:kill
echo %date% %time% MainSoC %T% C at or above %KILL%: ending the programs that use the GPU >> %LOG%
for /f %%p in ('C:\ps5\gpu\ps5gpu2.exe clients 2^>nul') do echo   ending pid %%p >> %LOG% & taskkill /f /pid %%p >> %LOG% 2>&1

:fanmax
set CALM=0
if %HOT%==1 goto wait
echo %date% %time% MainSoC %T% C at or above %FANMAX%: fan to full speed >> %LOG%
C:\ps5\ps5therm.exe set max >> %LOG% 2>&1
set HOT=1
goto wait

:calm
if %HOT%==0 goto wait
set /a CALM+=1
if %CALM% lss 24 goto wait
echo %date% %time% MainSoC %T% C, below %COOL% for two minutes: fan back to automatic >> %LOG%
C:\ps5\ps5therm.exe set auto >> %LOG% 2>&1
set HOT=0
set CALM=0

:wait
rem A full-screen program that died leaves the screen on its last picture and its GPU memory taken: put the desktop back
rem and free it (does nothing, quickly, when all is well).
C:\ps5\gpu\ps5gpu2.exe housekeeping >> %LOG% 2>&1
rem A stuck GPU freezes the desktop now that the compositor renders on it. "health" says 2 when the command processor
rem has halted, 3 while work is waiting; waiting with the same fence value four looks in a row (15-20 s) is stuck too.
set HS=
set HF=
for /f "tokens=2,4" %%a in ('C:\ps5\gpu\ps5gpu2.exe health 2^>nul') do set HS=%%a& set HF=%%b
if "%HS%"=="2" goto gpustuck
if not "%HS%"=="3" set STUCK=0
if not "%HS%"=="3" set LASTF=
if not "%HS%"=="3" goto sleep
if "%HF%"=="%LASTF%" set /a STUCK+=1
if not "%HF%"=="%LASTF%" set STUCK=0
set LASTF=%HF%
if %STUCK% geq 3 goto gpustuck
:sleep
ping -n 6 127.0.0.1 > nul
goto loop

:gpustuck
echo %date% %time% GPU stuck (health %HS%, fence %HF%): recovering >> %LOG%
call C:\ps5\gpu\gpu-recover.cmd >> %LOG% 2>&1
set STUCK=0
set LASTF=
goto sleep
