#pragma once
// Порт statements/conditional.py
#include "../node.h"
#include "../nvm.h"
#include "../jsgen.h"
#include "block.h"

class ConditionalStatement : public Node {
public:
    NodePtr cond, _if, _else; // _else может быть nullptr
    ConditionalStatement(NodePtr c, NodePtr i, NodePtr e)
        : cond(std::move(c)), _if(std::move(i)), _else(std::move(e)) {}

    Emitted javascript(Nvm& vm) override {
        std::string out = "if (" + cond->javascript(vm).getCpp() +
                          ") " + _if->javascript(vm).getCpp();
        if (_else)
            out += " else " + _else->javascript(vm).getCpp();
        return Emitted(out);
    }

    Emitted cpp(Nvm& vm) override {
        std::string pad = cpp_pad(vm);
        std::string code = "if ";
        code += "(" + cond->cpp(vm).getCpp() + ")";
        code += " {\n";
        code += _if->cpp(vm).getCpp();
        code += pad + "}";
        if (_else) {
            code += " else {\n";
            code += _else->cpp(vm).getCpp();
            code += pad + "}";
        }
        return Emitted(code);
    }
    NObject eval(Nvm& vm) override {
        if (cond->eval(vm).asBool()) return _if->eval(vm);
        if (_else) return _else->eval(vm);
        return NObject();
    }
};
