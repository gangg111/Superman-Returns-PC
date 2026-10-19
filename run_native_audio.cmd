@echo off
rem Gra z natywnym silnikiem dzwieku (docs/natywny-dzwiek-plan.md).
rem   run_native_audio.cmd              pelny silnik natywny (full, domyslnie)
rem   run_native_audio.cmd native       natywny miks, oryginal liczy obok
rem   run_native_audio.cmd off          oryginalny silnik (do porownan)
rem   run_native_audio.cmd shadow       oryginal gra, natywny liczy obok i porownuje (log)
rem   run_native_audio.cmd shadow-bank  jak shadow, natywny z banku probek
setlocal
set "ROOT=%~dp0"
set "BUILD=%ROOT%port\out\build\win-amd64-sdksrc"
set "EXE=%BUILD%\superman_returns.exe"
rem Bez bloku if (...): nawiasy w sciezce, np. "(USA)", zamykaly go przedwczesnie.
if not exist "%EXE%" goto :missing
set "SR_NATIVE_AUDIO=full"
if not "%~1"=="" set "SR_NATIVE_AUDIO=%~1"
if not defined SR_NATIVE_AUDIO_DIR set "SR_NATIVE_AUDIO_DIR=%ROOT%..\SupermanReturnsPC\native_audio"
if not exist "%ROOT%logs" mkdir "%ROOT%logs"
start "" /D "%BUILD%" "%EXE%" --game_data_root="%ROOT%game" --log_file="%ROOT%logs\game_native_audio.log" --mnk_mode --mnk_mouse
exit /b 0

:missing
echo Brak "%EXE%". Zbuduj wariant sdksrc.
pause
exit /b 1
