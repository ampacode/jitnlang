#pragma once
// Порт FunctionDeclaration из statements/function.py
#include "../node.h"
#include "../nvm.h"
#include "../jsgen.h"
#include "block.h"

class FunctionDeclaration : public Node {
public:
    std::string type, name;
    // args: name -> {type, base}
    std::vector<std::pair<std::string,
        std::pair<std::string, NodePtr>>> args;
    std::vector<NodePtr> statements;
    bool decorated = false;
    bool is_ctor = false;   // конструктор класса: сигнатура без типа
    bool is_static = false; // статический член класса

    FunctionDeclaration(std::string t, std::string n, decltype(args) a,
                        std::vector<NodePtr> st)
        : type(std::move(t)), name(std::move(n)),
          args(std::move(a)), statements(std::move(st)) {}

    Emitted javascript(Nvm& vm) override {
        return Emitted("function " + name + "(" +
                       js::args(vm, args) + ") " +
                       js::block(vm, statements));
    }

    // сигнатура для forward-decl: "rtype name(type a, type b)"
    std::string signature(Nvm& vm) {
        std::vector<std::string> sig;
        std::vector<std::string> parts;
        for (auto& pr : args) {
            std::string t = pr.second.first;
            if (t == "auto" || t == "anti" || t.empty())
                t = vm.inferParamType(pr.first, statements);
            if (t.empty()) t = "auto";
            parts.push_back(t + " " + pr.first);
            sig.push_back(t);
        }
        std::string rtype = type;
        if (is_ctor) rtype = "";
        else if (rtype == "auto" || rtype == "anti" || rtype.empty())
            rtype = vm.inferReturnType(statements);
        if (!is_ctor && rtype.empty()) rtype = "void";
        vm.func_types[name] = rtype.empty() ? name : rtype;
        vm.func_arg_types[name] = sig;
        std::string prefix = is_static ? "static " : "";
        if (!rtype.empty()) prefix += rtype + " ";
        return prefix + name + "(" +
               [&] { std::string s; for (size_t i = 0; i < parts.size(); ++i) {
                   if (i) s += ", "; s += parts[i]; } return s; }() + ")";
    }

    Emitted cpp(Nvm& vm) override {
        vm.pushScope();
        try {
            std::string sig = signature(vm);
            int indent = vm.cpp_indent;
            vm.cpp_indent = indent + 1;
            std::string body;
            for (auto& s : statements) {
                Emitted r = s->cpp(vm);
                if (r.anticpp) continue;
                body += cpp_pad(vm) + r.getCpp() + ";\n";
            }
            vm.cpp_indent = indent;
            vm.popScope();
            Emitted e(sig + " {\n" + body + cpp_pad(vm) + "}");
            e.is_function_declaration = true;
            return e;
        } catch (...) {
            vm.popScope();
            throw;
        }
    }
    NObject eval(Nvm& vm) override;
};
