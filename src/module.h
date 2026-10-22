#pragma once
// Дескриптор C++-модуля стандартной библиотеки.
//
// Модуль = две половины:
//   modules/nlm_<name>.h/.cpp — реализация, собирается ОДИН РАЗ
//     в libnlmodules.a (прекомпиляция: сгенерированный код только
//     линкуется с ней — скорость компиляции программ не страдает).
//   дескриптор здесь — что модуль даёт компилятору и интерпретатору.
#include <string>
#include <vector>

class Nvm;

struct ModuleDescriptor {
    const char* name;              // имя для using <name>
    const char* header;            // заголовок для сгенерированного кода
                                   // ("nlm_math.h", "" — если не нужен)
    std::vector<std::string> libs; // флаги линковки ("nlmodules", "ws2_32")
    // регистрация в интерпретаторе: кладёт в vm.vars[alias] словарь
    // с NObject-обёртками над nl<name>_* функциями.
    void (*register_eval)(Nvm&, const std::string& alias);
    // имя объекта в nlang-коде (math., file., ...) — совпадает с name
    const char* alias;
    // C++-объявления, вставляемые в declarationsCode (как CPP_DECLARATIONS
    // в py-includedlib — для классов вроде ActivationContext)
    const char* cpp_declarations;
};

const ModuleDescriptor* find_module(const std::string& name);
