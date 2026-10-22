#pragma once
// Порт FunctionExpression из statements/function.py —
// лямбда: (args) { body } / int(args) { body } / anti(args) { body }
#include "../node.h"
#include "../nvm.h"
#include "../jsgen.h"
#include "../statements/block.h" // cpp_pad

class FunctionExpression : public Node {
public:
    // args: name -> {type, base}
    std::vector<std::pair<std::string,
        std::pair<std::string, NodePtr>>> args;
    std::vector<NodePtr> statements;
    std::string return_type; // "", "int", "anti", ...

    FunctionExpression(decltype(args) a, std::vector<NodePtr> st,
                       std::string rt = "")
        : args(std::move(a)), statements(std::move(st)),
          return_type(std::move(rt)) {}

    Emitted javascript(Nvm& vm) override {
        return Emitted("function(" + js::args(vm, args) + ") " +
                       js::block(vm, statements));
    }

    Emitted cpp(Nvm& vm) override {
        vm.pushScope();
        try {
            std::string arglist, hint;
            for (auto& pr : args) {
                std::string t = pr.second.first;
                if (t == "auto" || t == "anti" || t.empty())
                    t = vm.inferParamType(pr.first, statements);
                if (t.empty()) t = "auto";
                if (!arglist.empty()) arglist += ", ";
                arglist += t + " " + pr.first;
                vm.setVariableType(pr.first, t);
            }
            std::string ret;
            if (return_type == "auto" || return_type == "anti") {
                std::string d = vm.inferReturnType(statements);
                if (!d.empty() && d != "auto") ret = " -> " + d;
            } else if (!return_type.empty()) {
                ret = " -> " + return_type;
            }
            int indent = vm.cpp_indent;
            vm.cpp_indent = indent + 1;
            std::string body;
            for (auto& s : statements) {
                Emitted r = s->cpp(vm);
                if (!r.code.empty())
                    body += cpp_pad(vm) + r.getCpp() + ";\n";
            }
            vm.cpp_indent = indent;
            vm.popScope();
            return Emitted("[&](" + arglist + ")" + ret + " {\n" +
                           body + cpp_pad(vm, 0) + "}");
        } catch (...) {
            vm.popScope();
            throw;
        }
    }
    NObject eval(Nvm& vm) override;
};
