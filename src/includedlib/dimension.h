#pragma once
// Дескриптор модуля dimension.
// Объекта в nlang-коде нет — dimN это псевдопеременные
// (dim2.f(), dim2_data["k"]), их разбирает GetterExpression
// и в cpp-, и в eval-пути. register_eval только ставит флаг.
#include "../module.h"
#include "../nvm.h"
#include "../../modules/nlm_dimension.h"

namespace includedlib {

inline void register_dimension(Nvm& vm, const std::string&) {
    vm.dim_module_used = true;
}

inline const ModuleDescriptor dimension_desc = {
    "dimension", "nlm_dimension.h",
#ifdef _WIN32
    {"nlmodules"},
#else
    {"nlmodules", "pthread"},
#endif
    &register_dimension, "dimension", nullptr
};

} // namespace includedlib
