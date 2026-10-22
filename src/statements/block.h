#pragma once
// Порт statements/block.py — BlockStatement + cpp_pad
#include "../node.h"
#include "../nvm.h"
#include "../jsgen.h"

inline std::string cpp_pad(Nvm& vm, int extra) {
    return std::string(4 * (vm.cpp_indent + extra), ' ');
}

class BlockStatement : public Node {
public:
    std::vector<NodePtr> block;
    explicit BlockStatement(std::vector<NodePtr> b) : block(std::move(b)) {}

    Emitted javascript(Nvm& vm) override {
        return Emitted(js::block(vm, block));
    }

    Emitted cpp(Nvm& vm) override {
        std::string code;
        int indent = vm.cpp_indent;
        vm.cpp_indent = indent + 1;
        std::string pad = std::string(4 * (indent + 1), ' ');
        vm.pushScope();
        try {
            for (auto& s : block) {
                if (!s) continue;
                Emitted res = s->cpp(vm);
                code += pad + res.getCpp() + ";\n";
            }
        } catch (...) {
            vm.popScope();
            vm.cpp_indent = indent;
            throw;
        }
        vm.popScope();
        vm.cpp_indent = indent;
        return Emitted(code);
    }
    NObject eval(Nvm& vm) override {
        NObject r;
        for (auto& s : block) {
            if (vm.return_pending) break; // return разматывает блоки
            if (s) { vm.exec_note(s->line); r = s->eval(vm); }
        }
        return r;
    }
};
