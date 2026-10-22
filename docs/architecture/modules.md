# Система модулей

Модули — стандартная библиотека nlang. `using <math>` в начале файла
подключает модуль: его функции доступны и в интерпретаторе (`--eval`),
и в скомпилированном коде.

## Как устроен модуль

Три части:

```
modules/nlm_math.h      — прототипы функций (идут в сгенерированный код)
modules/nlm_math.cpp    — реализация → libnlmodules.a (собирается 1 раз)
src/includedlib/math.h  — дескриптор + eval-регистратор
```

**Прекомпиляция.** `libnlmodules.a` собирается один раз вместе с самим
компилятором nlang. Программа на nlang при компиляции получает
`#include "nlm_math.h"` и линкуется с готовой библиотекой — время
сборки программы почти не зависит от размера модулей.

**Два зеркальных пути** (принцип из AGENTS.md):

- *cpp-путь:* `math.floor(3.7)` → `GetterExpression` видит, что `math` —
  модуль, и печатает `nlmath_floor(3.7)`. Прямой вызов C++-функции,
  ноль накладных расходов.
- *eval-путь:* `useModule` кладёт в `vm.vars["math"]` словарь
  `NObject`-функций — тонкие обёртки над теми же `nlmath_*` реализациями.

## Дескриптор модуля

```cpp
const ModuleDescriptor math_desc = {
    "math",          // имя для using <math>
    "nlm_math.h",    // заголовок в сгенерированном коде
    {"nlmodules"},   // флаги линковки
    &register_math,  // функция регистрации в интерпретаторе
    "math",          // имя объекта в nlang-коде (math.)
    nullptr          // C++-объявления (классы — как ActivationContext)
};
```

## Как добавить модуль (3 шага)

1. `modules/nlm_mylib.h` + `modules/nlm_mylib.cpp` — функции
   `nlmylib_имя(...)`.
2. `src/includedlib/mylib.h` — `register_mylib` (словарь обёрток)
   и `mylib_desc` по образцу `math.h`.
3. Одна строка в `src/includedlib/modules.cpp` (таблица `REGISTRY`).

Если `using <x>` не находит модуль в реестре — имя трактуется как
C++-заголовок и вставляется в `#include <x>` (обратная совместимость
с `using <iostream>`).

## Что решает Nvm::useModule

- `used_modules` — какие модули активны (для `GetterExpression`)
- `required_includes` — заголовки в сгенерированный файл
- `link_libs` — флаги `-l` для g++
- `module_declarations` — C++-классы/код, вставляемый в объявления
- `register_eval` — объект модуля для интерпретатора
