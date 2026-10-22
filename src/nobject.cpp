#include "nobject.h"
#include "nvm.h"
#include "ninclude.h"
#include "hotswap.h"
#include "jit.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include <cstdio>

// ---------- ctors ----------
// _this — ленивый: пустая shared_ptr (16 байт, без аллокации).
// было: make_shared на КАЖДЫЙ NObject — ~2 malloc на каждый
// временный объект, главная цена eval-циклов.
NObject::NObject() : _value(std::monostate{}) {}

#define NOBJ_CTOR(T, V) \
    NObject::NObject(T v_) : _value(V) {}

NOBJ_CTOR(bool, v_)
NOBJ_CTOR(int, (long long)v_)
NOBJ_CTOR(long long, v_)
NOBJ_CTOR(double, v_)
NOBJ_CTOR(const char*, std::string(v_))
NOBJ_CTOR(std::string, std::move(v_))
NOBJ_CTOR(ListPtr, std::move(v_))
NOBJ_CTOR(DictPtr, std::move(v_))
NOBJ_CTOR(FuncPtr, std::move(v_))
#undef NOBJ_CTOR

long long NObject::asInt() const {
    if (isInt()) return std::get<long long>(_value);
    if (isDouble()) return (long long)std::get<double>(_value);
    if (isBool()) return std::get<bool>(_value) ? 1 : 0;
    if (isStr()) { try { return std::stoll(asStr()); } catch (...) { return 0; } }
    return 0;
}
double NObject::asDouble() const {
    if (isDouble()) return std::get<double>(_value);
    if (isInt()) return (double)std::get<long long>(_value);
    if (isBool()) return std::get<bool>(_value) ? 1.0 : 0.0;
    if (isStr()) { try { return std::stod(asStr()); } catch (...) { return 0.0; } }
    return 0.0;
}
bool NObject::asBool() const {
    if (isBool()) return std::get<bool>(_value);
    if (isInt()) return std::get<long long>(_value) != 0;
    if (isDouble()) return std::get<double>(_value) != 0.0;
    if (isStr()) return !asStr().empty();
    if (isList()) return !asList()->empty();
    if (isDict()) return !asDict()->empty();
    if (isFunc()) return true;
    return false;
}

NObject* NObject::attr(const std::string& name) {
    if (!_this) return nullptr;
    auto it = _this->find(name);
    return it != _this->end() ? &it->second : nullptr;
}

// ---------- to-string ----------
NObject nstr(const NObject& v) {
    if (v.isNull()) return NObject(std::string("null"));
    if (v.isBool()) return NObject(v.asBool() ? "true" : "false");
    if (v.isInt()) return NObject(std::to_string(std::get<long long>(v._value)));
    if (v.isDouble()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%g", v.asDouble());
        return NObject(std::string(buf));
    }
    if (v.isStr()) return v;
    if (v.isList()) {
        std::string s = "[";
        auto l = v.asList();
        for (size_t i = 0; i < l->size(); ++i) {
            if (i) s += ", ";
            s += nstr((*l)[i]).asStr();
        }
        return NObject(s + "]");
    }
    if (v.isDict()) {
        std::string s = "{";
        bool first = true;
        for (auto& kv : *v.asDict()) {
            if (!first) s += ", ";
            first = false;
            s += kv.first + ": " + nstr(kv.second).asStr();
        }
        return NObject(s + "}");
    }
    if (v.isFunc()) return NObject(std::string("<fn ") + v.asFunc()->name + ">");
    return NObject(std::string("?"));
}

// ---------- вызов ----------
// исполняет тело nlang-функции; прототип — реализован в eval.cpp
NObject execFunctionBody(Nvm& vm, FuncPtr fn, const NArgs& args);

NObject NObject::call(Nvm& vm, const NArgs& args) const {
    if (!isFunc()) {
        // модуль-словарь с "__call__" вызывается как функция:
        // `str(x)` достаётся встроенному str даже когда
        // `using <str>` занял имя под модуль
        if (isDict()) {
            auto d = asDict();
            auto it = d->find("__call__");
            if (it != d->end() && it->second.isFunc())
                return it->second.call(vm, args);
        }
        return *this;
    }
    FuncPtr fn = asFunc();
    // bound-метод: прокидываем bound_self через a.self.
    // NArgs копируем ТОЛЬКО если нужно подставить self —
    // обычные вызовы (fibo) идут по ссылке без копии.
    NArgs tmp;
    const NArgs* pa = &args;
    if (!fn->bound_self.isNull() && args.self.isNull()) {
        tmp = args;
        tmp.self = fn->bound_self;
        pa = &tmp;
    }
    const NArgs& a = *pa;
    // hot-reload: файл изменился — перезагружаем и зовём свежую версию
    if (!fn->hot_path.empty() && vm.hot_reload) {
        long long mt = nlang_mtime(fn->hot_path);
        if (mt != fn->hot_mtime) {
            nlang_include_eval(vm, fn->hot_path);
            auto it = vm.vars.find(fn->name);
            if (it != vm.vars.end() && it->second.isFunc() &&
                it->second.asFunc().get() != fn.get())
                return it->second.call(vm, a);
        }
    }
    // hot-swap: уже заменена на dll (опубликовано фоновым потоком)
    if (fn->hot_state.load(std::memory_order_acquire) == 2) {
        auto callee = reinterpret_cast<NLHotCall>(
            fn->hot_abi.load(std::memory_order_acquire));
        if (callee && fn->hot_kinds)
            return nlang_hot_invoke(callee, *fn->hot_kinds, a,
                                    fn->hot_ret_model);
    }
    NativeFn impl = fn->impl.load(std::memory_order_acquire);
    if (impl) return impl(vm, a);          // скомпилированная версия
    if (fn->stdimpl) return fn->stdimpl(vm, a); // нативная с захватом
    // hot-swap: горячая интерпретируемая функция -> dll (в фоне)
    if (vm.hot_swap && fn->decl_node &&
        fn->hot_state.load(std::memory_order_acquire) == 0 &&
        fn->hot_calls.fetch_add(1, std::memory_order_relaxed) + 1 >=
            vm.hot_threshold)
        nlang_maybe_hotswap(vm, fn);
    // mini-JIT "anti nlang": компилируем tintable-функцию в
    // машинный код при первом вызове (миллисекунды, без g++).
    // маршаллим только int-аргументы точного числа параметров —
    // иначе честный интерпретируемый путь.
    if (fn->jit_state.load(std::memory_order_relaxed) == 0) {
        static const bool no_jit = std::getenv("NLANG_NO_JIT");
        static const char* jit_only = std::getenv("NLANG_JIT_ONLY");
        bool only_ok = !jit_only ||
            strstr(fn->name.c_str(), jit_only) != nullptr;
        if (fn->decl_node && !no_jit && only_ok) {
            auto code = nl_jit_compile(fn.get());
            if (code) {
                fn->jit_code = code;
                fn->jit = code->entry;
                fn->jit_state.store(1, std::memory_order_relaxed);
            } else {
                fn->jit_state.store(-1, std::memory_order_relaxed);
            }
        } else {
            fn->jit_state.store(-1, std::memory_order_relaxed);
        }
    }
    // трассировка (qt.execTrace): обходим jit — построчное
    // исполнение нужно для подсветки «исполняется сейчас»
    if (fn->jit && !vm.exec_trace && a.named.empty() &&
        a.pos.size() == fn->args.size() && a.pos.size() <= 32) {
        long long av[32];
        size_t i = 0;
        for (; i < a.pos.size(); ++i) {
            if (!a.pos[i].isInt() && !a.pos[i].isBool()) break;
            av[i] = a.pos[i].asInt();
        }
        if (i == a.pos.size()) {
            // retobj: объектный результат пишется сюда —
            // int возвращается через rax
            NObject ret;
            long long r = fn->jit(&vm, av, &ret);
            if (!vm.pending_error.empty()) {
                std::string e = std::move(vm.pending_error);
                vm.pending_error.clear();
                if (std::getenv("NLANG_JIT_DEBUG"))
                    fprintf(stderr, "pending_error: %s\n",
                            e.c_str());
                throw NLangError{std::move(e), 0};
            }
            return ret.isNull() ? NObject(r) : ret;
        }
    }
    return execFunctionBody(vm, fn, a);    // интерпретируем тело
}
