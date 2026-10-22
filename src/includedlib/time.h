#pragma once
// Дескриптор модуля time + eval-регистратор.
#include "../module.h"
#include "../nvm.h"
#include "../../modules/nlm_time.h"
#include "math.h" // num

namespace includedlib {

inline void register_time(Nvm& vm, const std::string& alias) {
    auto d = std::make_shared<std::map<std::string, NObject>>();
    auto add = [&](const char* n, std::function<NObject(Nvm&, const NArgs&)> f) {
        auto fn = std::make_shared<NFunction>();
        fn->name = n;
        fn->stdimpl = std::move(f);
        (*d)[n] = NObject(fn);
    };
    add("now",    [](Nvm&, const NArgs&) { return NObject(nltime_now()); });
    add("nowSec", [](Nvm&, const NArgs&) { return NObject(nltime_nowSec()); });
    add("unix",   [](Nvm&, const NArgs&) { return NObject(nltime_unix()); });
    add("sleep",  [](Nvm&, const NArgs& a) { nltime_sleep(num(a,0)); return NObject(); });
    add("date",   [](Nvm&, const NArgs&) { return NObject(nltime_date()); });
    add("clock",  [](Nvm&, const NArgs&) { return NObject(nltime_clock()); });
    vm.set(alias, NObject(d));
}

inline const ModuleDescriptor time_desc = {
    "time", "nlm_time.h", {"nlmodules"}, &register_time, "time", nullptr
};

} // namespace includedlib
