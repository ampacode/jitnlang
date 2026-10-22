#pragma once
// nlang -> JavaScript (порт compiler/js/jsgen.py).
//
// Эмиссия — через Node::javascript(vm): каждый узел знает,
// как сгенерировать себя на JS, ровно так же как через cpp().
// Этот файл: только общие helpers (js::*) и драйвер .nlb.
#include <string>
#include <vector>
#include <unordered_map>
#include "node.h"
#include "type.h"
class Nvm;

// ---------- js:: helpers для Node::javascript ----------

namespace js {

inline std::string op(int t) {
    static const std::unordered_map<int, std::string> ops = {
        {GT, ">"}, {LT, "<"}, {EQEQ, "==="}, {EXCLEQ, "!=="},
        {LTEQ, "<="}, {GTEQ, ">="}, {AMPAMP, "&&"}, {BARBAR, "||"},
        {PLUS, "+"}, {MINUS, "-"}, {STAR, "*"}, {SLASH, "/"},
        {PERCENT, "%"}, {STARSTAR, "**"}, {DOUBLESLASH, "__fd__"},
        {MINUSEQ, "-="}, {PLUSEQ, "+="}, {STAREQ, "*="},
        {SLASHEQ, "/="}, {EXCL, "!"},
        {INCREMENT, "++"}, {DECREMENT, "--"},
    };
    auto it = ops.find(t);
    return it == ops.end() ? "?" : it->second;
}

// nlang-строка -> JS-строка. StringExpression::value уже
// декодирован (\n -> real char) — остаётся только экранировать.
inline std::string str_lit(const std::string& v) {
    std::string out = "\"";
    for (char c : v) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
        }
    }
    return out + "\"";
}

// сигнатура FunctionDeclaration/FunctionExpression args:
// name -> {type, base}
using FnArgs = std::vector<std::pair<std::string,
    std::pair<std::string, NodePtr>>>;

// "a, b = 1" — args с дефолтами
inline std::string args(Nvm& vm, const FnArgs& a) {
    std::string out;
    for (size_t i = 0; i < a.size(); ++i) {
        if (i) out += ", ";
        out += a[i].first;
        if (a[i].second.second)
            out += " = " + a[i].second.second->javascript(vm).getCpp();
    }
    return out;
}

// "(a, b, c)" — позиционные + именованные как позиционные
inline std::string call_args(
    Nvm& vm, const std::vector<NodePtr>& pos,
    const std::vector<std::pair<std::string, NodePtr>>& named) {
    std::string out = "(";
    bool first = true;
    auto one = [&](const NodePtr& n) {
        if (!first) out += ", ";
        first = false;
        out += n->javascript(vm).getCpp();
    };
    for (auto& a : pos) one(a);
    for (auto& a : named) one(a.second);
    return out + ")";
}

// одна строчка: ';' если узел сам не поставил
inline std::string stmt(NodePtr n, Nvm& vm) {
    if (!n) return "";
    std::string s = n->javascript(vm).getCpp();
    if (s.empty()) return s;
    char last = s.back();
    if (last != ';' && last != '}') s += ";";
    return s;
}

// "{ s1; s2; }" — тело блока (список стейтментов)
inline std::string block(Nvm& vm, const std::vector<NodePtr>& st) {
    std::string out = "{\n";
    for (auto& s : st)
        out += "    " + stmt(s, vm) + "\n";
    return out + "}";
}

} // namespace js

// ---------- драйвер .nlb (в jsgen.cpp) ----------

std::string nlang_compile_to_js(Nvm& vm,
                                const std::vector<NodePtr>& stmts);

// frontend/<name>.nlb -> static/frontend/<name>.js
// возвращает url "/frontend/<name>.js" ("" если файла нет)
std::string nlang_compile_nlb(Nvm& vm, const std::string& name);

// все frontend/*.nlb -> static/frontend/*.js (как py run.py)
void nlang_compile_all_nlb(Nvm& vm);

// атрибут src=".../app.nlb" -> "/frontend/app.js"
std::string nlang_resolve_nlb_attr(const std::string& aval, Nvm& vm);
