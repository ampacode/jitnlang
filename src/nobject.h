#pragma once
// Порт nobject.py — NObject + функциональные значения.
// _value — variant; _this — таблица методов/полей (как у py-NObject).
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#include <atomic>
#include <functional>
#include "type.h"

struct NObject;
struct NFunction;
class Nvm;
struct Node;

using NObjectPtr = NObject*;
using ListPtr = std::shared_ptr<std::vector<NObject>>;
using DictPtr = std::shared_ptr<std::map<std::string, NObject>>;
using FuncPtr = std::shared_ptr<NFunction>;
using NodePtr = std::shared_ptr<Node>;

struct NArgs;

// Нативная реализация — сюда компилятор подставит
// скомпилированный код (tiered hot-swap, точка А).
// (NArgs здесь ещё неполный — для указателя достаточно)
using NativeFn = NObject (*)(Nvm&, const NArgs&);

struct NObject {
    using Data = std::variant<std::monostate, bool, long long, double,
                              std::string, ListPtr, DictPtr, FuncPtr>;
    Data _value;
    // методы/поля — как _this в nobject.py
    std::shared_ptr<std::unordered_map<std::string, NObject>> _this;

    NObject();
    NObject(bool b);
    NObject(int i);
    NObject(long long i);
    NObject(double d);
    NObject(const char* s);
    NObject(std::string s);
    NObject(ListPtr l);
    NObject(DictPtr d);
    NObject(FuncPtr f);

    // --- типы ---
    bool isNull() const { return std::holds_alternative<std::monostate>(_value); }
    bool isBool() const { return std::holds_alternative<bool>(_value); }
    bool isInt() const { return std::holds_alternative<long long>(_value); }
    bool isDouble() const { return std::holds_alternative<double>(_value); }
    bool isNum() const { return isInt() || isDouble(); }
    bool isStr() const { return std::holds_alternative<std::string>(_value); }
    bool isList() const { return std::holds_alternative<ListPtr>(_value); }
    bool isDict() const { return std::holds_alternative<DictPtr>(_value); }
    bool isFunc() const { return std::holds_alternative<FuncPtr>(_value); }

    // --- доступ ---
    long long asInt() const;
    double asDouble() const;
    bool asBool() const;
    const std::string& asStr() const { return std::get<std::string>(_value); }
    ListPtr asList() const { return std::get<ListPtr>(_value); }
    DictPtr asDict() const { return std::get<DictPtr>(_value); }
    FuncPtr asFunc() const { return std::get<FuncPtr>(_value); }

    // поле/метод из _this (nullptr если нет)
    NObject* attr(const std::string& name);
    // таблица атрибутов по запросу — создаёт при первом обращении
    std::unordered_map<std::string, NObject>& this_map() {
        if (!_this)
            _this = std::make_shared<
                std::unordered_map<std::string, NObject>>();
        return *_this;
    }

    // вызов: nobject(args) — как __call__ в nobject.py
    NObject call(Nvm& vm, const NArgs& args) const;

    // nval() — «сырое» значение; в C++ эквивалент — сам NObject,
    // оставлено для структурного сходства с py-версией.
    NObject nval() { return *this; }
};

struct NFunction {
    std::string name;
    // параметры: имя -> {type, base(default-выражение)}
    std::vector<std::pair<std::string, std::pair<std::string, NodePtr>>> args;
    std::vector<NodePtr> statements;   // тело (пусто для нативных)
    NObject bound_self;                  // bound-метод: self-объект
    std::atomic<NativeFn> impl{nullptr}; // hot-swap точка
    // нативная реализация с захватом (модули eval-пути)
    std::function<NObject(Nvm&, const NArgs&)> stdimpl;
    // горячая перезагрузка: файл-источник и mtime на момент загрузки
    std::string hot_path;
    long long hot_mtime = -1;
    // hot-swap: AST-декларация для перекомпиляции в dll
    NodePtr decl_node;
    std::atomic<long long> hot_calls{0}; // счётчик вызовов
    // 0=нет 1=компилируется в фоне 2=заменена на dll -1=не вышло
    std::atomic<int> hot_state{0};
    // указатель на dll-функцию + типы аргументов
    // (пишутся до store hot_state=2, дальше только читаются)
    std::atomic<void*> hot_abi{nullptr};
    std::shared_ptr<const std::vector<int>> hot_kinds;
    std::string hot_ret_model; // ret-тип — модель: привязать методы
    // JIT "anti nlang": jit_code держит исполняемый буфер
    // живым, jit — вход (long long fn(Nvm*, args, retobj))
    // 0 — не проверяли, 1 — jit готов, -1 — не tintable
    std::shared_ptr<void> jit_code;
    long long (*jit)(Nvm*, const long long*,
                     NObject*) = nullptr;
    std::atomic<int> jit_state{0};
};

// small-vector: N слотов inline, heap только при переполнении.
// убирает malloc на каждый вызов функции (основная цена
// рекурсии — после lock/return, дальше только аллокации)
template<typename T, size_t N = 4>
struct SmallVec {
    T buf[N];
    T* heap = nullptr;
    size_t n = 0, cap = N;

    SmallVec() = default;
    SmallVec(const SmallVec& o) : n(o.n), cap(N) {
        T* d = buf;
        if (n > N) { heap = new T[n]; d = heap; cap = n; }
        for (size_t i = 0; i < n; ++i) d[i] = o.data()[i];
    }
    SmallVec& operator=(const SmallVec& o) {
        if (this == &o) return *this;
        delete[] heap; heap = nullptr;
        n = o.n; cap = N;
        T* d = buf;
        if (n > N) { heap = new T[n]; d = heap; cap = n; }
        for (size_t i = 0; i < n; ++i) d[i] = o.data()[i];
        return *this;
    }
    SmallVec(SmallVec&& o) noexcept : n(o.n), cap(o.cap) {
        if (o.heap) { heap = o.heap; o.heap = nullptr; }
        else for (size_t i = 0; i < n; ++i) buf[i] = std::move(o.buf[i]);
        o.n = 0;
    }
    SmallVec& operator=(SmallVec&& o) noexcept {
        if (this == &o) return *this;
        delete[] heap; heap = o.heap; o.heap = nullptr;
        n = o.n; cap = o.cap;
        if (!heap) { cap = N; for (size_t i = 0; i < n; ++i) buf[i] = std::move(o.buf[i]); }
        o.n = 0;
        return *this;
    }
    ~SmallVec() { delete[] heap; }

    T* data() { return heap ? heap : buf; }
    const T* data() const { return heap ? heap : buf; }
    size_t size() const { return n; }
    bool empty() const { return n == 0; }
    T& operator[](size_t i) { return data()[i]; }
    const T& operator[](size_t i) const { return data()[i]; }
    T& back() { return data()[n - 1]; }
    const T& back() const { return data()[n - 1]; }
    void push_back(const T& v) {
        grow();
        data()[n++] = v;
    }
    void push_back(T&& v) {
        grow();
        data()[n++] = std::move(v);
    }
    void clear() { n = 0; }
    T* begin() { return data(); }
    T* end() { return data() + n; }
    const T* begin() const { return data(); }
    const T* end() const { return data() + n; }
private:
    void grow() {
        if (n < cap) return;
        size_t nc = cap * 2;
        T* nh = new T[nc];
        for (size_t i = 0; i < n; ++i) nh[i] = std::move(data()[i]);
        delete[] heap;
        heap = nh; cap = nc;
    }
};

// Аргументы вызова: позиционные + именованные (как dict args в python)
struct NArgs {
    SmallVec<NObject> pos;
    std::map<std::string, NObject> named;
    NObject self; // bound-метод: объект, к которому привязан вызов
};

// поток управления / ошибки
struct ReturnException { NObject value; };
struct BreakException {};
struct ContinueException {};
struct NLangError {
    std::string msg;
    int line = 0;
};

NObject nstr(const NObject& v); // to-string (str() в nlang)
