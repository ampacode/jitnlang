#pragma once
// Порт expressions/binary.py
#include "../node.h"
#include "../nvm.h"
#include "../jsgen.h"
#include "getter.h"
#include <cmath>

class BinaryExpression : public Node {
public:
    int _type;
    NodePtr expr1, expr2; // expr2 == nullptr ~ 'bt' (для ++/--)
    BinaryExpression(int t, NodePtr a, NodePtr b)
        : _type(t), expr1(std::move(a)), expr2(std::move(b)) {}

    Emitted javascript(Nvm& vm) override {
        // порт py jsgen BinaryExpression; // -> Math.floor(a/b)
        std::string op = js::op(_type);
        std::string l = expr1->javascript(vm).getCpp();
        if (!expr2) return Emitted("(" + l + op + ")"); // ++/--
        std::string r = expr2->javascript(vm).getCpp();
        if (op == "__fd__")
            return Emitted("Math.floor(" + l + " / " + r + ")");
        return Emitted("(" + l + " " + op + " " + r + ")");
    }

    Emitted cpp(Nvm& vm) override {
        std::string r1 = expr1->cpp(vm).getCpp();
        std::string r2 = expr2 ? expr2->cpp(vm).getCpp() : "";
        switch (_type) {
            case PLUS: return Emitted("(" + r1 + " + " + r2 + ")");
            case MINUS: return Emitted("(" + r1 + " - " + r2 + ")");
            case STAR: return Emitted("(" + r1 + " * " + r2 + ")");
            // / и % — через nlrt-хелперы: eval-семантика
            // (true-division, fmod для double, проверка нуля)
            case SLASH:
                return Emitted("(nl_div((double)(" + r1 +
                               "), (double)(" + r2 + ")))");
            case PERCENT:
                return Emitted("(nl_mod(" + r1 + ", " +
                               r2 + "))");
            case DOUBLESLASH:
                return Emitted("((int)floor((double)" + r1 +
                               " / (double)" + r2 + "))");
            case STARSTAR:
                return Emitted("pow(" + r1 + ", " + r2 + ")");
            case PLUSEQ: return Emitted("(" + r1 + " += " + r2 + ")");
            case MINUSEQ: return Emitted("(" + r1 + " -= " + r2 + ")");
            case SLASHEQ:
                // eval: x /= y — double-деление, результат
                // усечётся до типа x при записи (как vars[x])
                return Emitted("(" + r1 + " = nl_div((double)(" +
                               r1 + "), (double)(" + r2 + ")))");
            case PERCENTEQ:
                return Emitted("(" + r1 + " = nl_mod(" + r1 +
                               ", " + r2 + "))");
            case STAREQ: return Emitted("(" + r1 + " *= " + r2 + ")");
            case INCREMENT: return Emitted("(" + r1 + "++)");
            case DECREMENT: return Emitted("(" + r1 + "--)");
        }
        return Emitted(r1);
    }

    NObject eval(Nvm& vm) override;
};
