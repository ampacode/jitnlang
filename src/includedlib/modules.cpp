// Реестр встроенных модулей. Добавить модуль:
//   1. modules/nlm_<name>.h/.cpp — реализация (прекомпилируется
//      в libnlmodules.a)
//   2. src/includedlib/<name>.h — ModuleDescriptor + register_eval
//   3. одна строка в таблице ниже
#include "../module.h"
#include "math.h"
#include "str.h"
#include "file.h"
#include "time.h"
#include "random.h"
#include "json.h"
#include "dimension.h"
#include "regex.h"
#if !defined(__EMSCRIPTEN__)
// internet — серверные сокеты, crypto — bcrypt, tcp — сырые сокеты: под wasm нет
#include "crypto.h"
#include "internet.h"
#include "tcp.h"
#endif
#include "sys.h"
#ifdef NLANG_HAS_QT
#include "qt.h"
#endif
#ifdef NLANG_HAS_QTGL
#include "gl.h"
#endif

static const ModuleDescriptor* REGISTRY[] = {
    &includedlib::math_desc,
    &includedlib::str_desc,
    &includedlib::file_desc,
    &includedlib::time_desc,
    &includedlib::random_desc,
    &includedlib::json_desc,
    &includedlib::dimension_desc,
    &includedlib::regex_desc,
#if !defined(__EMSCRIPTEN__)
    &includedlib::internet_desc,
    &includedlib::holonet_desc,
    &includedlib::crypto_desc,
    &includedlib::tcp_desc,
#endif
    &includedlib::sys_desc,
#ifdef NLANG_HAS_QT
    &includedlib::qt_desc,
#endif
#ifdef NLANG_HAS_QTGL
    &includedlib::gl_desc,
#endif
};

const ModuleDescriptor* find_module(const std::string& name) {
    for (auto* d : REGISTRY)
        if (name == d->name) return d;
    return nullptr;
}
