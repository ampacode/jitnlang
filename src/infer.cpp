// Порт inferType/inferParamType/inferReturnType из nlcnvm.py
#include "nvm.h"
#include "node.h"
#include "expressions/number.h"
#include "expressions/string.h"
#include "expressions/variable.h"
#include "expressions/binary.h"
#include "expressions/conditional.h"
#include "expressions/unary.h"
#include "expressions/list.h"
#include "expressions/_dict.h"
#include "expressions/calling.h"
#include "expressions/getter.h"
#include "expressions/function.h"
#include "statements/_return.h"
#include "statements/conditional.h"
#include "statements/_while.h"
#include "statements/_for.h"
#include "statements/foreach.h"
#include "statements/block.h"
#include "statements/assignment.h"
#include <functional>

// числовые типы, которые можно слить в один вывод
static const std::set<std::string> PARAM_NUMERIC = {
    "int", "long", "double", "float", "short", "char",
    "unsigned", "size_t", "intptr_t", "uintptr_t",
    "int64_t", "uint64_t", "long long",
};

std::string Nvm::inferType(NodePtr expr) {
    if (!expr) return "";
    if (auto n = std::dynamic_pointer_cast<NumberExpression>(expr))
        return n->is_float ? "double" : "int";
    if (std::dynamic_pointer_cast<StringExpression>(expr)) {
        requireInclude("string");
        return "std::string";
    }
    if (auto l = std::dynamic_pointer_cast<ListExpression>(expr)) {
        // [1,2,3] -> vector<int>: типы всех элементов сливаются —
        // только числовые смешиваются (в vector<double>)
        if (l->list.empty()) return "";
        std::set<std::string> uniq;
        for (auto& e : l->list) {
            std::string t = inferType(e);
            if (t.empty()) return "";
            uniq.insert(t);
        }
        requireInclude("vector");
        if (uniq.size() == 1)
            return "std::vector<" + *uniq.begin() + ">";
        bool numeric = true;
        for (auto& u : uniq)
            if (!PARAM_NUMERIC.count(u)) { numeric = false; break; }
        return numeric ? "std::vector<double>" : "";
    }
    if (std::dynamic_pointer_cast<DictExpression>(expr))
        return "JValue";
    if (auto c = std::dynamic_pointer_cast<ConditionalExpression>(expr))
        return "bool";
    if (auto u = std::dynamic_pointer_cast<UnaryExpression>(expr))
        return inferType(u->expr);
    if (auto b = std::dynamic_pointer_cast<BinaryExpression>(expr))
        return inferType(b->expr1);
    if (auto f = std::dynamic_pointer_cast<FunctionExpression>(expr))
        return "auto";
    if (auto c = std::dynamic_pointer_cast<CallingExpression>(expr)) {
        if (auto v = std::dynamic_pointer_cast<VariableExpression>(c->getter)) {
            if (class_names.count(v->text)) return v->text; // Person(...)
            auto it = func_types.find(v->text);
            return it != func_types.end() ? it->second : "";
        }
        return "";
    }
    if (auto g = std::dynamic_pointer_cast<GetterExpression>(expr)) {
        if (g->list.empty()) return "";
        if (g->list.size() == 1) return inferType(g->list[0]);
        NodePtr head = g->list[0];
        NodePtr last = g->list.back();
        if (auto c = std::dynamic_pointer_cast<CallingExpression>(head)) {
            if (auto v = std::dynamic_pointer_cast<VariableExpression>(c->getter)) {
                auto it = func_types.find(v->text);
                if (it != func_types.end()) return it->second;
            }
        }
        if (auto c = std::dynamic_pointer_cast<CallingExpression>(last)) {
            // obj.method() — по методу угадываем
            std::string m = c->str();
            std::string ot;
            if (auto v = std::dynamic_pointer_cast<VariableExpression>(
                    head))
                ot = getVariableType(v->text);
            std::string rt = ufcs_rtype(ot, m);
            if (!rt.empty()) return rt;
            if (m == "strip" || m == "replace" || m == "export")
                return "std::string";
            if (m == "length") return "long long";
            if (m == "split") return "std::vector<std::string>";
            return ""; // неизвестный метод — лучше auto
        }
        if (auto v = std::dynamic_pointer_cast<VariableExpression>(head))
            return getVariableType(v->text);
        return "";
    }
    if (auto v = std::dynamic_pointer_cast<VariableExpression>(expr)) {
        if (v->text == "true" || v->text == "false") return "bool";
        if (v->text == "null") return "auto";
        return getVariableType(v->text);
    }
    return "";
}

// ---------- _walk_exprs / _expr_name (порт из nlcnvm.py) ----------

#include "statements/function.h"
#include "statements/_watch.h"
#include "statements/using.h"
#include "expressions/new.h"

// обход всех подвыражений/подстейтментов узла.
// граница: внутрь FunctionExpression/FunctionDeclaration не спускаемся
// (у вложенной функции свои параметры и свои return).
static void walk_exprs(const NodePtr& n,
                       const std::function<void(const NodePtr&)>& visit) {
    if (!n) return;
    visit(n);
    if (std::dynamic_pointer_cast<FunctionExpression>(n) ||
        std::dynamic_pointer_cast<FunctionDeclaration>(n))
        return;

    auto rec = [&](const NodePtr& c) { walk_exprs(c, visit); };
    auto rec_all = [&](const std::vector<NodePtr>& v) {
        for (auto& c : v) walk_exprs(c, visit);
    };

    // --- выражения ---
    if (auto e = std::dynamic_pointer_cast<BinaryExpression>(n)) {
        rec(e->expr1); rec(e->expr2); return;
    }
    if (auto e = std::dynamic_pointer_cast<ConditionalExpression>(n)) {
        rec(e->first); rec(e->second); return;
    }
    if (auto e = std::dynamic_pointer_cast<UnaryExpression>(n)) {
        rec(e->expr); return;
    }
    if (auto e = std::dynamic_pointer_cast<ListExpression>(n)) {
        rec_all(e->list); return;
    }
    if (auto e = std::dynamic_pointer_cast<DictExpression>(n)) {
        for (auto& kv : e->dictionary) { rec(kv.first); rec(kv.second); }
        return;
    }
    if (auto e = std::dynamic_pointer_cast<CallingExpression>(n)) {
        rec(e->getter); rec_all(e->pos_args);
        for (auto& na : e->named_args) rec(na.second);
        return;
    }
    if (auto e = std::dynamic_pointer_cast<GetterExpression>(n)) {
        rec_all(e->list); return;
    }
    if (auto e = std::dynamic_pointer_cast<ArrayAccessExpression>(n)) {
        rec_all(e->access); return;
    }
    if (auto e = std::dynamic_pointer_cast<NewExpression>(n)) {
        rec(e->getter); rec_all(e->pos_args); return;
    }
    // --- стейтменты ---
    if (auto s = std::dynamic_pointer_cast<AssignmentStatement>(n)) {
        rec(s->getter); rec(s->value); return;
    }
    if (auto s = std::dynamic_pointer_cast<DeclarationStatement>(n)) {
        rec(s->value); rec(s->array_size); return;
    }
    if (auto s = std::dynamic_pointer_cast<BlockStatement>(n)) {
        rec_all(s->block); return;
    }
    if (auto s = std::dynamic_pointer_cast<ConditionalStatement>(n)) {
        rec(s->cond); rec(s->_if); rec(s->_else); return;
    }
    if (auto s = std::dynamic_pointer_cast<WhileStatement>(n)) {
        rec(s->expr); rec(s->block); return;
    }
    if (auto s = std::dynamic_pointer_cast<ForStatement>(n)) {
        rec(s->decl); rec(s->_while); rec(s->_do); rec(s->block); return;
    }
    if (auto s = std::dynamic_pointer_cast<ForeachStatement>(n)) {
        rec(s->expr); rec(s->block); return;
    }
    if (auto s = std::dynamic_pointer_cast<ReturnStatement>(n)) {
        rec(s->expr); return;
    }
    if (auto s = std::dynamic_pointer_cast<WatchStatement>(n)) {
        rec(s->body);
        for (auto& c : s->catches) rec(c.block);
        return;
    }
}

// имя голой переменной: x -> "x", x[i]/x.f/x.m() -> ""
// (тип объекта вызова/индекса != тип результата выражения —
// req.get("id") + "x" не делает req строкой)
static std::string bare_var_name(const NodePtr& e) {
    if (!e) return "";
    if (auto v = std::dynamic_pointer_cast<VariableExpression>(e))
        return v->text;
    return "";
}

// имя переменной в голове выражения: x / x[i] / x.f -> "x"
static std::string expr_name(const NodePtr& e) {
    if (!e) return "";
    if (auto v = std::dynamic_pointer_cast<VariableExpression>(e))
        return v->text;
    if (auto g = std::dynamic_pointer_cast<GetterExpression>(e))
        return g->list.empty() ? "" : expr_name(g->list[0]);
    return "";
}

// общее разрешение списка кандидатов в один тип (как в py)
static std::string resolve_candidates(
        const std::vector<std::string>& candidates) {
    std::set<std::string> uniq;
    for (auto& c : candidates)
        if (!c.empty() && c != "auto") uniq.insert(c);
    if (uniq.empty()) return "auto";
    if (uniq.size() == 1) return *uniq.begin();
    bool numeric = true;
    for (auto& u : uniq)
        if (!PARAM_NUMERIC.count(u)) { numeric = false; break; }
    if (numeric)
        return (uniq.count("double") || uniq.count("float"))
               ? "double" : "int";
    return "auto";
}

// ---------- inferParamType: тип параметра по использованию в теле ----------
// a += 1        -> тип правой части (или int)
// a + x / x + a -> тип другой стороны (или double)
// a < x / x < a -> тип другой стороны
// f(a)          -> тип параметра f из сигнатуры (func_arg_types)
std::string Nvm::inferParamType(
        const std::string& name,
        const std::vector<NodePtr>& statements) {
    std::vector<std::string> candidates;
    for (auto& st : statements) {
        walk_exprs(st, [&](const NodePtr& node) {
            if (auto b = std::dynamic_pointer_cast<BinaryExpression>(node)) {
                int t = b->_type;
                if (t == PLUSEQ || t == MINUSEQ || t == STAREQ ||
                    t == SLASHEQ || t == PERCENTEQ) {
                    if (bare_var_name(b->expr1) == name) {
                        std::string r = inferType(b->expr2);
                        candidates.push_back(r.empty() ? "int" : r);
                    }
                } else if (t == PLUS || t == MINUS || t == STAR ||
                           t == SLASH || t == PERCENT || t == STARSTAR ||
                           t == DOUBLESLASH) {
                    if (bare_var_name(b->expr1) == name) {
                        std::string r = inferType(b->expr2);
                        candidates.push_back(r.empty() ? "double" : r);
                    } else if (bare_var_name(b->expr2) == name) {
                        std::string r = inferType(b->expr1);
                        candidates.push_back(r.empty() ? "double" : r);
                    }
                }
            } else if (auto c =
                    std::dynamic_pointer_cast<ConditionalExpression>(node)) {
                if (bare_var_name(c->first) == name) {
                    std::string r = inferType(c->second);
                    if (!r.empty()) candidates.push_back(r);
                } else if (bare_var_name(c->second) == name) {
                    std::string r = inferType(c->first);
                    if (!r.empty()) candidates.push_back(r);
                }
            } else if (auto c =
                    std::dynamic_pointer_cast<CallingExpression>(node)) {
                std::string fname = expr_name(c->getter);
                auto sig = func_arg_types.find(fname);
                if (sig != func_arg_types.end())
                    for (size_t i = 0; i < c->pos_args.size() &&
                            i < sig->second.size(); ++i)
                        if (bare_var_name(c->pos_args[i]) == name)
                            candidates.push_back(sig->second[i]);
            }
        });
    }
    return resolve_candidates(candidates);
}

// ---------- inferReturnType: тип возврата по return-выражениям ----------
// собираются ВСЕ return тела (без вложенных функций — их граница
// отсекает walk_exprs) и сливаются как в inferParamType.
std::string Nvm::inferReturnType(
        const std::vector<NodePtr>& statements) {
    std::vector<std::string> candidates;
    for (auto& st : statements) {
        walk_exprs(st, [&](const NodePtr& node) {
            auto r = std::dynamic_pointer_cast<ReturnStatement>(node);
            if (r && r->expr) {
                std::string t = inferType(r->expr);
                if (!t.empty() && t != "auto") candidates.push_back(t);
            }
        });
    }
    return resolve_candidates(candidates);
}
