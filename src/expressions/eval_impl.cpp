// Реализации eval/cpp для цепочек: GetterExpression,
// ArrayAccessExpression, BinaryExpression.eval,
// FunctionDeclaration/FunctionExpression.eval, ForeachStatement.eval
#include "../node.h"
#include "../nvm.h"
#include "variable.h"
#include "string.h"
#include "calling.h"
#include "getter.h"
#include "binary.h"
#include "function.h"
#include "list.h"
#include "conditional.h"
#include "../statements/function.h"
#include "../statements/foreach.h"
#include "../statements/_class.h"
#include <cmath>
#include <thread>
#include <mutex>
#include <map>

// ================= bound-методы (как _this в nobject.py) =================

static FuncPtr boundNative(const std::string& name, NativeFn fn,
                           NObject self) {
    auto f = std::make_shared<NFunction>();
    f->name = name;
    f->bound_self = std::move(self);
    f->impl.store(fn, std::memory_order_release);
    return f;
}

static NObject m_append(Nvm&, const NArgs& a) {
    a.self.asList()->push_back(a.pos.empty() ? NObject() : a.pos[0]);
    return NObject();
}
static NObject m_pop(Nvm&, const NArgs& a) {
    auto l = a.self.asList();
    if (l->empty()) throw NLangError{"pop из пустого списка"};
    NObject v = l->back();
    l->pop_back();
    return v;
}
static NObject m_remove(Nvm&, const NArgs& a) {
    auto l = a.self.asList();
    if (!a.pos.empty())
        for (auto it = l->begin(); it != l->end(); ++it)
            if (nstr(*it).asStr() == nstr(a.pos[0]).asStr()) {
                l->erase(it);
                break;
            }
    return NObject();
}
static NObject m_size(Nvm&, const NArgs& a) {
    const NObject& s = a.self;
    if (s.isList()) return NObject((long long)s.asList()->size());
    if (s.isStr()) return NObject((long long)s.asStr().size());
    if (s.isDict()) return NObject((long long)s.asDict()->size());
    return NObject(0);
}
static NObject m_strip(Nvm&, const NArgs& a) {
    const std::string& s = a.self.asStr();
    size_t b = s.find_first_not_of(" \t\n\r");
    size_t e = s.find_last_not_of(" \t\n\r");
    return NObject(b == std::string::npos ? std::string()
                                          : s.substr(b, e - b + 1));
}
static NObject m_replace(Nvm&, const NArgs& a) {
    std::string r = a.self.asStr();
    if (a.pos.size() < 2) return NObject(r);
    std::string from = a.pos[0].asStr(), to = a.pos[1].asStr();
    if (from.empty()) return NObject(r);
    size_t p = 0;
    while ((p = r.find(from, p)) != std::string::npos) {
        r.replace(p, from.size(), to);
        p += to.size();
    }
    return NObject(r);
}
static NObject m_split(Nvm&, const NArgs& a) {
    auto l = std::make_shared<std::vector<NObject>>();
    const std::string& s = a.self.asStr();
    std::string d = a.pos.empty() ? " " : a.pos[0].asStr();
    size_t p = 0, f;
    while ((f = s.find(d, p)) != std::string::npos) {
        l->push_back(NObject(s.substr(p, f - p)));
        p = f + d.size();
    }
    l->push_back(NObject(s.substr(p)));
    return NObject(l);
}
static NObject m_get(Nvm&, const NArgs& a) {
    auto d = a.self.asDict();
    if (a.pos.empty()) return NObject();
    auto it = d->find(nstr(a.pos[0]).asStr());
    return it != d->end() ? it->second : NObject();
}
static NObject m_str(Nvm&, const NArgs& a) { return nstr(a.self); }
static NObject m_times(Nvm& vm, const NArgs& a) {
    long long n = a.self.asInt();
    if (!a.pos.empty() && a.pos[0].isFunc()) {
        NArgs cb;
        for (long long i = 0; i < n; ++i) {
            cb.pos.clear();
            cb.pos.push_back(NObject(i));
            a.pos[0].call(vm, cb);
        }
    }
    return NObject();
}

// резолв имени метода/поля на объекте — аналог res._this.get(name)
// entryLookup: только пользовательские поля (attr + dict entries)
static NObject* entryLookup(NObject& obj, const std::string& name) {
    if (NObject* p = obj.attr(name)) return p;
    if (obj.isDict()) {
        auto d = obj.asDict();
        auto it = d->find(name);
        if (it != d->end()) return &it->second;
    }
    return nullptr;
}
// directMethod: встроенные методы -> NativeFn без обёртки в
// NFunction (иначе каждый v.push(x) = make_shared + bound_self)
static NativeFn directMethod(const NObject& obj,
                             const std::string& name) {
    if (obj.isList()) {
        if (name == "append" || name == "push") return &m_append;
        if (name == "pop") return &m_pop;
        if (name == "remove") return &m_remove;
        if (name == "size" || name == "length") return &m_size;
    }
    if (obj.isStr()) {
        if (name == "size" || name == "length") return &m_size;
        if (name == "strip") return &m_strip;
        if (name == "replace") return &m_replace;
        if (name == "split") return &m_split;
    }
    if (obj.isDict()) {
        if (name == "get") return &m_get;
        if (name == "size" || name == "length") return &m_size;
    }
    if (obj.isNum()) {
        if (name == "times") return &m_times;
        if (name == "str") return &m_str;
        if (name == "size") return &m_size;
    }
    return nullptr;
}
static NObject memberLookup(NObject& obj, const std::string& name) {
    if (NObject* p = entryLookup(obj, name)) return *p;
    if (obj.isList()) {
        if (name == "append" || name == "push")
            return NObject(boundNative(name, &m_append, obj));
        if (name == "pop") return NObject(boundNative(name, &m_pop, obj));
        if (name == "remove") return NObject(boundNative(name, &m_remove, obj));
        if (name == "size" || name == "length")
            return NObject(boundNative(name, &m_size, obj));
    }
    if (obj.isStr()) {
        if (name == "size" || name == "length")
            return NObject(boundNative(name, &m_size, obj));
        if (name == "strip") return NObject(boundNative(name, &m_strip, obj));
        if (name == "replace") return NObject(boundNative(name, &m_replace, obj));
        if (name == "split") return NObject(boundNative(name, &m_split, obj));
    }
    if (obj.isDict()) {
        if (name == "get") return NObject(boundNative(name, &m_get, obj));
        if (name == "size" || name == "length")
            return NObject(boundNative(name, &m_size, obj));
    }
    if (obj.isNum()) {
        if (name == "times") return NObject(boundNative(name, &m_times, obj));
        if (name == "str") return NObject(boundNative(name, &m_str, obj));
        if (name == "size") return NObject(boundNative(name, &m_size, obj));
    }
    return NObject();
}

// ================= ArrayAccessExpression =================

Emitted ArrayAccessExpression::cpp(Nvm& vm) {
    // a[i] -> nlAt(a, (i)) — 1-индексация + проверка границ
    std::string code = variable_cpp;
    for (auto& ac : access) {
        std::string a = ac->cpp(vm).getCpp();
        NodePtr inner = ac;
        if (auto g = static_cast<GetterExpression*>(inner->asGetter()))
            if (g->list.size() == 1) inner = g->list[0];
        if (static_cast<StringExpression*>(inner->asStrExpr()))
            code = "nlAt(" + code + ", " + a + ")";
        else
            code = "nlAt(" + code + ", (" + a + "))";
    }
    return Emitted(code);
}

NObject ArrayAccessExpression::eval(Nvm& vm) {
    NObject res = variable;
    for (auto& ac : access) {
        NObject idx = ac->eval(vm);
        if (res.isList()) {
            auto l = res.asList();
            long long i = idx.asInt();
            if (i < 0 || (size_t)i >= l->size())
                throw NLangError{"индекс вне диапазона: " +
                                 std::to_string(idx.asInt()), line};
            res = (*l)[i];
        } else if (res.isStr()) {
            long long i = idx.asInt();
            const std::string& s = res.asStr();
            if (i < 0 || (size_t)i >= s.size())
                throw NLangError{"индекс вне диапазона", line};
            res = NObject(std::string(1, s[i]));
        } else if (res.isDict()) {
            auto d = res.asDict();
            auto it = d->find(nstr(idx).asStr());
            res = it != d->end() ? it->second : NObject();
        } else {
            throw NLangError{"индексация не-коллекции", line};
        }
    }
    return res;
}

// ================= GetterExpression =================

// dimN для eval-пути: общие dict-хранилища и потоки
static std::map<int, std::map<std::string, DictPtr>> eval_dim_store;
static std::vector<std::thread> eval_dim_threads;
static std::mutex eval_dim_lock;

NObject GetterExpression::eval(Nvm& vm) {
    // --- dimN домены (eval-путь) ---
    if (vm.dim_module_used && !list.empty()) {
        if (auto v0 = static_cast<VariableExpression*>(list[0]->asVar())) {
            int dimN = -1;
            std::string slot;
            const std::string& h = v0->text;
            if (h.rfind("dim", 0) == 0) {
                size_t p = 3;
                while (p < h.size() && isdigit((unsigned char)h[p])) ++p;
                if (p > 3) {
                    dimN = std::stoi(h.substr(3, p - 3));
                    if (p < h.size() && h[p] == '_')
                        slot = h.substr(p + 1);
                }
            }
            if (dimN > 0 && !slot.empty()) {
                NObject res = NObject(eval_dim_store[dimN][slot]
                    ? eval_dim_store[dimN][slot]
                    : (eval_dim_store[dimN][slot] =
                        std::make_shared<std::map<std::string, NObject>>()));
                // далее обычная цепочка поверх dict
                for (size_t i = 1; i < list.size(); ++i) {
                    NodePtr expr = list[i];
                    if (auto acc = static_cast<ArrayAccessExpression*>(expr->asArr())) {
                        acc->variable = res;
                        res = acc->eval(vm);
                        continue;
                    }
                    if (auto call = static_cast<CallingExpression*>(expr->asCall())) {
                        const std::string& mn = call->str();
                        NArgs args;
                        for (auto& a : call->pos_args)
                            args.pos.push_back(a->eval(vm));
                        if (NObject* p = entryLookup(res, mn)) {
                            res = p->call(vm, args);
                        } else if (NativeFn f = directMethod(res, mn)) {
                            args.self = res;
                            res = f(vm, args);
                        } else {
                            res = memberLookup(res, mn).call(vm, args);
                        }
                        continue;
                    }
                    if (auto v = static_cast<VariableExpression*>(expr->asVar()))
                        res = memberLookup(res, v->text);
                }
                return res;
            }
            if (dimN > 0 && list.size() > 1) {
                if (auto call = static_cast<CallingExpression*>(list[1]->asCall())) {
                    std::string mname = call->str();
                    if (mname == "joinAll") {
                        std::lock_guard<std::mutex> lk(eval_dim_lock);
                        for (auto& t : eval_dim_threads)
                            if (t.joinable()) t.join();
                        eval_dim_threads.clear();
                        return NObject();
                    }
                    if (mname == "alloc" || mname == "allocFree" ||
                        mname == "reset")
                        return NObject(); // арена — только cpp-путь
                    NObject fn;
                    auto it = vm.vars.find(mname);
                    if (it != vm.vars.end()) fn = it->second;
                    NArgs args;
                    for (auto& a : call->pos_args)
                        args.pos.push_back(a->eval(vm));
                    if (dimN <= 1 || !fn.isFunc())
                        return fn.call(vm, args);
                    std::lock_guard<std::mutex> lk(eval_dim_lock);
                    eval_dim_threads.emplace_back(
                        [&vm, fn, args]() mutable { fn.call(vm, args); });
                    return NObject();
                }
            }
        }
    }
    NObject res = list[0]->eval(vm);
    for (size_t i = 1; i < list.size(); ++i) {
        NodePtr expr = list[i];
        if (auto acc = static_cast<ArrayAccessExpression*>(expr->asArr())) {
            acc->variable = res;
            res = acc->eval(vm);
            continue;
        }
        if (auto call = static_cast<CallingExpression*>(expr->asCall())) {
            const std::string& mn = call->str();
            NArgs args;
            for (auto& a : call->pos_args) args.pos.push_back(a->eval(vm));
            for (auto& a : call->named_args)
                args.named[a.first] = a.second->eval(vm);
            // поля/методы пользователя > встроенные direct > fallback
            if (NObject* p = entryLookup(res, mn)) {
                res = p->call(vm, args);
            } else if (NativeFn f = directMethod(res, mn)) {
                args.self = res;
                res = f(vm, args);
            } else {
                res = memberLookup(res, mn).call(vm, args);
            }
            continue;
        }
        // VariableExpression — поле/метод без вызова
        std::string name;
        if (auto v = static_cast<VariableExpression*>(expr->asVar()))
            name = v->text;
        else
            name = nstr(expr->eval(vm)).asStr();
        res = memberLookup(res, name);
    }
    return res;
}

// слот для записи: [i] или .field на dict
static NObject* slotRef(NObject& obj, NodePtr acc, Nvm& vm, int line) {
    if (auto a = static_cast<ArrayAccessExpression*>(acc->asArr())) {
        if (a->access.size() != 1)
            throw NLangError{"многомерное присваивание — позже", line};
        NObject idx = a->access[0]->eval(vm);
        if (obj.isList()) {
            auto l = obj.asList();
            long long i = idx.asInt();
            if (i < 0 || (size_t)i >= l->size())
                throw NLangError{"индекс вне диапазона", line};
            return &(*l)[i];
        }
        if (obj.isDict())
            return &(*obj.asDict())[nstr(idx).asStr()];
        throw NLangError{"присваивание по индексу не-коллекции", line};
    }
    if (auto v = static_cast<VariableExpression*>(acc->asVar())) {
        if (obj.isDict())
            return &(*obj.asDict())[v->text];
        throw NLangError{"нет поля '" + v->text + "'", line};
    }
    throw NLangError{"недопустимая цель присваивания", line};
}

NObject* GetterExpression::ref(Nvm& vm) {
    // x = v — единственный элемент-переменная
    if (list.size() == 1) {
        if (auto v = static_cast<VariableExpression*>(
                list[0]->asVar())) {
            // запись через указатель — dirty-track; stamp —
            // пропуск повторной записи ключа в том же кадре
            if (v->_rec_frame != vm.lock_frame() ||
                v->_rec_lockgen != vm.lock_gen) {
                vm.lock_record(v->text);
                v->_rec_frame = vm.lock_frame();
                v->_rec_lockgen = vm.lock_gen;
            }
            return v->slot(vm, true);
        }
    }
    NObject res;
    size_t start = 1;
    // dimN_var[...] — запись в eval-хранилище домена
    if (vm.dim_module_used) {
        if (auto v0 = static_cast<VariableExpression*>(list[0]->asVar())) {
            const std::string& h = v0->text;
            if (h.rfind("dim", 0) == 0) {
                size_t p = 3;
                while (p < h.size() && isdigit((unsigned char)h[p])) ++p;
                if (p > 3 && p < h.size() && h[p] == '_') {
                    int dimN = std::stoi(h.substr(3, p - 3));
                    std::string slot = h.substr(p + 1);
                    auto& d = eval_dim_store[dimN][slot];
                    if (!d)
                        d = std::make_shared<
                            std::map<std::string, NObject>>();
                    res = NObject(d);
                    start = 1;
                }
            }
        }
    }
    if (res.isNull())
        res = list[0]->eval(vm);
    // цепочка: резолвим всё кроме последнего элемента
    for (size_t i = start; i + 1 < list.size(); ++i) {
        NodePtr expr = list[i];
        if (auto acc = static_cast<ArrayAccessExpression*>(expr->asArr())) {
            acc->variable = res;
            res = acc->eval(vm);
        } else if (auto v = static_cast<VariableExpression*>(expr->asVar())) {
            res = memberLookup(res, v->text);
        } else {
            throw NLangError{"недопустимая цель присваивания", line};
        }
    }
    return slotRef(res, list.back(), vm, line);
}

void GetterExpression::assign(Nvm& vm, NObject value) {
    *ref(vm) = std::move(value);
}

// ================= BinaryExpression.eval =================

static NObject arith(int op, const NObject& l, const NObject& r, int line) {
    if (op == PLUS && (l.isStr() || r.isStr()))
        return NObject(nstr(l).asStr() + nstr(r).asStr());
    if (l.isNum() && r.isNum()) {
        bool dbl = l.isDouble() || r.isDouble();
        double ld = l.asDouble(), rd = r.asDouble();
        long long li = l.asInt(), ri = r.asInt();
        switch (op) {
            case PLUS: return dbl ? NObject(ld + rd) : NObject(li + ri);
            case MINUS: return dbl ? NObject(ld - rd) : NObject(li - ri);
            case STAR: return dbl ? NObject(ld * rd) : NObject(li * ri);
            case SLASH: {
                // как в py-интерпретаторе: 6/2 -> int 3, 7/2 -> 3.5
                if (rd == 0.0) throw NLangError{"деление на ноль", line};
                // int-путь без floor: один div + проверка остатка
                if (!dbl) {
                    if (li % ri == 0) return NObject(li / ri);
                    return NObject(ld / rd);
                }
                double d = ld / rd;
                if (d == std::floor(d)) return NObject((long long)d);
                return NObject(d);
            }
            case PERCENT:
                if (ri == 0) throw NLangError{"деление на ноль", line};
                return dbl ? NObject(std::fmod(ld, rd)) : NObject(li % ri);
            case DOUBLESLASH:
                // // — floor-деление, как в py-интерпретаторе
                // (res1 // res2) и в compiled ((int)floor(...))
                if (ri == 0) throw NLangError{"деление на ноль", line};
                return NObject((long long)std::floor(
                    (double)li / (double)ri));
            case STARSTAR: return NObject(std::pow(ld, rd));
        }
    }
    throw NLangError{"несовместимые операнды", line};
}

// общий код: интерпретатор и jit-runtime зовут одно
NObject nl_arith(int op, const NObject& l, const NObject& r,
                 int line) {
    return arith(op, l, r, line);
}

NObject BinaryExpression::eval(Nvm& vm) {
    // составные присваивания и ++/-- — через lvalue геттера
    switch (_type) {
        case PLUSEQ: case MINUSEQ: case STAREQ:
        case SLASHEQ: case PERCENTEQ:
        case INCREMENT: case DECREMENT: {
            auto g = static_cast<GetterExpression*>(
                expr1->asGetter());
            if (!g) throw NLangError{"недопустимая цель", line};
            NObject* slot = g->ref(vm);
            int base = _type == PLUSEQ ? PLUS :
                       _type == MINUSEQ ? MINUS :
                       _type == STAREQ ? STAR :
                       _type == SLASHEQ ? SLASH : PERCENT;
            if (_type == INCREMENT || _type == DECREMENT) {
                long long d = _type == INCREMENT ? 1 : -1;
                *slot = slot->isDouble() ? NObject(slot->asDouble() + d)
                                       : NObject(slot->asInt() + d);
                return *slot;
            }
            // += на строках: append в слоте — амортизированный
            // рост вместо новой строки на каждую итерацию (O(n²))
            NObject rhs = expr2->eval(vm);
            if (base == PLUS && slot->isStr() && rhs.isStr()) {
                std::get<std::string>(slot->_value) += rhs.asStr();
                return *slot;
            }
            *slot = arith(base, *slot, rhs, line);
            return *slot;
        }
        default:
            return arith(_type, expr1->eval(vm), expr2->eval(vm), line);
    }
}

// ================= функции =================

// исполнение тела nlang-функции — порт call() из FunctionDeclaration.eval
NObject execFunctionBody(Nvm& vm, FuncPtr fn, const NArgs& args) {
    vm.lock();
    vm.return_pending = false; // топ-уровневый return — не утечёт в вызовы
    // сохраняем старые значения имён параметров —
    // иначе рекурсия затирает n в плоском vars.
    // имя — по указателю (fn->args живёт дольше вызова,
    // без копий строк на каждый call)
    struct SavedParam { const std::string* name; bool had;
                        NObject old; NObject* ptr; };
    SmallVec<SavedParam, 8> saved; // без malloc до 8 параметров
    for (size_t i = 0; i < fn->args.size(); ++i) {
        const std::string& pname = fn->args[i].first;
        NodePtr base = fn->args[i].second.second;
        auto it0 = vm.vars.find(pname);
        bool had = it0 != vm.vars.end();
        NObject old = had ? it0->second : NObject();
        auto it = args.named.find(pname);
        // прямой записью — восстановление уже делает saved,
        // lock_record здесь лишний hash
        NObject& slot = vm.vars[pname];
        if (it != args.named.end()) {
            slot = it->second;
        } else if (i < args.pos.size()) {
            slot = args.pos[i];
        } else if (base) {
            slot = base->eval(vm);
        }
        saved.push_back({&pname, had, old, &slot});
    }
    // bound-метод (self = экземпляр класса): поля прокидываются
    // в скоуп как обычные переменные и пишутся обратно в dict
    std::vector<std::string> self_fields;
    std::map<std::string, std::pair<bool, NObject>> self_saved;
    DictPtr self_dict;
    // self: из args (явный) или bound_self функции
    const NObject& self =
        args.self.isNull() ? fn->bound_self : args.self;
    if (self.isDict()) {
        self_dict = self.asDict();
        for (auto& kv : *self_dict) {
            self_fields.push_back(kv.first);
            auto it = vm.vars.find(kv.first);
            self_saved[kv.first] = {it != vm.vars.end(),
                                    it != vm.vars.end()
                                        ? it->second : NObject()};
            vm.set(kv.first, kv.second);
        }
    }
    auto restore = [&] {
        // обратная запись полей в экземпляр
        if (self_dict)
            for (auto& k : self_fields) {
                auto it = vm.vars.find(k);
                if (it != vm.vars.end()) (*self_dict)[k] = it->second;
            }
        for (auto& kv : self_saved)
            if (kv.second.first) vm.vars[kv.first] = kv.second.second;
            else vm.vars_erase(kv.first);
        for (auto& sp : saved)
            if (sp.had) *sp.ptr = sp.old; // через указатель — без find
            else vm.vars_erase(*sp.name);
    };
    for (auto& s : fn->statements) {
        if (vm.return_pending) break;
        vm.exec_note(s->line);
        s->eval(vm);
    }
    NObject res;
    if (vm.return_pending) {
        res = vm.return_value;
        vm.return_pending = false;
    }
    restore();
    vm.unlock();
    return res;
}

static FuncPtr mkInterpreted(const std::string& name,
        const std::vector<std::pair<std::string,
            std::pair<std::string, NodePtr>>>& args,
        const std::vector<NodePtr>& statements) {
    auto f = std::make_shared<NFunction>();
    f->name = name;
    f->args = args;
    f->statements = statements;
    return f;
}

NObject FunctionDeclaration::eval(Nvm& vm) {
    FuncPtr fn = mkInterpreted(name, args, statements);
    fn->decl_node = shared_from_this(); // для hot-swap
    vm.set(name, NObject(fn));
    vm.func_types[name] = type;
    return NObject(fn);
}

NObject FunctionExpression::eval(Nvm& vm) {
    return NObject(mkInterpreted("<lambda>", args, statements));
}

// ================= ClassStatement.eval =================
// Person(...) -> dict: поля + методы с bound_self = экземпляр.

FuncPtr make_class_factory(const std::string& name,
                           std::vector<ClassMember> members) {
    auto fn = std::make_shared<NFunction>();
    fn->name = name;
    fn->stdimpl = [name, members](Nvm& vm, const NArgs& args) -> NObject {
        auto d = std::make_shared<std::map<std::string, NObject>>();
        NObject inst(d);
        FuncPtr ctor;
        for (auto& m : members) {
            if (auto decl =
                std::dynamic_pointer_cast<DeclarationStatement>(m.decl)) {
                if (decl->value) {
                    (*d)[decl->name] = decl->value->eval(vm);
                } else {
                    // дефолт по типу — как value-init в C++
                    const std::string& t = decl->type;
                    if (t == "int" || t == "long" || t == "short" ||
                        t == "long long" || t == "size_t")
                        (*d)[decl->name] = NObject(0LL);
                    else if (t == "double" || t == "float")
                        (*d)[decl->name] = NObject(0.0);
                    else if (t == "bool")
                        (*d)[decl->name] = NObject(false);
                    else if (t == "string" || t == "std::string")
                        (*d)[decl->name] = NObject(std::string());
                    else
                        (*d)[decl->name] = NObject();
                }
            } else if (auto fdecl =
                std::dynamic_pointer_cast<FunctionDeclaration>(m.decl)) {
                auto mfn = mkInterpreted(fdecl->name, fdecl->args,
                                         fdecl->statements);
                mfn->bound_self = inst;
                if (fdecl->is_ctor || fdecl->name == name) ctor = mfn;
                else (*d)[fdecl->name] = NObject(mfn);
            }
        }
        if (ctor) {
            ctor->bound_self = inst;
            NArgs empty;
            ctor->stdimpl = nullptr;
            NObject c(ctor);
            c.call(vm, args);
        }
        return inst;
    };
    return fn;
}

NObject ClassStatement::eval(Nvm& vm) {
    register_abi_fields(vm); // класс через hot-ABI
    vm.set(name, NObject(make_class_factory(name, members)));
    return NObject();
}

// ================= ForeachStatement.eval =================

NObject ForeachStatement::eval(Nvm& vm) {
    NObject res = expr->eval(vm);
    auto run = [&] {
        try { block->eval(vm);
            return !vm.return_pending; }
        catch (BreakException&) { return false; }
        catch (ContinueException&) { return true; }
    };
    if (res.isList()) {
        auto l = res.asList();
        for (size_t i = 0; i < l->size(); ++i) {
            if (!key.empty()) vm.set(key, NObject((long long)i));
            vm.set(value, (*l)[i]);
            if (!run()) break;
        }
    } else if (res.isDict()) {
        for (auto& kv : *res.asDict()) {
            if (!key.empty()) vm.set(key, NObject(kv.first));
            vm.set(value, kv.second);
            if (!run()) break;
        }
    } else if (res.isStr()) {
        const std::string& s = res.asStr();
        for (size_t i = 0; i < s.size(); ++i) {
            if (!key.empty()) vm.set(key, NObject((long long)i));
            vm.set(value, NObject(std::string(1, s[i])));
            if (!run()) break;
        }
    } else {
        throw NLangError{"foreach по не-коллекции", line};
    }
    return NObject();
}
