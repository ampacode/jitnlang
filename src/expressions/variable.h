#pragma once
// Порт expressions/variable.py — VariableExpression + ArrayAccessExpression
#include "../node.h"
#include "../nvm.h"

class VariableExpression : public Node {
public:
    std::string text;
    // указательный кеш: узел unordered_map стабилен пока ключ
    // не удалён; erase бампает vm.vars_gen -> кеш промахивается
    // и перечитывает. Hash-lookup только на промахе.
    mutable NObject* _slot = nullptr;
    mutable size_t _gen = ~size_t(0);
    // stamp "записано в этом lock-кадре" — пропуск повторного
    // lock_record для x += 1 в цикле (один hash вместо двух)
    mutable const void* _rec_frame = nullptr;
    mutable size_t _rec_lockgen = 0;

    explicit VariableExpression(std::string t) : text(std::move(t)) {}
    void* asVar() override { return this; }

    Emitted cpp(Nvm& vm) override {
        return Emitted(vm.getCppVariableAddress(text));
    }
    Emitted javascript(Nvm&) override { return Emitted(text); }
    // слот в vars — для чтения (eval) и записи (ref).
    // for_write=true: создаёт ключ, если нет (как vars[name])
    NObject* slot(Nvm& vm, bool for_write = false) const {
        if (_slot && _gen == vm.vars_gen) return _slot;
        NObject* p;
        if (for_write) {
            p = &vm.vars[text];
        } else {
            p = vm.get(text);
            if (!p) return nullptr;
        }
        _slot = p;
        _gen = vm.vars_gen;
        return p;
    }
    void invalidate() { _slot = nullptr; }
    NObject eval(Nvm& vm) override {
        if (NObject* p = slot(vm)) return *p;
        throw NLangError{"неопределённая переменная '" + text + "'", line};
    }
    bool isAnti(Nvm& vm) override { return vm.antiGet(text) != nullptr; }
    NObject anticpp2(Nvm& vm) { return *vm.antiGet(text); }
};

// доступ по индексу: a[i] — в py живёт в variable.py
class ArrayAccessExpression : public Node {
public:
    std::vector<NodePtr> access;
    // «переменная» к которой применяется — проставляет GetterExpression
    // при эмиссии (variable = parent.cppExpression)
    std::string variable_cpp;
    NObject variable; // для eval

    explicit ArrayAccessExpression(std::vector<NodePtr> a)
        : access(std::move(a)) {}
    void* asArr() override { return this; }

    Emitted cpp(Nvm& vm) override;
    NObject eval(Nvm& vm) override;
};
