# YtDownloader

Descargador de vídeos y audio de YouTube en C++17, basado en **youtube-dl**.

- Elige **vídeo o audio**, **calidad** y **formato de salida**.
- **No necesita ffmpeg ni youtube-dl instalados**: si no los encuentra, los descarga
  la primera vez en la carpeta `bin/`, junto al ejecutable.
- **Interfaz gráfica estilo Windows 11** (fondo Mica, tema claro/oscuro y color de acento del sistema),
  modo interactivo con menús en consola, o modo línea de comandos para scripts.

## Compilar

Hace falta CMake ≥ 3.16 y un compilador con C++17 (MSVC 2019+, GCC 9+, Clang 10+).

```powershell
cmake -S . -B build
cmake --build build --config Release
```

El ejecutable queda en `build/Release/ytdownloader.exe` (Windows) o en `build/ytdownloader`.
En MSVC se enlaza el runtime estático, así que el `.exe` se puede copiar a otro PC sin instalar nada.

### Generar el .exe para distribuir

```powershell
.\build.bat              # UN SOLO .exe (~85 MB) con youtube-dl y ffmpeg dentro
.\build.bat actualizar   # igual, pero vuelve a descargar youtube-dl/ffmpeg
.\build.bat ligero       # .exe pequeño (~400 KB) que descarga las dependencias al usarse
```

El resultado queda en `dist\ytdownloader.exe` (y en `YtDownloader.zip`). Para distribuirlo basta con
copiar ese único archivo.

Cómo funciona la versión completa:
1. Se compila la versión ligera y se usa para descargar youtube-dl, ffmpeg y ffprobe.
2. Se meten en `build-dist\deps.zip` y CMake lo incrusta en el `.exe` como recurso de Windows
   (`-DYTDL_EMBED_DEPS=ruta\deps.zip`).
3. En el primer uso, el programa extrae el recurso en `%LOCALAPPDATA%\YtDownloader\bin` (una sola vez:
   guarda un sello con el hash y solo vuelve a extraer si cambia el `.exe`).

El script localiza Visual Studio/Build Tools con `vswhere` (y su CMake si no hay otro en el PATH) y
compila en Release x64. El `.exe` solo depende de `KERNEL32.dll` y `SHELL32.dll`.

## Uso

Sin argumentos se abre la interfaz gráfica (en Windows 11 con fondo Mica; en Windows 10 con fondo sólido):

```powershell
.\ytdownloader.exe
```

La ventana recuerda el tipo, la calidad, el formato y la carpeta de destino (en `HKCU\Software\YtDownloader`).
Se puede pegar un enlace con Ctrl+V en cualquier momento y empezar con Intro.

Modo interactivo con menús en la consola:

```powershell
.\ytdownloader.exe -i
```

> El `.exe` es una aplicación de ventana: al usarlo desde la línea de comandos escribe en la consola desde la que
> se lanza, pero PowerShell no espera a que termine. En scripts usa `Start-Process -Wait` o redirige la salida
> (`.\ytdownloader.exe ... | Out-Host`).

Línea de comandos:

```powershell
# Vídeo hasta 1080p en mkv
.\ytdownloader.exe -q 1080 -f mkv "https://www.youtube.com/watch?v=XXXX"

# Audio mp3 a 320 kbps con carátula, en la carpeta "musica"
.\ytdownloader.exe -a -q 320 -f mp3 --thumbnail -o musica "https://www.youtube.com/watch?v=XXXX"

# Ver los formatos disponibles
.\ytdownloader.exe -F "https://www.youtube.com/watch?v=XXXX"
```

| Opción | Descripción |
|---|---|
| `-v`, `--video` / `-a`, `--audio` | Vídeo (por defecto) o solo audio |
| `-q`, `--quality` | Vídeo: `best`, `worst`, `2160`, `1440`, `1080`, `720`, `480`, `360`, `240`, `144`. Audio: `best` o kbps (`320`, `256`, `192`, `160`, `128`, `96`, `64`) |
| `-f`, `--format` | Vídeo: `mp4` (por defecto), `mkv`, `webm`, `avi`, `flv`. Audio: `mp3` (por defecto), `m4a`, `aac`, `opus`, `ogg`, `flac`, `wav` |
| `-o`, `--output` | Carpeta de destino |
| `-t`, `--template` | Plantilla de nombre de youtube-dl (por defecto `%(title)s.%(ext)s`) |
| `-p`, `--playlist` | Descargar la lista entera si la URL lleva `list=` |
| `-F`, `--list-formats` | Listar formatos y salir |
| `--code` | Selector `-f` manual de youtube-dl (avanzado) |
| `--thumbnail` | Incrustar miniatura (solo mp3) |
| `--no-metadata` | No escribir metadatos |
| `--ytdl RUTA` | Usar otro ejecutable de youtube-dl |
| `--update` | Volver a descargar youtube-dl y ffmpeg |
| `-y`, `--yes` | Instalar dependencias sin preguntar |

## Cómo funciona

1. **Dependencias** (`src/deps.cpp`): busca youtube-dl y ffmpeg/ffprobe primero en `bin/` y luego en el `PATH`.
   Si faltan, pide permiso y los descarga:
   - youtube-dl: compilación nightly oficial (`ytdl-org/ytdl-nightly`). La versión "estable" de 2021 ya no funciona con YouTube.
   - ffmpeg: Windows → build *essentials* de gyan.dev; Linux → BtbN; macOS → evermeet.cx.
   - Para descargar y descomprimir usa `curl` y `tar`, que vienen con Windows 10/11 (con PowerShell como alternativa).
2. **Opciones** (`src/options.cpp`): traduce lo que eliges a argumentos de youtube-dl:
   - Vídeo: `bestvideo[height<=H]+bestaudio` (para mp4 prefiere H.264+AAC y para webm, VP9+Opus), unidos con
     `--merge-output-format`. Para `avi` o si la fuente no coincide, `--recode-video` convierte (solo cuando hace falta).
   - Audio: `--extract-audio --audio-format F --audio-quality Q`.
   - Si no hay ffmpeg (porque rechazas la descarga), se limita a los formatos que traen audio y vídeo juntos, sin convertir, y te avisa.
3. **Interfaz** (`src/gui.cpp`): Win32 + Direct2D/DirectWrite, sin dependencias externas. Mica con
   `DwmSetWindowAttribute(DWMWA_SYSTEMBACKDROP_TYPE)`, barra de título integrada y controles Fluent dibujados a mano.
   youtube-dl se lanza con `--newline` y se lee su salida para mostrar el progreso; cancelar cierra todo el árbol
   de procesos (un *job object*).
4. **Procesos** (`src/process.cpp`): lanza youtube-dl sin pasar por una shell (`CreateProcessW` / `fork`+`execvp`), así que
   las URLs y rutas con espacios, `&` o tildes funcionan bien. La consola de Windows se pone en UTF-8.

## Notas

- En Linux/macOS youtube-dl necesita **python3**.
- La primera ejecución descarga unos 130 MB (ffmpeg es casi todo).
- Descarga solo contenido que tengas derecho a guardar.
