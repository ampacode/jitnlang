#pragma once
// Порт statements/_return.py + _break.py + _continue.py + skip
#include "../node.h"
#include "../nvm.h"

class ReturnStatement : public Node {
public:
    NodePtr expr; // nullptr — голый return
    explicit ReturnStatement(NodePtr e) : expr(std::move(e)) {}
    Emitted cpp(Nvm& vm) override {
        if (!expr) return Emitted("return");
        return Emitted("return " + expr->cpp(vm).getCpp());
    }
    Emitted javascript(Nvm& vm) override {
        if (!expr) return Emitted("return");
        return Emitted("return " + expr->javascript(vm).getCpp());
    }
    NObject eval(Nvm& vm) override {
        vm.return_value = expr ? expr->eval(vm) : NObject();
        vm.return_pending = true;
        return vm.return_value;
    }
};

class BreakStatement : public Node {
public:
    Emitted cpp(Nvm&) override { return Emitted("break"); }
    Emitted javascript(Nvm&) override { return Emitted("break"); }
    NObject eval(Nvm&) override { throw BreakException{}; }
};

class ContinueStatement : public Node {
public:
    Emitted cpp(Nvm&) override { return Emitted("continue"); }
    Emitted javascript(Nvm&) override { return Emitted("continue"); }
    NObject eval(Nvm&) override { throw ContinueException{}; }
};

class SkipStatement : public Node {
public:
    Emitted cpp(Nvm&) override { return Emitted(""); }
};
