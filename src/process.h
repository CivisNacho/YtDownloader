// Utilidades multiplataforma para lanzar procesos y localizar ejecutables.
#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace proc {

// Ejecuta `args[0]` con el resto como argumentos, heredando stdin/stdout/stderr.
// No pasa por ninguna shell, así que no hay problemas de comillas/inyección.
// Devuelve el código de salida del proceso o -1 si no se pudo lanzar.
int run(const std::vector<std::string>& args);

// Como run(), pero descartando la salida (útil para comprobar si algo funciona).
int runQuiet(const std::vector<std::string>& args);

// Permite cancelar desde otro hilo un proceso lanzado con runCapture() (y todos sus hijos).
class CancelToken {
public:
    void cancel();
    bool cancelled() const;
    // Uso interno de runCapture().
    bool attach(void* job);
    void detach();

private:
    mutable std::mutex m_;
    void* job_ = nullptr;
    bool cancelled_ = false;
};

// Ejecuta `args` sin ventana leyendo stdout+stderr juntos; llama a `onLine` por cada
// línea (separada por \n o \r) en UTF-8. Devuelve el código de salida, -1 si no se pudo
// lanzar o -2 si se canceló.
int runCapture(const std::vector<std::string>& args, const std::function<void(const std::string&)>& onLine,
               CancelToken* cancel = nullptr);

// Busca un ejecutable en el PATH. En Windows prueba también las extensiones de PATHEXT.
std::optional<fs::path> findInPath(const std::string& name);

// Directorio donde está el ejecutable actual.
fs::path executableDir();

// Lee una línea de la entrada estándar en UTF-8 (en la consola de Windows usa
// ReadConsoleW para que funcionen tildes y eñes). Devuelve false en EOF.
bool readLine(std::string& out);

// Prepara la consola para entrada/salida UTF-8 (no hace nada fuera de Windows).
void setupConsole();

// Argumentos de línea de comandos en UTF-8.
std::vector<std::string> utf8Args(int argc, char** argv);

// Conversión entre rutas y UTF-8 (en Windows fs::path usa UTF-16 internamente).
std::string toUtf8(const fs::path& p);
fs::path fromUtf8(const std::string& s);

}  // namespace proc
