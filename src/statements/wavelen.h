#pragma once
// wavelen <fn> <ms|hz> — периодический вызов функции.
// Синтаксис станционного диалекта: «длина волны» тика ядра.
//   wavelen tick 16     — каждые 16 мс
//   wavelen pulse.check 60hz — 60 раз в секунду
// eval: записывает привязку в vm.__wavelen_<fn> — ядро
// (boot.js / нативный рантайм) читает их и вешает таймеры.
#include "../node.h"
#include "../nvm.h"
#include "../jsgen.h"

class WavelenStatement : public Node {
public:
    std::string fn;
    int ms = 16;
    WavelenStatement(std::string f, int m)
        : fn(std::move(f)), ms(m) {}

    NObject eval(Nvm& vm) override {
        vm.set("__wavelen_" + fn, NObject((long long)ms));
        return NObject();
    }
    Emitted cpp(Nvm& vm) override {
        vm.set("__wavelen_" + fn,
               NObject((long long)ms));
        return Emitted("");
    }
    Emitted javascript(Nvm& vm) override {
        return Emitted("");
    }
};
