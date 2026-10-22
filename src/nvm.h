#pragma once
// Порт nlcnvm.py — Nvm: плоское хранилище переменных + таблицы
// типов, скоупы var_types, UFCS-таблица, antiGet, inferType.
#include <set>
#include "nobject.h"

struct Node;
using NodePtr = std::shared_ptr<Node>;

class Nvm {
public:
    Nvm();

    // плоское хранилище переменных — vm[name], как dict в py-Nvm
    std::unordered_map<std::string, NObject> vars;
    // generation: бампается на каждом erase — кеши указателей
    // в VariableExpression инвалидируются одной операцией
    // (rehash НЕ инвалидирует указатели на элементы, только erase)
    size_t vars_gen = 0;
    void vars_erase(const std::string& n) {
        if (vars.erase(n)) ++vars_gen;
    }

    // --- таблицы типов / метаданные компиляции ---
    std::unordered_map<std::string, std::string> var_types;
    std::unordered_map<std::string, std::string> func_types;
    std::unordered_map<std::string, std::vector<std::string>> func_arg_types;
    std::unordered_map<std::string, std::string> dimensions;
    std::unordered_map<std::string, NObject> modules;
    std::unordered_map<std::string, NObject> antiassigned;
    std::unordered_map<std::string, std::string> antiassigned_types;
    std::vector<std::string> cppfunctions;
    std::set<std::string> required_includes;
    std::set<std::string> _auto_modules;
    std::string auto_module_code;
    std::set<std::string> class_names;    // имена объявленных классов
    // классы, чьи поля известны ABI (models[name] заполнен полями)
    std::set<std::string> class_data;
    // ORM: model X -> [(type, field)]; database -> имя БД (одна на программу)
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> models;
    std::map<std::string, std::string> databases; // name -> driver
    bool dim_module_used = false;         // using <dimension>
    bool hot_reload = false;              // --eval: следить за инклудами
    bool hot_swap = false;                // --eval: компилировать горячие ф-и
    int hot_threshold = 200;              // вызовов до компиляции
    // jit: ошибка из helper-вызова — исключения через
    // jit-кадр не разматываются, перевброс делает call()
    std::string pending_error;
    std::string runtime_dir;              // runtime/ (nlrt.h, hotabi.h)
    std::string compiler_dir;             // src/ (headers для dll)
    std::string source_dir;               // каталог входного .nlc
    std::set<std::string> used_modules;   // модули из using <x>
    std::unordered_map<std::string, std::string> alias_to_module; // net→internet
    std::set<std::string> link_libs;      // флаги линковки от модулей
    std::string module_declarations;      // cpp_declarations модулей
    int cpp_indent = 0;
    bool in_handler = false;
    bool needs_exceptions = false;
    // nls/css: name -> css-текст; палитры client/programmer
    std::unordered_map<std::string, std::string> css_blocks;
    std::map<std::string, long long> css_client_palette;
    std::map<std::string, std::string> css_prog_palette;

    // vm.get / vm[name] = v
    NObject* get(const std::string& nm);
    void set(const std::string& nm, NObject v);
    bool contains(const std::string& nm) const { return vars.count(nm) != 0; }

    // return без исключения: ReturnStatement ставит флаг,
    // циклы/блоки проверяют и разматываются, execFunctionBody
    // читает и сбрасывает. (было: throw на каждый return —
    // самый дорогой путь рекурсии)
    bool return_pending = false;
    NObject return_value;

    // lock/unlock — снимок ПИШЕМЫХ ключей на время вызова
    // функции (было: копия всего vars на каждый вызов — главный
    // тормоз рекурсии). lock_record зовётся из set()/ref() при
    // первой записи ключа под активным кадром.
    void lock();
    void unlock();
    void lock_record(const std::string& nm);

    // скоупы var_types (как {} в C++)
    void pushScope();
    void popScope();

    void setVariableType(const std::string& n, const std::string& t) { var_types[n] = t; }
    std::string getVariableType(const std::string& n) const;
    void setDimension(const std::string& n, const std::string& d) { dimensions[n] = d; }
    std::string getDimension(const std::string& n) const;

    // --- anti ---
    NObject* antiGet(const std::string& nm);
    void antiAssign(const std::string& t, const std::string& n, NObject v);
    bool isNameExistsInModules(const std::string& n) const { return modules.count(n) != 0; }
    bool checkIsFunctionCpp(const std::string& n) const;

    // --- кодогенерация ---
    void requireInclude(const std::string& inc) { required_includes.insert(inc); }
    void autoModule(const std::string& name);
    void useModule(const std::string& name); // using <x>
    bool isModule(const std::string& name) const;
    std::string getCppVariableAddress(const std::string& n) const;
    std::string module_func(const std::string& mod, const std::string& fn) const;
    std::string ufcs(const std::string& obj_type, const std::string& method) const;
    std::string ufcs_rtype(const std::string& obj_type, const std::string& method) const;
    std::string elem_type(const std::string& t) const;

    std::string inferType(NodePtr expr);
    std::string inferParamType(const std::string& name,
                               const std::vector<NodePtr>& statements);
    std::string inferReturnType(const std::vector<NodePtr>& statements);

    // stamp-дедуп для lock_record: текущий кадр + его генерация
    size_t lock_gen = 0;
    const void* lock_frame() const {
        return _lock_depth ? _lock_pool[_lock_depth - 1].get()
                           : nullptr;
    }

    // трассировка исполнения (включается через qt.execTrace):
    // строка скрипта -> когда её последний раз исполняли,
    // чтобы визуализация показывала реальную позицию кода
    bool exec_trace = false;
    std::unordered_map<int, long long> exec_last;
    static long long exec_now_ms();
    void exec_note(int line) {
        if (exec_trace && line >= 0)
            exec_last[line] = exec_now_ms();
    }

private:
    // стек кадров lock(): только переопределённые ключи.
    // pool переиспользуется между вызовами — ноль аллокаций
    // на глубокой рекурсии.
    struct LockFrame {
        std::unordered_map<std::string, NObject> old;
    };
    std::vector<std::unique_ptr<LockFrame>> _lock_pool;
    size_t _lock_depth = 0;
    std::vector<std::unordered_map<std::string, std::string>> _scope_stack;
};
