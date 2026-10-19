@echo off
rem Gra z natywnym silnikiem dzwieku (docs/natywny-dzwiek-plan.md).
rem   run_native_audio.cmd              dzwiek z silnika natywnego (bank probek)
rem   run_native_audio.cmd shadow       oryginal gra, natywny liczy obok i porownuje (log)
rem   run_native_audio.cmd shadow-bank  jak shadow, natywny z banku probek
setlocal
set "ROOT=%~dp0"
set "BUILD=%ROOT%port\out\build\win-amd64-sdksrc"
set "EXE=%BUILD%\superman_returns.exe"
if not exist "%EXE%" (
  echo Brak %EXE%. Zbuduj wariant sdksrc.
  exit /b 1
)
set "SR_NATIVE_AUDIO=native"
if not "%~1"=="" set "SR_NATIVE_AUDIO=%~1"
if not defined SR_NATIVE_AUDIO_DIR set "SR_NATIVE_AUDIO_DIR=%ROOT%..\SupermanReturnsPC\native_audio"
if not exist "%ROOT%logs" mkdir "%ROOT%logs"
start "" /D "%BUILD%" "%EXE%" --game_data_root="%ROOT%game" --log_file="%ROOT%logs\game_native_audio.log"
