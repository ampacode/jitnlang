#pragma once
// Порт statements/_watch.py — watch/reveal -> try/catch
#include "../node.h"
#include "../nvm.h"
#include "block.h"

class WatchStatement : public Node {
public:
    struct Catch {
        std::string type, var; // type может быть пустым
        NodePtr block;
    };
    NodePtr body;
    std::vector<Catch> catches;

    Emitted cpp(Nvm& vm) override {
        vm.needs_exceptions = true;
        vm.requireInclude("stdexcept");
        std::string pad = cpp_pad(vm);
        std::string code = "try {\n";
        code += body->cpp(vm).getCpp();
        code += pad + "}";
        for (auto& c : catches) {
            if (c.type.empty()) {
                // reveal (e) — переменная = сообщение ошибки
                code += " catch (std::exception& __e) {\n";
                if (!c.var.empty()) {
                    vm.setVariableType(c.var, "std::string");
                    code += cpp_pad(vm, 1) + "std::string " + c.var +
                            " = __e.what();\n";
                }
            } else {
                code += " catch (" + c.type + "& " +
                        (c.var.empty() ? "__e" : c.var) + ") {\n";
                if (!c.var.empty()) vm.setVariableType(c.var, c.type);
            }
            code += c.block->cpp(vm).getCpp();
            code += pad + "}";
        }
        return Emitted(code);
    }
    NObject eval(Nvm& vm) override {
        try {
            body->eval(vm);
        } catch (NLangError& e) {
            if (catches.empty()) throw;
            auto& c = catches[0];
            if (!c.var.empty()) vm.set(c.var, NObject(e.msg));
            c.block->eval(vm);
        } catch (std::exception& e) {
            if (catches.empty()) throw;
            auto& c = catches[0];
            if (!c.var.empty()) vm.set(c.var, NObject(std::string(e.what())));
            c.block->eval(vm);
        }
        return NObject();
    }
};
