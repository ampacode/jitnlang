#pragma once
// Инклуд nlang-файлов: using "file.nlc" / "file.nlb"
//
// Tiered-модель:
//   eval-путь — файл читается, лексится, парсится и исполняется
//     интерпретатором. Функции помечаются (hot_path, hot_mtime):
//     при вызове, если файл на диске изменился — перезагружается
//     (изменение сразу отображается).
//   cpp-путь — файл встраивается на этапе компиляции:
//     его statements эмитятся инлайн (как #include).
#include <string>
#include <vector>
#include "node.h"

class Nvm;

// mtime файла (0 если нет)
long long nlang_mtime(const std::string& path);
// прочитать+лекс+распарсить файл в statements
std::vector<NodePtr> nlang_parse_file(const std::string& path);
// eval-инклуд: выполнить файл и пометить функции hot-метками
void nlang_include_eval(Nvm& vm, const std::string& path);
// cpp-инклуд: сгенерировать код statements файла инлайн
std::string nlang_include_cpp(Nvm& vm, const std::string& path);
// view("name") -> views/<name>.vc: дерево VcExpression
NodePtr nlang_parse_view(Nvm& vm, const std::string& name);
// нативная регистрация view()/style() для eval-пути
void nlang_register_view(Nvm& vm);
// style("name") -> css-текст styles/<name>.nls
std::string nlang_style_css(Nvm& vm, const std::string& name);
// развернуть GetterExpression из одного элемента (для литералов)
NodePtr nlang_unwrap_getter(NodePtr n);
