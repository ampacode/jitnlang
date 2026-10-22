# Hot-swap: ABI и маршалинг типов

Горячая функция (>= `hot_threshold` вызовов) компилируется в dll
в фоновом потоке; следующие вызовы идут нативно через `nl_hot_call`.
Аргументы и результат передаются через `NLArg`/`NLRet`.

## Типы через ABI (kind)

| kind | тип | провод |
|------|-----|--------|
| 1 | int/long/short/size_t | `NLArg::i` |
| 2 | double/float | `NLArg::d` |
| 3 | string | `NLArg::str` |
| 4 | bool | `NLArg::i` |
| 5 | коллекции, JValue, **модели** | JSON-строка |

## Коллекции — как JSON

`vector<T>`, `list<T>`, `map<string,T>`, `dict<..>`, `JValue` —
сериализуются через `nljs_export`/`nljs_parse` (kind 5).

Вложенность рекурсивная: `nl_to<T>` — tag-dispatch
(`nl_to_impl(j,(T*)nullptr)`); `nl_jv` — перегрузки для
`vector<T>`/`map<K,T>`.

## Модели (ORM) через ABI

`model User` регистрируется в `vm.models`. Тип `User` в сигнатуре
функции → kind 5 (JSON-объект):

- dll: эмитятся `struct User { rowid; поля }`, `nl_jv(User)` и
  `nl_to_User(JValue)` — в начало dll до трамплинов.
- exe → dll: dict-экземпляр → `nljs_export`.
- dll → exe: `hs_from_jv` → dict; `fn->hot_ret_model` хранит имя
  модели → `nlang_orm_bind_instance` привязывает `save`/`remove`
  (реестр `g_orm_fields` заполняется в `ModelStatement::eval`).

Трамплины (dll → eval-функции) так же умеют модель-параметры.

## Пользовательские классы

`class Person` через ABI — как модели без `rowid`:

- `ClassStatement` (cpp и eval) регистрирует поля в
  `vm.models[name]` + помечает `vm.class_data`.
- dll: `struct Person` без rowid + `nl_to_Person`/`nl_jv`.
- exe → dll: dict-экземпляр (поля+методы) → JSON.
- dll → exe: `ret_model` в `class_data` → свежий экземпляр
  через фабрику `vm.vars[Person]` (методы), поля перезаписаны.
- ctor с обязательными аргументами → fallback: голый dict
  без методов.

## .nlb → JavaScript

- `Node::javascript(vm)` — третий таргет наравне с `cpp()`/`eval()`.
- `.nlb` — C-стиль `{ }` и `';'` (parser.allow_braces +
  indent-блоки выключены).
- `frontend/<name>.nlb` → `static/frontend/<name>.js`
  (compile_all_nlb при сборке + resolve в vc-атрибутах).
- `src=".../app.nlb"` → `/frontend/app.js` — в `view()`
  (eval — на каждый рендер, cpp — при эмиссии).

## Hot-reload ������

- `.nlc`/`.nlb`: `nlang_include_eval` + `mark_hot` � �������,
  ������ � model-������� �������� `hot_path`; ��� ������
  ��������� FNV-��� ����������� � ���� ��������������.
- `.vc`: `view("x")` ������ `views/x.vc` �� ������ ����� �
  ��������� ����� �����.
- `.nls`/`.css`: `style("x")` ������������ `styles/x.nls`
  �� ������ �����.
- cpp-�����: `view`/`style`/`using <file>` ������������ ��
  ����� ���������� (��� `#include`) � ��� � py.
