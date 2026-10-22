#pragma once
// Живой hot-swap: eval-функция, вызванная >= vm.hot_threshold раз,
// компилируется в dll в ФОНОВОМ потоке — вызовы продолжают идти
// через интерпретатор. Когда dll готова, hot_abi публикуется
// атомикой и следующие вызовы идут нативно.
// Неудача компиляции — тихий откат в интерпретацию (hot_state=-1).
#include <string>
#include <memory>
#include <vector>
#include "../runtime/hotabi.h"
#include "nobject.h"
class Nvm;

// счётчик + триггер; вызывается из NObject::call
void nlang_maybe_hotswap(Nvm& vm, std::shared_ptr<NFunction> fn);
// вызов dll-точки: NObject-args -> NLArg -> dll -> NObject
NObject nlang_hot_invoke(NLHotCall callee,
                         const std::vector<int>& kinds,
                         const NArgs& a,
                         const std::string& ret_model = "");
