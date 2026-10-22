// nlang -> wasm байткод: третья стадия аннигиляции.
// AST -> wasm-бинарь напрямую (без jsgen/текста) —
// модуль собирает все tintable-функции одним выстрелом:
// каждая fn -> export по имени, вызовы по индексу.
//
// Типы v2 (per-local, не один режим на функцию):
//   int/bool -> i32, dbl -> f64, str -> externref
//   (JS-строка напрямую — без linear memory и копирований)
// Строковые литералы -> импортируемые global externref.
// Строковые операции и builtin'ы -> импортируемые env-функции:
//   _cat _seq _print_e _print_i _print_f _str_i _str_f _len
//
// Подмножество v2:
//   Number, String(literal->global), Variable (local/param),
//   Binary(+ - * / % compound ++/--), comparisons,
//   ConditionalExpression (cmp + && ||), ExcludeExpression(!),
//   CallingExpression -> module fn по индексу / env-helper,
//   if/else, while, for(desugar), return, assign, decl, block,
//   break/continue. Унарный минус.
// Всё остальное -> WasmJitUnsupported -> стадия jit->js/eval.
#include "node.h"
#include "nvm.h"
#include "type.h"
#include "statements/function.h"
#include "statements/assignment.h"
#include "statements/conditional.h"
#include "statements/_while.h"
#include "statements/_for.h"
#include "statements/_return.h"
#include "statements/block.h"
#include "expressions/binary.h"
#include "expressions/calling.h"
#include "expressions/conditional.h"
#include "expressions/number.h"
#include "expressions/string.h"
#include "expressions/unary.h"
#include "expressions/variable.h"
#include "expressions/getter.h"
#include "expressions/list.h"
#include "expressions/_dict.h"
#include "expressions/vc.h"
#include "statements/foreach.h"
#include <cstring>
#include <map>
#include <vector>

struct WasmJitUnsupported {
    const char* why;
    WasmJitUnsupported(const char* w = "?") : why(w) {}
};
using Unsupported = WasmJitUnsupported;

namespace wasmjit {

// valtype-коды wasm (+ псевдо-тег для void)
constexpr uint8_t VT_I32 = 0x7F;
constexpr uint8_t VT_F64 = 0x7C;
constexpr uint8_t VT_EXT = 0x6F; // externref — JS-объект/строка
constexpr uint8_t VT_VOID = 0xFF;

// nlang-имя типа -> valtype (auto/anti/пусто -> f64)
static uint8_t type_vt(const std::string& t) {
    if (t == "int" || t == "bool") return VT_I32;
    if (t == "str" || t == "string" || t == "list" ||
        t == "dict" || t == "json" || t == "JValue" ||
        t == "object" || t == "any") return VT_EXT;
    if (t == "void") return VT_VOID;
    if (t == "anti" || t == "auto") return VT_EXT; // автотип=any
    return VT_F64; // dbl
}

// ---------- leb128 + примитивы ----------
struct Buf {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u32(uint32_t v) {           // unsigned leb128
        do { uint8_t x = v & 0x7f; v >>= 7;
             if (v) x |= 0x80; u8(x); } while (v);
    }
    void i64(int64_t v) {            // signed leb128
        bool more = true;
        while (more) {
            uint8_t x = v & 0x7f; v >>= 7;
            bool sign = x & 0x40;
            if ((v == 0 && !sign) || (v == -1 && sign)) more = false;
            else x |= 0x80;
            u8(x);
        }
    }
    void f64(double d) {
        uint8_t t[8]; std::memcpy(t, &d, 8);
        for (uint8_t x : t) u8(x);
    }
    void name(const std::string& s) {
        u32((uint32_t)s.size());
        b.insert(b.end(), s.begin(), s.end());
    }
    void section(uint8_t id, const Buf& p) {
        u8(id); u32((uint32_t)p.b.size());
        b.insert(b.end(), p.b.begin(), p.b.end());
    }
};

// ---------- импортируемые env-хелперы ----------
// индексы функций: сначала K хелперов, потом module fn
enum HelperIdx {
    H_CAT = 0,   // (externref,externref)->externref   str+str
    H_SEQ,       // (externref,externref)->i32         str==str
    H_PRINT_E,   // (externref)->void                  print(str)
    H_PRINT_I,   // (i32)->void                        print(int)
    H_PRINT_F,   // (f64)->void                        print(dbl)
    H_STR_I,     // (i32)->externref                   str(int)
    H_STR_F,     // (f64)->externref                   str(dbl)
    H_LEN,       // (externref)->i32                   len(str|list)
    H_ARR_NEW,   // ()->externref                      [...]
    H_ARR_PUSH,  // (ext,ext)->void                    l.push(v)
    H_ARR_GET,   // (ext,i32)->ext                     a[i] (и строки!)
    H_ARR_SET,   // (ext,i32,ext)->void                a[i]=v
    H_ARR_POP,   // (ext)->ext                         l.pop()
    H_BOX_I,     // (i32)->ext                         int->ext (JS identity)
    H_BOX_F,     // (f64)->ext                         dbl->ext
    H_UNBOX_I,   // (ext)->i32                         ext->int
    H_UNBOX_F,   // (ext)->f64                         ext->dbl
    H_OBJ_NEW,   // ()->ext                            {..}
    H_OBJ_GET,   // (ext,ext)->ext                     a.b / a["k"]
    H_OBJ_SET,   // (ext,ext,ext)->void                a.b=v / a["k"]=v
    H_TRUTHY,    // (ext)->i32                         if (obj)
    H_STR2_I,    // (ext)->i32                         int(str)
    H_STR2_F,    // (ext)->f64                         dbl(str)
    H_JOIN,      // (ext,ext)->ext                     l.join(sep)
    H_SLICE,     // (ext,i32,i32)->ext                 s[i:j] (j=-1=до конца)
    H_SPLIT,     // (ext,ext)->ext                     s.split(sep)
    H_CONTAINS,  // (ext,ext)->i32                     x in s / s.contains
    H_INDEX,     // (ext,ext)->i32                     indexOf
    H_LOWER,     // (ext)->ext                         s.lower()
    H_UPPER,     // (ext)->ext                         s.upper()
    H_REPLACE,   // (ext,ext,ext)->ext                 s.replace(a,b)
    H_MATCH,     // (ext,ext)->ext                     s.match(re)
    H_RE_TEST,   // (ext,ext)->i32                     re.test(s)
    // canvas: контекст и примитивы рисования —
    // nlang-имена canvas.rect/text/clear/line/circle
    H_CTX,       // ()->ext                            2d-контекст
    H_RECT,      // (ctx,i32,i32,i32,i32,ext)->void
    H_TEXT,      // (ctx,ext,i32,i32,ext)->void
    H_CLEAR,     // (ctx,i32,i32)->void
    H_LINE,      // (ctx,i32,i32,i32,i32,ext)->void
    H_CIRCLE,    // (ctx,i32,i32,i32,ext)->void
    H_HAS,       // (ext,ext)->i32                     k in o (dict)
    H_CHR,       // (i32)->ext                         chr(keyCode)
    H_FONT,      // (ctx,i32)->void                    ctx.font = px
    H_RRECT,     // (ctx,i32x5,ext)->void              скруглённый rect
    H_CLIP,      // (ctx,ext)->void                    clipboard.write
    H_PASTE,     // (ctx)->void                        clipboard.read->
                                                 // on.paste(text)
    H_SCRW,      // (ctx)->i32                         canvas.width
    H_SCRH,      // (ctx)->i32                         canvas.height
    // сеть и json: courier.order(url) -> _fetch -> on.dispatch(body)
    // json.parse(text) -> externref (JS-объект)
    H_FETCH,     // (ext)->void
    H_JSON,      // (ext)->ext
    // image-блок (photo3d): картинки и файлы —
    // display.pickimg/image/pix/save. Все display.* принимают
    // ctx первым параметром (см. ниже — эмит кладёт _ctx()).
    H_IMGPICK,   // (ctx,ext)->void     file picker -> on.image
    H_IMGDRAW,   // (ctx,ext,i32x4)->void drawImage
    H_IMGPIX,    // (ctx,ext,i32x6)->ext  RGBA-байты
    H_DL,        // (ctx,ext,ext)->void  скачать файл
    H_IMGMASK,   // (ctx,ext,i32x7,ext)->ext {lum,mask}
    H_DLPNG,     // (ctx,ext,i32x4,ext)->void кроп -> png
    H_IMGDRAWC,  // (ctx,ext,i32x8)->void drawImage+src
    H_QUAD,      // (ctx,i32x8,ext)->void залитый quad
    H_CLIPR,     // (ctx,i32x4)->void save+clip rect
    H_UNCLIP,    // (ctx)->void restore
    H_TRI,       // (ctx,i32x6,ext)->void залитый треуг.
    H_PICKTXT,   // (ctx,ext)->void файл->on.text(tag,name,txt)
    H_SSAVE,     // (ctx,ext,ext)->void localStorage.setItem
    H_SLOAD,     // (ctx,ext)->ext   localStorage.getItem|""
    // math.* -> Math.*: без ctx, все f64
    H_POW,       // (f64,f64)->f64
    H_EXP,       // (f64)->f64
    H_SQRT,      // (f64)->f64
    H_SIN,       // (f64)->f64
    H_COS,       // (f64)->f64
    H_ATAN2,     // (f64,f64)->f64
    H_LOG,       // (f64)->f64
    H_FLOOR,     // (f64)->f64
    H_ABS,       // (f64)->f64
    H_RAND,      // ()->f64
    H_COUNT
};
static const char* H_NAMES[H_COUNT] = {
    "_cat","_seq","_print_e","_print_i","_print_f",
    "_str_i","_str_f","_len",
    "_arr_new","_arr_push","_arr_get","_arr_set","_arr_pop",
    "_box_i","_box_f","_unbox_i","_unbox_f",
    "_obj_new","_obj_get","_obj_set","_truthy",
    "_int","_dbl",
    "_join","_slice","_split","_contains","_index",
    "_lower","_upper","_replace","_match","_re_test",
    "_ctx","_rect","_text","_clear","_line","_circle",
    "_has","_chr","_font","_rrect","_clip","_paste",
    "_scrw","_scrh","_fetch","_json",
    "_imgpick","_imgdraw","_imgpix","_dl",
    "_imgmask","_dlpng","_imgdrawc","_quad",
    "_clipr","_unclip","_tri","_picktxt",
    "_ssave","_sload",
    "_pow","_exp","_sqrt","_sin","_cos","_atan2",
    "_log","_floor","_abs","_rand"};

// ---------- контекст ----------
struct FnCtx {
    const FunctionDeclaration* decl;
    // name -> {local idx, valtype}
    std::map<std::string, std::pair<uint32_t,uint8_t>> locals;
    uint32_t n_locals = 0;
    std::vector<int> loop_labels;
    uint8_t result = VT_F64;
    // foreach-узел -> его номер (для скрытых __fe_ локалей)
    std::map<const void*, int> fe_ids;
    // глубина вложенности литералов -> слот темпа __tN
    int tmp_depth = 0;
};

struct FnSig {
    std::vector<uint8_t> params;
    uint8_t result = VT_F64;
};

// реестр: имя -> {fn idx в модуле, сигнатура}
static std::map<std::string, std::pair<uint32_t, FnSig>> g_fns;
// глобальные переменные модуля: имя -> {global idx, vt}
// (idx считается ПОСЛЕ импортированных lit-глобалов)
static std::map<std::string, std::pair<uint32_t,uint8_t>> g_globals;
// строковые литералы модуля -> global idx (после fn-импортов —
// globals отдельное индексное пространство: сначала импорты-lits)
static std::map<std::string, uint32_t> g_lits;
static std::vector<std::string> g_lit_order;

// имя переменной: VariableExpression или Getter[Variable]
static std::string plain_var(NodePtr n) {
    if (auto v = std::dynamic_pointer_cast<VariableExpression>(n))
        return v->text;
    auto g = std::dynamic_pointer_cast<GetterExpression>(n);
    if (!g || g->list.size() != 1) return "";
    auto v = std::dynamic_pointer_cast<VariableExpression>(g->list[0]);
    return v ? v->text : "";
}

// ---------- тип выражения (без эмиссии) ----------
static uint8_t expr_vt(FnCtx& c, NodePtr n);

static uint8_t getter_elem_vt(FnCtx& c, NodePtr n) {
    // getter любой глубины: expr_vt сам разбирает a[i]/a.b/m()
    return expr_vt(c, n);
}

static uint8_t call_vt(FnCtx& c, const CallingExpression* call) {
    std::string fname = plain_var(call->getter);
    if (fname == "str" || fname == "print_obj") return VT_EXT;
    if (fname == "len" || fname == "int") return VT_I32;
    if (fname == "dbl") return VT_F64;
    if (fname == "chr") return VT_EXT;
    if (fname == "print" || fname == "transmit") return VT_VOID;
    auto it = g_fns.find(fname);
    if (it == g_fns.end()) throw Unsupported{"call vt"};
    return it->second.second.result;
}

// метод на getter-цепочке: l.push(x) / a.pop() / s.len()
static uint8_t method_vt(FnCtx& c, const std::string& name) {
    if (name == "push") return VT_VOID;
    if (name == "pop" || name == "get" || name == "keys" ||
        name == "values" || name == "items" || name == "split" ||
        name == "slice" || name == "match" || name == "lower" ||
        name == "upper" || name == "replace" || name == "join" ||
        name == "trim") return VT_EXT;
    if (name == "len" || name == "size" || name == "count" ||
        name == "index" || name == "contains" || name == "has" ||
        name == "test")
        return VT_I32;
    throw Unsupported{"method vt"};
}

static uint8_t expr_vt(FnCtx& c, NodePtr n) {
    if (!n) throw Unsupported{"vt null"};
    if (std::dynamic_pointer_cast<StringExpression>(n)) return VT_EXT;
    if (auto num = std::dynamic_pointer_cast<NumberExpression>(n))
        return num->is_float ? VT_F64 : VT_I32;
    {
        std::string v = plain_var(n);
        if (!v.empty()) {
            auto it = c.locals.find(v);
            if (it != c.locals.end()) return it->second.second;
            auto gi = g_globals.find(v);
            if (gi != g_globals.end()) return gi->second.second;
            throw Unsupported{"vt local"};
        }
    }
    if (auto g = std::dynamic_pointer_cast<GetterExpression>(n)) {
        if (g->list.size() == 1) {
            if (auto call = std::dynamic_pointer_cast<
                    CallingExpression>(g->list[0]))
                return call_vt(c, call.get());
            return expr_vt(c, g->list[0]);
        }
        // canvas.*/display.*: w/h возвращают i32, остальное void
        if (auto cv = std::dynamic_pointer_cast<VariableExpression>(
                g->list[0]))
            if (cv->text == "canvas" || cv->text == "display") {
                std::string mname;
                if (auto cl = std::dynamic_pointer_cast<
                        CallingExpression>(g->list.back()))
                    mname = plain_var(cl->getter);
                else mname = plain_var(g->list.back());
                if (mname == "w" || mname == "h") return VT_I32;
                if (mname == "pix" || mname == "maskimg" ||
                    mname == "sload")
                    return VT_EXT;
                return VT_VOID;
            }
        // math.* — f64 (pow/exp/sqrt/sin/cos/... -> Math.*)
        if (auto cv = std::dynamic_pointer_cast<VariableExpression>(
                g->list[0])) {
            if (cv->text == "math")    return VT_F64;
            if (cv->text == "courier") return VT_VOID;
            if (cv->text == "json")    return VT_EXT;
            }
        // цепочка: a[i] / a.b / a.m(...)
        auto last = g->list.back();
        if (auto call = std::dynamic_pointer_cast<CallingExpression>(last)) {
            std::string mname = plain_var(call->getter);
            return method_vt(c, mname);
        }
        return VT_EXT; // a[i] / a.b
    }
    if (auto call = std::dynamic_pointer_cast<CallingExpression>(n))
        return call_vt(c, call.get());
    if (std::dynamic_pointer_cast<ListExpression>(n)) return VT_EXT;
    if (std::dynamic_pointer_cast<DictExpression>(n)) return VT_EXT;
    if (std::dynamic_pointer_cast<VcExpression>(n)) return VT_EXT;
    if (std::dynamic_pointer_cast<VcTextExpression>(n)) return VT_EXT;
    if (std::dynamic_pointer_cast<VcOutputExpression>(n))
        return VT_EXT;
    if (auto bin = std::dynamic_pointer_cast<BinaryExpression>(n)) {
        switch (bin->_type) {
            case PLUS: {
                uint8_t a = getter_elem_vt(c, bin->expr1);
                uint8_t b = getter_elem_vt(c, bin->expr2);
                if (a == VT_EXT || b == VT_EXT) return VT_EXT;
                return (a == VT_F64 || b == VT_F64) ? VT_F64 : VT_I32;
            }
            case MINUS: case STAR: case SLASH: case PERCENT: {
                uint8_t a = getter_elem_vt(c, bin->expr1);
                uint8_t b = getter_elem_vt(c, bin->expr2);
                return (a == VT_F64 || b == VT_F64) ? VT_F64 : VT_I32;
            }
            case GT: case LT: case EQEQ: case EXCLEQ:
            case LTEQ: case GTEQ:
                return VT_I32;
            case PLUSEQ: case MINUSEQ: case STAREQ:
            case SLASHEQ: case PERCENTEQ:
            case INCREMENT: case DECREMENT: {
                std::string v = plain_var(bin->expr1);
                auto it = c.locals.find(v);
                if (!v.empty()) {
                    if (it != c.locals.end())
                        return it->second.second;
                    auto gi = g_globals.find(v);
                    if (gi != g_globals.end())
                        return gi->second.second;
                }
                throw Unsupported{"compound vt"};
            }
            default: throw Unsupported{"bin vt"};
        }
    }
    if (auto ce = std::dynamic_pointer_cast<ConditionalExpression>(n))
        return VT_I32;
    if (std::dynamic_pointer_cast<ExcludeExpression>(n)) return VT_I32;
    if (auto un = std::dynamic_pointer_cast<UnaryExpression>(n))
        return getter_elem_vt(c, un->expr);
    throw Unsupported{"expr vt"};
}

static void add_lit(const std::string& s) {
    if (!g_lits.count(s)) {
        g_lits[s] = (uint32_t)g_lit_order.size();
        g_lit_order.push_back(s);
    }
}

// ---------- сборка литералов и локалей ----------
static void collect_lits(NodePtr n) {
    if (!n) return;
    if (auto s = std::dynamic_pointer_cast<StringExpression>(n)) {
        add_lit(s->value);
        return;
    }
    // имена членов в getter-цепочках тоже литералы: a.b -> "b"
    if (auto g = std::dynamic_pointer_cast<GetterExpression>(n)) {
        for (size_t i = 1; i < g->list.size(); ++i)
            if (auto v = std::dynamic_pointer_cast<VariableExpression>(
                    g->list[i]))
                add_lit(v->text);
    }
    // универсальный обход детей через известные поля
    auto kids = std::vector<NodePtr>();
    if (auto b = std::dynamic_pointer_cast<BinaryExpression>(n))
        kids = {b->expr1, b->expr2};
    else if (auto ce = std::dynamic_pointer_cast<ConditionalExpression>(n))
        kids = {ce->first, ce->second};
    else if (auto ex = std::dynamic_pointer_cast<ExcludeExpression>(n))
        kids = {ex->expr};
    else if (auto un = std::dynamic_pointer_cast<UnaryExpression>(n))
        kids = {un->expr};
    else if (auto call = std::dynamic_pointer_cast<CallingExpression>(n)) {
        kids = call->pos_args;
        for (auto& a : call->named_args) kids.push_back(a.second);
    }
    else if (auto g = std::dynamic_pointer_cast<GetterExpression>(n))
        kids = g->list;
    else if (auto l = std::dynamic_pointer_cast<ListExpression>(n))
        kids = l->list;
    else if (auto d = std::dynamic_pointer_cast<DictExpression>(n)) {
        for (auto& p : d->dictionary) {
            // ключ-идентификатор = строковый литерал: {name: v}
            if (auto kv = std::dynamic_pointer_cast<VariableExpression>(
                    p.first))
                add_lit(kv->text);
            else kids.push_back(p.first);
            kids.push_back(p.second);
        }
    }
    else if (auto fe = std::dynamic_pointer_cast<ForeachStatement>(n))
        kids = {fe->expr, fe->block};
    else if (auto aa = std::dynamic_pointer_cast<
             ArrayAccessExpression>(n))
        kids = aa->access;
    else if (auto i = std::dynamic_pointer_cast<ConditionalStatement>(n))
        kids = {i->cond, i->_if, i->_else};
    else if (auto w = std::dynamic_pointer_cast<WhileStatement>(n))
        kids = {w->expr, w->block};
    else if (auto f = std::dynamic_pointer_cast<ForStatement>(n))
        kids = {f->decl, f->_while, f->_do, f->block};
    else if (auto r = std::dynamic_pointer_cast<ReturnStatement>(n))
        kids = {r->expr};
    else if (auto a = std::dynamic_pointer_cast<AssignmentStatement>(n)) {
        kids = {a->value};
        // цель присваивания тоже getter: b.x = v / a[i] = v —
        // имена членов в ней должны попасть в литералы
        if (auto g = std::dynamic_pointer_cast<GetterExpression>(
                a->getter))
            for (size_t i = 1; i < g->list.size(); ++i)
                if (auto v = std::dynamic_pointer_cast<
                        VariableExpression>(g->list[i]))
                    add_lit(v->text);
    }
    else if (auto d = std::dynamic_pointer_cast<DeclarationStatement>(n))
        kids = {d->value};
    else if (auto bl = std::dynamic_pointer_cast<BlockStatement>(n))
        kids = bl->block;
    else if (auto vc = std::dynamic_pointer_cast<VcExpression>(n)) {
        // виджет-режим: имена атрибутов и тексты — литералы
        add_lit(vc->html_tag());
        if (!vc->id.empty()) add_lit(vc->id);
        for (auto& a : vc->attrs) {
            add_lit(a.first);
            if (a.second) kids.push_back(a.second);
        }
        for (auto& ch : vc->children) {
            if (auto te = std::dynamic_pointer_cast<
                    VcTextExpression>(ch))
                add_lit(te->text);
            kids.push_back(ch);
        }
    }
    else if (auto o = std::dynamic_pointer_cast<
             VcOutputExpression>(n))
        kids = {o->expr};
    for (auto& k : kids) collect_lits(k);
}

static void declare_local(FnCtx& c, const std::string& name,
                          uint8_t vt) {
    if (!c.locals.count(name))
        c.locals[name] = {c.n_locals++, vt};
}

static void collect_locals(NodePtr n, FnCtx& c) {
    if (!n) return;
    if (auto a = std::dynamic_pointer_cast<AssignmentStatement>(n)) {
        std::string v = a->_plain_var_name();
        // имя есть в модулярных глобалах и ещё не локаль —
        // пишем в глобал (flat-vars семантика nlang),
        // локаль не создаём
        if (!v.empty() && !c.locals.count(v) &&
            !g_globals.count(v)) {
            uint8_t vt = VT_F64;
            try { vt = expr_vt(c, a->value); }
            catch (Unsupported&) {}
            declare_local(c, v, vt);
        }
    }
    if (auto d = std::dynamic_pointer_cast<DeclarationStatement>(n)) {
        if (!c.locals.count(d->name)) {
            uint8_t vt = type_vt(d->type);
            if (d->type == "auto" || d->type == "anti" || d->type.empty()) {
                try { vt = expr_vt(c, d->value); } catch (Unsupported&) {}
            }
            declare_local(c, d->name, vt);
        }
    }
    if (auto b = std::dynamic_pointer_cast<BlockStatement>(n))
        for (auto& s : b->block) collect_locals(s, c);
    if (auto i = std::dynamic_pointer_cast<ConditionalStatement>(n)) {
        collect_locals(i->_if, c); collect_locals(i->_else, c);
    }
    if (auto w = std::dynamic_pointer_cast<WhileStatement>(n))
        collect_locals(w->block, c);
    if (auto f = std::dynamic_pointer_cast<ForStatement>(n)) {
        collect_locals(f->decl, c); collect_locals(f->block, c);
    }
    if (auto fe = std::dynamic_pointer_cast<ForeachStatement>(n)) {
        // скрытые: контейнер + индекс; видимые: value (+key=i32)
        int id = (int)c.fe_ids.size();
        c.fe_ids[fe.get()] = id;
        declare_local(c, "__fe_a" + std::to_string(id), VT_EXT);
        declare_local(c, "__fe_i" + std::to_string(id), VT_I32);
        declare_local(c, fe->value, VT_EXT);
        if (!fe->key.empty())
            declare_local(c, fe->key, VT_I32);
        collect_locals(fe->block, c);
    }
}

// ---------- эмиссия ----------
// emit_expr оставляет значение на стеке и ВОЗВРАЩАЕТ его valtype
static uint8_t emit_expr(Buf& code, FnCtx& c, NodePtr n);

// приведение вершины стека: числа через wasm-опы,
// ext<->num через импортируемые box/unbox-хелперы
static void convert(Buf& code, uint8_t from, uint8_t to) {
    if (from == to) return;
    if (from == VT_I32 && to == VT_F64) { code.u8(0xB7); return; }
    if (from == VT_F64 && to == VT_I32) { code.u8(0xAA); return; } // i32.trunc_f64_s
    if (from == VT_I32 && to == VT_EXT)
        { code.u8(0x10); code.u32(H_BOX_I); return; }
    if (from == VT_F64 && to == VT_EXT)
        { code.u8(0x10); code.u32(H_BOX_F); return; }
    if (from == VT_EXT && to == VT_I32)
        { code.u8(0x10); code.u32(H_UNBOX_I); return; }
    if (from == VT_EXT && to == VT_F64)
        { code.u8(0x10); code.u32(H_UNBOX_F); return; }
    throw Unsupported{"convert"};
}

static void emit_typed(Buf& code, FnCtx& c, NodePtr n,
                       uint8_t want) {
    uint8_t got = emit_expr(code, c, n);
    convert(code, got, want);
}

// условие -> i32: i32 как есть, f64 != 0.0, ext через _truthy
static void emit_bool(Buf& code, FnCtx& c, NodePtr n) {
    uint8_t t = emit_expr(code, c, n);
    if (t == VT_I32) return;
    if (t == VT_F64) {
        code.u8(0x44); code.f64(0.0);
        code.u8(0x62); // f64.ne
        return;
    }
    if (t == VT_EXT) {
        code.u8(0x10); code.u32(H_TRUTHY);
        return;
    }
    throw Unsupported{"bool"};
}

// emit_expr для элемента getter'а (разворачивание обёрток)
static uint8_t emit_elem(Buf& code, FnCtx& c, NodePtr n,
                         uint8_t want) {
    uint8_t got = emit_expr(code, c, n);
    convert(code, got, want);
    return want;
}

// вызов: module fn по индексу или env-хелпер
static void emit_call(Buf& code, FnCtx& c,
                      const CallingExpression* call) {
    std::string fname = plain_var(call->getter);
    if (!call->named_args.empty()) throw Unsupported{"named args"};

    // builtin-хелперы: выбор по типу первого аргумента
    auto arg_vt = [&]() -> uint8_t {
        if (call->pos_args.empty()) return VT_EXT;
        return expr_vt(c, call->pos_args[0]);
    };

    if (fname == "print" || fname == "transmit") {
        if (call->pos_args.size() != 1) throw Unsupported{"print arity"};
        uint8_t t = arg_vt();
        int h = t == VT_EXT ? H_PRINT_E : t == VT_I32 ? H_PRINT_I
                                                      : H_PRINT_F;
        emit_typed(code, c, call->pos_args[0], t);
        code.u8(0x10); code.u32((uint32_t)h);
        return;
    }
    if (fname == "str") {
        if (call->pos_args.size() != 1) throw Unsupported{"str arity"};
        uint8_t t = arg_vt();
        if (t == VT_EXT) { // str(str) — без изменений
            emit_typed(code, c, call->pos_args[0], VT_EXT);
        } else {
            emit_typed(code, c, call->pos_args[0], t);
            code.u8(0x10);
            code.u32((uint32_t)(t == VT_I32 ? H_STR_I : H_STR_F));
        }
        return;
    }
    if (fname == "len") {
        if (call->pos_args.size() != 1) throw Unsupported{"len arity"};
        emit_typed(code, c, call->pos_args[0], VT_EXT);
        code.u8(0x10); code.u32((uint32_t)H_LEN);
        return;
    }
    if (fname == "chr") {
        if (call->pos_args.size() != 1) throw Unsupported{"chr arity"};
        emit_typed(code, c, call->pos_args[0], VT_I32);
        code.u8(0x10); code.u32((uint32_t)H_CHR);
        return;
    }
    if (fname == "int") {
        if (call->pos_args.size() != 1) throw Unsupported{"int arity"};
        uint8_t t = arg_vt();
        if (t == VT_EXT) {
            emit_typed(code, c, call->pos_args[0], VT_EXT);
            code.u8(0x10); code.u32((uint32_t)H_STR2_I);
        } else {
            emit_typed(code, c, call->pos_args[0], VT_I32);
        }
        return;
    }
    if (fname == "dbl") {
        if (call->pos_args.size() != 1) throw Unsupported{"dbl arity"};
        uint8_t t = arg_vt();
        if (t == VT_EXT) {
            emit_typed(code, c, call->pos_args[0], VT_EXT);
            code.u8(0x10); code.u32((uint32_t)H_STR2_F);
        } else {
            emit_typed(code, c, call->pos_args[0], VT_F64);
        }
        return;
    }

    // вызов функции модуля
    auto it = g_fns.find(fname);
    if (it == g_fns.end()) {
        static std::string m;
        m = "call target " + fname;
        throw Unsupported{m.c_str()};
    }
    const FnSig& sig = it->second.second;
    if (sig.params.size() != call->pos_args.size())
        throw Unsupported{"arity"};
    for (size_t i = 0; i < call->pos_args.size(); ++i)
        emit_typed(code, c, call->pos_args[i], sig.params[i]);
    code.u8(0x10); code.u32(it->second.first);
}

// общий числовой бинарь: + - * / %
static uint8_t emit_arith(Buf& code, FnCtx& c,
                          BinaryExpression* bin, bool is_plus) {
    uint8_t a = expr_vt(c, bin->expr1);
    uint8_t b = expr_vt(c, bin->expr2);
    if (is_plus && (a == VT_EXT || b == VT_EXT)) {
        // конкатенация: всё в externref через _cat
        // (числа сначала str() через хелперы)
        auto to_ext = [&](NodePtr x, uint8_t t) {
            if (t == VT_EXT) { emit_typed(code, c, x, VT_EXT); return; }
            emit_typed(code, c, x, t);
            code.u8(0x10);
            code.u32((uint32_t)(t == VT_I32 ? H_STR_I : H_STR_F));
        };
        to_ext(bin->expr1, a);
        to_ext(bin->expr2, b);
        code.u8(0x10); code.u32((uint32_t)H_CAT);
        return VT_EXT;
    }
    // externref в арифметике — unbox: если второй операнд
    // f64 — оба в double, иначе оба в i32 (box-поля dict
    // чаще всего целые: координаты, размеры)
    uint8_t mode = (a == VT_F64 || b == VT_F64) ? VT_F64 : VT_I32;
    emit_typed(code, c, bin->expr1, mode);
    emit_typed(code, c, bin->expr2, mode);
    static const std::map<int, uint8_t> f64m = {
        {PLUS, 0xA0}, {MINUS, 0xA1}, {STAR, 0xA2},
        {SLASH, 0xA3}, {PERCENT, 0xA9}}; // f64.rem
    static const std::map<int, uint8_t> i32m = {
        {PLUS, 0x6A}, {MINUS, 0x6B}, {STAR, 0x6C},
        {SLASH, 0x6E}, {PERCENT, 0x6F}}; // div_s/rem_s
    code.u8((mode == VT_I32 ? i32m : f64m).at(bin->_type));
    return mode;
}

static uint8_t emit_expr(Buf& code, FnCtx& c, NodePtr n) {
    if (!n) throw Unsupported{"null expr"};

    if (auto num = std::dynamic_pointer_cast<NumberExpression>(n)) {
        if (num->is_float) {
            code.u8(0x44); code.f64(num->dvalue); return VT_F64;
        }
        code.u8(0x41); code.i64(num->ivalue); return VT_I32;
    }

    if (auto s = std::dynamic_pointer_cast<StringExpression>(n)) {
        auto it = g_lits.find(s->value);
        if (it == g_lits.end()) throw Unsupported{"literal"};
        code.u8(0x23); code.u32(it->second); // global.get lit
        return VT_EXT;
    }

    {
        std::string v = plain_var(n);
        if (!v.empty()) {
            auto it = c.locals.find(v);
            if (it != c.locals.end()) {
                code.u8(0x20); code.u32(it->second.first);
                return it->second.second;
            }
            auto gi = g_globals.find(v);
            if (gi != g_globals.end()) {
                code.u8(0x23); code.u32(gi->second.first); // global.get
                return gi->second.second;
            }
            throw Unsupported{"local"};
        }
    }

    if (auto g = std::dynamic_pointer_cast<GetterExpression>(n)) {
        if (g->list.size() == 1)
            return emit_expr(code, c, g->list[0]);
        // canvas.m(...) — рисование: _ctx() + аргументы + хелпер
        if (auto cv = std::dynamic_pointer_cast<VariableExpression>(
                g->list[0]))
            if ((cv->text == "canvas" || cv->text == "display") &&
                g->list.size() == 2) {
                auto call = std::dynamic_pointer_cast<
                    CallingExpression>(g->list[1]);
                if (!call) throw Unsupported{"canvas call"};
                std::string mname = plain_var(call->getter);
                // name -> {helper, arg-vts}
                static const std::map<std::string,
                    std::pair<int, std::vector<uint8_t>>> cvm = {
                    {"rect",  {H_RECT,   {VT_I32,VT_I32,VT_I32,VT_I32,VT_EXT}}},
                    {"text",  {H_TEXT,   {VT_EXT,VT_I32,VT_I32,VT_EXT}}},
                    {"rune",  {H_TEXT,   {VT_EXT,VT_I32,VT_I32,VT_EXT}}},
                    {"font",  {H_FONT,   {VT_I32}}},
                    {"rrect", {H_RRECT,  {VT_I32,VT_I32,VT_I32,VT_I32,
                                          VT_I32,VT_EXT}}},
                    {"clip",  {H_CLIP,   {VT_EXT}}},
                    {"paste", {H_PASTE,  {}}},
                    {"w",     {H_SCRW,   {}}},
                    {"h",     {H_SCRH,   {}}},
                    {"clear", {H_CLEAR,  {VT_I32,VT_I32}}},
                    {"line",  {H_LINE,   {VT_I32,VT_I32,VT_I32,VT_I32,VT_EXT}}},
                    {"circle",{H_CIRCLE, {VT_I32,VT_I32,VT_I32,VT_EXT}}},
                    // image/файлы (photo3d)
                    {"pickimg",{H_IMGPICK,{VT_EXT}}},
                    {"image", {H_IMGDRAW,{VT_EXT,VT_I32,VT_I32,
                                          VT_I32,VT_I32}}},
                    {"pix",   {H_IMGPIX, {VT_EXT,VT_I32,VT_I32,
                                          VT_I32,VT_I32,VT_I32,
                                          VT_I32}}},
                    {"save",  {H_DL,     {VT_EXT,VT_EXT}}},
                    {"maskimg",{H_IMGMASK,
                                {VT_EXT,VT_I32,VT_I32,VT_I32,
                                 VT_I32,VT_I32,VT_I32,VT_I32,
                                 VT_EXT}}},
                    {"png",   {H_DLPNG,  {VT_EXT,VT_I32,VT_I32,
                                          VT_I32,VT_I32,VT_EXT}}},
                    {"imagec",{H_IMGDRAWC,
                               {VT_EXT,VT_I32,VT_I32,VT_I32,
                                VT_I32,VT_I32,VT_I32,VT_I32,
                                VT_I32}}},
                    {"quad", {H_QUAD, {VT_I32,VT_I32,VT_I32,
                                       VT_I32,VT_I32,VT_I32,
                                       VT_I32,VT_I32,VT_EXT}}},
                    {"clipr",{H_CLIPR,{VT_I32,VT_I32,VT_I32,
                                       VT_I32}}},
                    {"unclip",{H_UNCLIP,{}}},
                    {"tri",  {H_TRI,  {VT_I32,VT_I32,VT_I32,
                                       VT_I32,VT_I32,VT_I32,
                                       VT_EXT}}},
                    {"picktxt",{H_PICKTXT,{VT_EXT}}},
                    {"ssave",{H_SSAVE,{VT_EXT,VT_EXT}}},
                    {"sload",{H_SLOAD,{VT_EXT}}},
                };
                auto mi = cvm.find(mname);
                if (mi == cvm.end()) throw Unsupported{"canvas method"};
                const auto& hm = mi->second;
                if (hm.second.size() != call->pos_args.size())
                    throw Unsupported{"canvas arity"};
                code.u8(0x10); code.u32((uint32_t)H_CTX); // ctx
                for (size_t ai = 0; ai < call->pos_args.size(); ++ai)
                    emit_typed(code, c, call->pos_args[ai],
                               hm.second[ai]);
                code.u8(0x10); code.u32((uint32_t)hm.first);
                if (hm.first == H_SCRW || hm.first == H_SCRH)
                    return VT_I32;
                if (hm.first == H_IMGPIX || hm.first == H_IMGMASK ||
                    hm.first == H_SLOAD)
                    return VT_EXT;
                return VT_VOID;
            }
        // math.m(x..) -> Math.m — без ctx, чистая f64-математика
        if (auto cv = std::dynamic_pointer_cast<VariableExpression>(
                g->list[0]))
            if (cv->text == "math" && g->list.size() == 2) {
                auto call = std::dynamic_pointer_cast<
                    CallingExpression>(g->list[1]);
                if (!call) throw Unsupported{"math call"};
                std::string mname = plain_var(call->getter);
                static const std::map<std::string,
                    std::pair<int,int>> mm = {
                    {"pow",  {H_POW, 2}}, {"exp",   {H_EXP, 1}},
                    {"sqrt", {H_SQRT,1}}, {"sin",   {H_SIN, 1}},
                    {"cos",  {H_COS, 1}}, {"atan2", {H_ATAN2,2}},
                    {"log",  {H_LOG, 1}}, {"floor", {H_FLOOR,1}},
                    {"abs",  {H_ABS, 1}}, {"rand",  {H_RAND,0}},
                };
                auto mi = mm.find(mname);
                if (mi == mm.end()) throw Unsupported{"math method"};
                if ((int)call->pos_args.size() != mi->second.second)
                    throw Unsupported{"math arity"};
                for (auto& a : call->pos_args)
                    emit_typed(code, c, a, VT_F64);
                code.u8(0x10); code.u32((uint32_t)mi->second.first);
                return VT_F64;
            }
        // courier.order(url) — исходящий запрос из сектора:
        // асинхронно, ответ приходит в on.dispatch(body)
        if (auto cv = std::dynamic_pointer_cast<VariableExpression>(
                g->list[0]))
            if (cv->text == "courier" && g->list.size() == 2) {
                auto call = std::dynamic_pointer_cast<
                    CallingExpression>(g->list[1]);
                if (!call || plain_var(call->getter) != "order" ||
                    call->pos_args.size() != 1)
                    throw Unsupported{"courier method"};
                emit_typed(code, c, call->pos_args[0], VT_EXT);
                code.u8(0x10); code.u32((uint32_t)H_FETCH);
                return VT_VOID;
            }
        // json.parse(t) / json.import(t) -> JS-объект
        if (auto cv = std::dynamic_pointer_cast<VariableExpression>(
                g->list[0]))
            if (cv->text == "json" && g->list.size() == 2) {
                auto call = std::dynamic_pointer_cast<
                    CallingExpression>(g->list[1]);
                std::string m = call ? plain_var(call->getter) : "";
                if (!call || (m != "parse" && m != "import") ||
                    call->pos_args.size() != 1)
                    throw Unsupported{"json method"};
                emit_typed(code, c, call->pos_args[0], VT_EXT);
                code.u8(0x10); code.u32((uint32_t)H_JSON);
                return VT_EXT;
            }
        // цепочка: база ext, дальше по элементам
        uint8_t last_vt = VT_EXT;
        if (auto lc = std::dynamic_pointer_cast<CallingExpression>(
                g->list.back()))
            last_vt = method_vt(c, plain_var(lc->getter));
        emit_typed(code, c, g->list[0], VT_EXT);
        for (size_t i = 1; i < g->list.size(); ++i) {
            NodePtr el = g->list[i];
            if (auto acc = std::dynamic_pointer_cast<
                    ArrayAccessExpression>(el)) {
                // a[i]: i32-индекс -> _arr_get (списки/строки),
                // ext-ключ -> _obj_get (dict)
                if (acc->access.size() != 1)
                    throw Unsupported{"multi index"};
                uint8_t kvt = expr_vt(c, acc->access[0]);
                if (kvt == VT_EXT) {
                    emit_typed(code, c, acc->access[0], VT_EXT);
                    code.u8(0x10); code.u32(H_OBJ_GET);
                } else {
                    emit_typed(code, c, acc->access[0], VT_I32);
                    code.u8(0x10); code.u32(H_ARR_GET);
                }
            } else if (auto call = std::dynamic_pointer_cast<
                    CallingExpression>(el)) {
                // a.m(args) — методы через env-хелперы
                std::string mname = plain_var(call->getter);
                auto args0 = [&]() -> uint8_t {
                    if (call->pos_args.empty())
                        throw Unsupported{"method arity"};
                    return expr_vt(c, call->pos_args[0]);
                };
                bool last = (i + 1 == g->list.size());
                // методы (obj уже на стеке первым аргументом)
                static const std::map<std::string, std::pair<int,int>>
                    meth = {
                    // name -> {helper, nargs}
                    {"push",   {H_ARR_PUSH, 1}},
                    {"pop",    {H_ARR_POP, 0}},
                    {"join",   {H_JOIN, 1}},
                    {"slice",  {H_SLICE, -1}}, // 1 или 2 аргумента
                    {"split",  {H_SPLIT, 1}},
                    {"contains",{H_CONTAINS, 1}},
                    {"has",    {H_HAS, 1}},
                    {"index",  {H_INDEX, 1}},
                    {"lower",  {H_LOWER, 0}},
                    {"upper",  {H_UPPER, 0}},
                    {"replace",{H_REPLACE, 2}},
                    {"match",  {H_MATCH, 1}},
                    {"test",   {H_RE_TEST, 1}},
                    {"len",    {H_LEN, 0}},
                    {"size",   {H_LEN, 0}},
                };
                auto mi = meth.find(mname);
                if (mi == meth.end()) throw Unsupported{"method"};
                int nargs = mi->second.second;
                if (nargs >= 0 &&
                    (int)call->pos_args.size() != nargs)
                    throw Unsupported{"method arity"};
                if (nargs < 0 &&
                    (call->pos_args.size() < 1 ||
                     call->pos_args.size() > 2))
                    throw Unsupported{"method arity"};
                for (auto& a : call->pos_args)
                    emit_typed(code, c, a,
                        mi->second.first == H_SLICE ? VT_I32 : VT_EXT);
                // slice с 1 аргументом: второй = -1 (до конца)
                if (mi->second.first == H_SLICE &&
                    call->pos_args.size() == 1) {
                    code.u8(0x41); code.i64(-1);
                }
                code.u8(0x10); code.u32((uint32_t)mi->second.first);
                if (mname == "push") {
                    if (!last) throw Unsupported{"push chain"};
                    return VT_VOID;
                }
                if (!last) {
                    // результат не-ext: цепочка дальше не пойдёт
                    uint8_t rv = method_vt(c, mname);
                    if (rv != VT_EXT)
                        throw Unsupported{"method chain"};
                }
            } else if (auto mv = std::dynamic_pointer_cast<
                    VariableExpression>(el)) {
                // a.b -> _obj_get(a, "b")
                auto lit = g_lits.find(mv->text);
                if (lit == g_lits.end())
                    throw Unsupported{"member name"};
                code.u8(0x23); code.u32(lit->second); // "b"
                code.u8(0x10); code.u32(H_OBJ_GET);
            } else {
                throw Unsupported{"getter elem"};
            }
        }
        return last_vt;
    }

    if (auto call = std::dynamic_pointer_cast<CallingExpression>(n)) {
        emit_call(code, c, call.get());
        return call_vt(c, call.get());
    }

    if (auto l = std::dynamic_pointer_cast<ListExpression>(n)) {
        // [a, b] -> arr_new в темп слота __tN (N = глубина
        // вложенности — dict внутри list не затирает чужой темп)
        code.u8(0x10); code.u32(H_ARR_NEW);
        std::string tn = "__t" + std::to_string(c.tmp_depth++);
        auto it = c.locals.find(tn);
        if (it == c.locals.end()) {
            declare_local(c, tn, VT_EXT);
            it = c.locals.find(tn);
        }
        code.u8(0x21); code.u32(it->second.first); // set tmp
        for (auto& el : l->list) {
            code.u8(0x20); code.u32(it->second.first); // get arr
            emit_typed(code, c, el, VT_EXT);
            code.u8(0x10); code.u32(H_ARR_PUSH);     // (arr, v)
        }
        code.u8(0x20); code.u32(it->second.first); // get arr
        --c.tmp_depth;
        return VT_EXT;
    }

    if (auto d = std::dynamic_pointer_cast<DictExpression>(n)) {
        code.u8(0x10); code.u32(H_OBJ_NEW);
        std::string tn = "__t" + std::to_string(c.tmp_depth++);
        auto it = c.locals.find(tn);
        if (it == c.locals.end()) {
            declare_local(c, tn, VT_EXT);
            it = c.locals.find(tn);
        }
        code.u8(0x21); code.u32(it->second.first); // set tmp
        for (auto& p : d->dictionary) {
            code.u8(0x20); code.u32(it->second.first); // get obj
            // ключ-идентификатор = литерал {name: v}, иначе expr
            if (auto kv = std::dynamic_pointer_cast<VariableExpression>(
                    p.first)) {
                auto lit = g_lits.find(kv->text);
                if (lit == g_lits.end())
                    throw Unsupported{"dict key"};
                code.u8(0x23); code.u32(lit->second);
            } else {
                emit_typed(code, c, p.first, VT_EXT);
            }
            emit_typed(code, c, p.second, VT_EXT);  // val
            code.u8(0x10); code.u32(H_OBJ_SET);      // (o,k,v)
        }
        code.u8(0x20); code.u32(it->second.first); // get obj
        --c.tmp_depth;
        return VT_EXT;
    }

    // vc-разметка в wasm-пути = dict-дерево виджетов:
    //   <ampa {x=20}> -> {type:"div", x:20, children:[...]}
    // Текстовые дети -> {type:"text", text:"..."}
    if (auto vce = std::dynamic_pointer_cast<VcExpression>(n)) {
        auto push_lit = [&](const std::string& s) {
            auto lit = g_lits.find(s);
            if (lit == g_lits.end())
                throw Unsupported{"vc lit"};
            code.u8(0x23); code.u32(lit->second);
        };
        auto tmp = [&](int slot) {
            std::string tn = "__t" + std::to_string(slot);
            auto it = c.locals.find(tn);
            if (it == c.locals.end()) {
                declare_local(c, tn, VT_EXT);
                it = c.locals.find(tn);
            }
            return it->second.first;
        };
        // слоты: +0 объект, +8 children-массив, +16 текст-дитя
        int slot = c.tmp_depth++;
        uint32_t tobj = tmp(slot), tarr = tmp(slot + 8),
                 ttxt = tmp(slot + 16);
        code.u8(0x10); code.u32(H_OBJ_NEW);
        code.u8(0x21); code.u32(tobj);
        auto set_kv = [&](const std::string& k,
                          std::function<void()> emitv) {
            code.u8(0x20); code.u32(tobj);
            push_lit(k);
            emitv();
            code.u8(0x10); code.u32(H_OBJ_SET);
        };
        set_kv("type", [&]{ push_lit(vce->html_tag()); });
        if (!vce->id.empty())
            set_kv("id", [&]{ push_lit(vce->id); });
        for (auto& a : vce->attrs) {
            if (a.first == "tag") continue;
            if (!a.second)
                set_kv(a.first, [&]{ code.u8(0x41); code.i64(1);
                            code.u8(0x10); code.u32(H_BOX_I); });
            else
                set_kv(a.first,
                       [&]{ emit_typed(code, c, a.second, VT_EXT); });
        }
        // children -> массив диктов/текстов
        code.u8(0x10); code.u32(H_ARR_NEW);
        code.u8(0x21); code.u32(tarr);
        for (auto& ch : vce->children) {
            if (!ch) continue;
            bool is_txt =
                std::dynamic_pointer_cast<VcTextExpression>(ch)
                || std::dynamic_pointer_cast<VcOutputExpression>(ch);
            bool is_vc = std::dynamic_pointer_cast<VcExpression>(ch)
                         != nullptr;
            if (!is_txt && !is_vc) continue; // statement — пропуск
            code.u8(0x20); code.u32(tarr);
            if (is_txt) {
                // {type:"text", text:<v>}
                code.u8(0x10); code.u32(H_OBJ_NEW);
                code.u8(0x21); code.u32(ttxt);
                code.u8(0x20); code.u32(ttxt);
                push_lit("type"); push_lit("text");
                code.u8(0x10); code.u32(H_OBJ_SET);
                code.u8(0x20); code.u32(ttxt);
                push_lit("text");
                if (auto te = std::dynamic_pointer_cast<
                        VcTextExpression>(ch))
                    push_lit(te->text);
                else
                    emit_typed(code, c,
                        std::dynamic_pointer_cast<
                            VcOutputExpression>(ch)->expr, VT_EXT);
                code.u8(0x10); code.u32(H_OBJ_SET);
                code.u8(0x20); code.u32(ttxt);
            } else {
                emit_expr(code, c, ch); // nested vc -> dict
            }
            code.u8(0x10); code.u32(H_ARR_PUSH);
        }
        set_kv("children", [&]{ code.u8(0x20); code.u32(tarr); });
        code.u8(0x20); code.u32(tobj);
        --c.tmp_depth;
        return VT_EXT;
    }
    if (std::dynamic_pointer_cast<VcTextExpression>(n))
        return VT_EXT;

    if (auto bin = std::dynamic_pointer_cast<BinaryExpression>(n)) {
        int t = bin->_type;
        switch (t) {
            case PLUS:  return emit_arith(code, c, bin.get(), true);
            case MINUS: case STAR: case SLASH: case PERCENT:
                return emit_arith(code, c, bin.get(), false);
            case GT: case LT: case EQEQ: case EXCLEQ:
            case LTEQ: case GTEQ: {
                uint8_t a = expr_vt(c, bin->expr1);
                uint8_t b = expr_vt(c, bin->expr2);
                if (a == VT_EXT && b == VT_EXT) {
                    if (t != EQEQ && t != EXCLEQ)
                        throw Unsupported{"ext cmp"};
                    emit_typed(code, c, bin->expr1, VT_EXT);
                    emit_typed(code, c, bin->expr2, VT_EXT);
                    code.u8(0x10); code.u32((uint32_t)H_SEQ);
                    if (t == EXCLEQ) code.u8(0x45); // eqz
                    return VT_I32;
                }
                if (a == VT_EXT || b == VT_EXT)
                    throw Unsupported{"mixed cmp"};
                uint8_t mode = (a == VT_F64 || b == VT_F64)
                             ? VT_F64 : VT_I32;
                emit_typed(code, c, bin->expr1, mode);
                emit_typed(code, c, bin->expr2, mode);
                static const std::map<int, uint8_t> f64m = {
                    {LT, 0x63}, {GT, 0x64}, {LTEQ, 0x65},
                    {GTEQ, 0x66}, {EQEQ, 0x61}, {EXCLEQ, 0x62}};
                static const std::map<int, uint8_t> i32m = {
                    {LT, 0x48}, {GT, 0x4A}, {LTEQ, 0x4C},
                    {GTEQ, 0x4E}, {EQEQ, 0x46}, {EXCLEQ, 0x47}};
                code.u8((mode == VT_I32 ? i32m : f64m).at(t));
                return VT_I32;
            }
            case PLUSEQ: case MINUSEQ: case STAREQ:
            case SLASHEQ: case PERCENTEQ: {
                std::string v = plain_var(bin->expr1);
                auto it = c.locals.find(v);
                auto gi = (it == c.locals.end())
                        ? g_globals.find(v) : g_globals.end();
                bool is_local = (it != c.locals.end());
                if (v.empty() || (!is_local && gi == g_globals.end()))
                    throw Unsupported{"compound var"};
                // глобал: get; e2; op; global.set; global.get
                if (!is_local) {
                    uint8_t gvt = gi->second.second;
                    code.u8(0x23); code.u32(gi->second.first);
                    emit_typed(code, c, bin->expr2, gvt);
                    static const std::map<int, uint8_t> gf64m = {
                        {PLUSEQ, 0xA0}, {MINUSEQ, 0xA1},
                        {STAREQ, 0xA2}, {SLASHEQ, 0xA3},
                        {PERCENTEQ, 0xA9}};
                    static const std::map<int, uint8_t> gi32m = {
                        {PLUSEQ, 0x6A}, {MINUSEQ, 0x6B},
                        {STAREQ, 0x6C}, {SLASHEQ, 0x6E},
                        {PERCENTEQ, 0x6F}};
                    code.u8((gvt == VT_I32 ? gi32m : gf64m).at(t));
                    code.u8(0x24); code.u32(gi->second.first);
                    code.u8(0x23); code.u32(gi->second.first);
                    return gvt;
                }
                uint8_t vt = it->second.second;
                code.u8(0x20); code.u32(it->second.first);
                if (vt == VT_EXT) {
                    if (t != PLUSEQ) throw Unsupported{"ext compound"};
                    emit_typed(code, c, bin->expr2, VT_EXT);
                    code.u8(0x10); code.u32((uint32_t)H_CAT);
                } else {
                    emit_typed(code, c, bin->expr2, vt);
                    static const std::map<int, uint8_t> f64m = {
                        {PLUSEQ, 0xA0}, {MINUSEQ, 0xA1},
                        {STAREQ, 0xA2}, {SLASHEQ, 0xA3},
                        {PERCENTEQ, 0xA9}};
                    static const std::map<int, uint8_t> i32m = {
                        {PLUSEQ, 0x6A}, {MINUSEQ, 0x6B},
                        {STAREQ, 0x6C}, {SLASHEQ, 0x6E},
                        {PERCENTEQ, 0x6F}};
                    code.u8((vt == VT_I32 ? i32m : f64m).at(t));
                }
                code.u8(0x22); code.u32(it->second.first); // tee
                return vt;
            }
            case INCREMENT: case DECREMENT: {
                std::string v = plain_var(bin->expr1);
                auto it = c.locals.find(v);
                bool is_local = (it != c.locals.end());
                uint8_t vt;
                uint32_t idx;
                if (v.empty() || (!is_local &&
                    g_globals.find(v) == g_globals.end()))
                    throw Unsupported{"inc var"};
                if (is_local) { vt = it->second.second; idx = it->second.first; }
                else { auto gi = g_globals.at(v);
                       vt = gi.second; idx = gi.first; }
                code.u8(is_local ? 0x20 : 0x23); code.u32(idx);
                if (vt == VT_I32) { code.u8(0x41); code.i64(1); }
                else { code.u8(0x44); code.f64(1.0); }
                code.u8(vt == VT_I32
                        ? (t == INCREMENT ? 0x6A : 0x6B)
                        : (t == INCREMENT ? 0xA0 : 0xA1));
                if (is_local) {
                    code.u8(0x22); code.u32(idx); // local.tee
                } else {
                    code.u8(0x24); code.u32(idx); // global.set
                    code.u8(0x23); code.u32(idx); // global.get
                }
                return vt;
            }
            default: throw Unsupported{"binary op"};
        }
    }

    if (auto ce = std::dynamic_pointer_cast<ConditionalExpression>(n)) {
        switch (ce->type) {
            case AMPAMP: case BARBAR: {
                // короткое замыкание — как в eval:
                // &&: if first then second else 0
                // ||: if first then 1 else second
                emit_bool(code, c, ce->first);
                code.u8(0x04); code.u8(0x7F);   // if (result i32)
                if (ce->type == AMPAMP) {
                    emit_bool(code, c, ce->second);
                    code.u8(0x05);              // else
                    code.u8(0x41); code.i64(0);
                } else {
                    code.u8(0x41); code.i64(1);
                    code.u8(0x05);              // else
                    emit_bool(code, c, ce->second);
                }
                code.u8(0x0B);                  // end
                return VT_I32;
            }
            case LT: case GT: case EQEQ: case EXCLEQ:
            case LTEQ: case GTEQ: {
                uint8_t a = expr_vt(c, ce->first);
                uint8_t b = expr_vt(c, ce->second);
                if (a == VT_EXT && b == VT_EXT) {
                    if (ce->type != EQEQ && ce->type != EXCLEQ)
                        throw Unsupported{"ext cmp"};
                    emit_typed(code, c, ce->first, VT_EXT);
                    emit_typed(code, c, ce->second, VT_EXT);
                    code.u8(0x10); code.u32((uint32_t)H_SEQ);
                    if (ce->type == EXCLEQ) code.u8(0x45);
                    return VT_I32;
                }
                if (a == VT_EXT || b == VT_EXT)
                    throw Unsupported{"mixed cmp"};
                uint8_t mode = (a == VT_F64 || b == VT_F64)
                             ? VT_F64 : VT_I32;
                emit_typed(code, c, ce->first, mode);
                emit_typed(code, c, ce->second, mode);
                static const std::map<int, uint8_t> f64m = {
                    {LT, 0x63}, {GT, 0x64}, {LTEQ, 0x65},
                    {GTEQ, 0x66}, {EQEQ, 0x61}, {EXCLEQ, 0x62}};
                static const std::map<int, uint8_t> i32m = {
                    {LT, 0x48}, {GT, 0x4A}, {LTEQ, 0x4C},
                    {GTEQ, 0x4E}, {EQEQ, 0x46}, {EXCLEQ, 0x47}};
                code.u8((mode == VT_I32 ? i32m : f64m).at(ce->type));
                return VT_I32;
            }
            default: throw Unsupported{"cond op"};
        }
    }

    if (auto ex = std::dynamic_pointer_cast<ExcludeExpression>(n)) {
        emit_bool(code, c, ex->expr);
        code.u8(0x45); // i32.eqz
        return VT_I32;
    }

    if (auto un = std::dynamic_pointer_cast<UnaryExpression>(n)) {
        if (un->type != MINUS) throw Unsupported{"unary"};
        uint8_t t = expr_vt(c, un->expr);
        if (t == VT_I32) {
            code.u8(0x41); code.i64(0);
            emit_typed(code, c, un->expr, VT_I32);
            code.u8(0x6B); // i32.sub
        } else {
            code.u8(0x44); code.f64(0.0);
            emit_typed(code, c, un->expr, VT_F64);
            code.u8(0xA1); // f64.sub
        }
        return t;
    }

    throw Unsupported{typeid(*n.get()).name()};
}

// префикс getter-цепочки list[0..n) -> ext на стеке
// (для a[i]=v / a.b=v: база без последнего элемента)
static void emit_getter_base(Buf& code, FnCtx& c,
                             GetterExpression* g, size_t n) {
    emit_typed(code, c, g->list[0], VT_EXT);
    for (size_t i = 1; i < n; ++i) {
        NodePtr el = g->list[i];
        if (auto acc = std::dynamic_pointer_cast<
                ArrayAccessExpression>(el)) {
            if (acc->access.size() != 1)
                throw Unsupported{"multi index"};
            uint8_t kvt = expr_vt(c, acc->access[0]);
            if (kvt == VT_EXT) {
                emit_typed(code, c, acc->access[0], VT_EXT);
                code.u8(0x10); code.u32(H_OBJ_GET);
            } else {
                emit_typed(code, c, acc->access[0], VT_I32);
                code.u8(0x10); code.u32(H_ARR_GET);
            }
        } else if (auto mv = std::dynamic_pointer_cast<
                VariableExpression>(el)) {
            auto lit = g_lits.find(mv->text);
            if (lit == g_lits.end())
                throw Unsupported{"member name"};
            code.u8(0x23); code.u32(lit->second);
            code.u8(0x10); code.u32(H_OBJ_GET);
        } else {
            throw Unsupported{"getter base"};
        }
    }
}

// ---------- стейтменты ----------
static void emit_stmt(Buf& code, FnCtx& c, NodePtr n) {
    if (!n) return;

    if (auto b = std::dynamic_pointer_cast<BlockStatement>(n)) {
        for (auto& s : b->block) emit_stmt(code, c, s);
        return;
    }

    if (auto r = std::dynamic_pointer_cast<ReturnStatement>(n)) {
        if (r->expr) {
            if (c.result == VT_VOID) emit_expr(code, c, r->expr),
                code.u8(0x1A); // drop + return
            else emit_typed(code, c, r->expr, c.result);
        } else if (c.result == VT_I32) {
            code.u8(0x41); code.i64(0);
        } else if (c.result == VT_EXT) {
            code.u8(0xD0); code.u8(0x6F); // ref.null extern
        } else if (c.result == VT_F64) {
            code.u8(0x44); code.f64(0.0);
        }
        code.u8(0x0F); // return
        return;
    }

    if (auto a = std::dynamic_pointer_cast<AssignmentStatement>(n)) {
        std::string v = a->_plain_var_name();
        if (!v.empty()) {
            auto it = c.locals.find(v);
            if (it != c.locals.end()) {
                emit_typed(code, c, a->value, it->second.second);
                code.u8(0x21); code.u32(it->second.first); // local.set
                return;
            }
            auto gi = g_globals.find(v);
            if (gi != g_globals.end()) {
                emit_typed(code, c, a->value, gi->second.second);
                code.u8(0x24); code.u32(gi->second.first); // global.set
                return;
            }
            throw Unsupported{"assign var"};
        }
        // a[i] = v / a.b = v — цепочка [base, last]
        auto g = std::dynamic_pointer_cast<GetterExpression>(
            a->getter);
        if (g && g->list.size() >= 2) {
            NodePtr last = g->list.back();
            if (auto acc = std::dynamic_pointer_cast<
                    ArrayAccessExpression>(last)) {
                if (acc->access.size() != 1)
                    throw Unsupported{"multi index"};
                // базовый путь (все элементы кроме последнего)
                emit_getter_base(code, c, g.get(), g->list.size()-1);
                uint8_t kvt = expr_vt(c, acc->access[0]);
                if (kvt == VT_EXT) {
                    emit_typed(code, c, acc->access[0], VT_EXT);
                    emit_typed(code, c, a->value, VT_EXT);
                    code.u8(0x10); code.u32(H_OBJ_SET);
                } else {
                    emit_typed(code, c, acc->access[0], VT_I32);
                    emit_typed(code, c, a->value, VT_EXT);
                    code.u8(0x10); code.u32(H_ARR_SET);
                }
                return;
            }
            if (auto mv = std::dynamic_pointer_cast<
                    VariableExpression>(last)) {
                emit_getter_base(code, c, g.get(), g->list.size()-1);
                auto lit = g_lits.find(mv->text);
                if (lit == g_lits.end())
                    throw Unsupported{"member name"};
                code.u8(0x23); code.u32(lit->second); // "b"
                emit_typed(code, c, a->value, VT_EXT);
                code.u8(0x10); code.u32(H_OBJ_SET);
                return;
            }
        }
        throw Unsupported{"assign target"};
    }

    if (auto d = std::dynamic_pointer_cast<DeclarationStatement>(n)) {
        auto it = c.locals.find(d->name);
        if (it == c.locals.end()) throw Unsupported{"decl var"};
        if (d->value) {
            emit_typed(code, c, d->value, it->second.second);
            code.u8(0x21); code.u32(it->second.first);
        }
        return;
    }

    if (auto i = std::dynamic_pointer_cast<ConditionalStatement>(n)) {
        emit_bool(code, c, i->cond);
        code.u8(0x04); code.u8(0x40);      // if (void)
        emit_stmt(code, c, i->_if);
        if (i->_else) {
            code.u8(0x05);
            emit_stmt(code, c, i->_else);
        }
        code.u8(0x0B);                     // end
        return;
    }

    if (auto w = std::dynamic_pointer_cast<WhileStatement>(n)) {
        code.u8(0x02); code.u8(0x40);      // block
        code.u8(0x03); code.u8(0x40);      // loop
        c.loop_labels.push_back(1);
        emit_bool(code, c, w->expr);
        code.u8(0x45);                     // i32.eqz
        code.u8(0x0D); code.u32(1);        // br_if 1
        emit_stmt(code, c, w->block);
        code.u8(0x0C); code.u32(0);        // br 0
        code.u8(0x0B);
        code.u8(0x0B);
        c.loop_labels.pop_back();
        return;
    }

    if (auto f = std::dynamic_pointer_cast<ForStatement>(n)) {
        emit_stmt(code, c, f->decl);
        code.u8(0x02); code.u8(0x40);
        code.u8(0x03); code.u8(0x40);
        c.loop_labels.push_back(1);
        if (f->_while) {
            emit_bool(code, c, f->_while);
            code.u8(0x45);
            code.u8(0x0D); code.u32(1);
        }
        emit_stmt(code, c, f->block);
        if (f->_do) emit_stmt(code, c, f->_do);
        code.u8(0x0C); code.u32(0);
        code.u8(0x0B);
        code.u8(0x0B);
        c.loop_labels.pop_back();
        return;
    }

    if (auto fe = std::dynamic_pointer_cast<ForeachStatement>(n)) {
        // for v : xs -> __fe_a = xs; __fe_i = 0;
        // loop { i<len? v=a[i]; key=i; block; i++ }
        int id = c.fe_ids.at(fe.get());
        std::string an = "__fe_a" + std::to_string(id);
        std::string in = "__fe_i" + std::to_string(id);
        uint32_t ai = c.locals.at(an).first;
        uint32_t ii = c.locals.at(in).first;
        uint32_t vi = c.locals.at(fe->value).first;
        // __fe_a = xs
        emit_typed(code, c, fe->expr, VT_EXT);
        code.u8(0x21); code.u32(ai);
        // __fe_i = 0
        code.u8(0x41); code.i64(0);
        code.u8(0x21); code.u32(ii);
        // block { loop { i<len; eqz; br_if 1; v=a[i]; key?; body; i++; br 0 } }
        code.u8(0x02); code.u8(0x40);
        code.u8(0x03); code.u8(0x40);
        c.loop_labels.push_back(1);
        code.u8(0x20); code.u32(ii);
        code.u8(0x20); code.u32(ai);
        code.u8(0x10); code.u32(H_LEN);
        code.u8(0x48);                    // i32.lt_s
        code.u8(0x45);                    // eqz
        code.u8(0x0D); code.u32(1);       // br_if 1
        // v = a[i]
        code.u8(0x20); code.u32(ai);
        code.u8(0x20); code.u32(ii);
        code.u8(0x10); code.u32(H_ARR_GET);
        code.u8(0x21); code.u32(vi);
        // key = i (если есть)
        if (!fe->key.empty()) {
            uint32_t ki = c.locals.at(fe->key).first;
            code.u8(0x20); code.u32(ii);
            code.u8(0x21); code.u32(ki);
        }
        emit_stmt(code, c, fe->block);
        // i++
        code.u8(0x20); code.u32(ii);
        code.u8(0x41); code.i64(1);
        code.u8(0x6A);
        code.u8(0x21); code.u32(ii);
        code.u8(0x0C); code.u32(0);
        code.u8(0x0B);
        code.u8(0x0B);
        c.loop_labels.pop_back();
        return;
    }

    if (std::dynamic_pointer_cast<BreakStatement>(n)) {
        if (c.loop_labels.empty()) throw Unsupported{"break"};
        code.u8(0x0C);
        code.u32((uint32_t)c.loop_labels.back());
        return;
    }
    if (std::dynamic_pointer_cast<ContinueStatement>(n)) {
        if (c.loop_labels.empty()) throw Unsupported{"continue"};
        code.u8(0x0C);
        code.u32((uint32_t)c.loop_labels.back() - 1);
        return;
    }

    // выражение-стейтмент: drop если есть значение
    if (emit_expr(code, c, n) != VT_VOID)
        code.u8(0x1A); // drop
}

// ---------- модуль ----------
inline std::vector<uint8_t> build_module(Nvm& vm) {
    struct Cand { std::string name; FuncPtr fn; };
    std::vector<Cand> cands;
    for (auto& kv : vm.vars) {
        if (!kv.second.isFunc()) continue;
        auto f = kv.second.asFunc();
        if (!f || !f->decl_node) continue;
        auto d = std::dynamic_pointer_cast<FunctionDeclaration>(
            f->decl_node);
        if (d) cands.push_back({kv.first, f});
    }
    if (cands.empty()) return {};

    // реестр имён: fn idx = H_COUNT + i (после импортов-хелперов)
    std::vector<FnSig> sigs(cands.size());
    for (size_t i = 0; i < cands.size(); ++i) {
        auto d = std::dynamic_pointer_cast<FunctionDeclaration>(
            cands[i].fn->decl_node);
        for (auto& a : d->args)
            sigs[i].params.push_back(type_vt(a.second.first));
        sigs[i].result = type_vt(d->type);
        g_fns[cands[i].name] =
            {(uint32_t)(H_COUNT + i), sigs[i]};
        for (auto& s : d->statements) collect_lits(s);
    }

    // глобальные переменные: int/dbl/str/list/dict из
    // vm.vars -> mutable wasm-глобалы (idx после lit).
    // ext-глобалы стартуют с ref.null — инициализация
    // значением идёт в boot() (присваивание внутри fn)
    std::vector<std::pair<std::string,uint8_t>> gorder;
    for (auto& kv : vm.vars) {
        const NObject& o = kv.second;
        if (o.isFunc()) continue;
        uint8_t vt = o.isInt() ? VT_I32
                   : o.isDouble() ? VT_F64 : VT_EXT;
        if (kv.first.rfind("__", 0) == 0) continue; // служебные
        if (kv.first == "print" || kv.first == "transmit")
            continue;
        uint32_t idx =
            (uint32_t)(g_lit_order.size() + gorder.size());
        g_globals[kv.first] = {idx, vt};
        gorder.push_back({kv.first, vt});
    }

    // ---- type-секция: хелперы + fn-сигнатуры ----
    auto sig_key = [](const std::vector<uint8_t>& p,
                      uint8_t r) {
        std::string k((char*)p.data(), p.size());
        k += '#'; k += (char)r;
        return k;
    };
    std::map<std::string, uint32_t> typeidx;
    std::vector<std::pair<std::vector<uint8_t>,uint8_t>> siglist;
    auto add_type = [&](const std::vector<uint8_t>& p, uint8_t r) {
        std::string k = sig_key(p, r);
        if (!typeidx.count(k)) {
            typeidx[k] = (uint32_t)siglist.size();
            siglist.push_back({p, r});
        }
        return typeidx[k];
    };
    // хелпер-типы (порядок = HelperIdx)
    std::vector<uint8_t> e0;
    std::vector<uint8_t> ee = {VT_EXT, VT_EXT};
    std::vector<uint8_t> eee = {VT_EXT, VT_EXT, VT_EXT};
    std::vector<uint8_t> ei = {VT_EXT, VT_I32};
    std::vector<uint8_t> eie = {VT_EXT, VT_I32, VT_EXT};
    std::vector<uint8_t> e1 = {VT_EXT};
    std::vector<uint8_t> i1 = {VT_I32};
    std::vector<uint8_t> f1 = {VT_F64};
    std::vector<uint32_t> htype(H_COUNT);
    htype[H_CAT]     = add_type(ee, VT_EXT);
    htype[H_SEQ]     = add_type(ee, VT_I32);
    htype[H_PRINT_E] = add_type(e1, VT_VOID);
    htype[H_PRINT_I] = add_type(i1, VT_VOID);
    htype[H_PRINT_F] = add_type(f1, VT_VOID);
    htype[H_STR_I]   = add_type(i1, VT_EXT);
    htype[H_STR_F]   = add_type(f1, VT_EXT);
    htype[H_LEN]     = add_type(e1, VT_I32);
    htype[H_ARR_NEW] = add_type(e0, VT_EXT);
    htype[H_ARR_PUSH]= add_type(ee, VT_VOID);
    htype[H_ARR_GET] = add_type(ei, VT_EXT);
    htype[H_ARR_SET] = add_type(eie, VT_VOID);
    htype[H_ARR_POP] = add_type(e1, VT_EXT);
    htype[H_BOX_I]   = add_type(i1, VT_EXT);
    htype[H_BOX_F]   = add_type(f1, VT_EXT);
    htype[H_UNBOX_I] = add_type(e1, VT_I32);
    htype[H_UNBOX_F] = add_type(e1, VT_F64);
    htype[H_OBJ_NEW] = add_type(e0, VT_EXT);
    htype[H_OBJ_GET] = add_type(ee, VT_EXT);
    htype[H_OBJ_SET] = add_type(eee, VT_VOID);
    htype[H_TRUTHY]  = add_type(e1, VT_I32);
    htype[H_STR2_I]  = add_type(e1, VT_I32);
    htype[H_STR2_F]  = add_type(e1, VT_F64);
    htype[H_JOIN]    = add_type(ee, VT_EXT);
    htype[H_SLICE]   = add_type({VT_EXT,VT_I32,VT_I32}, VT_EXT);
    htype[H_SPLIT]   = add_type(ee, VT_EXT);
    htype[H_CONTAINS]= add_type(ee, VT_I32);
    htype[H_INDEX]   = add_type(ee, VT_I32);
    htype[H_LOWER]   = add_type(e1, VT_EXT);
    htype[H_UPPER]   = add_type(e1, VT_EXT);
    htype[H_REPLACE] = add_type(eee, VT_EXT);
    htype[H_MATCH]   = add_type(ee, VT_EXT);
    htype[H_RE_TEST] = add_type(ee, VT_I32);
    // canvas-сигнатуры
    htype[H_CTX]     = add_type(e0, VT_EXT);
    htype[H_RECT]    = add_type(
        {VT_EXT,VT_I32,VT_I32,VT_I32,VT_I32,VT_EXT}, VT_VOID);
    htype[H_TEXT]    = add_type(
        {VT_EXT,VT_EXT,VT_I32,VT_I32,VT_EXT}, VT_VOID);
    htype[H_CLEAR]   = add_type({VT_EXT,VT_I32,VT_I32}, VT_VOID);
    htype[H_LINE]    = add_type(
        {VT_EXT,VT_I32,VT_I32,VT_I32,VT_I32,VT_EXT}, VT_VOID);
    htype[H_CIRCLE]  = add_type(
        {VT_EXT,VT_I32,VT_I32,VT_I32,VT_EXT}, VT_VOID);
    htype[H_HAS]     = add_type(ee, VT_I32);
    htype[H_CHR]     = add_type(i1, VT_EXT);
    htype[H_FONT]    = add_type({VT_EXT,VT_I32}, VT_VOID);
    htype[H_RRECT]   = add_type(
        {VT_EXT,VT_I32,VT_I32,VT_I32,VT_I32,VT_I32,VT_EXT},
        VT_VOID);
    htype[H_CLIP]    = add_type(ee, VT_VOID);
    htype[H_PASTE]   = add_type(e0, VT_VOID);
    // w/h тоже принимают ctx — иначе _ctx() остаётся
    // на стеке и ломает сигнатуру вызывающего
    htype[H_SCRW]    = add_type(e1, VT_I32);
    htype[H_SCRH]    = add_type(e1, VT_I32);
    htype[H_FETCH]   = add_type(e1, VT_VOID);
    htype[H_JSON]    = add_type(e1, VT_EXT);
    // image/файлы: ctx первым — как у всех display.*
    htype[H_IMGPICK] = add_type(ee, VT_VOID);
    htype[H_IMGDRAW] = add_type(
        {VT_EXT,VT_EXT,VT_I32,VT_I32,VT_I32,VT_I32}, VT_VOID);
    htype[H_IMGPIX]  = add_type(
        {VT_EXT,VT_EXT,VT_I32,VT_I32,VT_I32,VT_I32,
         VT_I32,VT_I32}, VT_EXT);
    htype[H_DL]      = add_type(eee, VT_VOID);
    htype[H_IMGMASK] = add_type(
        {VT_EXT,VT_EXT,VT_I32,VT_I32,VT_I32,VT_I32,
         VT_I32,VT_I32,VT_I32,VT_EXT}, VT_EXT);
    htype[H_DLPNG]   = add_type(
        {VT_EXT,VT_EXT,VT_I32,VT_I32,VT_I32,VT_I32,
         VT_EXT}, VT_VOID);
    htype[H_IMGDRAWC] = add_type(
        {VT_EXT,VT_EXT,VT_I32,VT_I32,VT_I32,VT_I32,
         VT_I32,VT_I32,VT_I32,VT_I32}, VT_VOID);
    htype[H_QUAD]   = add_type(
        {VT_EXT,VT_I32,VT_I32,VT_I32,VT_I32,
         VT_I32,VT_I32,VT_I32,VT_I32,VT_EXT}, VT_VOID);
    htype[H_CLIPR]  = add_type(
        {VT_EXT,VT_I32,VT_I32,VT_I32,VT_I32}, VT_VOID);
    htype[H_UNCLIP] = add_type({VT_EXT}, VT_VOID);
    htype[H_TRI]    = add_type(
        {VT_EXT,VT_I32,VT_I32,VT_I32,VT_I32,
         VT_I32,VT_I32,VT_EXT}, VT_VOID);
    htype[H_PICKTXT] = add_type(ee, VT_VOID);
    htype[H_SSAVE]  = add_type(eee, VT_VOID);
    htype[H_SLOAD]  = add_type(ee, VT_EXT);
    // math.* — f64 only
    htype[H_POW]     = add_type({VT_F64,VT_F64}, VT_F64);
    htype[H_EXP]     = add_type(f1, VT_F64);
    htype[H_SQRT]    = add_type(f1, VT_F64);
    htype[H_SIN]     = add_type(f1, VT_F64);
    htype[H_COS]     = add_type(f1, VT_F64);
    htype[H_ATAN2]   = add_type({VT_F64,VT_F64}, VT_F64);
    htype[H_LOG]     = add_type(f1, VT_F64);
    htype[H_FLOOR]   = add_type(f1, VT_F64);
    htype[H_ABS]     = add_type(f1, VT_F64);
    htype[H_RAND]    = add_type(e0, VT_F64);
    // fn-типы
    std::vector<uint32_t> ftype(cands.size());
    for (size_t i = 0; i < cands.size(); ++i)
        ftype[i] = add_type(sigs[i].params, sigs[i].result);

    Buf tsec;
    tsec.u32((uint32_t)siglist.size());
    for (auto& [params, r] : siglist) {
        tsec.u8(0x60);
        tsec.u32((uint32_t)params.size());
        for (uint8_t vt : params) tsec.u8(vt);
        if (r == VT_VOID) tsec.u32(0);
        else { tsec.u32(1); tsec.u8(r); }
    }

    // ---- import-секция: env-хелперы + литералы-глобалы ----
    Buf isec;
    isec.u32((uint32_t)(H_COUNT + g_lit_order.size()));
    for (int i = 0; i < H_COUNT; ++i) {
        isec.name("env"); isec.name(H_NAMES[i]);
        isec.u8(0x00);                 // func
        isec.u32(htype[i]);
    }
    for (size_t i = 0; i < g_lit_order.size(); ++i) {
        isec.name("env");
        isec.name("lit" + std::to_string(i));
        isec.u8(0x03);                 // global
        isec.u8(VT_EXT); isec.u8(0x00); // externref immutable
    }

    // ---- function-секция ----
    Buf fsec; fsec.u32((uint32_t)cands.size());
    for (size_t i = 0; i < cands.size(); ++i) fsec.u32(ftype[i]);

    // ---- global-секция: mutable int/dbl переменные ----
    Buf gsec; gsec.u32((uint32_t)gorder.size());
    for (auto& [nm, vt] : gorder) {
        gsec.u8(vt); gsec.u8(0x01);      // vt, mut
        const NObject& o = vm.vars.at(nm);
        if (vt == VT_I32) { gsec.u8(0x41); gsec.i64(o.asInt()); }
        else if (vt == VT_F64) { gsec.u8(0x44); gsec.f64(o.asDouble()); }
        else { gsec.u8(0xD0); gsec.u8(0x6F); } // ref.null extern
        gsec.u8(0x0B);                    // end init-expr
    }

    // ---- export-секция ----
    Buf esec; esec.u32((uint32_t)cands.size());
    for (size_t i = 0; i < cands.size(); ++i) {
        esec.name(cands[i].name);
        esec.u8(0x00);
        esec.u32((uint32_t)(H_COUNT + i));
    }

    // ---- code-секция ----
    Buf csec; csec.u32((uint32_t)cands.size());
    for (size_t i = 0; i < cands.size(); ++i) {
        auto d = std::dynamic_pointer_cast<FunctionDeclaration>(
            cands[i].fn->decl_node);
        FnCtx ctx; ctx.decl = d.get();
        ctx.result = sigs[i].result;
        for (auto& a : d->args)
            declare_local(ctx, a.first, type_vt(a.second.first));
        for (auto& s : d->statements) collect_locals(s, ctx);
        // пул скрытых темпов для литералов/vc (по глубине
        // вложенности; vc берёт три слота: +0 obj, +8 arr,
        // +16 text) — декларируем до записи locals
        for (int ti = 0; ti < 24; ++ti)
            declare_local(ctx, "__t" + std::to_string(ti), VT_EXT);

        Buf body;
        // локали: runs в ПОРЯДКЕ объявления — индексы
        // позиционные, группировка по типу их ломает
        std::vector<uint8_t> lorder(ctx.n_locals, VT_F64);
        for (auto& kv : ctx.locals)
            lorder[kv.second.first] = kv.second.second;
        std::vector<std::pair<uint32_t,uint8_t>> runs;
        for (size_t li = d->args.size(); li < ctx.n_locals; ++li) {
            uint8_t vt = lorder[li];
            if (!runs.empty() && runs.back().second == vt)
                runs.back().first++;
            else runs.push_back({1, vt});
        }
        body.u32((uint32_t)runs.size());
        for (auto& [cnt, vt] : runs) {
            body.u32(cnt); body.u8(vt);
        }

        Buf instr;
        try {
            for (auto& s : d->statements) emit_stmt(instr, ctx, s);
        } catch (Unsupported& u) {
            static std::string msg;
            msg = cands[i].name + ":" + u.why;
            throw Unsupported{msg.c_str()};
        }
        if (ctx.result != VT_VOID) {
            // неявный нулевой результат
            if (ctx.result == VT_I32) { instr.u8(0x41); instr.i64(0); }
            else if (ctx.result == VT_EXT) { instr.u8(0xD0); instr.u8(0x6F); } // ref.null extern
            else { instr.u8(0x44); instr.f64(0.0); }
        }
        instr.u8(0x0B);                 // end
        body.b.insert(body.b.end(), instr.b.begin(), instr.b.end());
        csec.u32((uint32_t)body.b.size());
        csec.b.insert(csec.b.end(), body.b.begin(), body.b.end());
    }

    Buf m;
    m.u8(0); m.u8('a'); m.u8('s'); m.u8('m');
    m.u8(1); m.u8(0); m.u8(0); m.u8(0);
    m.section(1, tsec);
    m.section(2, isec);
    m.section(3, fsec);
    m.section(6, gsec);
    m.section(7, esec);
    m.section(10, csec);
    return m.b;
}

} // namespace wasmjit

// точка входа из wasm_main: модуль всех tintable-функций
std::string nlang_wasm_module(Nvm& vm) {
    wasmjit::g_fns.clear();
    wasmjit::g_globals.clear();
    wasmjit::g_lits.clear();
    wasmjit::g_lit_order.clear();
    auto b = wasmjit::build_module(vm);
    return std::string((const char*)b.data(), b.size());
}

// строковые литералы модуля (JSON-массив) — JS подаёт их
// как импортируемые externref-глобалы lit0..litN
const std::vector<std::string>& nlang_wasm_literals() {
    return wasmjit::g_lit_order;
}
