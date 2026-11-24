// Localización y descarga automática de youtube-dl y ffmpeg.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct Tools {
    // Prefijo de comando para lanzar youtube-dl (p.ej. {"python3", ".../youtube-dl"} en Linux).
    std::vector<std::string> ytdl;
    // Directorio con ffmpeg/ffprobe para pasar a --ffmpeg-location. Vacío = está en el PATH.
    fs::path ffmpegDir;
    bool haveFfmpeg = false;
};

struct DepsOptions {
    fs::path binDir;          // Carpeta local donde se guardan las dependencias descargadas
    std::string ytdlOverride; // Ruta indicada por el usuario con --ytdl
    bool forceUpdate = false; // Re-descargar aunque ya existan
    bool autoInstall = true;  // Descargar sin preguntar
    // Opcionales (para la interfaz gráfica). Si están vacíos se usa la consola.
    std::function<void(const std::string&)> log;  // Mensajes de estado
    std::function<bool(const std::string&)> ask;  // Confirmación sí/no
};

// true si este ejecutable lleva youtube-dl y ffmpeg incrustados (build "completa").
bool hasEmbeddedDeps();

// Carpeta por defecto para las dependencias: junto al .exe, o en
// %LOCALAPPDATA%\YtDownloader\bin si van incrustadas (el .exe puede estar en
// una carpeta sin permisos de escritura, como Program Files).
fs::path defaultBinDir();

// Garantiza que youtube-dl y ffmpeg estén disponibles, descargándolos si hace falta.
// Lanza std::runtime_error si youtube-dl no puede conseguirse. Si ffmpeg falla,
// devuelve haveFfmpeg = false y el llamador decide qué hacer.
Tools ensureTools(const DepsOptions& opts);
