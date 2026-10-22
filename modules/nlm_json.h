#pragma once
// Модуль json — разбор и сборка JSON-текста.
//
// Простыми словами: JSON — текстовый формат «словарей и списков»
// вида {"a": 1, "b": [1, 2]}. json.import(text) превращает текст
// в JValue (наш словарь/список), json.export(value) — обратно.
#include <string>
#include "nlrt.h"

JValue nljson_import(const std::string& text);
std::string nljson_export(const JValue& value);
std::string nljson_exportPretty(const JValue& value, long long indent);
