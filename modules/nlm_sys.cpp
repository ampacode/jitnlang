// Порт includedlib/sys.py — реализация nlsys_*.
#include "nlm_sys.h"
#include <cstdlib>
#include <cstdio>
#include <filesystem>

static int g_argc = 0;
static char** g_argv = nullptr;

void nlsys_capture(int argc, char** argv) {
    g_argc = argc;
    g_argv = argv;
}
long long nlsys_argc() { return g_argc; }
std::vector<std::string> nlsys_args() {
    std::vector<std::string> out;
    for (int i = 0; i < g_argc; ++i)
        out.push_back(g_argv[i] ? g_argv[i] : "");
    return out;
}
std::string nlsys_arg(long long i) {
    return (i >= 0 && i < g_argc && g_argv[i]) ? g_argv[i] : "";
}
std::string nlsys_env(const std::string& name) {
    const char* v = std::getenv(name.c_str());
    return v ? v : "";
}
void nlsys_envSet(const std::string& n, const std::string& v) {
#ifdef _WIN32
    _putenv_s(n.c_str(), v.c_str());
#else
    setenv(n.c_str(), v.c_str(), 1);
#endif
}
std::string nlsys_exec(const std::string& cmd) {
#ifdef _WIN32
    FILE* p = _popen(cmd.c_str(), "r");
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    if (!p) return std::string();
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), p)) out += buf;
#ifdef _WIN32
    _pclose(p);
#else
    pclose(p);
#endif
    return out;
}
long long nlsys_execCode(const std::string& cmd) {
    return (long long)std::system(cmd.c_str());
}
void nlsys_exit(long long code) { std::exit((int)code); }
std::string nlsys_cwd() {
    return std::filesystem::current_path().string();
}
