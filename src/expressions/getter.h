#pragma once
// Порт expressions/getter.py — цепочка a.b[i].c(args)
#include "../node.h"
#include "../nvm.h"
#include "../jsgen.h"
#include "variable.h"
#include "calling.h"
#include "string.h"
#include "../ninclude.h"

class GetterExpression : public Node {
public:
    void* asGetter() override { return this; }
    std::vector<NodePtr> list;
    explicit GetterExpression(std::vector<NodePtr> l) : list(std::move(l)) {}

    Emitted javascript(Nvm& vm) override {
        // порт py jsgen GetterExpression: a.b() -> a.b(); .size() -> .length
        std::string code;
        static const std::unordered_map<std::string, std::string>
            js_funcs = {
            {"cout","console.log"}, {"print","console.log"},
            {"log","console.log"}, {"to_string","String"},
            {"stoll","parseInt"}, {"stoi","parseInt"},
            {"stod","parseFloat"},
            {"sqrt","Math.sqrt"}, {"floor","Math.floor"},
            {"ceil","Math.ceil"}, {"abs","Math.abs"},
            {"pow","Math.pow"}, {"round","Math.round"},
            {"min","Math.min"}, {"max","Math.max"},
            {"isnan","isNaN"},
        };
        for (size_t i = 0; i < list.size(); ++i) {
            NodePtr el = list[i];
            if (auto call = std::dynamic_pointer_cast<
                    CallingExpression>(el)) {
                std::string cname = call->str();
                if (cname == "strlen") {
                    std::string ca = js::call_args(vm, call->pos_args,
                                                   call->named_args);
                    code += ca.substr(1, ca.size() - 2) + ".length";
                } else {
                    auto it = js_funcs.find(cname);
                    bool mapped = it != js_funcs.end() &&
                        !(code.size() >= 5 &&
                          code.substr(code.size() - 5) == "Math.") &&
                        !(code.size() >= 8 &&
                          code.substr(code.size() - 8) == "console.");
                    code += mapped ? it->second : cname;
                    code += js::call_args(vm, call->pos_args,
                                          call->named_args);
                }
            } else if (auto ac = std::dynamic_pointer_cast<
                    ArrayAccessExpression>(el)) {
                for (auto& acc : ac->access)
                    code += "[" + acc->javascript(vm).getCpp() +
                            " - 1]"; // 1-индексация nlang
            } else {
                code += el->javascript(vm).getCpp();
            }
            if (i + 1 < list.size() &&
                !std::dynamic_pointer_cast<
                    ArrayAccessExpression>(list[i + 1]))
                code += ".";
        }
        auto rep = [](std::string& s, const std::string& a,
                      const std::string& b) {
            size_t p;
            while ((p = s.find(a)) != std::string::npos)
                s.replace(p, a.size(), b);
        };
        rep(code, ".size()", ".length");
        rep(code, ".empty()", ".length === 0");
        return Emitted(code);
    }

    Emitted cpp(Nvm& vm) override {
        if (list.size() == 1) return list[0]->cpp(vm);

        // --- dimN домены (модуль dimension) ---
        // dim2.f()         -> dimSpawn([]{ f(); })  (dim1 — синхронно)
        // dim2.alloc(n)    -> dimAlloc(2, n)
        // dim2_data["k"]   -> dimSlot(2,"data")["k"]
        std::string code;

        if (auto v0 = std::dynamic_pointer_cast<VariableExpression>(list[0]);
            v0 && vm.dim_module_used) {
            int dimN = -1;
            std::string slot;
            const std::string& h = v0->text;
            if (h.rfind("dim", 0) == 0) {
                size_t p = 3;
                while (p < h.size() && isdigit((unsigned char)h[p])) ++p;
                if (p > 3) {
                    dimN = std::stoi(h.substr(3, p - 3));
                    if (p < h.size() && h[p] == '_')
                        slot = h.substr(p + 1);
                }
            }
            if (dimN > 0) {
                if (!slot.empty()) {
                    code = "dimSlot(" + std::to_string(dimN) +
                           ",\"" + slot + "\")";
                    // дальше — обычный getter поверх dimSlot(...)
                    std::string out = code;
                    for (size_t i = 1; i < list.size(); ++i) {
                        NodePtr need = list[i];
                        if (auto acc = std::dynamic_pointer_cast<
                                ArrayAccessExpression>(need)) {
                            for (auto& a : acc->access)
                                out += "[" + a->cpp(vm).getCpp() + "]";
                        } else if (auto v = std::dynamic_pointer_cast<
                                VariableExpression>(need)) {
                            out += "[\"" + v->text + "\"]";
                        } else if (auto c = std::dynamic_pointer_cast<
                                CallingExpression>(need)) {
                            std::string argstr;
                            for (size_t k = 0; k < c->pos_args.size(); ++k) {
                                if (k) argstr += ",";
                                argstr += c->pos_args[k]->cpp(vm).getCpp();
                            }
                            out += "[\"" + c->str() + "\"](" +
                                   argstr + ")";
                        }
                    }
                    return Emitted(out);
                }
                // dimN.method(args)
                if (list.size() > 1) {
                    if (auto call = std::dynamic_pointer_cast<
                            CallingExpression>(list[1])) {
                        std::string mname = call->str();
                        std::string argstr;
                        for (size_t k = 0; k < call->pos_args.size(); ++k) {
                            if (k) argstr += ",";
                            argstr += call->pos_args[k]->cpp(vm).getCpp();
                        }
                        if (mname == "alloc")
                            code = "dimAlloc(" + std::to_string(dimN) +
                                   "," + argstr + ")";
                        else if (mname == "allocFree")
                            code = "dimAllocFree(" + std::to_string(dimN) +
                                   "," + argstr + ")";
                        else if (mname == "reset")
                            code = "dimReset(" + std::to_string(dimN) + ")";
                        else if (mname == "joinAll")
                            code = "dimJoinAll()";
                        else if (dimN <= 1)
                            code = mname + "(" + argstr + ")";
                        else
                            code = "dimSpawn([=](){" + mname + "(" +
                                   argstr + ");})";
                        return Emitted(code);
                    }
                }
            }
        }

        NodePtr parent = list[0];
        bool skip_next = false;
        // Model.method(...) — статический вызов ORM через ::
        bool is_model = false;
        if (auto v0 = std::dynamic_pointer_cast<VariableExpression>(
                list[0]))
            is_model = vm.models.count(v0->text) != 0;
        for (size_t i = 0; i < list.size(); ++i) {
            NodePtr need = list[i];
            if (skip_next) { skip_next = false; continue; }

            // Model.name -> Model::name
            if (i == 0 && is_model) {
                code += need->cpp(vm).getCpp();
                if (list.size() > 1)
                    code += "::";
                continue;
            }
            // Model.filter(name="x") -> Model::q().filter("name","x")
            //   named args становятся вложенными условиями query-builder
            if (i == 1 && is_model) {
                if (auto call = std::dynamic_pointer_cast<
                        CallingExpression>(need);
                    call && !call->named_args.empty() &&
                    (call->str() == "filter" ||
                     call->str() == "where" ||
                     call->str() == "filterLike" ||
                     call->str() == "filterInt")) {
                    code += "q()";
                    std::string m = call->str();
                    for (auto& na : call->named_args)
                        code += "." + m + "(\"" + na.first + "\"," +
                                na.second->cpp(vm).getCpp() + ")";
                    for (auto& a : call->pos_args)
                        code += "." + m + "(" +
                                a->cpp(vm).getCpp() + ")";
                    if (i + 1 < list.size() &&
                        !std::dynamic_pointer_cast<
                            ArrayAccessExpression>(list[i + 1]))
                        code += ".";
                    continue;
                }
            }

            // модуль: math.floor(x) -> nlmath_floor(x)
            //         math.pi       -> nlmath_pi
            // (только первый вызов цепочки: crypto.f(x).size()
            //  у size() уже не модульный тип)
            if (i == 1) {
                if (auto v0 = std::dynamic_pointer_cast<VariableExpression>(
                        list[0]); v0 && vm.isModule(v0->text)) {
                    if (auto call = std::dynamic_pointer_cast<CallingExpression>(need)) {
                        std::string mf = vm.module_func(v0->text, call->str());
                        if (!mf.empty()) {
                            std::string argstr;
                            for (size_t k = 0; k < call->pos_args.size(); ++k) {
                                if (k) argstr += ",";
                                argstr += call->pos_args[k]->cpp(vm).getCpp();
                            }
                            for (auto& na : call->named_args) {
                                if (!argstr.empty()) argstr += ",";
                                argstr += na.second->cpp(vm).getCpp();
                            }
                            code = mf + "(" + argstr + ")";
                            if (i + 1 < list.size() &&
                                !std::dynamic_pointer_cast<
                                    ArrayAccessExpression>(list[i + 1]))
                                code += ".";
                            continue;
                        }
                    }
                    if (auto v = std::dynamic_pointer_cast<VariableExpression>(need)) {
                        // константа модуля — inline-функция: nlmath_pi()
                        code = "nl" + v0->text + "_" + v->text + "()";
                        if (i + 1 < list.size() &&
                            !std::dynamic_pointer_cast<
                                ArrayAccessExpression>(list[i + 1]))
                            code += ".";
                        continue;
                    }
                }
            }

            // UFCS: obj.method(args) -> helper по типу объекта
            if (i > 0) {
                if (auto call = std::dynamic_pointer_cast<CallingExpression>(need)) {
                    std::string mname = call->str();
                    std::string hname;
                    if (auto v = std::dynamic_pointer_cast<VariableExpression>(list[0]))
                        hname = v->text;
                    std::string tmpl = vm.ufcs(vm.getVariableType(hname), mname);
                    if (!tmpl.empty()) {
                        std::string obj = code;
                        if (!obj.empty() && obj.back() == '.') obj.pop_back();
                        std::string argstr;
                        for (size_t k = 0; k < call->pos_args.size(); ++k) {
                            if (k) argstr += ",";
                            argstr += call->pos_args[k]->cpp(vm).getCpp();
                        }
                        std::string out = tmpl;
                        auto rep = [&](const std::string& k,
                                       const std::string& v) {
                            size_t p;
                            while ((p = out.find(k)) != std::string::npos)
                                out.replace(p, k.size(), v);
                        };
                        rep("{o}", obj);
                        rep("{args}", argstr);
                        rep("{comma_args}",
                            argstr.empty() ? "" : "," + argstr);
                        rep("{elem}",
                            vm.elem_type(vm.getVariableType(hname)));
                        code = out;
                        continue;
                    }
                }
            }
            if (auto acc = std::dynamic_pointer_cast<ArrayAccessExpression>(need)) {
                // a[i] -> nlAt(a, i): заменяет накопленный префикс
                acc->variable_cpp = code;
                parent = need;
                code = acc->cpp(vm).getCpp();
            } else {
                code += need->cpp(vm).getCpp();
            }
            if (i + 1 < list.size() &&
                !std::dynamic_pointer_cast<ArrayAccessExpression>(list[i + 1]))
                code += ".";
        }
        return Emitted(code);
    }

    NObject eval(Nvm& vm) override;
    void assign(Nvm& vm, NObject value);
    // lvalue: ссылка на слот (для +=, ++ и т.п.)
    NObject* ref(Nvm& vm);
};
