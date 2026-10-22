// jit_rt: рантайм-помощники для jit-кода "anti nlang".
// интерпретатор — та же скомпилированная программа:
// jit-эмиттер зовёт эти функции напрямую, как делает
// сгенерированный temp.cpp. мост int64 <-> NObject.
#include "nobject.h"
#include "nvm.h"
#include "node.h"
#include <cstring>
#include <string>
#include <vector>
#include <cmath>

// arith() живёт в expressions/eval_impl.cpp — общий
// код интерпретатора и jit-пути
NObject nl_arith(int op, const NObject& l, const NObject& r,
                 int line);
static NObject nl_cmp_eval(int type, const NObject& l,
                           const NObject& r);

extern "C" {
void nl_jit_layout_dump();

// ---------------- object-слоты ----------------
// obj-слот в кадре — полноценный NObject (placement new)
void nl_jit_obj_init(NObject* p) { new (p) NObject(); }
void nl_jit_obj_dtor(NObject* p) { p->~NObject(); }
void nl_jit_obj_set(NObject* d, const NObject* s) { *d = *s; }

// ---------------- переменные ----------------
// указатель на слот vars — для чтения И записи
NObject* nl_jit_var(Nvm* vm, const char* name) {
    return &vm->vars[name];
}
void nl_jit_var_get(Nvm* vm, const char* name, NObject* out) {
    auto it = vm->vars.find(name);
    *out = it != vm->vars.end() ? it->second : NObject();
}
void nl_jit_var_set(Nvm* vm, const char* name,
                    const NObject* v) {
    vm->vars[name] = *v;
}

// ---------------- конверсии ----------------
long long nl_jit_to_int(const NObject* v) { return v->asInt(); }
void nl_jit_of_int(NObject* out, long long v) {
    *out = NObject(v);
}
void nl_jit_of_double_bits(NObject* out, long long bits) {
    double d;
    std::memcpy(&d, &bits, 8);
    *out = NObject(d);
}
// fp-домен: аргументы double в xmm0/xmm1, возврат в xmm0
void nl_jit_of_dbl(NObject* out, double v) {
    *out = NObject(v);
}
double nl_jit_to_dbl(const NObject* v) { return v->asDouble(); }
double nl_jit_fmod(double a, double b) {
    return std::fmod(a, b);
}
void nl_jit_div_zero(Nvm* vm) {
    vm->pending_error = "деление на ноль";
}
void nl_jit_str_init(NObject* out, const char* s, long long n) {
    *out = NObject(std::string(s, (size_t)n));
}
void nl_jit_to_str(NObject* out, const NObject* v) {
    *out = nstr(*v);
}
// строковая склейка: slot += литерал / объект
void nl_jit_strcat_lit(NObject* slot, const char* s,
                       long long n) {
    if (!slot->isStr()) *slot = NObject(std::string{});
    std::get<std::string>(slot->_value).append(s, (size_t)n);
}
void nl_jit_strcat_obj(NObject* slot, const NObject* rhs) {
    if (!slot->isStr()) *slot = NObject(nstr(*slot).asStr());
    std::get<std::string>(slot->_value) += nstr(*rhs).asStr();
}

// ---------------- арифметика / сравнения ----------------
// out = arith(op, l, r) — та же семантика что eval
void nl_jit_arith(int op, const NObject* l, const NObject* r,
                  NObject* out) {
    *out = nl_arith(op, *l, *r, 0);
}
long long nl_jit_cmp(int op, const NObject* l,
                     const NObject* r) {
    if (l->isNum() && r->isNum()) {
        double a = l->asDouble(), b = r->asDouble();
        switch (op) {
        case 0: return a < b;
        case 1: return a <= b;
        case 2: return a > b;
        case 3: return a >= b;
        case 4: return a == b;
        case 5: return a != b;
        }
    }
    if (l->isStr() && r->isStr()) {
        const std::string& a = l->asStr(), &b = r->asStr();
        switch (op) {
        case 0: return a < b;
        case 1: return a <= b;
        case 2: return a > b;
        case 3: return a >= b;
        case 4: return a == b;
        case 5: return a != b;
        }
    }
    switch (op) {
    case 4: return l->_value == r->_value;
    case 5: return !(l->_value == r->_value);
    }
    return 0;
}

// ---------------- списки ----------------
void nl_jit_list_init(NObject* out) {
    *out = NObject(std::make_shared<std::vector<NObject>>());
}
void nl_jit_list_push_ll(NObject* list, long long v) {
    nl_jit_layout_dump();
    if (!list->isList()) nl_jit_list_init(list);
    list->asList()->push_back(NObject(v));
}
void nl_jit_list_push_obj(NObject* list, const NObject* v) {
    if (!list->isList()) nl_jit_list_init(list);
    list->asList()->push_back(*v);
}
long long nl_jit_list_size(const NObject* list) {
    return list->isList() ? (long long)list->asList()->size()
                          : 0;
}
// ---------------- быстрый путь списков ----------------
// data() списка — один раз на цикл, дальше jit читает
// элементы напрямую (vector<NObject> — плоский буфер)
const NObject* nl_jit_list_data(const NObject* v) {
    nl_jit_layout_dump();
    if (!v->isList()) return nullptr;
    return v->asList()->data();
}
// layout-пробники — зовутся на этапе компиляции jit,
// оффсеты варианта не хардкодим: ll-пейлоад ищем по
// маркеру, индекс-дискриминант — по различию типов
long long nl_jit_layout_ll_off() {
    NObject a((long long)0x1122334455667788ll);
    const unsigned char* p =
        (const unsigned char*)&a._value;
    for (size_t i = 0; i + 8 <= sizeof(NObject::Data); ++i)
        if (*(const long long*)(p + i) ==
            0x1122334455667788ll)
            return (long long)i;
    return -1;
}
long long nl_jit_layout_idx_off() {
    // байт-дискриминант: оффсет, где во всех образцах
    // лежит _value.index() — padding-mусор не считается
    NObject a((long long)0);
    NObject b(1.5);
    NObject c(std::string("x"));
    NObject d(false);
    const unsigned char* pa =
        (const unsigned char*)&a._value;
    const unsigned char* pb =
        (const unsigned char*)&b._value;
    const unsigned char* pc =
        (const unsigned char*)&c._value;
    const unsigned char* pd =
        (const unsigned char*)&d._value;
    unsigned char ia = (unsigned char)a._value.index();
    unsigned char ib = (unsigned char)b._value.index();
    unsigned char ic = (unsigned char)c._value.index();
    unsigned char id = (unsigned char)d._value.index();
    for (size_t i = 0; i < sizeof(NObject::Data); ++i)
        if (pa[i] == ia && pb[i] == ib &&
            pc[i] == ic && pd[i] == id)
            return (long long)i;
    return -1;
}
long long nl_jit_layout_this_off() {
    return (long long)offsetof(NObject, _this);
}
long long nl_jit_layout_nobj_size() {
    return (long long)sizeof(NObject);
}
// layout-диагностика — одноразовый дамп при DEBUG
void nl_jit_layout_dump() {
    if (!getenv("NLANG_JIT_DEBUG")) return;
    static bool done = false;
    if (done) return;
    done = true;
    fprintf(stderr, "layout: nobj=%zu ll=%lld "
            "idx=%lld this=%lld\n",
            sizeof(NObject), nl_jit_layout_ll_off(),
            nl_jit_layout_idx_off(),
            nl_jit_layout_this_off());
    // сырые байты variant для int/double/string
    NObject objs[3] = {NObject((long long)5),
                       NObject(1.5),
                       NObject(std::string("hi"))};
    for (int k = 0; k < 3; ++k) {
        const unsigned char* p =
            (const unsigned char*)&objs[k]._value;
        for (size_t i = 0; i < sizeof(NObject::Data); ++i)
            fprintf(stderr, "%02x", p[i]);
        fprintf(stderr, "\n");
    }
}
long long nl_jit_at_ll(const NObject* list, long long i) {
    if (!list->isList() || i < 0 ||
        (size_t)i >= list->asList()->size())
        return 0;
    return (*list->asList())[i].asInt();
}
void nl_jit_at_obj(const NObject* list, long long i,
                   NObject* out) {
    if (!list->isList() || i < 0 ||
        (size_t)i >= list->asList()->size()) {
        *out = NObject();
        return;
    }
    *out = (*list->asList())[i];
}

// ---------------- вызовы ----------------
// named-вызов: args — массив NObject* (уже собранные),
// результат кладём в thread_local temp и возвращаем
// указатель — jit копирует в свой out-слот.
static thread_local NObject nl_jit_call_tmp;
NObject* nl_jit_call_named(Nvm* vm, const char* name,
                           NObject** argv, int n) {
    static const bool dbg = std::getenv("NLANG_JIT_DEBUG");
    NArgs a;
    for (int i = 0; i < n; ++i) a.pos.push_back(*argv[i]);
    auto it = vm->vars.find(name);
    if (dbg)
        fprintf(stderr, "call_named %s n=%d found=%d\n",
                name, n,
                (int)(it != vm->vars.end()));
    if (it != vm->vars.end() && it->second.isFunc())
        nl_jit_call_tmp = it->second.call(*vm, a);
    else
        nl_jit_call_tmp = NObject();
    if (dbg) fprintf(stderr, "call_named %s done\n", name);
    return &nl_jit_call_tmp;
}
// прямой вызов по NObject* (разрешён при компиляции —
// функция живёт в vars, указатель стабилен)
NObject* nl_jit_call_obj(Nvm* vm, NObject* callee,
                         NObject** argv, int n) {
    NArgs a;
    for (int i = 0; i < n; ++i) a.pos.push_back(*argv[i]);
    nl_jit_call_tmp = callee->call(*vm, a);
    return &nl_jit_call_tmp;
}
// метод по имени на объекте: push/size/... через обычный
// memberLookup-путь: временный getter eval.
// здесь только int-частые методы списков/строк:
long long nl_jit_size(const NObject* v) {
    if (v->isList()) return (long long)v->asList()->size();
    if (v->isStr()) return (long long)v->asStr().size();
    if (v->isDict()) return (long long)v->asDict()->size();
    return 0;
}

// ---------------- escape hatch ----------------
// не-tintable выражение: обратно в интерпретатор —
// та же программа, просто подпрограмма
long long nl_jit_eval_int(Nvm* vm, Node* n) {
    try {
        return n->eval(*vm).asInt();
    } catch (std::exception& e) {
        // исключение через jit-кадр не разматывается
        // (нет unwind-таблиц) — протокол pending_error,
        // call() перевбросит после выхода из jit-кода
        if (std::getenv("NLANG_JIT_DEBUG"))
            fprintf(stderr, "eval_int exc: %s\n", e.what());
        vm->pending_error = e.what();
        return 0;
    }
}
void nl_jit_eval_obj(Nvm* vm, Node* n, NObject* out) {
    try {
        *out = n->eval(*vm);
    } catch (std::exception& e) {
        if (std::getenv("NLANG_JIT_DEBUG"))
            fprintf(stderr, "eval_obj exc: %s\n", e.what());
        vm->pending_error = e.what();
        *out = NObject();
    }
}
// не-tintable оператор: 0=норм 1=return 2=break 3=continue
// 4=ошибка (pending_error — перевбросит call())
long long nl_jit_exec_stmt(Nvm* vm, Node* n) {
    try {
        n->eval(*vm);
    } catch (BreakException&) {
        return 2;
    } catch (ContinueException&) {
        return 3;
    } catch (std::exception& e) {
        if (std::getenv("NLANG_JIT_DEBUG"))
            fprintf(stderr, "exec_stmt exc: %s\n", e.what());
        vm->pending_error = e.what();
        return 4;
    }
    if (vm->return_pending) {
        // флаг снимаем, значение ОСТАВЛЯЕМ — caller
        // заберёт его через nl_jit_retval_int/obj
        vm->return_pending = false;
        return 1;
    }
    return 0;
}
long long nl_jit_retval_int(Nvm* vm) {
    return vm->return_value.asInt();
}
void nl_jit_retval_obj(Nvm* vm, NObject* out) {
    *out = vm->return_value;
}

} // extern "C"
