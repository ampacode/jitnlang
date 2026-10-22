// Модуль json — рекурсивный разбор JSON.
// Реализация живёт в runtime/nljson.h (нужна hot-swap dll без
// линковки к libnlmodules); здесь — тонкие обёртки.
#include "nlm_json.h"
#include "nljson.h"

JValue nljson_import(const std::string& text) {
    return nljs_parse(text);
}
std::string nljson_export(const JValue& value) {
    return nljs_export(value);
}
std::string nljson_exportPretty(const JValue& value, long long indent) {
    return nljs_exportPretty(value, indent);
}
