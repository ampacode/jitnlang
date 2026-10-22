// Порт run.py — пайплайн: lexer -> parser -> cpp() -> temp.cpp -> g++ -> exe
#include "run.h"
#include "lexer.h"
#include "parser/nparse.h"
#include "statements/function.h"
#include "statements/using.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdlib>

// вырезает '#'-комментарии (вне строк) — как в run.py
std::string stripComments(const std::string& src) {
    std::string out;
    bool inStr = false;
    char quote = 0;
    for (size_t i = 0; i < src.size(); ++i) {
        char c = src[i];
        if (inStr) {
            out += c;
            if (c == '\\' && i + 1 < src.size()) out += src[++i];
            else if (c == quote) inStr = false;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            inStr = true; quote = c; out += c;
        } else if (c == '#') {
            while (i < src.size() && src[i] != '\n') ++i;
            if (i < src.size()) out += '\n';
        } else out += c;
    }
    return out;
}

// get_code() из run.py: statements -> formatMain
CompileResult get_code(const std::string& filename) {
    Nvm vm;
    std::ifstream f(filename, std::ios::binary);
    if (!f) throw NLangError{"не могу открыть " + filename};
    std::stringstream ss;
    ss << f.rdbuf();
    std::string program = stripComments(ss.str());
    vm.source_dir = filename.substr(0, filename.find_last_of("/\\") == std::string::npos
                                   ? 0 : filename.find_last_of("/\\"));

    Lexer lexer;
    NParser parser;
    auto tokens = lexer.tokenize(program, true);
    auto statements = parser.program(tokens);

    // forward-declarations: nlang-функции видят друг друга
    std::string protoCode;
    for (auto& st : statements) {
        auto fd = std::dynamic_pointer_cast<FunctionDeclaration>(st);
        if (fd) protoCode += fd->signature(vm) + ";\n    ";
    }

    std::string mainCode, declarationsCode, globalsCode,
                usingDirectiveCode;
    for (auto& st : statements) {
        Emitted res = st->cpp(vm);
        if (res.code.empty()) continue;
        if (res.is_function_declaration) {
            declarationsCode += "    " + res.getCpp() + "\n";
        } else {
            mainCode += "    " + res.getCpp() + ";\n";
        }
    }

    // includes: базовые + required
    std::string inc = "#include \"nlrt.h\"\n";
    for (auto& r : vm.required_includes) {
        if (r.find('<') != std::string::npos || r.find('.') != std::string::npos ||
            r.find('/') != std::string::npos)
            inc += "#include <" + r + ">\n";
        else
            inc += "#include <" + r + ">\n";
    }
    if (vm.needs_exceptions) inc += "#include <stdexcept>\n";

    std::string cpp =
        inc +
        "using namespace std;\n" +
        "\n// ---- forward decls ----\n" + protoCode +
        "\n// ---- module declarations ----\n" + vm.module_declarations +
        "\n// ---- declarations ----\n" + declarationsCode +
        "\n// ---- globals ----\n" + globalsCode +
        "\nint main(int argc, char** argv) {\n" +
        // sys: захват argv (как py $SYS_INIT$)
        (vm.used_modules.count("sys")
            ? "    nlsys_capture(argc, argv);\n" : "") +
        mainCode +
        "    return 0;\n}\n";
    return {cpp, std::move(vm)};
}
