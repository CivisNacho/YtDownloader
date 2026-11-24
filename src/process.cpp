#include "process.h"

#include <cstdlib>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace proc {

#ifdef _WIN32

static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// Entrecomilla un argumento según las reglas de CommandLineToArgvW / CRT de MSVC.
static std::wstring quoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') { ++it; ++backslashes; }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}

static int runImpl(const std::vector<std::string>& args, bool quiet) {
    if (args.empty()) return -1;
    std::wstring cmd;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) cmd += L' ';
        cmd += quoteArg(widen(args[i]));
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    HANDLE nul = INVALID_HANDLE_VALUE;
    if (quiet) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                          OPEN_EXISTING, 0, nullptr);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nul;
        si.hStdOutput = nul;
        si.hStdError = nul;
    }

    PROCESS_INFORMATION pi{};
    // Sin consola propia (interfaz gráfica) evitamos que cada proceso hijo abra una ventana negra.
    DWORD flags = GetConsoleWindow() ? 0 : CREATE_NO_WINDOW;
    std::wstring app = widen(args[0]);
    BOOL ok = CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &si, &pi);
    if (!ok) {
        // Si no es ruta absoluta, dejamos que Windows lo busque en el PATH.
        ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &si, &pi);
    }
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) return -1;

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

void CancelToken::cancel() {
    std::lock_guard<std::mutex> lock(m_);
    cancelled_ = true;
    if (job_) TerminateJobObject(static_cast<HANDLE>(job_), 1);
}

bool CancelToken::cancelled() const {
    std::lock_guard<std::mutex> lock(m_);
    return cancelled_;
}

bool CancelToken::attach(void* job) {
    std::lock_guard<std::mutex> lock(m_);
    job_ = job;
    return !cancelled_;
}

void CancelToken::detach() {
    std::lock_guard<std::mutex> lock(m_);
    job_ = nullptr;
}

int runCapture(const std::vector<std::string>& args, const std::function<void(const std::string&)>& onLine,
               CancelToken* cancel) {
    if (args.empty()) return -1;
    std::wstring cmd;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) cmd += L' ';
        cmd += quoteArg(widen(args[i]));
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return -1;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;

    // Un "job" agrupa youtube-dl y sus hijos (python, ffmpeg) para poder cancelarlos todos juntos.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
    }

    PROCESS_INFORMATION pi{};
    DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
    std::wstring app = widen(args[0]);
    BOOL ok = CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &si, &pi);
    if (!ok) ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(readEnd);
        if (job) CloseHandle(job);
        return -1;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    bool proceed = cancel ? cancel->attach(job) : true;
    if (proceed) ResumeThread(pi.hThread);
    else TerminateProcess(pi.hProcess, 1);

    std::string pending;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) && got > 0) {
        for (DWORD i = 0; i < got; ++i) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (!pending.empty()) onLine(pending);
                pending.clear();
            } else {
                pending.push_back(c);
            }
        }
    }
    if (!pending.empty()) onLine(pending);
    CloseHandle(readEnd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    if (cancel) cancel->detach();
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    if (cancel && cancel->cancelled()) return -2;
    return (int)code;
}

std::optional<fs::path> findInPath(const std::string& name) {
    const wchar_t* pathEnv = _wgetenv(L"PATH");
    if (!pathEnv) return std::nullopt;
    std::vector<std::wstring> exts = {L""};
    if (const wchar_t* pe = _wgetenv(L"PATHEXT")) {
        std::wstringstream ss(pe);
        std::wstring e;
        while (std::getline(ss, e, L';'))
            if (!e.empty()) exts.push_back(e);
    } else {
        exts.push_back(L".exe");
    }
    std::wstringstream ss(pathEnv);
    std::wstring dir;
    std::wstring wname = widen(name);
    while (std::getline(ss, dir, L';')) {
        if (dir.empty()) continue;
        for (const auto& e : exts) {
            fs::path p = fs::path(dir) / (wname + e);
            std::error_code ec;
            if (fs::is_regular_file(p, ec)) return p;
        }
    }
    return std::nullopt;
}

fs::path executableDir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n < buf.size()) { buf.resize(n); break; }
        buf.resize(buf.size() * 2);
    }
    return fs::path(buf).parent_path();
}

std::string toUtf8(const fs::path& p) { return narrow(p.wstring()); }
fs::path fromUtf8(const std::string& s) { return fs::path(widen(s)); }

bool readLine(std::string& out) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (!GetConsoleMode(in, &mode)) return static_cast<bool>(std::getline(std::cin, out));  // stdin redirigido
    std::wstring line;
    wchar_t buf[512];
    for (;;) {
        DWORD read = 0;
        if (!ReadConsoleW(in, buf, 512, &read, nullptr) || read == 0) {
            if (line.empty()) return false;
            break;
        }
        line.append(buf, read);
        if (!line.empty() && line.back() == L'\n') break;
    }
    while (!line.empty() && (line.back() == L'\n' || line.back() == L'\r')) line.pop_back();
    if (line.size() == 1 && line[0] == 0x1A) return false;  // Ctrl+Z
    out = narrow(line);
    return true;
}

void setupConsole() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

std::vector<std::string> utf8Args(int, char**) {
    std::vector<std::string> out;
    int n = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 0; i < n; ++i) out.push_back(narrow(wargv[i]));
    LocalFree(wargv);
    return out;
}

#else  // POSIX

void CancelToken::cancel() {
    std::lock_guard<std::mutex> lock(m_);
    cancelled_ = true;
}
bool CancelToken::cancelled() const {
    std::lock_guard<std::mutex> lock(m_);
    return cancelled_;
}
bool CancelToken::attach(void* job) {
    std::lock_guard<std::mutex> lock(m_);
    job_ = job;
    return !cancelled_;
}
void CancelToken::detach() {
    std::lock_guard<std::mutex> lock(m_);
    job_ = nullptr;
}

int runCapture(const std::vector<std::string>& args, const std::function<void(const std::string&)>& onLine,
               CancelToken* cancel) {
    if (args.empty() || (cancel && cancel->cancelled())) return -1;
    std::string cmd;
    for (const auto& a : args) cmd += "'" + a + "' ";
    FILE* f = popen((cmd + "2>&1").c_str(), "r");
    if (!f) return -1;
    std::string line;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n' || c == '\r') { if (!line.empty()) onLine(line); line.clear(); }
        else line.push_back((char)c);
    }
    if (!line.empty()) onLine(line);
    int st = pclose(f);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int runImpl(const std::vector<std::string>& args, bool quiet) {
    if (args.empty()) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (quiet) {
            int fd = open("/dev/null", O_RDWR);
            if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); }
        }
        std::vector<char*> argv;
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        return code == 127 ? -1 : code;
    }
    return -1;
}

std::optional<fs::path> findInPath(const std::string& name) {
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return std::nullopt;
    std::stringstream ss(pathEnv);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) continue;
        fs::path p = fs::path(dir) / name;
        std::error_code ec;
        if (fs::is_regular_file(p, ec) && access(p.c_str(), X_OK) == 0) return p;
    }
    return std::nullopt;
}

fs::path executableDir() {
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) == 0) {
        std::error_code ec;
        auto p = fs::canonical(buf.c_str(), ec);
        if (!ec) return p.parent_path();
    }
#else
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.parent_path();
#endif
    return fs::current_path();
}

std::string toUtf8(const fs::path& p) { return p.string(); }
fs::path fromUtf8(const std::string& s) { return fs::path(s); }

bool readLine(std::string& out) { return static_cast<bool>(std::getline(std::cin, out)); }

void setupConsole() {}

std::vector<std::string> utf8Args(int argc, char** argv) { return std::vector<std::string>(argv, argv + argc); }

#endif

int run(const std::vector<std::string>& args) { return runImpl(args, false); }
int runQuiet(const std::vector<std::string>& args) { return runImpl(args, true); }

}  // namespace proc
