#pragma once
// Порт statements/using.py + nlang_using.py
#include "../node.h"
#include "../nvm.h"
#include "../module.h"
#include "../ninclude.h"

class UsingStatement : public Node {
public:
    std::string name;
    explicit UsingStatement(std::string n) : name(std::move(n)) {}

    Emitted cpp(Nvm& vm) override {
        // using <math> — встроенный модуль; иначе — C++-заголовок
        if (find_module(name)) {
            vm.useModule(name);
            return Emitted("");
        }
        vm.requireInclude(name);
        return Emitted("");
    }
    NObject eval(Nvm& vm) override {
        if (find_module(name)) {
            vm.useModule(name);
            return NObject();
        }
        return NObject();
    }
};

class NamespaceStatement : public Node {
public:
    std::string name;
    explicit NamespaceStatement(std::string n) : name(std::move(n)) {}
    Emitted cpp(Nvm&) override {
        return Emitted("using namespace " + name);
    }
};

class NlangUsingStatement : public Node {
public:
    std::string name; // путь к .nlc/.nlb
    explicit NlangUsingStatement(std::string n) : name(std::move(n)) {}

    // cpp-путь: файл встраивается на этапе компиляции (как #include)
    Emitted cpp(Nvm& vm) override {
        return Emitted("\n" + nlang_include_cpp(vm, name));
    }
    // eval-путь: файл исполняется интерпретатором; функции
    // получают hot-метки — изменение видно при следующем вызове
    NObject eval(Nvm& vm) override {
        nlang_include_eval(vm, name);
        return NObject();
    }
};
