@echo off
rem ===========================================================================
rem  Genera un ytdownloader.exe listo para distribuir en la carpeta dist\
rem
rem  Uso:
rem    build.bat              -> UN SOLO .exe con youtube-dl y ffmpeg dentro
rem    build.bat actualizar   -> igual, pero vuelve a descargar youtube-dl/ffmpeg
rem    build.bat ligero       -> .exe pequeno; descarga youtube-dl/ffmpeg al usarse
rem ===========================================================================
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

cd /d "%~dp0"
set "OUT=build-dist"
set "DIST_DIR=dist"
set "ZIP_NAME=YtDownloader.zip"
set "MODE=completo"
set "REFRESH=0"
if /i "%~1"=="ligero"     set "MODE=ligero"
if /i "%~1"=="actualizar" set "REFRESH=1"

rem --- 1. Localizar Visual Studio / Build Tools con vswhere ------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] No se encuentra Visual Studio ni Build Tools.
    echo         Instala "Build Tools para Visual Studio" con la carga
    echo         "Desarrollo para el escritorio con C++":
    echo         https://visualstudio.microsoft.com/es/downloads/
    exit /b 1
)

set "VS_PATH="
set "VS_VER="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion`) do set "VS_VER=%%i"
if not defined VS_PATH (
    echo [ERROR] Visual Studio esta instalado pero sin las herramientas de C++.
    exit /b 1
)

for /f "tokens=1 delims=." %%v in ("%VS_VER%") do set "VS_MAJOR=%%v"
if "%VS_MAJOR%"=="16" set "GENERATOR=Visual Studio 16 2019"
if "%VS_MAJOR%"=="17" set "GENERATOR=Visual Studio 17 2022"
if "%VS_MAJOR%"=="18" set "GENERATOR=Visual Studio 18 2026"
if not defined GENERATOR (
    echo [ERROR] Version de Visual Studio no soportada: %VS_VER%
    exit /b 1
)
echo Usando: %GENERATOR%  ^(%VS_PATH%^)

rem --- 2. Localizar CMake (PATH o el que trae Visual Studio) -----------------
set "CMAKE=cmake"
where cmake >nul 2>&1
if errorlevel 1 (
    set "CMAKE=%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if not exist "!CMAKE!" (
        echo [ERROR] No se encuentra CMake. Instalalo desde https://cmake.org/download/
        exit /b 1
    )
)

rem --- 3. Compilar la version ligera (tambien sirve para bajar las dependencias)
echo.
echo === Compilando version ligera ===
"%CMAKE%" -S . -B "%OUT%\ligero" -G "%GENERATOR%" -A x64 -DYTDL_EMBED_DEPS=
if errorlevel 1 goto :fail
"%CMAKE%" --build "%OUT%\ligero" --config Release --parallel
if errorlevel 1 goto :fail
set "LIGHT_EXE=%OUT%\ligero\Release\ytdownloader.exe"
set "FINAL_EXE=%LIGHT_EXE%"

if /i "%MODE%"=="ligero" goto :package

rem --- 4. Conseguir youtube-dl, ffmpeg y ffprobe y meterlos en un zip --------
set "DEPS_DIR=%OUT%\ligero\Release\bin"
set "DEPS_ZIP=%CD%\%OUT%\deps.zip"
set "NEED=%REFRESH%"
if not exist "%DEPS_DIR%\youtube-dl.exe" set "NEED=1"
if not exist "%DEPS_DIR%\ffmpeg.exe"     set "NEED=1"
if not exist "%DEPS_DIR%\ffprobe.exe"    set "NEED=1"
if "%NEED%"=="1" (
    echo.
    echo === Descargando youtube-dl y ffmpeg ===
    "%LIGHT_EXE%" --update --yes
    if errorlevel 1 goto :fail
) else (
    echo.
    echo Reutilizando youtube-dl y ffmpeg ya descargados ^(usa "build.bat actualizar" para renovarlos^).
)

if exist "%DEPS_ZIP%" del /q "%DEPS_ZIP%"
"%SystemRoot%\System32\tar.exe" -a -c -f "%DEPS_ZIP%" -C "%DEPS_DIR%" youtube-dl.exe ffmpeg.exe ffprobe.exe
if errorlevel 1 goto :fail

rem --- 5. Compilar la version completa con el zip incrustado ----------------
echo.
echo === Compilando version completa (con ffmpeg dentro del .exe) ===
"%CMAKE%" -S . -B "%OUT%\completo" -G "%GENERATOR%" -A x64 "-DYTDL_EMBED_DEPS=%DEPS_ZIP%"
if errorlevel 1 goto :fail
"%CMAKE%" --build "%OUT%\completo" --config Release --parallel --clean-first
if errorlevel 1 goto :fail
set "FINAL_EXE=%OUT%\completo\Release\ytdownloader.exe"

rem --- 6. Empaquetar ---------------------------------------------------------
:package
echo.
echo === Empaquetando ===
if exist "%DIST_DIR%" rmdir /s /q "%DIST_DIR%"
mkdir "%DIST_DIR%"
copy /y "%FINAL_EXE%" "%DIST_DIR%\" >nul || goto :fail
copy /y "README.md" "%DIST_DIR%\" >nul

if exist "%ZIP_NAME%" del /q "%ZIP_NAME%"
"%SystemRoot%\System32\tar.exe" -a -c -f "%ZIP_NAME%" -C "%DIST_DIR%" ytdownloader.exe README.md
if errorlevel 1 echo [AVISO] No se pudo crear %ZIP_NAME%; la carpeta %DIST_DIR% sigue disponible.

for %%F in ("%DIST_DIR%\ytdownloader.exe") do set "SIZE=%%~zF"
set /a SIZE_MB=%SIZE% / 1048576

echo.
echo ===========================================================
echo  Listo: %CD%\%DIST_DIR%\ytdownloader.exe  ^(%SIZE_MB% MB^)
if exist "%ZIP_NAME%" echo         %CD%\%ZIP_NAME%
if /i "%MODE%"=="ligero" (
    echo  youtube-dl y ffmpeg se descargaran en el primer uso.
) else (
    echo  youtube-dl y ffmpeg van DENTRO del .exe: basta con copiar ese archivo.
    echo  En el primer uso se extraen en %%LOCALAPPDATA%%\YtDownloader\bin
)
echo ===========================================================
exit /b 0

:fail
echo.
echo [ERROR] La compilacion ha fallado.
exit /b 1
