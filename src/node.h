#pragma once
// Базовый узел AST. Мультитаргетная эмиссия, как в py:
// cpp/javascript/html/css + eval + anti-семейство +
// linasm/winasm/antinlang (ассемблерные таргеты).
#include <string>
#include <memory>
#include "nobject.h"

class Nvm;

// Результат эмиссии — аналог CppXxxExpression.getCpp()
struct Emitted {
    std::string code;
    bool anticpp = false;
    bool is_function_declaration = false;
    bool is_list_expression = false;
    Emitted() = default;
    explicit Emitted(std::string c) : code(std::move(c)) {}
    const std::string& getCpp() const { return code; }
};

// cpp_pad — общий helper отступов (реализация в statements/block.h)
std::string cpp_pad(Nvm& vm, int extra = 0);

struct Node : std::enable_shared_from_this<Node> {
    int line = 0, col = 0;
    virtual ~Node() = default;

    // ---- таргеты ----
    virtual Emitted cpp(Nvm&) { return Emitted{}; }
    virtual Emitted javascript(Nvm&) { return Emitted{}; }
    virtual Emitted html(Nvm&) { return Emitted{}; }
    virtual Emitted css(Nvm&) { return Emitted{}; }

    // ---- интерпретатор ----
    virtual NObject eval(Nvm&) { return NObject(); }

    // ---- лёгкий downcast без RTTI (горячие eval-пути) ----
    // ~2ns виртуальный вызов против ~40ns dynamic_cast.
    // узлы переопределяют свой метод и возвращают this.
    virtual void* asGetter() { return nullptr; }
    virtual void* asVar() { return nullptr; }
    virtual void* asCall() { return nullptr; }
    virtual void* asArr() { return nullptr; }
    virtual void* asNum() { return nullptr; }
    virtual void* asStrExpr() { return nullptr; }

    // ---- anti-режимы ----
    virtual bool isAnti(Nvm&) { return false; }
    virtual Emitted anticpp(Nvm&) { return Emitted{}; }
    virtual Emitted antijavascript(Nvm&) { return Emitted{}; }
    virtual Emitted antihtml(Nvm&) { return Emitted{}; }
    virtual Emitted anticss(Nvm&) { return Emitted{}; }
    virtual NObject antieval(Nvm&) { return NObject(); }

    // ---- asm-таргеты ("anti nlang") ----
    // интерпретатор как компилятор: asm — одновременно
    // и слой исполнения, и эмиссия. linasm/winasm —
    // текстовый GAS для двух ABI, antinlang — прямая
    // эмиссия машинного кода (аннигиляция интерпретатора:
    // узел сам пишет байты в исполняемый буфер).
    virtual Emitted linasm(Nvm&) { return Emitted{}; }   // System V x64
    virtual Emitted winasm(Nvm&) { return Emitted{}; }   // Windows x64
    virtual Emitted antinlang(Nvm&) { return Emitted{}; } // JIT-эмиссия
};
