#pragma once
// Порт expressions/calling.py
#include "../node.h"
#include "../nvm.h"
#include "../jsgen.h"
#include "variable.h"
#include "string.h"
#include "../ninclude.h"

class CallingExpression : public Node {
public:
    void* asCall() override { return this; }
    NodePtr getter; // обычно VariableExpression (вызываемое)
    // args: позиционные + именованные
    std::vector<NodePtr> pos_args;
    std::vector<std::pair<std::string, NodePtr>> named_args;
    bool cppFunction = false;

    CallingExpression(NodePtr g,
                      std::vector<NodePtr> pos,
                      std::vector<std::pair<std::string, NodePtr>> named = {})
        : getter(std::move(g)), pos_args(std::move(pos)),
          named_args(std::move(named)) {}

    bool isAnti(Nvm& vm) override {
        return vm.isNameExistsInModules(
            getter->cpp(vm).getCpp());
    }

    Emitted cpp(Nvm& vm) override {
        // view("x") — compile-time парсинг views/x.vc
        if (str() == "view" && pos_args.size() == 1) {
            NodePtr arg = nlang_unwrap_getter(pos_args[0]);
            if (auto se = std::dynamic_pointer_cast<StringExpression>(arg))
                return Emitted(
                    nlang_parse_view(vm, se->value)->cpp(vm).getCpp());
        }
        // style("x") — styles/x.nls -> "<style>...</style>"
        if (str() == "style" && pos_args.size() == 1) {
            NodePtr arg = nlang_unwrap_getter(pos_args[0]);
            if (auto se = std::dynamic_pointer_cast<StringExpression>(arg)) {
                std::string html = "<style>" +
                    nlang_style_css(vm, se->value) + "</style>";
                return Emitted("std::string(\"" +
                               StringExpression::esc(html) + "\")");
            }
        }
        std::string modeText = getter->cpp(vm).getCpp();
        std::string args;
        bool first = true;
        for (auto& a : pos_args) {
            if (a->isAnti(vm)) continue;
            if (!first) args += ",";
            args += a->cpp(vm).getCpp();
            first = false;
        }
        for (auto& a : named_args) {
            if (!first) args += ",";
            args += a.second->cpp(vm).getCpp();
            first = false;
        }
        return Emitted(modeText + "(" + args + ")");
    }
    Emitted javascript(Nvm& vm) override {
        // порт py jsgen CallingExpression
        std::string name = getter->javascript(vm).getCpp();
        std::string ca = js::call_args(vm, pos_args, named_args);
        if (name == "to_string")
            return Emitted("String(" +
                           ca.substr(1, ca.size() - 2) + ")");
        if (name == "cout" || name == "print" || name == "log")
            return Emitted("console.log" + ca);
        if (name == "new")
            return Emitted("new " + ca.substr(1, ca.size() - 2));
        return Emitted(name + ca);
    }
    NObject eval(Nvm& vm) override {
        NObject variable = getter->eval(vm);
        NArgs args;
        for (auto& a : pos_args) args.pos.push_back(a->eval(vm));
        for (auto& a : named_args) args.named[a.first] = a.second->eval(vm);
        return variable.call(vm, args);
    }
    std::string str() const {
        if (auto v = std::dynamic_pointer_cast<VariableExpression>(getter))
            return v->text;
        return "?";
    }
};
