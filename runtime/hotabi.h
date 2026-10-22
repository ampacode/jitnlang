#pragma once
// ABI-мост для hot-swap: хост (интерпретатор) -> скомпилированная dll.
// Через границу dll идут только POD — std::string и shared_ptr
// нельзя, у них разные реализации в exe и dll.
//
// Поток вызова:
//   хост пакует NObject'ы -> NLArg[] по объявленным типам параметров,
//   зовёт nl_hot_call(), dll распаковывает -> зовёт скомпилированное
//   тело -> пакует результат в NLRet.
extern "C" {

enum NLType : int {
    NL_VOID = 0,
    NL_INT = 1,
    NL_DOUBLE = 2,
    NL_STRING = 3,
    NL_BOOL = 4,
};

struct NLArg {
    int type;            // NLType
    long long i;         // int/bool
    double d;            // double
    const char* str;     // string (живёт на стороне хоста во время вызова)
};

struct NLRet {
    int type;
    long long i;
    double d;
    char str[65536];     // результат-строка/JSON копируется сюда
};

// точка входа dll; возвращает 0 = ok, -1 = аргументы не сошлись
typedef int (*NLHotCall)(const NLArg* args, int nargs, NLRet* ret);

}
