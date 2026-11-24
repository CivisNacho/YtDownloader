// Opciones de descarga y traducción a argumentos de youtube-dl.
#pragma once

#include <string>
#include <vector>

enum class Mode { Video, Audio };

struct DownloadOptions {
    std::vector<std::string> urls;
    Mode mode = Mode::Video;
    // Vídeo: "best", "worst" o altura máxima ("1080", "720"...).
    // Audio: "best" o bitrate en kbps ("320", "192"...).
    std::string quality = "best";
    // Vídeo: mp4, mkv, webm, avi, flv, mov.  Audio: mp3, m4a, aac, opus, ogg, flac, wav.
    std::string format;
    std::string outputDir = ".";
    std::string outputTemplate = "%(title)s.%(ext)s";
    std::string formatCode;  // -f manual (avanzado); ignora quality
    bool playlist = false;
    bool listFormats = false;
    bool embedThumbnail = false;
    bool addMetadata = true;
};

// Valores admitidos (para validar y para los menús interactivos).
const std::vector<std::string>& videoQualities();
const std::vector<std::string>& audioQualities();
const std::vector<std::string>& videoFormats();
const std::vector<std::string>& audioFormats();

std::string defaultFormat(Mode mode);

// Devuelve un mensaje de error vacío si las opciones son válidas.
std::string validate(const DownloadOptions& o);

// Construye los argumentos de youtube-dl (sin el ejecutable ni las URLs).
// `ffmpegDir` vacío significa que ffmpeg está en el PATH; `haveFfmpeg` false
// degrada a formatos que no necesitan post-procesado.
std::vector<std::string> buildYtdlArgs(const DownloadOptions& o, const std::string& ffmpegDir, bool haveFfmpeg,
                                       std::vector<std::string>& warnings);
