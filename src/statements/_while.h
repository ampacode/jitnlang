#pragma once
// Порт statements/_while.py
#include "../node.h"
#include "../nvm.h"
#include "../jsgen.h"
#include "block.h"

class WhileStatement : public Node {
public:
    NodePtr expr, block;
    WhileStatement(NodePtr e, NodePtr b) : expr(std::move(e)), block(std::move(b)) {}

    Emitted javascript(Nvm& vm) override {
        return Emitted("while (" + expr->javascript(vm).getCpp() +
                       ") " + block->javascript(vm).getCpp());
    }

    Emitted cpp(Nvm& vm) override {
        std::string code = "while";
        code += "( " + expr->cpp(vm).getCpp() + " ) " + "{\n";
        code += block->cpp(vm).getCpp();
        code += cpp_pad(vm) + "}";
        return Emitted(code);
    }
    NObject eval(Nvm& vm) override {
        while (!vm.return_pending && expr->eval(vm).asBool()) {
            try { block->eval(vm); }
            catch (BreakException&) { break; }
            catch (ContinueException&) { continue; }
        }
        return NObject();
    }
};
