#include "deps.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "process.h"

#ifdef YTDL_EMBEDDED
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

#ifdef _WIN32
const char* kExe = ".exe";
const char* kYtdlUrl = "https://github.com/ytdl-org/ytdl-nightly/releases/latest/download/youtube-dl.exe";
const char* kFfmpegUrl = "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip";
const char* kFfmpegArchive = "ffmpeg.zip";
#else
const char* kExe = "";
const char* kYtdlUrl = "https://github.com/ytdl-org/ytdl-nightly/releases/latest/download/youtube-dl";
#if defined(__APPLE__)
const char* kFfmpegUrl = "https://evermeet.cx/ffmpeg/getrelease/zip";
const char* kFfprobeUrl = "https://evermeet.cx/ffmpeg/getrelease/ffprobe/zip";
const char* kFfmpegArchive = "ffmpeg.zip";
#elif defined(__aarch64__)
const char* kFfmpegUrl = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-linuxarm64-gpl.tar.xz";
const char* kFfmpegArchive = "ffmpeg.tar.xz";
#else
const char* kFfmpegUrl = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-linux64-gpl.tar.xz";
const char* kFfmpegArchive = "ffmpeg.tar.xz";
#endif
#endif

std::string exeName(const std::string& base) { return base + kExe; }

// Opciones de la llamada en curso a ensureTools() (para log/confirmación).
const DepsOptions* g_opts = nullptr;

void say(const std::string& msg) {
    if (g_opts && g_opts->log) g_opts->log(msg);
    else std::cout << msg << "\n";
}

void warn(const std::string& msg) {
    if (g_opts && g_opts->log) g_opts->log(msg);
    else std::cerr << msg << "\n";
}

#ifdef _WIN32
// Usamos las herramientas que trae Windows 10/11 (curl.exe y tar.exe = bsdtar,
// que sabe descomprimir .zip) por ruta absoluta para no coger otras versiones del PATH.
fs::path system32Tool(const char* name) {
    const char* root = std::getenv("SystemRoot");
    fs::path p = fs::path(root ? root : "C:\\Windows") / "System32" / name;
    return p;
}
#endif

bool confirm(const std::string& question, bool autoYes) {
    if (autoYes) return true;
    if (g_opts && g_opts->ask) return g_opts->ask(question);
    std::cout << question << " [S/n]: " << std::flush;
    std::string line;
    if (!proc::readLine(line)) return false;
    return line.empty() || line[0] == 's' || line[0] == 'S' || line[0] == 'y' || line[0] == 'Y';
}

// Descarga `url` en `dest` (vía fichero temporal + rename para no dejar ficheros a medias).
bool download(const std::string& url, const fs::path& dest) {
    fs::path tmp = dest;
    tmp += ".part";
    std::error_code ec;
    fs::remove(tmp, ec);
    say("  -> " + url);

    int rc = -1;
#ifdef _WIN32
    fs::path curl = system32Tool("curl.exe");
    if (fs::exists(curl, ec)) {
        rc = proc::run({proc::toUtf8(curl), "-L", "--fail", "--progress-bar", "-o", proc::toUtf8(tmp), url});
    } else {
        // Windows antiguo sin curl: recurrimos a PowerShell.
        std::string ps = "$ProgressPreference='SilentlyContinue'; Invoke-WebRequest -UseBasicParsing -Uri '" + url +
                         "' -OutFile '" + proc::toUtf8(tmp) + "'";
        rc = proc::run({"powershell", "-NoProfile", "-Command", ps});
    }
#else
    if (proc::findInPath("curl")) {
        rc = proc::run({"curl", "-L", "--fail", "--progress-bar", "-o", tmp.string(), url});
    } else if (proc::findInPath("wget")) {
        rc = proc::run({"wget", "-q", "--show-progress", "-O", tmp.string(), url});
    } else {
        warn("Error: se necesita 'curl' o 'wget' para descargar dependencias.");
        return false;
    }
#endif
    if (rc != 0 || !fs::exists(tmp, ec) || fs::file_size(tmp, ec) == 0) {
        fs::remove(tmp, ec);
        return false;
    }
    fs::remove(dest, ec);
    fs::rename(tmp, dest, ec);
    return !ec;
}

bool extract(const fs::path& archive, const fs::path& outDir) {
    std::error_code ec;
    fs::create_directories(outDir, ec);
#ifdef _WIN32
    fs::path tar = system32Tool("tar.exe");
    if (fs::exists(tar, ec))
        return proc::run({proc::toUtf8(tar), "-xf", proc::toUtf8(archive), "-C", proc::toUtf8(outDir)}) == 0;
    std::string ps = "Expand-Archive -Force -LiteralPath '" + proc::toUtf8(archive) + "' -DestinationPath '" +
                     proc::toUtf8(outDir) + "'";
    return proc::run({"powershell", "-NoProfile", "-Command", ps}) == 0;
#elif defined(__APPLE__)
    return proc::run({"tar", "-xf", archive.string(), "-C", outDir.string()}) == 0;
#else
    if (archive.extension() == ".zip") return proc::run({"unzip", "-oq", archive.string(), "-d", outDir.string()}) == 0;
    return proc::run({"tar", "-xJf", archive.string(), "-C", outDir.string()}) == 0;
#endif
}

// Busca recursivamente un fichero por nombre y lo copia a `destDir`.
bool pickFile(const fs::path& searchRoot, const std::string& name, const fs::path& destDir) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(searchRoot, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().filename() == name) {
            fs::path dest = destDir / name;
            fs::copy_file(it->path(), dest, fs::copy_options::overwrite_existing, ec);
            if (ec) return false;
#ifndef _WIN32
            fs::permissions(dest, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                            fs::perm_options::add, ec);
#endif
            return true;
        }
    }
    return false;
}

std::vector<std::string> ytdlCommand(const fs::path& script) {
#ifdef _WIN32
    return {proc::toUtf8(script)};
#else
    // En Linux/macOS youtube-dl es un zipapp de Python: lo lanzamos explícitamente con python3.
    if (proc::findInPath("python3")) return {"python3", script.string()};
    if (proc::findInPath("python")) return {"python", script.string()};
    return {script.string()};
#endif
}

bool ytdlWorks(const std::vector<std::string>& cmd) {
    auto args = cmd;
    args.push_back("--version");
    return proc::runQuiet(args) == 0;
}

bool ffmpegWorks(const fs::path& ffmpeg, const fs::path& ffprobe) {
    return proc::runQuiet({proc::toUtf8(ffmpeg), "-version"}) == 0 &&
           proc::runQuiet({proc::toUtf8(ffprobe), "-version"}) == 0;
}

bool installFfmpeg(const fs::path& binDir) {
    std::error_code ec;
    fs::path archive = binDir / kFfmpegArchive;
    fs::path tmpDir = binDir / "_ffmpeg_tmp";
    fs::remove_all(tmpDir, ec);

    bool ok = download(kFfmpegUrl, archive) && extract(archive, tmpDir);
#if defined(__APPLE__)
    // evermeet.cx distribuye ffprobe en un zip aparte.
    if (ok) {
        fs::path probeZip = binDir / "ffprobe.zip";
        ok = download(kFfprobeUrl, probeZip) && extract(probeZip, tmpDir);
        fs::remove(probeZip, ec);
    }
#endif
    if (ok) ok = pickFile(tmpDir, exeName("ffmpeg"), binDir) && pickFile(tmpDir, exeName("ffprobe"), binDir);

    fs::remove(archive, ec);
    fs::remove_all(tmpDir, ec);
    return ok;
}

#ifdef YTDL_EMBEDDED
// El .zip con youtube-dl.exe, ffmpeg.exe y ffprobe.exe va como recurso RCDATA del .exe
// (ver CMakeLists.txt). Se extrae una sola vez; un "sello" con el hash del recurso
// permite detectar que el .exe se ha actualizado y hay que volver a extraer.
struct Blob {
    const unsigned char* data = nullptr;
    size_t size = 0;
};

Blob embeddedBlob() {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(YTDL_DEPS_RESOURCE_ID), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
    if (!res) return {};
    HGLOBAL h = LoadResource(nullptr, res);
    if (!h) return {};
    return {static_cast<const unsigned char*>(LockResource(h)), SizeofResource(nullptr, res)};
}

std::string blobStamp(const Blob& b) {
    uint64_t hash = 1469598103934665603ULL;  // FNV-1a 64
    for (size_t i = 0; i < b.size; ++i) {
        hash ^= b.data[i];
        hash *= 1099511628211ULL;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%016llx-%zu", static_cast<unsigned long long>(hash), b.size);
    return buf;
}

void extractEmbedded(const fs::path& binDir) {
    Blob blob = embeddedBlob();
    if (!blob.data || blob.size == 0) return;

    std::error_code ec;
    const fs::path stampFile = binDir / "embedded.stamp";
    const std::string stamp = blobStamp(blob);
    bool filesPresent = fs::exists(binDir / "youtube-dl.exe", ec) && fs::exists(binDir / "ffmpeg.exe", ec) &&
                        fs::exists(binDir / "ffprobe.exe", ec);
    if (filesPresent) {
        std::ifstream in(stampFile);
        std::string current;
        if (std::getline(in, current) && current == stamp) return;  // ya extraído
    }

    say("Preparando youtube-dl y ffmpeg (solo la primera vez)...");
    fs::path zip = binDir / "_embedded.zip";
    {
        std::ofstream out(zip, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(blob.data), static_cast<std::streamsize>(blob.size));
        if (!out) {
            warn("Aviso: no se pudieron extraer los componentes incluidos.");
            fs::remove(zip, ec);
            return;
        }
    }
    bool ok = extract(zip, binDir);
    fs::remove(zip, ec);
    if (ok) {
        std::ofstream(stampFile, std::ios::trunc) << stamp << "\n";
    } else {
        warn("Aviso: no se pudieron extraer los componentes incluidos; se intentará descargarlos.");
    }
}
#endif

}  // namespace

bool hasEmbeddedDeps() {
#ifdef YTDL_EMBEDDED
    return true;
#else
    return false;
#endif
}

fs::path defaultBinDir() {
#ifdef YTDL_EMBEDDED
    if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA")) return fs::path(local) / "YtDownloader" / "bin";
#endif
    return proc::executableDir() / "bin";
}

Tools ensureTools(const DepsOptions& opts) {
    struct Scope {
        const DepsOptions* prev;
        explicit Scope(const DepsOptions* o) : prev(g_opts) { g_opts = o; }
        ~Scope() { g_opts = prev; }
    } scope(&opts);
    Tools tools;
    std::error_code ec;
    fs::create_directories(opts.binDir, ec);

#ifdef YTDL_EMBEDDED
    if (!opts.forceUpdate) extractEmbedded(opts.binDir);
#endif

    // ---------------- youtube-dl ----------------
    if (!opts.ytdlOverride.empty()) {
        tools.ytdl = ytdlCommand(proc::fromUtf8(opts.ytdlOverride));
        if (!ytdlWorks(tools.ytdl))
            throw std::runtime_error("No se puede ejecutar youtube-dl en: " + opts.ytdlOverride);
    } else {
        fs::path local = opts.binDir / exeName("youtube-dl");
        bool found = false;
        if (!opts.forceUpdate) {
            if (fs::exists(local, ec) && ytdlWorks(ytdlCommand(local))) {
                tools.ytdl = ytdlCommand(local);
                found = true;
            } else if (auto p = proc::findInPath("youtube-dl")) {
                auto cmd = std::vector<std::string>{proc::toUtf8(*p)};
                if (ytdlWorks(cmd)) { tools.ytdl = cmd; found = true; }
            }
        }
        if (!found) {
            say("youtube-dl no está disponible (o se ha pedido actualizarlo).");
            if (!confirm("¿Descargarlo ahora en " + proc::toUtf8(opts.binDir) + "?", opts.autoInstall))
                throw std::runtime_error("youtube-dl es necesario para continuar.");
            say("Descargando youtube-dl...");
            if (!download(kYtdlUrl, local)) throw std::runtime_error("Fallo al descargar youtube-dl.");
#ifndef _WIN32
            fs::permissions(local, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                            fs::perm_options::add, ec);
#endif
            tools.ytdl = ytdlCommand(local);
            if (!ytdlWorks(tools.ytdl)) {
#ifdef _WIN32
                throw std::runtime_error("youtube-dl se descargó pero no se puede ejecutar.");
#else
                throw std::runtime_error("youtube-dl se descargó pero no se puede ejecutar (¿está instalado python3?).");
#endif
            }
            say("youtube-dl instalado correctamente.");
        }
    }

    // ---------------- ffmpeg / ffprobe ----------------
    fs::path localFfmpeg = opts.binDir / exeName("ffmpeg");
    fs::path localFfprobe = opts.binDir / exeName("ffprobe");
    if (!opts.forceUpdate) {
        if (fs::exists(localFfmpeg, ec) && fs::exists(localFfprobe, ec) && ffmpegWorks(localFfmpeg, localFfprobe)) {
            tools.ffmpegDir = opts.binDir;
            tools.haveFfmpeg = true;
            return tools;
        }
        auto sysFfmpeg = proc::findInPath("ffmpeg");
        auto sysFfprobe = proc::findInPath("ffprobe");
        if (sysFfmpeg && sysFfprobe && ffmpegWorks(*sysFfmpeg, *sysFfprobe)) {
            tools.haveFfmpeg = true;  // youtube-dl lo encontrará en el PATH
            return tools;
        }
    }

    say("ffmpeg no está disponible (o se ha pedido actualizarlo). Es necesario para unir audio+vídeo\n"
        "en alta calidad y para convertir formatos.");
    if (!confirm("¿Descargar ffmpeg ahora (~120-160 MB) en " + proc::toUtf8(opts.binDir) + "?", opts.autoInstall))
        return tools;
    say("Descargando ffmpeg...");
    if (installFfmpeg(opts.binDir) && ffmpegWorks(localFfmpeg, localFfprobe)) {
        tools.ffmpegDir = opts.binDir;
        tools.haveFfmpeg = true;
        say("ffmpeg instalado correctamente.");
    } else {
        warn("Aviso: no se pudo instalar ffmpeg.");
    }
    return tools;
}
