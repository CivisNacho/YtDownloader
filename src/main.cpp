// YtDownloader: descargador de vídeos/audio de YouTube basado en youtube-dl.
// Descarga automáticamente youtube-dl y ffmpeg si no están instalados.

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "deps.h"
#include "gui.h"
#include "options.h"
#include "process.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <io.h>
#endif

namespace {

struct AppOptions {
    DownloadOptions dl;
    DepsOptions deps;
    bool formatGiven = false;
    bool interactive = false;
    bool updateOnly = false;
};

void printHelp() {
    std::cout <<
        R"(YtDownloader - descargador de YouTube basado en youtube-dl

Uso:
  ytdownloader                       Abre la interfaz gráfica (en Windows)
  ytdownloader -i                    Modo interactivo en consola (menús)
  ytdownloader [opciones] URL [URL...]

Opciones:
  -v, --video            Descargar vídeo (por defecto)
  -a, --audio            Descargar solo audio
  -q, --quality Q        Vídeo: best | worst | 2160 | 1440 | 1080 | 720 | 480 | 360 | 240 | 144
                         Audio: best | 320 | 256 | 192 | 160 | 128 | 96 | 64  (kbps)
  -f, --format F         Vídeo: mp4 | mkv | webm | avi | flv              (por defecto mp4)
                         Audio: mp3 | m4a | aac | opus | ogg | flac | wav (por defecto mp3)
  -o, --output DIR       Carpeta de destino (por defecto la actual)
  -t, --template T       Plantilla de nombre de youtube-dl (por defecto "%(title)s.%(ext)s")
  -p, --playlist         Descargar la lista de reproducción completa si la URL la incluye
  -F, --list-formats     Mostrar los formatos disponibles del vídeo y salir
      --code CODE        Selector de formato manual de youtube-dl (-f), ignora --quality
      --thumbnail        Incrustar la miniatura (solo audio mp3)
      --no-metadata      No añadir metadatos (título, artista...) al fichero
      --ytdl RUTA        Usar un ejecutable de youtube-dl concreto
      --update           Volver a descargar youtube-dl y ffmpeg y salir
  -i, --interactive      Modo interactivo con menús en la consola
  -y, --yes              Descargar dependencias sin preguntar
  -h, --help             Mostrar esta ayuda

Ejemplos:
  ytdownloader -q 1080 -f mkv https://www.youtube.com/watch?v=XXXX
  ytdownloader -a -f mp3 -q 320 -o musica https://www.youtube.com/watch?v=XXXX
)";
}

// Parsea argv. Devuelve false si hay que salir (ayuda o error), con `exitCode` fijado.
bool parseArgs(const std::vector<std::string>& args, AppOptions& app, int& exitCode) {
    if (args.size() <= 1) {
        app.interactive = true;
        return true;
    }
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto need = [&](const char* name) -> std::string {
            if (i + 1 >= args.size()) throw std::runtime_error(std::string("Falta el valor de ") + name);
            return args[++i];
        };
        if (a == "-h" || a == "--help") { printHelp(); exitCode = 0; return false; }
        else if (a == "-v" || a == "--video") app.dl.mode = Mode::Video;
        else if (a == "-a" || a == "--audio") app.dl.mode = Mode::Audio;
        else if (a == "-q" || a == "--quality") app.dl.quality = need("--quality");
        else if (a == "-f" || a == "--format") { app.dl.format = need("--format"); app.formatGiven = true; }
        else if (a == "-o" || a == "--output") app.dl.outputDir = need("--output");
        else if (a == "-t" || a == "--template") app.dl.outputTemplate = need("--template");
        else if (a == "-p" || a == "--playlist") app.dl.playlist = true;
        else if (a == "-F" || a == "--list-formats") app.dl.listFormats = true;
        else if (a == "--code") app.dl.formatCode = need("--code");
        else if (a == "--thumbnail") app.dl.embedThumbnail = true;
        else if (a == "--no-metadata") app.dl.addMetadata = false;
        else if (a == "--ytdl") app.deps.ytdlOverride = need("--ytdl");
        else if (a == "--update") { app.deps.forceUpdate = true; app.updateOnly = true; }
        else if (a == "-y" || a == "--yes") app.deps.autoInstall = true;
        else if (a == "-i" || a == "--interactive") app.interactive = true;
        else if (!a.empty() && a[0] == '-') throw std::runtime_error("Opción desconocida: " + a + " (usa --help)");
        else app.dl.urls.push_back(a);
    }
    if (!app.formatGiven) app.dl.format = defaultFormat(app.dl.mode);
    if (app.interactive && !app.dl.urls.empty()) app.interactive = false;
    return true;
}

#ifdef _WIN32
// El .exe es de subsistema "Windows" para que la interfaz gráfica no abra una consola.
// En modo línea de comandos nos enganchamos a la consola desde la que se lanzó (o creamos
// una nueva para el modo interactivo). Devuelve true si se usa la consola del proceso padre.
bool isRedirected(DWORD id) {
    HANDLE h = GetStdHandle(id);
    if (!h || h == INVALID_HANDLE_VALUE) return false;
    DWORD t = GetFileType(h);
    return t == FILE_TYPE_DISK || t == FILE_TYPE_PIPE;
}

bool setupCliConsole(bool ownConsole) {
    bool outR = isRedirected(STD_OUTPUT_HANDLE), errR = isRedirected(STD_ERROR_HANDLE),
         inR = isRedirected(STD_INPUT_HANDLE);
    bool parent = false;
    if (ownConsole || !AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    else parent = true;
    FILE* f = nullptr;
    if (!outR && freopen_s(&f, "CONOUT$", "w", stdout) == 0)
        SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdout)));
    if (!errR && freopen_s(&f, "CONOUT$", "w", stderr) == 0)
        SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));
    if (!inR && freopen_s(&f, "CONIN$", "r", stdin) == 0)
        SetStdHandle(STD_INPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdin)));
    std::cout.clear();
    std::cerr.clear();
    std::cin.clear();
    if (parent && !outR) std::cout << "\n";  // el intérprete ya ha mostrado su prompt
    return parent;
}
#endif

// ---------------------------------------------------------------- Modo interactivo

std::string ask(const std::string& prompt, const std::string& def = "") {
    std::cout << prompt;
    if (!def.empty()) std::cout << " [" << def << "]";
    std::cout << ": " << std::flush;
    std::string line;
    if (!proc::readLine(line)) throw std::runtime_error("Entrada cerrada.");
    // Recorta espacios y comillas (útil al pegar rutas desde el explorador).
    auto b = line.find_first_not_of(" \t\"");
    auto e = line.find_last_not_of(" \t\"");
    line = b == std::string::npos ? "" : line.substr(b, e - b + 1);
    return line.empty() ? def : line;
}

std::string label(const std::string& value, Mode mode, bool isQuality) {
    if (!isQuality) return value;
    if (value == "best") return "Mejor disponible";
    if (value == "worst") return "Peor (la más ligera)";
    return mode == Mode::Video ? value + "p" : value + " kbps";
}

// Muestra un menú numerado y devuelve la opción elegida (por defecto la primera).
std::string choose(const std::string& title, const std::vector<std::string>& opts, Mode mode, bool isQuality) {
    std::cout << "\n" << title << "\n";
    for (size_t i = 0; i < opts.size(); ++i)
        std::cout << "  " << (i + 1) << ") " << label(opts[i], mode, isQuality) << "\n";
    for (;;) {
        std::string s = ask("Elige una opción", "1");
        try {
            size_t n = std::stoul(s);
            if (n >= 1 && n <= opts.size()) return opts[n - 1];
        } catch (...) {
        }
        // También se acepta escribir el valor directamente (p.ej. "720" o "mkv").
        for (const auto& o : opts)
            if (o == s) return o;
        std::cout << "Opción no válida.\n";
    }
}

bool interactiveOnce(const Tools& tools, AppOptions& app) {
    DownloadOptions o;
    std::cout << "\n==============================================\n";
    std::string url = ask("URL del vídeo (vacío para salir)");
    if (url.empty()) return false;
    o.urls.push_back(url);

    std::string type = choose("¿Qué quieres descargar?", {"Vídeo", "Audio"}, Mode::Video, false);
    o.mode = type == "Audio" ? Mode::Audio : Mode::Video;

    if (o.mode == Mode::Video) {
        o.quality = choose("Calidad máxima de vídeo:", videoQualities(), o.mode, true);
        o.format = choose("Formato de salida:", videoFormats(), o.mode, false);
    } else {
        o.quality = choose("Calidad de audio:", audioQualities(), o.mode, true);
        o.format = choose("Formato de salida:", audioFormats(), o.mode, false);
        if (o.format == "mp3") {
            std::string t = ask("¿Incrustar la miniatura como carátula? (s/n)", "s");
            o.embedThumbnail = t[0] == 's' || t[0] == 'S';
        }
    }

    if (url.find("list=") != std::string::npos) {
        std::string p = ask("\nLa URL pertenece a una lista. ¿Descargar la lista completa? (s/n)", "n");
        o.playlist = p[0] == 's' || p[0] == 'S';
    }

    o.outputDir = ask("\nCarpeta de destino", app.dl.outputDir);

    std::vector<std::string> warnings;
    auto args = tools.ytdl;
    auto extra = buildYtdlArgs(o, proc::toUtf8(tools.ffmpegDir), tools.haveFfmpeg, warnings);
    args.insert(args.end(), extra.begin(), extra.end());
    args.insert(args.end(), o.urls.begin(), o.urls.end());
    for (const auto& w : warnings) std::cout << "Aviso: " << w << "\n";

    std::cout << "\nDescargando...\n\n";
    int rc = proc::run(args);
    std::cout << (rc == 0 ? "\n✔ Descarga completada.\n" : "\n✘ youtube-dl terminó con errores (código " +
                                                              std::to_string(rc) + ").\n");
    app.dl.outputDir = o.outputDir;  // recordar la carpeta para la siguiente
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    auto args = proc::utf8Args(argc, argv);
#ifdef _WIN32
    if (args.size() <= 1) return runGui();
    bool wantsInteractive = false;
    for (size_t i = 1; i < args.size(); ++i)
        if (args[i] == "-i" || args[i] == "--interactive") wantsInteractive = true;
    bool parentConsole = setupCliConsole(wantsInteractive);
#endif
    proc::setupConsole();

    AppOptions app;
    app.deps.binDir = defaultBinDir();
    app.deps.autoInstall = false;
#ifdef _WIN32
    // Con la consola del padre compartida no podemos preguntar (el intérprete también lee
    // del teclado), así que las dependencias se descargan sin confirmación.
    if (parentConsole) app.deps.autoInstall = true;
#endif

    try {
        int exitCode = 0;
        if (!parseArgs(args, app, exitCode)) return exitCode;

        if (!app.interactive && !app.updateOnly) {
            std::string err = validate(app.dl);
            if (!err.empty()) {
                std::cerr << "Error: " << err << "\n";
                return 2;
            }
        }

        if (app.interactive) std::cout << "YtDownloader - descargador de YouTube (youtube-dl)\n\nComprobando dependencias...\n";
        Tools tools = ensureTools(app.deps);

        if (app.updateOnly) {
            std::cout << "Dependencias actualizadas.\n";
            return tools.haveFfmpeg ? 0 : 1;
        }
        if (!tools.haveFfmpeg)
            std::cout << "Aviso: se continuará sin ffmpeg (calidad y formatos limitados).\n";

        if (app.interactive) {
            while (interactiveOnce(tools, app)) {}
            return 0;
        }

        std::vector<std::string> warnings;
        auto cmd = tools.ytdl;
        auto extra = buildYtdlArgs(app.dl, proc::toUtf8(tools.ffmpegDir), tools.haveFfmpeg, warnings);
        cmd.insert(cmd.end(), extra.begin(), extra.end());
        cmd.insert(cmd.end(), app.dl.urls.begin(), app.dl.urls.end());
        for (const auto& w : warnings) std::cerr << "Aviso: " << w << "\n";

        int rc = proc::run(args);
        if (rc < 0) {
            std::cerr << "Error: no se pudo ejecutar youtube-dl.\n";
            return 1;
        }
        return rc;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
