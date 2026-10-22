#pragma once
// Порт statements/_for.py — без оптимизатора (try_optimize_for позже)
#include "../node.h"
#include "../nvm.h"
#include "../jsgen.h"
#include "../opt.h"
#include "block.h"

class ForStatement : public Node {
public:
    NodePtr decl, _while, _do, block;
    Emitted javascript(Nvm& vm) override {
        auto strip = [](std::string s) {
            while (!s.empty() &&
                   (s.back() == ';' || s.back() == ' ' ||
                    s.back() == '\n'))
                s.pop_back();
            return s;
        };
        std::string d = decl ? strip(js::stmt(decl, vm)) : "";
        std::string inc = _do ? strip(js::stmt(_do, vm)) : "";
        std::string cond = _while ? _while->javascript(vm).getCpp() : "";
        return Emitted("for (" + d + "; " + cond + "; " + inc +
                       ") " + block->javascript(vm).getCpp());
    }
    ForStatement(NodePtr d, NodePtr w, NodePtr s, NodePtr b)
        : decl(std::move(d)), _while(std::move(w)),
          _do(std::move(s)), block(std::move(b)) {}

    Emitted cpp(Nvm& vm) override {
        vm.pushScope();
        try {
            std::string decl_txt = decl->cpp(vm).getCpp();
            std::string cond_txt = _while->cpp(vm).getCpp();
            std::string incr_txt = _do->cpp(vm).getCpp();
            std::string body_txt = block->cpp(vm).getCpp();
            // nlang-оптимизатор: горячие for -> AVX2 asm
            std::string code = nl_try_optimize_for(
                decl_txt, cond_txt, incr_txt, body_txt);
            if (code.empty()) {
                code = "for (" + decl_txt + "; " + cond_txt +
                       "; " + incr_txt + ")";
                code += " {\n";
                code += body_txt;
                code += cpp_pad(vm) + "}";
            }
            vm.popScope();
            return Emitted(code);
        } catch (...) {
            vm.popScope();
            throw;
        }
    }
    NObject eval(Nvm& vm) override {
        decl->eval(vm);
        while (!vm.return_pending && _while->eval(vm).asBool()) {
            try { block->eval(vm); }
            catch (BreakException&) { break; }
            catch (ContinueException&) {}
            _do->eval(vm);
        }
        return NObject();
    }
};
