// Точка входа wasm-сборки: nlang-интерпретатор в браузере.
// Экспортирует nl_run(source) -> строка захваченного stdout.
// JIT->JS: nl_fn_js(name) отдаёт JS-исходник функции —
// страница оборачивает его в new Function и вешает на fn.jit.
#ifdef __EMSCRIPTEN__
#include "lexer.h"
#include "parser/nparse.h"
#include "nvm.h"
#include "ninclude.h"
#include "run.h" // stripComments
#include "includedlib/json.h" // from_jvalue/to_jvalue
#include <emscripten.h>
#include <sstream>
#include <iostream>
#include <string>
#include <memory>

static std::string g_out;
static std::unique_ptr<Nvm> g_vm;

// src/wasmjit.cpp — nlang -> wasm-бинарь (C++ линковка)
std::string nlang_wasm_module(Nvm& vm);
const std::vector<std::string>& nlang_wasm_literals();
struct WasmJitUnsupported { const char* why; };

extern "C" {

// прогон программы в eval-режиме; возврат — stdout+ошибка текстом
EMSCRIPTEN_KEEPALIVE
const char* nl_run(const char* src) {
    g_out.clear();
    std::ostringstream capture;
    auto* old = std::cout.rdbuf(capture.rdbuf());
    auto* olde = std::cerr.rdbuf(capture.rdbuf());
    try {
        Lexer lexer;
        NParser parser;
        if (!g_vm) {
            g_vm = std::make_unique<Nvm>();
            nlang_register_view(*g_vm);
        }
        auto toks = lexer.tokenize(stripComments(src), true);
        auto statements = parser.program(toks);
        for (auto& st : statements) {
            g_vm->exec_note(st->line);
            st->eval(*g_vm);
        }
    } catch (LexError& e) {
        capture << "лексер " << (e.line + 1) << ":" << e.col
                << " " << e.msg << "\n";
    } catch (ParseError& e) {
        capture << "парсер " << (e.line + 1) << ":" << e.col
                << " " << e.msg << "\n";
    } catch (NLangError& e) {
        capture << "nlang: " << e.msg << "\n";
    } catch (std::exception& e) {
        capture << "c++: " << e.what() << "\n";
    }
    std::cout.rdbuf(old);
    std::cerr.rdbuf(olde);
    g_out = capture.str();
    return g_out.c_str();
}

// сброс окружения между запусками
EMSCRIPTEN_KEEPALIVE
void nl_reset() {
    g_vm = std::make_unique<Nvm>();
    nlang_register_view(*g_vm);
}

// ---------- JIT -> JS: аннигиляция функций ----------
// nl_fn_js("fib") -> JS-исходник "function fib(n){...}"
// (Node::javascript по AST функции) или "" — функция
// не nlang-объявленная (stdimpl) — остаётся через nl_call.
EMSCRIPTEN_KEEPALIVE
const char* nl_fn_js(const char* name) {
    static std::string out;
    out.clear();
    if (!g_vm || !name) return "";
    NObject* o = g_vm->get(name);
    if (!o || !o->isFunc()) return "";
    FuncPtr f = o->asFunc();
    if (!f || !f->decl_node) return "";
    try {
        out = f->decl_node->javascript(*g_vm).getCpp();
    } catch (...) {
        out.clear();
    }
    return out.c_str();
}

// вызов nlang-функции из JS: args — JSON-массив,
// возврат — JSON (число/строка/bool/list/dict/null).
EMSCRIPTEN_KEEPALIVE
const char* nl_call(const char* name, const char* args_json) {
    static std::string out;
    out = "null";
    if (!g_vm || !name) return out.c_str();
    try {
        NArgs args;
        if (args_json && *args_json) {
            JValue jargs = nljson_import(args_json);
            if (jargs.isArray())
                for (long long i = 0; i < jargs.size(); ++i)
                    args.pos.push_back(
                        includedlib::from_jvalue(jargs[i]));
        }
        NObject* o = g_vm->get(name);
        if (!o) return out.c_str();
        NObject r = o->call(*g_vm, args);
        out = nljson_export(includedlib::to_jvalue(r));
    } catch (NLangError& e) {
        out = "{\"__err\":" +
              nljson_export(JValue(e.msg)) + "}";
    } catch (std::exception& e) {
        out = "{\"__err\":" +
              nljson_export(JValue(e.what())) + "}";
    }
    return out.c_str();
}

// стадия 3: nlang -> wasm-бинарь одним модулем (base64).

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string b64(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i + 2 < s.size()) {
        uint32_t x = ((uint8_t)s[i] << 16) | ((uint8_t)s[i+1] << 8) |
                     (uint8_t)s[i+2];
        out += B64[(x >> 18) & 63]; out += B64[(x >> 12) & 63];
        out += B64[(x >> 6) & 63];  out += B64[x & 63];
        i += 3;
    }
    if (i < s.size()) {
        uint32_t x = (uint8_t)s[i] << 16;
        bool two = i + 1 < s.size();
        if (two) x |= (uint8_t)s[i+1] << 8;
        out += B64[(x >> 18) & 63];
        out += B64[(x >> 12) & 63];
        out += two ? B64[(x >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

EMSCRIPTEN_KEEPALIVE
const char* nl_wasm_module() {
    static std::string out;
    out.clear();
    if (!g_vm) return "";
    try {
        std::string bin = nlang_wasm_module(*g_vm);
        if (bin.empty()) return "";
        out = b64(bin);
    } catch (std::exception& e) {
        out = std::string("ERR:") + e.what();
    } catch (const WasmJitUnsupported& u) {
        out = std::string("ERR:unsupported:") + u.why;
    } catch (...) {
        out = "ERR:unknown";
    }
    return out.c_str();
}

// JSON-массив строковых литералов последнего модуля —
// JS подаёт их как env.lit0..litN (externref-глобалы)
EMSCRIPTEN_KEEPALIVE
const char* nl_wasm_literals() {
    static std::string out;
    JValue::Array a;
    for (auto& s : nlang_wasm_literals())
        a.push_back(JValue(s));
    out = nljson_export(JValue(a));
    return out.c_str();
}

// JSON [{fn, ms}] — wavelen-привязки: какие функции ядро
// должно звать периодически (tick 16ms, pulse.check 60hz)
EMSCRIPTEN_KEEPALIVE
const char* nl_wasm_wavelens() {
    static std::string out;
    JValue::Array a;
    for (auto& kv : g_vm->vars) {
        const std::string& n = kv.first;
        if (n.rfind("__wavelen_", 0) != 0) continue;
        JValue::Object o;
        o["fn"] = JValue(n.substr(10));
        o["ms"] = JValue(kv.second.asInt());
        a.push_back(JValue(std::move(o)));
    }
    out = nljson_export(JValue(a));
    return out.c_str();
}

// список имён nlang-функций (decl_node != null) — для jit-витрины
EMSCRIPTEN_KEEPALIVE
const char* nl_fn_names() {
    static std::string out;
    out.clear();
    if (!g_vm) return "";
    JValue::Array a;
    for (auto& kv : g_vm->vars) {
        if (kv.second.isFunc() && kv.second.asFunc()->decl_node)
            a.push_back(JValue(kv.first));
    }
    out = nljson_export(JValue(a));
    return out.c_str();
}

} // extern "C"
#endif // __EMSCRIPTEN__
