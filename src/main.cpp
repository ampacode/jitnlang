// CLI — порт драйвера run.py: nlang <file.nlc> [-o out] [-b] [--tokens] [--cpp]
#include "run.h"
#include "lexer.h"
#include "parser/nparse.h"
#include "nvm.h"
#include "ninclude.h"
#include "../modules/nlm_sys.h"
#include "jsgen.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// nlang->wasm (src/wasmjit.cpp) — та же эмиссия, что в браузере
std::string nlang_wasm_module(Nvm& vm);
const std::vector<std::string>& nlang_wasm_literals();
struct WasmJitUnsupported { const char* why; };

// ---- sector -> wasm: nlang <x.nlc> -bwasm / -fwasm ----
// charter.nlg задаёт entry и libs; склеиваем как boot.js
// (libs перед entry), прогоняем, эмитим wasm-модуль
// + .lits.json (externref-литералы) + .waves.json (wavelen).
static bool sector_wasm(const std::string& script) {
    fs::path dir = fs::path(script).parent_path();
    if (dir.empty()) dir = fs::current_path();
    // устав сектора: "entry <file>" / "libs <f> <f>..."
    std::string entry = "app.nlb";
    std::vector<std::string> libs;
    {
        std::ifstream ch(dir / "charter.nlg");
        std::string line;
        while (std::getline(ch, line)) {
            auto sp = line.find(' ');
            if (line.empty() || line[0] == '#' || sp == std::string::npos)
                continue;
            std::string key = line.substr(0, sp);
            // пробелы-разделители может быть несколько
            auto b = line.find_first_not_of(" \t", sp);
            if (b == std::string::npos) continue;
            std::string val = line.substr(b);
            if (key == "entry") entry = val;
            else if (key == "libs") {
                std::istringstream ls(val);
                std::string w;
                while (ls >> w) libs.push_back(w);
            }
        }
    }
    auto read_all = [](const fs::path& p) -> std::string {
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        return ss.str();
    };
    std::string full;
    for (auto& l : libs)
        full += read_all(dir / l) + "\n";
    full += read_all(dir / entry);

    Lexer lexer;
    NParser parser;
    Nvm vm;
    vm.source_dir = dir.string();
    nlang_register_view(vm);
    auto toks = lexer.tokenize(stripComments(full), true);
    auto statements = parser.program(toks);
    for (auto& st : statements) {
        vm.exec_note(st->line);
        st->eval(vm);
    }
    std::string bin;
    try {
        bin = nlang_wasm_module(vm);
    } catch (const WasmJitUnsupported& u) {
        std::cerr << "wasm: неподдерживаемое: " << u.why << "\n";
        return false;
    } catch (std::exception& e) {
        std::cerr << "wasm: " << e.what() << "\n";
        return false;
    }
    if (bin.empty()) { std::cerr << "wasm: нет кандидатов\n"; return false; }

    fs::path stem = dir / entry;
    stem.replace_extension("");
    auto jsq = [](const std::string& s) {
        std::string r = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\') { r += '\\'; r += c; }
            else if (c == '\n') r += "\\n";
            else if (c == '\r') r += "\\r";
            else if (c == '\t') r += "\\t";
            else if ((unsigned char)c >= 0x20) r += c;
        }
        return r + "\"";
    };
    {   // .wasm
        std::ofstream o(stem.string() + ".wasm", std::ios::binary);
        o.write(bin.data(), (std::streamsize)bin.size());
    }
    {   // .lits.json — ["lit",...]
        std::ofstream o(stem.string() + ".lits.json");
        o << "[";
        bool first = true;
        for (auto& s : nlang_wasm_literals()) {
            if (!first) o << ",";
            first = false;
            o << jsq(s);
        }
        o << "]";
    }
    {   // .waves.json — [{"fn":"tick","ms":500}...]
        std::ofstream o(stem.string() + ".waves.json");
        o << "[";
        bool first = true;
        for (auto& kv : vm.vars) {
            const std::string& n = kv.first;
            if (n.rfind("__wavelen_", 0) != 0) continue;
            if (!first) o << ",";
            first = false;
            o << "{\"fn\":" << jsq(n.substr(10))
              << ",\"ms\":" << kv.second.asInt() << "}";
        }
        o << "]";
    }
    std::cout << "wasm: " << stem.string() << ".wasm ("
              << bin.size() << "b) + lits + waves\n";
    return true;
}

// какой g++ вызывать для temp.cpp: NLANG_CXX env ->
// ucrt64 (наш тулчейн — libpq/sqlite/openssl) -> PATH
static std::string nlang_cxx() {
    if (const char* e = std::getenv("NLANG_CXX"))
        if (*e) return e;
    const char* cand = "C:/msys64/ucrt64/bin/g++.exe";
    if (fs::exists(cand)) return cand;
    return "g++";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "nlang <file.nlc> [-o out.exe] [-b] [--tokens] [--cpp] [--eval]\n";
        return 1;
    }
    std::string path = argv[1];
    std::string out;
    bool buildOnly = false, dumpTokens = false, dumpCpp = false,
         evalMode = false, buildWasm = false, frontWasm = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) out = argv[++i];
        else if (a == "-b") buildOnly = true;
        else if (a == "--tokens") dumpTokens = true;
        else if (a == "--cpp") dumpCpp = true;
        else if (a == "--eval") evalMode = true;
        // -bwasm: собрать <entry>.wasm сектора и выйти.
        // -fwasm: серверный режим — wasm собирается перед
        // listen; браузеру клиента отдаётся готовый модуль,
        // интерпретатор (nlang.js/nlang.wasm) не грузится.
        // charter.nlg задаёт entry и libs сектора.
        else if (a == "-bwasm") buildWasm = true;
        else if (a == "-fwasm") frontWasm = true;
        // --kill-last: убить процесс, который сидит на порту
        // сервера (dev-рестарт без ручной зачистки)
        else if (a == "--kill-last")
            _putenv("NLANG_KILL_LAST=1");
    }

    if (buildWasm || frontWasm)
        if (!sector_wasm(path)) return 1;
    if (buildWasm && !evalMode && !frontWasm) return 0;

    try {
        if (evalMode) {
            // интерпретаторный прогон: st->eval(vm) без g++
            std::ifstream f(path);
            std::stringstream ss; ss << f.rdbuf();
            Lexer lexer;
            NParser parser;
            Nvm vm;
            nlsys_capture(argc, argv);
            vm.hot_reload = true; // инклуды следят за изменениями
            vm.hot_swap = true;   // горячие функции -> dll
            if (const char* th = std::getenv("NLANG_HOT_THRESHOLD"))
                vm.hot_threshold = std::atoi(th);
            vm.source_dir = fs::path(path).parent_path().string();
            if (vm.source_dir.empty())
                vm.source_dir = fs::current_path().string();
            // каталоги для hot-swap компиляции
            {
                fs::path root = fs::absolute(argv[0]).parent_path();
                if (root.filename() == "build") root = root.parent_path();
                vm.runtime_dir = (root / "runtime").string();
                vm.compiler_dir = (root / "src").string();
            }
            auto toks = lexer.tokenize(stripComments(ss.str()), true);
            auto statements = parser.program(toks);
            nlang_register_view(vm); // view() нативный
            for (auto& st : statements) {
                vm.exec_note(st->line);
                st->eval(vm);
            }
            return 0;
        }

        if (dumpTokens) {
            std::ifstream f(path);
            std::stringstream ss; ss << f.rdbuf();
            Lexer lx;
            for (auto& t : lx.tokenize(stripComments(ss.str()), true))
                std::cout << t.line + 1 << ":" << t.col
                          << "  " << t.type << "  '" << t.text << "'\n";
            return 0;
        }

        auto res = get_code(path);
        res.vm.source_dir = fs::path(path).parent_path().string();
        if (res.vm.source_dir.empty())
            res.vm.source_dir = fs::current_path().string();

        // frontend/*.nlb -> static/frontend/*.js (как run.py)
        nlang_compile_all_nlb(res.vm);

        // каталог вывода — рядом с исходником, как compiler/cppOut
        fs::path outdir = fs::path(path).parent_path() / "cppOut";
        fs::create_directories(outdir);
        fs::path cppPath = outdir / "temp.cpp";
        {
            std::ofstream o(cppPath);
            o << res.cppCode;
        }
        if (dumpCpp) {
            std::cout << res.cppCode;
            return 0;
        }

        if (out.empty())
            out = (outdir / "a.exe").string();
        // g++ — как в run.py: -O3 Release
        fs::path runtime = fs::path(argv[0]).parent_path() / "runtime";
        if (!fs::exists(runtime / "nlrt.h"))
            runtime = fs::current_path() / "nlang-cpp" / "runtime";
        // модули требуют и include-пути modules/, и линковку .a
        fs::path modules_dir = fs::path(argv[0]).parent_path() / "modules";
        if (!fs::exists(modules_dir))
            modules_dir = fs::current_path() / "nlang-cpp" / "modules";
        std::string cxx = nlang_cxx();
        // ucrt64/bin нужен в PATH и для самого g++ (его DLLs),
        // и для импортных либ модулей
        std::string bindir;
        if (cxx != "g++")
            bindir = cxx.substr(0, cxx.find_last_of("/\\"));
        // статичный рантайм (libgcc/libstdc++/winpthread),
        // но НЕ полный -static — иначе -lpq берёт libpq.a
        // вместо libpq.dll.a и тащит весь libpgport/ldap
        // -march=native: AVX2-паттерны оптимизатора (opt.cpp)
        // компилируются в реальные SIMD-инструкции, иначе
        // сработает скалярный фолбэк #if !__AVX2__
        std::string cmd = "\"" + cxx + "\" -O3 -march=native -std=c++20"
            " -static-libgcc -static-libstdc++"
            " -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic -I\"" +
            runtime.string() + "\" -I\"" + modules_dir.string() +
            "\" \"" + cppPath.string() + "\" -o \"" + out + "\"";
        // порядок линковки важен: libnlmodules.a использует
        // системные либы (bcrypt/ws2_32) — идёт первым
        auto emit = [&](const std::string& lib) {
            cmd += " -L\"" + modules_dir.string() + "\" -l" + lib;
        };
        if (res.vm.link_libs.count("nlmodules")) emit("nlmodules");
        for (auto& lib : res.vm.link_libs)
            if (lib != "nlmodules") emit(lib);
        std::string full = bindir.empty() ? cmd
            : "set \"PATH=" + bindir + ";%PATH%\" && " + cmd;
        if (std::getenv("NLANG_DEBUG_CMD"))
            std::cerr << "cmd: " << full << "\n";
        int rc = std::system(full.c_str());
        if (rc != 0) {
            std::cerr << "g++ завершился с кодом " << rc
                      << " (см. " << cppPath << ")\n";
            return rc;
        }
        if (buildOnly) {
            std::cout << "собрано: " << out << "\n";
            return 0;
        }
        // модульные dll (libpq и пр.) ищутся по PATH — добавляем
        // bin тулчейна при запуске пользовательской программы
        std::string run = "\"" + out + "\"";
        if (!bindir.empty())
            run = "set \"PATH=" + bindir + ";%PATH%\" && " + run;
        return std::system(run.c_str());
    } catch (LexError& e) {
        std::cerr << path << ":" << e.line + 1 << ":" << e.col
                  << " лексер: " << e.msg << "\n";
    } catch (ParseError& e) {
        std::cerr << path << ":" << e.line + 1 << ":" << e.col
                  << " парсер: " << e.msg << "\n";
    } catch (NLangError& e) {
        std::cerr << path << ": " << e.msg << "\n";
    }
    return 1;
}
