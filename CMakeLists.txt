cmake_minimum_required(VERSION 3.16)
project(YtDownloader LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ytdownloader
    src/main.cpp
    src/process.cpp
    src/deps.cpp
    src/options.cpp
)

if(MSVC)
    # Código fuente en UTF-8 y runtime estático para que el .exe no dependa de las DLL de VC++.
    target_compile_options(ytdownloader PRIVATE /utf-8 /W4 /permissive-)
    set_property(TARGET ytdownloader PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(ytdownloader PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(MINGW)
    target_compile_options(ytdownloader PRIVATE -Wall -Wextra)
    target_link_options(ytdownloader PRIVATE -static)
else()
    target_compile_options(ytdownloader PRIVATE -Wall -Wextra)
endif()

if(WIN32)
    # Interfaz gráfica (Direct2D + Mica). El .exe es de subsistema Windows; en modo
    # línea de comandos se engancha a la consola desde la que se lanza.
    target_sources(ytdownloader PRIVATE src/gui.cpp assets/app.manifest)
    target_link_libraries(ytdownloader PRIVATE shell32 d2d1 dwrite dwmapi windowscodecs ole32 uuid)
    target_compile_definitions(ytdownloader PRIVATE UNICODE _UNICODE)
    if(MSVC)
        target_link_options(ytdownloader PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    else()
        target_link_options(ytdownloader PRIVATE -mwindows)
    endif()

    # Icono de la aplicación (assets/app.ico, generado a partir de assets/icon.svg).
    set(_ico "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.ico")
    file(TO_NATIVE_PATH "${_ico}" _ico_native)
    string(REPLACE "\\" "\\\\" _ico_rc "${_ico_native}")
    set(_icon_rc "${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc")
    file(WRITE "${_icon_rc}" "1 ICON \"${_ico_rc}\"\n")
    target_sources(ytdownloader PRIVATE "${_icon_rc}")
    set_source_files_properties("${_icon_rc}" PROPERTIES OBJECT_DEPENDS "${_ico}")
endif()

# -----------------------------------------------------------------------------
# Build "todo en uno": incrusta un .zip con youtube-dl.exe, ffmpeg.exe y
# ffprobe.exe dentro del ejecutable como recurso de Windows.
#   cmake -DYTDL_EMBED_DEPS=ruta/deps.zip ...
# -----------------------------------------------------------------------------
set(YTDL_EMBED_DEPS "" CACHE FILEPATH "Zip con youtube-dl/ffmpeg/ffprobe a incrustar en el .exe (solo Windows)")
if(YTDL_EMBED_DEPS)
    if(NOT WIN32)
        message(FATAL_ERROR "YTDL_EMBED_DEPS solo está soportado en Windows")
    endif()
    if(NOT EXISTS "${YTDL_EMBED_DEPS}")
        message(FATAL_ERROR "No existe YTDL_EMBED_DEPS: ${YTDL_EMBED_DEPS}")
    endif()
    file(TO_NATIVE_PATH "${YTDL_EMBED_DEPS}" _deps_native)
    string(REPLACE "\\" "\\\\" _deps_rc "${_deps_native}")
    set(_rc "${CMAKE_CURRENT_BINARY_DIR}/embedded_deps.rc")
    file(WRITE "${_rc}" "101 RCDATA \"${_deps_rc}\"\n")
    target_sources(ytdownloader PRIVATE "${_rc}")
    # Recompila el recurso si cambia el zip.
    set_source_files_properties("${_rc}" PROPERTIES OBJECT_DEPENDS "${YTDL_EMBED_DEPS}")
    target_compile_definitions(ytdownloader PRIVATE YTDL_EMBEDDED YTDL_DEPS_RESOURCE_ID=101)
    message(STATUS "Incrustando dependencias desde ${YTDL_EMBED_DEPS}")
endif()
