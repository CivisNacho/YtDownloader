#include "options.h"

#include <algorithm>
#include <cctype>

namespace {

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

bool isNumber(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

}  // namespace

const std::vector<std::string>& videoQualities() {
    static const std::vector<std::string> v = {"best", "2160", "1440", "1080", "720", "480", "360", "240", "144", "worst"};
    return v;
}

const std::vector<std::string>& audioQualities() {
    static const std::vector<std::string> v = {"best", "320", "256", "192", "160", "128", "96", "64"};
    return v;
}

// Formatos que youtube-dl sabe generar con --merge-output-format / --recode-video (más "original").
const std::vector<std::string>& videoFormats() {
    static const std::vector<std::string> v = {"original", "mp4", "mkv", "webm", "avi", "flv"};
    return v;
}

const std::vector<std::string>& audioFormats() {
    static const std::vector<std::string> v = {"original", "mp3", "m4a", "aac", "opus", "ogg", "flac", "wav"};
    return v;
}

std::string defaultFormat(Mode mode) { return mode == Mode::Video ? "mp4" : "mp3"; }

std::string validate(const DownloadOptions& o) {
    if (o.urls.empty()) return "No se ha indicado ninguna URL.";
    const auto& fmts = o.mode == Mode::Video ? videoFormats() : audioFormats();
    if (!contains(fmts, o.format)) {
        std::string msg = "Formato '" + o.format + "' no válido para " +
                          (o.mode == Mode::Video ? "vídeo" : "audio") + ". Opciones:";
        for (const auto& f : fmts) msg += " " + f;
        return msg;
    }
    if (o.formatCode.empty()) {
        if (o.mode == Mode::Video && !(o.quality == "best" || o.quality == "worst" || isNumber(o.quality)))
            return "Calidad de vídeo no válida: '" + o.quality + "'. Usa best, worst o una altura (1080, 720...).";
        if (o.mode == Mode::Audio && !(o.quality == "best" || isNumber(o.quality)))
            return "Calidad de audio no válida: '" + o.quality + "'. Usa best o un bitrate en kbps (320, 192...).";
    }
    return {};
}

std::vector<std::string> buildYtdlArgs(const DownloadOptions& o, const std::string& ffmpegDir, bool haveFfmpeg,
                                       std::vector<std::string>& warnings) {
    std::vector<std::string> a;

    if (!ffmpegDir.empty()) {
        a.push_back("--ffmpeg-location");
        a.push_back(ffmpegDir);
    }

    a.push_back(o.playlist ? "--yes-playlist" : "--no-playlist");

    if (o.listFormats) {
        a.push_back("--list-formats");
        return a;
    }

    std::string out = o.outputDir;
    if (!out.empty() && out.back() != '/' && out.back() != '\\') out += '/';
    a.push_back("-o");
    a.push_back(out + (o.playlist ? "%(playlist_title)s/%(playlist_index)03d - " : "") + o.outputTemplate);

    if (o.mode == Mode::Video) {
        std::string sel = o.formatCode;
        if (sel.empty()) {
            if (o.quality == "worst") {
                sel = haveFfmpeg ? "worstvideo+worstaudio/worst" : "worst";
            } else {
                std::string h = o.quality == "best" ? "" : "[height<=" + o.quality + "]";
                if (!haveFfmpeg) {
                    // Sin ffmpeg solo podemos bajar formatos "progresivos" (audio+vídeo en un fichero),
                    // que en YouTube suelen limitarse a 360p.
                    sel = (o.format == "original") ? ("best" + h + "/best")
                                                   : ("best" + h + "[ext=" + o.format + "]/best" + h + "/best");
                } else if (o.format == "mp4") {
                    // Preferimos H.264 + AAC para máxima compatibilidad con mp4.
                    sel = "bestvideo" + h + "[ext=mp4]+bestaudio[ext=m4a]/bestvideo" + h + "+bestaudio/best" + h +
                          "/best";
                } else if (o.format == "webm") {
                    sel = "bestvideo" + h + "[ext=webm]+bestaudio[ext=webm]/bestvideo" + h + "+bestaudio/best" + h +
                          "/best";
                } else {
                    sel = "bestvideo" + h + "+bestaudio/best" + h + "/best";
                }
            }
        }
        a.push_back("-f");
        a.push_back(sel);

        if (haveFfmpeg) {
            if (o.format != "original") {
                static const std::vector<std::string> mergeable = {"mkv", "mp4", "webm", "flv"};
                a.push_back("--merge-output-format");
                a.push_back(contains(mergeable, o.format) ? o.format : "mkv");
                // youtube-dl no recodifica si el fichero ya está en el formato pedido,
                // así que esto solo actúa cuando hace falta (p.ej. avi o un fallback).
                a.push_back("--recode-video");
                a.push_back(o.format);
            }
            if (o.addMetadata) a.push_back("--add-metadata");
        } else {
            warnings.push_back(o.format == "original"
                                   ? "Sin ffmpeg: la calidad queda limitada a los formatos con audio y vídeo juntos "
                                     "(normalmente 360p)."
                                   : "Sin ffmpeg: la calidad queda limitada a los formatos con audio y vídeo juntos "
                                     "(normalmente 360p) y no se convertirá a '" +
                                         o.format + "'.");
        }
    } else {  // Audio
        a.push_back("-f");
        if (!o.formatCode.empty()) {
            a.push_back(o.formatCode);
        } else if (haveFfmpeg || o.format == "original") {
            a.push_back("bestaudio/best");
        } else {
            a.push_back("bestaudio[ext=m4a]/bestaudio/best");
        }

        if (o.format == "original") {
            if (haveFfmpeg && o.addMetadata) a.push_back("--add-metadata");
        } else if (haveFfmpeg) {
            a.push_back("--extract-audio");
            a.push_back("--audio-format");
            a.push_back(o.format == "ogg" ? "vorbis" : o.format);
            a.push_back("--audio-quality");
            // 0 = mejor calidad VBR; si no, bitrate fijo (p.ej. 192K).
            a.push_back(o.quality == "best" ? "0" : o.quality + "K");
            if (o.format == "flac" || o.format == "wav") {
                if (o.quality != "best")
                    warnings.push_back("El bitrate se ignora en formatos sin pérdida (" + o.format + ").");
            }
            if (o.addMetadata) a.push_back("--add-metadata");
            if (o.embedThumbnail) {
                if (o.format == "mp3")
                    a.push_back("--embed-thumbnail");
                else
                    warnings.push_back("La miniatura solo se incrusta en mp3 (otros formatos requieren AtomicParsley).");
            }
        } else {
            warnings.push_back("Sin ffmpeg: se guardará el audio original (m4a/webm) sin convertir a '" + o.format +
                               "'.");
        }
    }
    return a;
}
