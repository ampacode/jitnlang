// mini-JIT "anti nlang": AST -> x64 машинный код, мимо g++.
//
// Идея: интерпретатор — уже скомпилированная программа.
// Поэтому эмиттер порождает вызовы ТЕХ ЖЕ рантайм-помощников
// (jit_rt.cpp), что и сгенерированный temp.cpp, а всё, что не
// умеет нативно, уходит обратно в node->eval() как
// подпрограмму (escape hatch). По мере роста покрытия
// интерпретатор "растворяется" в машинном коде.
//
// Два домена значений:
//   Int — rax = int64 (слоты в кадре = int64)
//   Obj — rax = NObject* (слот в кадре = полноценный NObject)
// Сигнатура: long long fn(Nvm* vm, const long long* args,
//                         NObject* retobj)
// rcx=vm, rdx=args, r8=retobj. Результат int -> rax,
// объектный -> *retobj (тогда rax=0).
//
// Кадр (оффсеты от rbp):
//   8=rbx 16=rdi 24=rsi 32=vm 40=retobj
//   int-слоты со слота 48, далее int-scratch (self-call
//   аргументы + staging), далее obj-регион (NObject слоты,
//   init/dtor рантаймом в прологе/эпилоге), далее argv
//   указателей. Строковые литералы в конце буфера кода.
// Двухпроходная компиляция: dry-run считает layout,
// второй проход эмитит с готовыми оффсетами.
#include "jit.h"
#include "nobject.h"
#include "nvm.h"
#include "node.h"
#include "type.h"
#include "expressions/variable.h"
#include "expressions/number.h"
#include "expressions/string.h"
#include "expressions/binary.h"
#include "expressions/conditional.h"
#include "expressions/getter.h"
#include "expressions/calling.h"
#include "expressions/list.h"
#include "statements/_return.h"
#include "statements/_while.h"
#include "statements/_for.h"
#include "statements/conditional.h"
#include "statements/block.h"
#include "statements/assignment.h"
#include "statements/foreach.h"
#include <cstdio>
#include <cstdlib>
#include <set>

// mini-JIT: два бэкенда — x86-64 (десктоп) и arm64 (Android).
// Общая часть — JitCtx/emit/двухпроходка; различия — JitBuf-
// методы, пролог/эпилог и системный доступ к исполняемой
// памяти. На неподдержанных архитектурах — заглушка.
#if defined(__x86_64__) || defined(_M_X64)
  #define NL_JIT_X64 1
#elif defined(__aarch64__) || defined(__arm64__)
  #define NL_JIT_ARM64 1
#endif

#if defined(NL_JIT_X64) || defined(NL_JIT_ARM64)

#ifdef NL_JIT_X64
#include <windows.h>
#else
#include <sys/mman.h>
#endif

// байт-эметтеры — арх-нейтральные (u32 = 4 байта LE,
// на arm64 это как раз инструкционное слово)
void JitBuf::u8(uint8_t b) {
    if (len >= cap) throw std::runtime_error("jit overflow");
    code[len++] = b;
}
void JitBuf::u32(uint32_t v) {
    u8(v & 0xff); u8((v >> 8) & 0xff);
    u8((v >> 16) & 0xff); u8((v >> 24) & 0xff);
}
void JitBuf::u64(uint64_t v) { u32((uint32_t)v); u32(v >> 32); }

#if defined(NL_JIT_X64)

// ---------------- JitBuf — x86-64 ----------------

JitBuf::JitBuf() {
    cap = 1 << 18; // 256K — литералы и код в одном буфере
    code = (uint8_t*)VirtualAlloc(nullptr, cap,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!code) throw std::runtime_error("VirtualAlloc failed");
}
JitBuf::~JitBuf() {
    if (code) VirtualFree(code, 0, MEM_RELEASE);
}
void JitBuf::patch32(size_t at, int32_t rel) {
    std::memcpy(code + at, &rel, 4);
}

void JitBuf::ret() { u8(0xC3); }
void JitBuf::push_rax() { u8(0x50); }
void JitBuf::push_reg(int r) { u8(0x50 + r); }
void JitBuf::pop_rax() { u8(0x58); }
void JitBuf::pop_reg(int r) { u8(0x58 + r); }
void JitBuf::push_rbp() { u8(0x55); }
void JitBuf::mov_rbp_rsp() { u8(0x48); u8(0x89); u8(0xE5); }
void JitBuf::mov_rax_imm64(int64_t v) {
    u8(0x48); u8(0xB8); u64((uint64_t)v);
}
void JitBuf::mov_rax_rbx() { u8(0x48); u8(0x89); u8(0xD8); }
void JitBuf::mov_rbx_rax() { u8(0x48); u8(0x89); u8(0xC3); }
void JitBuf::mov_rax_rdx() { u8(0x48); u8(0x89); u8(0xD0); }
void JitBuf::mov_rax_rcx() { u8(0x48); u8(0x89); u8(0xC8); }
// disp32-формы — без ограничения ±127
void JitBuf::mov_rax_mem_rbp(int off) { // [rbp-off] -> rax
    u8(0x48); u8(0x8B); u8(0x85); u32((uint32_t)-off);
}
void JitBuf::mov_mem_rbp_rax(int off) { // rax -> [rbp-off]
    u8(0x48); u8(0x89); u8(0x85); u32((uint32_t)-off);
}
void JitBuf::mov_mem_rbp_reg(int off, int r) {
    u8(r >= 8 ? 0x4C : 0x48);
    u8(0x89); u8(0x85 + (r & 7) * 8);
    u32((uint32_t)-off);
}
void JitBuf::mov_reg_mem_rbp(int off, int r) { // r = [rbp-off]
    u8(r >= 8 ? 0x4C : 0x48);
    u8(0x8B); u8(0x85 + (r & 7) * 8);
    u32((uint32_t)-off);
}
void JitBuf::mov_rcx_mem_rbp(int off) { // [rbp-off] -> rcx
    u8(0x48); u8(0x8B); u8(0x8D); u32((uint32_t)-off);
}
void JitBuf::lea_rdx_rbp(int off) { // rdx = rbp - off
    u8(0x48); u8(0x8D); u8(0x95); u32((uint32_t)-off);
}
void JitBuf::add_rax_rbx() { u8(0x48); u8(0x01); u8(0xD8); }
void JitBuf::sub_rax_rbx() { u8(0x48); u8(0x29); u8(0xD8); }
void JitBuf::imul_rax_rbx() {
    u8(0x48); u8(0x0F); u8(0xAF); u8(0xC3);
}
void JitBuf::cqo_idiv_rbx() {
    u8(0x48); u8(0x99); u8(0x48); u8(0xF7); u8(0xFB);
}
void JitBuf::cmp_rax_rbx() { u8(0x48); u8(0x39); u8(0xD8); }
void JitBuf::test_rax_rax() { u8(0x48); u8(0x85); u8(0xC0); }
void JitBuf::setcc_al(int cc) {
    u8(0x0F); u8(0x90 + cc); u8(0xC0);
}
void JitBuf::movzx_rax_al() {
    u8(0x48); u8(0x0F); u8(0xB6); u8(0xC0);
}
size_t JitBuf::jmp_rel32() {
    u8(0xE9); size_t at = len; u32(0); return at;
}
size_t JitBuf::jcc_rel32(int cc) {
    u8(0x0F); u8(0x80 + cc); size_t at = len; u32(0); return at;
}
void JitBuf::patch_here(size_t at) {
    patch32(at, (int32_t)(len - (at + 4)));
}
size_t JitBuf::call_rel32() {
    u8(0xE8); size_t at = len; u32(0); return at;
}
void JitBuf::sub_rsp(int n) {
    u8(0x48); u8(0x81); u8(0xEC); u32((uint32_t)n);
}
void JitBuf::add_rsp(int n) {
    u8(0x48); u8(0x81); u8(0xC4); u32((uint32_t)n);
}
// --- sse2: double-домен (xmm0..7, modrm 3-бит) ---
// xmm <- (double)rax
void JitBuf::cvtsi2sd_xmm_rax(int x) {
    u8(0xF2); u8(0x48); u8(0x0F); u8(0x2A);
    u8(0xC0 + (x << 3));
}
// xmm <- (double)[rbp-off] — disp32 (оффсеты >127)
void JitBuf::cvtsi2sd_xmm_mem(int x, int off) {
    u8(0xF2); u8(0x48); u8(0x0F); u8(0x2A);
    u8(0x85 + (x << 3)); u32((uint32_t)-off);
}
// rax <- (long long)xmm — усечение, как asInt()
void JitBuf::cvttsd2si_rax_xmm(int x) {
    u8(0xF2); u8(0x48); u8(0x0F); u8(0x2C);
    u8(0xC0 + (x << 3));
}
// xmm <- биты rax
void JitBuf::movq_xmm_rax(int x) {
    u8(0x66); u8(0x48); u8(0x0F); u8(0x6E);
    u8(0xC0 + (x << 3));
}
// xmm <- [rbp-off]
void JitBuf::movsd_xmm_mem(int x, int off) {
    u8(0xF2); u8(0x0F); u8(0x10);
    u8(0x85 + (x << 3)); u32((uint32_t)-off);
}
// [rbp-off] <- xmm
void JitBuf::movsd_mem_xmm(int x, int off) {
    u8(0xF2); u8(0x0F); u8(0x11);
    u8(0x85 + (x << 3)); u32((uint32_t)-off);
}
// xmm_d <- xmm_s
void JitBuf::movsd_xmm_xmm(int d, int s) {
    u8(0xF2); u8(0x0F); u8(0x10);
    u8(0xC0 + (d << 3) + s);
}
void JitBuf::sdsd_xmm(int op, int d, int s) { // op: 58 add,5C sub,59 mul,5E div
    u8(0xF2); u8(0x0F); u8(op);
    u8(0xC0 + (d << 3) + s);
}
void JitBuf::ucomisd_xmm(int a, int b) {
    u8(0x66); u8(0x0F); u8(0x2E);
    u8(0xC0 + (a << 3) + b);
}
void JitBuf::pxor_xmm(int a, int b) { // обнуление: pxor x,x
    u8(0x66); u8(0x0F); u8(0xEF);
    u8(0xC0 + (a << 3) + b);
}
// --- быстрый путь списков ---
void JitBuf::imul_rax_imm8(int v) {
    u8(0x48); u8(0x6B); u8(0xC0); u8((uint8_t)v);
}
void JitBuf::mov_rax_mem_rax(int d) {
    u8(0x48); u8(0x8B); u8(0x80); u32((uint32_t)d);
}
void JitBuf::mov_rbx_mem_rax(int d) {
    u8(0x48); u8(0x8B); u8(0x98); u32((uint32_t)d);
}
void JitBuf::mov_rax_mem_rbx(int d) {
    u8(0x48); u8(0x8B); u8(0x83); u32((uint32_t)d);
}
void JitBuf::mov_mem_rax_rdx(int d) {
    u8(0x48); u8(0x89); u8(0x90); u32((uint32_t)d);
}
void JitBuf::mov_mem_rax_rbx(int d) {
    u8(0x48); u8(0x89); u8(0x98); u32((uint32_t)d);
}
void JitBuf::mov_mem_rax_imm32(int d, int v) {
    u8(0x48); u8(0xC7); u8(0x80);
    u32((uint32_t)d); u32((uint32_t)v);
}
void JitBuf::mov_mem_rbx_rax(int d) {
    u8(0x48); u8(0x89); u8(0x83); u32((uint32_t)d);
}
void JitBuf::cmp_rax_mem_rbx(int d) {
    u8(0x48); u8(0x3B); u8(0x83); u32((uint32_t)d);
}
void JitBuf::cmp_mem_rax_imm8(int d, int v) {
    u8(0x80); u8(0xB8); u32((uint32_t)d); u8((uint8_t)v);
}
void JitBuf::mov_mem_rax_imm8(int d, int v) {
    u8(0xC6); u8(0x80); u32((uint32_t)d); u8((uint8_t)v);
}
void JitBuf::add_rax_imm8(int v) {
    u8(0x48); u8(0x83); u8(0xC0); u8((uint8_t)v);
}
// арх-нейтральные над общим API — x86-64 формы
void JitBuf::inc_rax() {
    u8(0x48); u8(0x83); u8(0xC0); u8(1);
}
void JitBuf::dec_rax() {
    u8(0x48); u8(0x83); u8(0xE8); u8(1);
}
void JitBuf::test_rbx_rbx() { u8(0x48); u8(0x85); u8(0xDB); }
void JitBuf::mov_rax_mem_rdx(int d) { // rax = [rdx+d]
    u8(0x48); u8(0x8B); u8(0x82); u32((uint32_t)d);
}
void JitBuf::jmp_abs(size_t target) {
    u8(0xE9);
    u32((uint32_t)(int32_t)((int64_t)target - (len + 4)));
}
void JitBuf::patch_frame(size_t at, int v) {
    patch32(at - 4, (int32_t)v); // на x86 — imm32 в конце инсн
}
void JitBuf::ret_acc() {}          // rax и есть return-рег
// литерал: 8-байтный адрес после rex+opcode mov-загрузки
void JitBuf::patch_lit(size_t at, uint64_t addr) {
    std::memcpy(code + at + 2, &addr, 8);
}
// сделать буфер исполняемым
static bool jit_make_exec(JitBuf& b) {
    DWORD old;
    return VirtualProtect(b.code, b.cap, PAGE_EXECUTE_READ,
                          &old) != 0;
}

#endif // NL_JIT_X64

// ---------------- компилятор ----------------

// помощники рантайма (jit_rt.cpp)
extern "C" {
void nl_jit_obj_init(NObject*);
void nl_jit_obj_dtor(NObject*);
void nl_jit_obj_set(NObject*, const NObject*);
NObject* nl_jit_var(Nvm*, const char*);
void nl_jit_var_get(Nvm*, const char*, NObject*);
void nl_jit_var_set(Nvm*, const char*, const NObject*);
long long nl_jit_to_int(const NObject*);
void nl_jit_of_int(NObject*, long long);
void nl_jit_of_double_bits(NObject*, long long);
void nl_jit_str_init(NObject*, const char*, long long);
void nl_jit_to_str(NObject*, const NObject*);
void nl_jit_strcat_lit(NObject*, const char*, long long);
void nl_jit_strcat_obj(NObject*, const NObject*);
void nl_jit_arith(int, const NObject*, const NObject*,
                  NObject*);
long long nl_jit_cmp(int, const NObject*, const NObject*);
void nl_jit_list_init(NObject*);
void nl_jit_list_push_ll(NObject*, long long);
void nl_jit_list_push_obj(NObject*, const NObject*);
long long nl_jit_list_size(const NObject*);
long long nl_jit_at_ll(const NObject*, long long);
void nl_jit_at_obj(const NObject*, long long, NObject*);
NObject* nl_jit_call_named(Nvm*, const char*, NObject**, int);
long long nl_jit_size(const NObject*);
long long nl_jit_eval_int(Nvm*, Node*);
void nl_jit_eval_obj(Nvm*, Node*, NObject*);
long long nl_jit_exec_stmt(Nvm*, Node*);
long long nl_jit_retval_int(Nvm*);
void nl_jit_retval_obj(Nvm*, NObject*);
void nl_jit_of_dbl(NObject*, double);
double nl_jit_to_dbl(const NObject*);
double nl_jit_fmod(double, double);
void nl_jit_div_zero(Nvm*);
const NObject* nl_jit_list_data(const NObject*);
long long nl_jit_layout_ll_off();
long long nl_jit_layout_idx_off();
long long nl_jit_layout_this_off();
long long nl_jit_layout_nobj_size();
}

enum class Dom { Int, Obj, Dbl };

struct JitCtx {
    JitBuf b;
    NFunction* fn = nullptr;
    bool dry = false; // dry-run: считаем layout, код мусорный
    static const int VM_SLOT = 32;
    static const int RET_SLOT = 40;
    std::map<std::string, int> slots;  // int-домен (оффсет)
    std::map<std::string, int> oslots; // obj-домен (index)
    int next_off = 40;      // последний int-оффсет
    int next_obj = 0;       // счётчик obj-слотов
    int int_arg_used = 0;   // int-scratch (staging + args)
    int argv_used = 0;      // argv указатели
    // layout — выставляется после dry-run
    int int_base = 0, obj_base = 0, argv_base = 0;
    // патчи только для литералов и переходов
    std::vector<std::pair<size_t,std::string>> lit_patches;
    std::vector<size_t> ret_jumps, self_calls;
    size_t subrsp_at = 0, addrsp_at = 0;
    std::vector<std::pair<std::vector<size_t>,
                          std::vector<size_t>>> loop_stack;

    int slot_of(const std::string& n) {
        auto it = slots.find(n);
        if (it != slots.end()) return it->second;
        return slots[n] = (next_off += 8);
    }
    // dbl-домен: slots[n] живёт, но там double-биты
    std::set<std::string> dset;
    // типизированные int-листы: vector<int> — элементы
    // по контракту типа ll (это позволяет for(x:v)
    // делать x int-варом и читать элементы напрямую)
    std::set<std::string> int_lists;
    bool is_dbl_var(const std::string& n) const {
        return slots.count(n) && dset.count(n) &&
               !oslots.count(n);
    }
    bool is_int_var(const std::string& n) const {
        return slots.count(n) && !oslots.count(n) &&
               !dset.count(n);
    }
    void promote_dbl(const std::string& n) { dset.insert(n); }
    void promote_obj(const std::string& n) {
        dset.erase(n);
        obj_slot_of(n);
    }
    int obj_slot_of(const std::string& n) {
        auto it = oslots.find(n);
        if (it != oslots.end()) return it->second;
        return oslots[n] = next_obj++;
    }
    bool is_obj_var(const std::string& n) const {
        return oslots.count(n);
    }
    // temp/arg-слот — пропускает индексы, занятые oslots
    // (pass-2: oslots предзаполнены из dry-run)
    int obj_slot() {
        while (true) {
            int i = next_obj++;
            bool used = false;
            for (auto& kv : oslots)
                if (kv.second == i) { used = true; break; }
            if (!used) return i;
        }
    }
    int int_stage(int n = 1) {                  // int64 staging
        int s = int_arg_used;
        int_arg_used += n * 8;
        return s;
    }
};

// снимаем GetterExpression-обёртку одиночного литерала
// строки (как plain_var, но для "x")
static StringExpression* plain_str(NodePtr n) {
    if (auto g = static_cast<GetterExpression*>(n->asGetter())) {
        if (g->list.size() == 1)
            return static_cast<StringExpression*>(
                g->list[0]->asStrExpr());
    }
    return static_cast<StringExpression*>(n->asStrExpr());
}

// снимаем GetterExpression-обёртку одиночного имени
static const std::string* plain_var(NodePtr n) {
    if (auto g = static_cast<GetterExpression*>(n->asGetter())) {
        if (g->list.size() == 1) {
            if (auto v = static_cast<VariableExpression*>(
                    g->list[0]->asVar()))
                return &v->text;
        }
    }
    if (auto v = static_cast<VariableExpression*>(n->asVar()))
        return &v->text;
    return nullptr;
}

// --- примитивы кодогенерации (арх-зависимые) ---
#if defined(NL_JIT_X64)

// rcx=1 rdx=2 r8=8 r9=9
static void mov_reg_imm64(JitBuf& b, int r, int64_t v) {
    b.u8(r >= 8 ? 0x49 : 0x48);
    b.u8(0xB8 + (r & 7));
    b.u64((uint64_t)v);
}
static void mov_reg_rax(JitBuf& b, int r) { // r = rax
    b.u8(r >= 8 ? 0x49 : 0x48);
    b.u8(0x89); b.u8(0xC0 + (r & 7));
}
static void lea_reg_rbp(JitBuf& b, int r, int off) {
    b.u8(r >= 8 ? 0x4C : 0x48);
    b.u8(0x8D); b.u8(0x85 + (r & 7) * 8);
    b.u32((uint32_t)-off);
}
static void xor_rax(JitBuf& b) { b.u8(0x48); b.u8(0x31); b.u8(0xC0); }
static void cmp_rax_imm8(JitBuf& b, int v) {
    b.u8(0x48); b.u8(0x83); b.u8(0xF8); b.u8((uint8_t)v);
}
// вызов рантайм-помощника: rax = &fn; rcx..r9 уже стоят.
// sub 48 сохраняет rsp≡0 (mod16) и даёт 32б shadow.
static void call_rt(JitCtx& c, void* fn) {
    mov_reg_imm64(c.b, 0, (int64_t)fn);
    c.b.sub_rsp(48);
    c.b.u8(0xFF); c.b.u8(0xD0); // call rax
    c.b.add_rsp(48);
}

#elif defined(NL_JIT_ARM64)
#include "jit_arm64.inc"
#endif
// доступ к регионам кадра (layout известен на 2м проходе)
static void mov_rax_scratch(JitCtx& c, int rel) {
    c.b.mov_rax_mem_rbp(c.int_base + rel);
}
static void mov_scratch_rax(JitCtx& c, int rel) {
    c.b.mov_mem_rbp_rax(c.int_base + rel);
}
static void lea_scratch(JitCtx& c, int r, int rel) {
    lea_reg_rbp(c.b, r, c.int_base + rel);
}
// адресация ячеек кадра: mov [rbp-off] покрывает адреса
// [rbp-off, rbp-off+8) — т.е. ячейка живёт НИЖЕ off.
// obj-слот i занимает (obj_base + i*56, obj_base+(i+1)*56]
// в оффсетах => lea = obj_base + (i+1)*56 (база объекта).
static void mov_rax_obj(JitCtx& c, int oidx) {
    lea_reg_rbp(c.b, 0,
        c.obj_base + (oidx + 1) * (int)sizeof(NObject));
}
static void lea_obj(JitCtx& c, int r, int oidx) {
    lea_reg_rbp(c.b, r,
        c.obj_base + (oidx + 1) * (int)sizeof(NObject));
}
// argv-ячейка j: оффсеты (argv_base + j*8, argv_base + (j+1)*8]
static void mov_argv_ptr(JitCtx& c, int rel) {
    c.b.mov_mem_rbp_rax(c.argv_base + rel + 8);
}
static void lea_argv(JitCtx& c, int r, int rel) {
    lea_reg_rbp(c.b, r, c.argv_base + rel + 8);
}
static void mov_reg_lit(JitCtx& c, int r,
                        const std::string& s) {
    size_t at = c.b.pos();           // начало load-последовательности
    mov_reg_imm64(c.b, r, 0);
    c.lit_patches.emplace_back(at, s);
}

// rax=ll -> rax=NObject*(out)
static void int_to_obj(JitCtx& c, int out) {
    mov_reg_rax(c.b, 2);              // rdx = v
    lea_obj(c, 1, out);               // rcx = out
    call_rt(c, (void*)&nl_jit_of_int);
    mov_rax_obj(c, out);
}
// rax=ptr -> rax=ll
static void obj_to_int(JitCtx& c) {
    mov_reg_rax(c.b, 1);              // rcx = ptr
    call_rt(c, (void*)&nl_jit_to_int);
}
// xmm0=double -> rax=NObject*(out): rcx=out, xmm1=v
static void dbl_to_obj(JitCtx& c, int out) {
    c.b.movsd_xmm_xmm(1, 0);          // xmm1 = v
    lea_obj(c, 1, out);               // rcx = out
    call_rt(c, (void*)&nl_jit_of_dbl);
    mov_rax_obj(c, out);
}
// rax=NObject* -> xmm0=double
static void obj_to_dbl(JitCtx& c) {
    mov_reg_rax(c.b, 1);              // rcx = ptr
    call_rt(c, (void*)&nl_jit_to_dbl);
}
// xmm0=double -> rax=ll (усечение, как asInt)
static void dbl_to_int(JitCtx& c) {
    c.b.cvttsd2si_rax_xmm(0);
}
// xmm-стейджинг в int-scratch (8 байт на ячейку)
static void movsd_scratch_xmm(JitCtx& c, int x, int rel) {
    c.b.movsd_mem_xmm(x, c.int_base + rel);
}
static void movsd_xmm_scratch(JitCtx& c, int x, int rel) {
    c.b.movsd_xmm_mem(x, c.int_base + rel);
}
// проверка "деление на ноль" для fp: b в xmm0
// == 0 -> pending_error + abort в эпилог
static void emit_div_zero_check(JitCtx& c) {
    c.b.pxor_xmm(7, 7);
    c.b.ucomisd_xmm(0, 7);
    size_t ok = c.b.jcc_rel32(0x5);    // b != 0 -> дальше
    c.b.mov_rcx_mem_rbp(JitCtx::VM_SLOT);
    call_rt(c, (void*)&nl_jit_div_zero);
    c.ret_jumps.push_back(c.b.jmp_rel32()); // abort -> epilogue
    c.b.patch_here(ok);
}

// --- emit ---
static Dom emit_expr(JitCtx& c, NodePtr n);
static void emit_stmt(JitCtx& c, NodePtr n);
static Dom emit_expr_fallback(JitCtx& c, NodePtr n);
static void emit_stmt_exec(JitCtx& c, NodePtr n);
static Dom emit_call(JitCtx& c, CallingExpression* call,
                     NodePtr n);

static void emit_block(JitCtx& c, NodePtr n) {
    if (auto bl = dynamic_cast<BlockStatement*>(n.get())) {
        for (auto& s : bl->block) emit_stmt(c, s);
    } else if (n) {
        emit_stmt(c, n);
    }
}

// неизвестная переменная -> vars (obj-домен)
static void emit_var_obj_get(JitCtx& c, const char* name,
                             int out) {
    c.b.mov_rcx_mem_rbp(JitCtx::VM_SLOT);
    mov_reg_lit(c, 2, name);
    lea_obj(c, 8, out);
    call_rt(c, (void*)&nl_jit_var_get);
    mov_rax_obj(c, out);
}

static Dom emit_expr(JitCtx& c, NodePtr n) {
    if (auto num = static_cast<NumberExpression*>(n->asNum())) {
        if (!num->is_float) {
            c.b.mov_rax_imm64(num->ivalue);
            return Dom::Int;
        }
        uint64_t bits;
        std::memcpy(&bits, &num->dvalue, 8);
        c.b.mov_rax_imm64((int64_t)bits);
        c.b.movq_xmm_rax(0);
        return Dom::Dbl;
    }
    if (auto str = static_cast<StringExpression*>(
            n->asStrExpr())) {
        int t = c.obj_slot();
        lea_obj(c, 1, t);
        mov_reg_lit(c, 2, str->value);
        mov_reg_imm64(c.b, 8, (int64_t)str->value.size());
        call_rt(c, (void*)&nl_jit_str_init);
        mov_rax_obj(c, t);
        return Dom::Obj;
    }
    if (auto v = static_cast<VariableExpression*>(n->asVar())) {
        if (c.is_obj_var(v->text)) {
            mov_rax_obj(c, c.obj_slot_of(v->text));
            return Dom::Obj;
        }
        if (c.is_dbl_var(v->text)) {
            c.b.movsd_xmm_mem(0, c.slot_of(v->text));
            return Dom::Dbl;
        }
        if (c.is_int_var(v->text)) {
            c.b.mov_rax_mem_rbp(c.slot_of(v->text));
            return Dom::Int;
        }
        int t = c.obj_slot();
        emit_var_obj_get(c, v->text.c_str(), t);
        return Dom::Obj;
    }
    if (auto g = static_cast<GetterExpression*>(n->asGetter())) {
        if (g->list.size() == 1)
            return emit_expr(c, g->list[0]);
        // методы на obj-переменной: x.push(v), x.size()
        if (auto call = static_cast<CallingExpression*>(
                g->list.back()->asCall())) {
            const std::string* obj = g->list.size() >= 2
                ? plain_var(g->list[g->list.size() - 2])
                : nullptr;
            std::string mn = call->str();
            if (obj && c.is_obj_var(*obj) &&
                (mn == "push" || mn == "append") &&
                call->pos_args.size() == 1 &&
                call->named_args.empty()) {
                Dom ad = emit_expr(c, call->pos_args[0]);
                long long ploff = nl_jit_layout_ll_off();
                long long idxoff = nl_jit_layout_idx_off();
                long long thisoff = nl_jit_layout_this_off();
                long long nsize = nl_jit_layout_nobj_size();
                if (ad == Dom::Int && ploff >= 0 &&
                    idxoff >= 0) {
                    // быстрый push: capacity-check +
                    // inline-init NObject(int); helper —
                    // только при росте вектора
                    int stv = c.int_stage();
                    mov_scratch_rax(c, stv);
                    int ooff = c.obj_base +
                        (c.obj_slot_of(*obj) + 1) * (int)nsize;
                    // rbx = vector* (ListPtr-пейлоад)
                    c.b.mov_reg_mem_rbp(
                        ooff - (int)ploff, 3);
                    c.b.mov_rax_mem_rbx(8);   // rax=finish
                    c.b.cmp_rax_mem_rbx(16);  // vs endcap
                    size_t jgrow = c.b.jcc_rel32(0x4);
                    // rax = elem — пишем NObject(v)
                    c.b.mov_reg_mem_rbp(
                        c.int_base + stv, 2);  // rdx = v
                    c.b.mov_mem_rax_rdx((int)ploff);
                    c.b.mov_mem_rax_imm8((int)idxoff, 2);
                    // _this (16б) — imm32-запись = 8 байт
                    c.b.mov_mem_rax_imm32((int)thisoff, 0);
                    c.b.mov_mem_rax_imm32((int)thisoff + 8, 0);
                    c.b.add_rax_imm8((int)nsize);
                    c.b.mov_mem_rbx_rax(8);   // finish++
                    size_t jdone = c.b.jmp_rel32();
                    // grow: rcx=list, rdx=v (из ячейки stv)
                    c.b.patch_here(jgrow);
                    lea_obj(c, 1, c.obj_slot_of(*obj));
                    c.b.mov_reg_mem_rbp(
                        c.int_base + stv, 2);
                    call_rt(c,
                        (void*)&nl_jit_list_push_ll);
                    c.b.patch_here(jdone);
                    return Dom::Int;
                }
                if (ad == Dom::Dbl) {
                    int t = c.obj_slot();
                    dbl_to_obj(c, t);
                    lea_obj(c, 0, t);
                }
                int st = c.int_stage();
                mov_scratch_rax(c, st);
                lea_obj(c, 1, c.obj_slot_of(*obj));
                mov_rax_scratch(c, st);
                mov_reg_rax(c.b, 2);
                call_rt(c, (void*)&nl_jit_list_push_obj);
                return Dom::Int;
            }
            if (obj && c.is_obj_var(*obj) &&
                (mn == "size" || mn == "length") &&
                call->pos_args.empty()) {
                lea_obj(c, 1, c.obj_slot_of(*obj));
                call_rt(c, (void*)&nl_jit_size);
                return Dom::Int;
            }
        }
        // индекс obj[i] (числовой список -> int)
        if (auto acc = static_cast<ArrayAccessExpression*>(
                g->list.back()->asArr())) {
            const std::string* obj = g->list.size() >= 2
                ? plain_var(g->list[g->list.size() - 2])
                : plain_var(g->list[0]);
            if (obj && c.is_obj_var(*obj) &&
                acc->access.size() == 1) {
                Dom id = emit_expr(c, acc->access[0]);
                if (id == Dom::Obj) obj_to_int(c);
                else if (id == Dom::Dbl) dbl_to_int(c);
                int st = c.int_stage();
                mov_scratch_rax(c, st);
                lea_obj(c, 1, c.obj_slot_of(*obj));
                mov_rax_scratch(c, st);
                mov_reg_rax(c.b, 2);
                call_rt(c, (void*)&nl_jit_at_ll);
                return Dom::Int;
            }
        }
        return emit_expr_fallback(c, n);
    }
    if (auto b = dynamic_cast<BinaryExpression*>(n.get())) {
        const std::string* tgt = plain_var(b->expr1);
        // ++/--
        if (b->_type == INCREMENT || b->_type == DECREMENT) {
            if (tgt && c.is_int_var(*tgt)) {
                int off = c.slot_of(*tgt);
                c.b.mov_rax_mem_rbp(off);
                if (b->_type == INCREMENT) c.b.inc_rax();
                else c.b.dec_rax();
                c.b.mov_mem_rbp_rax(off);
                return Dom::Int;
            }
            if (tgt && c.is_obj_var(*tgt)) {
                int one = c.obj_slot(), t = c.obj_slot();
                lea_obj(c, 1, one);
                mov_reg_imm64(c.b, 2, 1);
                call_rt(c, (void*)&nl_jit_of_int);
                mov_reg_imm64(c.b, 1,
                    b->_type == INCREMENT ? PLUS : MINUS);
                lea_obj(c, 2, c.obj_slot_of(*tgt));
                lea_obj(c, 8, one);
                lea_obj(c, 9, t);
                call_rt(c, (void*)&nl_jit_arith);
                lea_obj(c, 2, t);
                lea_obj(c, 1, c.obj_slot_of(*tgt));
                call_rt(c, (void*)&nl_jit_obj_set);
                mov_rax_obj(c, t);
                return Dom::Obj;
            }
            return emit_expr_fallback(c, n);
        }
        // op= (+= -= *= /= %=)
        bool assign_op = b->_type == PLUSEQ ||
            b->_type == MINUSEQ || b->_type == STAREQ ||
            b->_type == SLASHEQ || b->_type == PERCENTEQ;
        if (assign_op) {
            if (!tgt) return emit_expr_fallback(c, n);
            int op = b->_type == PLUSEQ ? PLUS :
                     b->_type == MINUSEQ ? MINUS :
                     b->_type == STAREQ ? STAR :
                     b->_type == SLASHEQ ? SLASH : PERCENT;
            // s += "x" — склейка in-place (как eval +=)
            if (op == PLUS && c.is_obj_var(*tgt) &&
                plain_str(b->expr2)) {
                auto* se = plain_str(b->expr2);
                lea_obj(c, 1, c.obj_slot_of(*tgt));
                mov_reg_lit(c, 2, se->value);
                mov_reg_imm64(c.b, 8,
                              (int64_t)se->value.size());
                call_rt(c, (void*)&nl_jit_strcat_lit);
                mov_rax_obj(c, c.obj_slot_of(*tgt));
                return Dom::Obj;
            }
            if (c.is_int_var(*tgt) &&
                (op == PLUS || op == MINUS || op == STAR ||
                 op == PERCENT || op == SLASH)) {
                int off = c.slot_of(*tgt);
                Dom rd = emit_expr(c, b->expr2);
                if (rd == Dom::Int && op == SLASH) {
                    // x /= int — eval: true-division -> dbl
                    c.promote_dbl(*tgt);
                    c.b.cvtsi2sd_xmm_mem(1, off);
                    c.b.cvtsi2sd_xmm_rax(0);
                    emit_div_zero_check(c);
                    c.b.sdsd_xmm(0x5E, 1, 0);
                    c.b.movsd_mem_xmm(1, off);
                    c.b.movsd_xmm_xmm(0, 1);
                    return Dom::Dbl;
                }
                if (rd == Dom::Int) {
                    c.b.mov_rbx_rax();
                    c.b.mov_rax_mem_rbp(off);
                    switch (op) {
                    case PLUS: c.b.add_rax_rbx(); break;
                    case MINUS: c.b.sub_rax_rbx(); break;
                    case STAR: c.b.imul_rax_rbx(); break;
                    case PERCENT:
                        c.b.test_rbx_rbx();
                        {
                            size_t ok = c.b.jcc_rel32(0x5);
                            c.b.mov_rcx_mem_rbp(
                                JitCtx::VM_SLOT);
                            call_rt(c,
                                (void*)&nl_jit_div_zero);
                            c.ret_jumps.push_back(
                                c.b.jmp_rel32());
                            c.b.patch_here(ok);
                        }
                        c.b.cqo_idiv_rbx();
                        c.b.mov_rax_rdx();
                        break;
                    }
                    c.b.mov_mem_rbp_rax(off);
                    return Dom::Int;
                }
                if (rd == Dom::Dbl) {
                    // x += dbl — промоушен int-var -> dbl
                    c.promote_dbl(*tgt);
                    int sdop = op == PLUS ? 0x58 :
                               op == MINUS ? 0x5C :
                               op == STAR ? 0x59 : 0;
                    c.b.cvtsi2sd_xmm_mem(1, off);
                    if (op == PERCENT) {
                        // fmod(x, r): xmm0=x, xmm1=r
                        c.b.movsd_xmm_xmm(2, 0);
                        c.b.movsd_xmm_xmm(0, 1);
                        c.b.movsd_xmm_xmm(1, 2);
                        call_rt(c, (void*)&nl_jit_fmod);
                        c.b.movsd_xmm_xmm(1, 0);
                    } else {
                        if (op == SLASH)
                            emit_div_zero_check(c);
                        c.b.sdsd_xmm(sdop ? sdop : 0x5E, 1, 0);
                    }
                    c.b.movsd_mem_xmm(1, off);
                    c.b.movsd_xmm_xmm(0, 1);
                    return Dom::Dbl;
                }
                // rhs — объект: промоушен int-var в obj-домен,
                // как eval (vars[x] становится другого типа).
                // r-ptr держим в scratch, пока готовим l
                int rs0 = c.int_stage();
                mov_scratch_rax(c, rs0);
                c.promote_obj(*tgt);
                int os = c.obj_slot_of(*tgt);
                int ltmp = c.obj_slot();
                c.b.mov_rax_mem_rbp(off);
                int_to_obj(c, ltmp);      // l: int -> obj
                lea_obj(c, 2, ltmp);
                mov_rax_scratch(c, rs0);
                mov_reg_rax(c.b, 8);      // r8 = rhs ptr
                int t = c.obj_slot();
                mov_reg_imm64(c.b, 1, op);
                lea_obj(c, 9, t);
                call_rt(c, (void*)&nl_jit_arith);
                lea_obj(c, 2, t);
                lea_obj(c, 1, os);
                call_rt(c, (void*)&nl_jit_obj_set);
                mov_rax_obj(c, t);
                return Dom::Obj;
            }
            // dbl-вар: x op= y на xmm — без хелперов
            if (c.is_dbl_var(*tgt)) {
                int off = c.slot_of(*tgt);
                Dom rd = emit_expr(c, b->expr2);
                if (rd == Dom::Obj) {
                    // x op= obj — eval: arith(как есть) ->
                    // промоушен в obj-домен. l = x -> obj
                    int t2 = c.obj_slot();
                    mov_reg_rax(c.b, 2);
                    lea_obj(c, 1, t2);
                    call_rt(c, (void*)&nl_jit_obj_set); // t2=r
                    int ltmp = c.obj_slot();
                    c.b.movsd_xmm_mem(0, off);
                    dbl_to_obj(c, ltmp);              // l=dbl->obj
                    int t = c.obj_slot();
                    c.promote_obj(*tgt);
                    mov_reg_imm64(c.b, 1, op);
                    lea_obj(c, 2, ltmp);
                    lea_obj(c, 8, t2);
                    lea_obj(c, 9, t);
                    call_rt(c, (void*)&nl_jit_arith);
                    lea_obj(c, 2, t);
                    lea_obj(c, 1, c.obj_slot_of(*tgt));
                    call_rt(c, (void*)&nl_jit_obj_set);
                    mov_rax_obj(c, t);
                    return Dom::Obj;
                }
                if (rd == Dom::Int) c.b.cvtsi2sd_xmm_rax(0);
                if (op == PERCENT) {
                    c.b.movsd_xmm_xmm(2, 0);
                    c.b.movsd_xmm_mem(0, off);
                    c.b.movsd_xmm_xmm(1, 2);
                    call_rt(c, (void*)&nl_jit_fmod);
                    c.b.movsd_mem_xmm(0, off);
                    return Dom::Dbl;
                }
                c.b.movsd_xmm_mem(1, off);
                if (op == SLASH) emit_div_zero_check(c);
                int sdop = op == PLUS ? 0x58 :
                           op == MINUS ? 0x5C :
                           op == STAR ? 0x59 : 0x5E;
                c.b.sdsd_xmm(sdop, 1, 0);
                c.b.movsd_mem_xmm(1, off);
                c.b.movsd_xmm_xmm(0, 1);
                return Dom::Dbl;
            }
            // obj-вар / int-вар без fast-path / глобальная:
            // nl_jit_arith + write-back
            int t = c.obj_slot(), rst = c.obj_slot();
            Dom rd = emit_expr(c, b->expr2);
            if (rd == Dom::Int) {
                int_to_obj(c, rst);
            } else if (rd == Dom::Dbl) {
                dbl_to_obj(c, rst);
            } else {
                mov_reg_rax(c.b, 2);
                lea_obj(c, 1, rst);
                call_rt(c, (void*)&nl_jit_obj_set);
            }
            mov_reg_imm64(c.b, 1, op);
            if (c.is_obj_var(*tgt)) {
                lea_obj(c, 2, c.obj_slot_of(*tgt));
            } else if (c.is_dbl_var(*tgt)) {
                int ltmp = c.obj_slot();
                c.b.movsd_xmm_mem(0, c.slot_of(*tgt));
                dbl_to_obj(c, ltmp);
                lea_obj(c, 2, ltmp);
            } else if (c.is_int_var(*tgt)) {
                // int-var + тяжёлый op (напр. /=) — промоушен:
                // l = int -> obj, пишем в новый obj-слот
                int ltmp = c.obj_slot();
                c.b.mov_rax_mem_rbp(c.slot_of(*tgt));
                int_to_obj(c, ltmp);
                lea_obj(c, 2, ltmp);
                c.obj_slot_of(*tgt); // фиксируем домен
            } else {
                int los = c.obj_slot();
                emit_var_obj_get(c, tgt->c_str(), los);
                lea_obj(c, 2, los);
            }
            lea_obj(c, 8, rst);
            lea_obj(c, 9, t);
            call_rt(c, (void*)&nl_jit_arith);
            lea_obj(c, 2, t);
            if (c.is_obj_var(*tgt)) {
                lea_obj(c, 1, c.obj_slot_of(*tgt));
                call_rt(c, (void*)&nl_jit_obj_set);
            } else {
                c.b.mov_rcx_mem_rbp(JitCtx::VM_SLOT);
                mov_reg_lit(c, 2, tgt->c_str());
                lea_obj(c, 8, t);
                call_rt(c, (void*)&nl_jit_var_set);
            }
            mov_rax_obj(c, t);
            return Dom::Obj;
        }
        // обычная бинарная операция
        bool int_op = b->_type == PLUS || b->_type == MINUS ||
                      b->_type == STAR || b->_type == PERCENT;
        Dom ld = emit_expr(c, b->expr1);
        int ls = c.int_stage();
        if (ld == Dom::Dbl) movsd_scratch_xmm(c, 0, ls);
        else mov_scratch_rax(c, ls);
        Dom rd = emit_expr(c, b->expr2);
        // fp-путь: / — всегда true-division (как eval);
        // + - * % — если хоть один операнд double
        if (ld != Dom::Obj && rd != Dom::Obj &&
            (b->_type == SLASH || ld == Dom::Dbl ||
             rd == Dom::Dbl)) {
            if (ld == Dom::Dbl) movsd_xmm_scratch(c, 1, ls);
            else c.b.cvtsi2sd_xmm_mem(1, c.int_base + ls);
            if (rd == Dom::Int) c.b.cvtsi2sd_xmm_rax(0);
            switch (b->_type) {
            case PLUS:
                c.b.sdsd_xmm(0x58, 1, 0);
                c.b.movsd_xmm_xmm(0, 1);
                return Dom::Dbl;
            case MINUS:
                c.b.sdsd_xmm(0x5C, 1, 0);
                c.b.movsd_xmm_xmm(0, 1);
                return Dom::Dbl;
            case STAR:
                c.b.sdsd_xmm(0x59, 1, 0);
                c.b.movsd_xmm_xmm(0, 1);
                return Dom::Dbl;
            case SLASH:
                emit_div_zero_check(c);
                c.b.sdsd_xmm(0x5E, 1, 0);
                c.b.movsd_xmm_xmm(0, 1);
                return Dom::Dbl;
            case PERCENT:
                // nl_jit_fmod(l, r): xmm0=l, xmm1=r
                c.b.movsd_xmm_xmm(2, 0);
                c.b.movsd_xmm_xmm(0, 1);
                c.b.movsd_xmm_xmm(1, 2);
                call_rt(c, (void*)&nl_jit_fmod);
                return Dom::Dbl;
            }
        }
        if (int_op && ld == Dom::Int && rd == Dom::Int) {
            c.b.mov_rbx_rax();
            mov_rax_scratch(c, ls);
            switch (b->_type) {
            case PLUS: c.b.add_rax_rbx(); break;
            case MINUS: c.b.sub_rax_rbx(); break;
            case STAR: c.b.imul_rax_rbx(); break;
            case PERCENT:
                // % 0 — "деление на ноль", как eval
                c.b.test_rbx_rbx();
                {
                    size_t ok = c.b.jcc_rel32(0x5);
                    c.b.mov_rcx_mem_rbp(JitCtx::VM_SLOT);
                    call_rt(c, (void*)&nl_jit_div_zero);
                    c.ret_jumps.push_back(c.b.jmp_rel32());
                    c.b.patch_here(ok);
                }
                c.b.cqo_idiv_rbx();
                c.b.mov_rax_rdx();
                break;
            }
            return Dom::Int;
        }
        // obj-путь: r -> NObject* (scratch), l -> rdx
        int rs = c.int_stage();
        if (rd == Dom::Int) {
            mov_scratch_rax(c, rs);   // пока ll
            int t2 = c.obj_slot();
            mov_rax_scratch(c, rs);
            int_to_obj(c, t2);
            lea_obj(c, 0, t2);
        } else if (rd == Dom::Dbl) {
            int t2 = c.obj_slot();
            dbl_to_obj(c, t2);
            lea_obj(c, 0, t2);
        }
        mov_scratch_rax(c, rs);       // r-пtr в scratch
        mov_rax_scratch(c, ls);
        int lidx;
        if (ld == Dom::Int) {
            lidx = c.obj_slot();
            int_to_obj(c, lidx);
            lea_obj(c, 0, lidx);
        } else if (ld == Dom::Dbl) {
            lidx = c.obj_slot();
            movsd_xmm_scratch(c, 0, ls);
            dbl_to_obj(c, lidx);
            lea_obj(c, 0, lidx);
        }
        mov_reg_rax(c.b, 2);          // rdx = l
        mov_rax_scratch(c, rs);
        mov_reg_rax(c.b, 8);          // r8 = r
        int t = c.obj_slot();
        mov_reg_imm64(c.b, 1, b->_type);
        lea_obj(c, 9, t);
        call_rt(c, (void*)&nl_jit_arith);
        mov_rax_obj(c, t);
        return Dom::Obj;
    }
    if (auto e = dynamic_cast<ConditionalExpression*>(
            n.get())) {
        int cc_op, cc;
        switch (e->type) {
        case LT:     cc_op = 0; cc = 0xC; break;
        case LTEQ:   cc_op = 1; cc = 0xE; break;
        case GT:     cc_op = 2; cc = 0xF; break;
        case GTEQ:   cc_op = 3; cc = 0xD; break;
        case EQEQ:   cc_op = 4; cc = 0x4; break;
        case EXCLEQ: cc_op = 5; cc = 0x5; break;
        default: return emit_expr_fallback(c, n);
        }
        Dom ld = emit_expr(c, e->first);
        int ls = c.int_stage();
        if (ld == Dom::Dbl) movsd_scratch_xmm(c, 0, ls);
        else mov_scratch_rax(c, ls);
        Dom rd = emit_expr(c, e->second);
        if (ld != Dom::Obj && rd != Dom::Obj) {
            if (ld == Dom::Int && rd == Dom::Int) {
                c.b.mov_rbx_rax();
                mov_rax_scratch(c, ls);
                c.b.cmp_rax_rbx();
                c.b.setcc_al(cc);
                c.b.movzx_rax_al();
                return Dom::Int;
            }
            // fp-сравнение: xmm1 = l, xmm0 = r.
            // ucomisd ставит CF — нужны беззнаковые cc
            if (ld == Dom::Dbl) movsd_xmm_scratch(c, 1, ls);
            else c.b.cvtsi2sd_xmm_mem(1, c.int_base + ls);
            if (rd == Dom::Int) c.b.cvtsi2sd_xmm_rax(0);
            c.b.ucomisd_xmm(1, 0);
            int dcc = cc_op == 0 ? 0x2 :  // <  -> setb
                      cc_op == 1 ? 0x6 :  // <= -> setbe
                      cc_op == 2 ? 0x7 :  // >  -> seta
                      cc_op == 3 ? 0x3 :  // >= -> setae
                      cc;                 // ==/!= — те же
            c.b.setcc_al(dcc);
            c.b.movzx_rax_al();
            return Dom::Int;
        }
        // obj-сравнение через nl_jit_cmp
        int rs = c.int_stage();
        if (rd == Dom::Int) {
            int t2 = c.obj_slot();
            int_to_obj(c, t2);
            lea_obj(c, 0, t2);
        } else if (rd == Dom::Dbl) {
            int t2 = c.obj_slot();
            dbl_to_obj(c, t2);
            lea_obj(c, 0, t2);
        }
        mov_scratch_rax(c, rs);
        mov_rax_scratch(c, ls);
        if (ld == Dom::Int) {
            int t1 = c.obj_slot();
            int_to_obj(c, t1);
            lea_obj(c, 0, t1);
        } else if (ld == Dom::Dbl) {
            int t1 = c.obj_slot();
            movsd_xmm_scratch(c, 0, ls);
            dbl_to_obj(c, t1);
            lea_obj(c, 0, t1);
        }
        mov_reg_rax(c.b, 2);          // rdx = l
        mov_rax_scratch(c, rs);
        mov_reg_rax(c.b, 8);          // r8 = r
        mov_reg_imm64(c.b, 1, cc_op);
        call_rt(c, (void*)&nl_jit_cmp);
        return Dom::Int;
    }
    if (auto call = static_cast<CallingExpression*>(
            n->asCall())) {
        return emit_call(c, call, n);
    }
    if (auto list = dynamic_cast<ListExpression*>(n.get())) {
        int t = c.obj_slot();
        lea_obj(c, 1, t);
        call_rt(c, (void*)&nl_jit_list_init);
        for (auto& el : list->list) {
            Dom ed = emit_expr(c, el);
            if (ed == Dom::Dbl) {
                int t2 = c.obj_slot();
                dbl_to_obj(c, t2);
                lea_obj(c, 0, t2);
            }
            int st = c.int_stage();
            mov_scratch_rax(c, st);
            lea_obj(c, 1, t);
            mov_rax_scratch(c, st);
            mov_reg_rax(c.b, 2);
            call_rt(c, ed == Dom::Int
                ? (void*)&nl_jit_list_push_ll
                : (void*)&nl_jit_list_push_obj);
        }
        mov_rax_obj(c, t);
        return Dom::Obj;
    }
    return emit_expr_fallback(c, n);
}

// --- синхронизация jit-кадра с vm.vars ---
// escape hatch зовёт интерпретатор — тот читает/пишет
// vars. Перед вызовом сливаем слоты туда, после —
// перечитываем (интерпретатор мог поменять). Внутренние
// @-переменные не трогаем.
static void emit_sync_out(JitCtx& c) {
    for (auto& kv : c.slots) {
        if (kv.first[0] == '@') continue;
        int t = c.obj_slot();
        if (c.dset.count(kv.first)) {
            // dbl-вар — через nl_jit_of_dbl
            c.b.movsd_xmm_mem(0, kv.second);
            c.b.movsd_xmm_xmm(1, 0);
            lea_obj(c, 1, t);
            call_rt(c, (void*)&nl_jit_of_dbl);
        } else {
            c.b.mov_rax_mem_rbp(kv.second);
            int_to_obj(c, t);
        }
        c.b.mov_rcx_mem_rbp(JitCtx::VM_SLOT);
        mov_reg_lit(c, 2, kv.first.c_str());
        lea_obj(c, 8, t);
        call_rt(c, (void*)&nl_jit_var_set);
    }
    for (auto& kv : c.oslots) {
        c.b.mov_rcx_mem_rbp(JitCtx::VM_SLOT);
        mov_reg_lit(c, 2, kv.first.c_str());
        lea_obj(c, 8, kv.second);
        call_rt(c, (void*)&nl_jit_var_set);
    }
}
static void emit_sync_in(JitCtx& c) {
    for (auto& kv : c.slots) {
        if (kv.first[0] == '@') continue;
        int t = c.obj_slot();
        emit_var_obj_get(c, kv.first.c_str(), t);
        if (c.dset.count(kv.first)) {
            lea_obj(c, 1, t);
            call_rt(c, (void*)&nl_jit_to_dbl);
            c.b.movsd_mem_xmm(0, kv.second);
        } else {
            lea_obj(c, 1, t);
            call_rt(c, (void*)&nl_jit_to_int);
            c.b.mov_mem_rbp_rax(kv.second);
        }
    }
    for (auto& kv : c.oslots) {
        int t = c.obj_slot();
        emit_var_obj_get(c, kv.first.c_str(), t);
        lea_obj(c, 2, t);
        lea_obj(c, 1, kv.second);
        call_rt(c, (void*)&nl_jit_obj_set);
    }
}

// escape hatch: произвольное выражение -> node->eval()
static Dom emit_expr_fallback(JitCtx& c, NodePtr n) {
    int t = c.obj_slot();
    emit_sync_out(c);
    c.b.mov_rcx_mem_rbp(JitCtx::VM_SLOT);
    mov_reg_imm64(c.b, 2, (int64_t)n.get());
    lea_obj(c, 8, t);
    call_rt(c, (void*)&nl_jit_eval_obj);
    emit_sync_in(c);
    mov_rax_obj(c, t);
    return Dom::Obj;
}

// вызов: self-рекурсия нативным call, остальное — через
// named-хелпер (jit→jit происходит внутри call()).
// time.now()/x.m() — getter из >1 звена — это не простое
// имя: уходим в eval-фоллбэк (иначе возьмём "time")
static Dom emit_call(JitCtx& c, CallingExpression* call,
                     NodePtr n) {
    std::string callee = call->str();
    if (callee == "?") {
        if (auto g = static_cast<GetterExpression*>(
                call->getter->asGetter())) {
            if (g->list.size() != 1) {
                return emit_expr_fallback(c, n);
            }
        }
        if (const std::string* pv = plain_var(call->getter))
            callee = *pv;
    }
    if (callee == "?" || callee.empty())
        return emit_expr_fallback(c, n);
    if (callee == c.fn->name && call->named_args.empty()) {
        // self-рекурсия: int-домен, аргументы в scratch
        // массив идёт ВВЕРХ — arg0 на глубочайшем оффсете
        int narg = (int)call->pos_args.size();
        int base = c.int_stage(narg);
        for (int i = 0; i < narg; ++i) {
            Dom d = emit_expr(c, call->pos_args[i]);
            if (d == Dom::Obj) obj_to_int(c);
            else if (d == Dom::Dbl) dbl_to_int(c);
            mov_scratch_rax(c, base + (narg - 1 - i) * 8);
        }
        lea_scratch(c, 2, base + (narg - 1) * 8);
        c.b.mov_rcx_mem_rbp(JitCtx::VM_SLOT);
        c.b.mov_reg_mem_rbp(JitCtx::RET_SLOT, 8);
        c.self_calls.push_back(c.b.call_rel32());
        return Dom::Int;
    }
    // named-вызов: argv — массив NObject* в кадре
    int narg = (int)call->pos_args.size();
    int avsite = c.argv_used;
    c.argv_used += narg * 8;
    for (int i = 0; i < narg; ++i) {
        int t = c.obj_slot();
        Dom d = emit_expr(c, call->pos_args[i]);
        if (d == Dom::Int) {
            int_to_obj(c, t);
        } else if (d == Dom::Dbl) {
            dbl_to_obj(c, t);
        } else {
            mov_reg_rax(c.b, 2);       // rdx = src
            lea_obj(c, 1, t);          // rcx = dst
            call_rt(c, (void*)&nl_jit_obj_set);
        }
        lea_obj(c, 0, t);              // rax = &arg
        mov_argv_ptr(c, avsite + (narg - 1 - i) * 8);
    }
    c.b.mov_rcx_mem_rbp(JitCtx::VM_SLOT);
    mov_reg_lit(c, 2, callee);
    lea_argv(c, 8, narg ? avsite + (narg - 1) * 8 : 0);
    mov_reg_imm64(c.b, 9, narg);
    call_rt(c, (void*)&nl_jit_call_named);
    int t = c.obj_slot();
    mov_reg_rax(c.b, 2);               // rdx = tmp
    lea_obj(c, 1, t);
    call_rt(c, (void*)&nl_jit_obj_set);
    mov_rax_obj(c, t);
    return Dom::Obj;
}

static void emit_stmt(JitCtx& c, NodePtr n) {
    if (!n) return;
    if (auto r = dynamic_cast<ReturnStatement*>(n.get())) {
        if (r->expr) {
            Dom d = emit_expr(c, r->expr);
            if (d == Dom::Obj) {
                mov_reg_rax(c.b, 2);
                c.b.mov_reg_mem_rbp(JitCtx::RET_SLOT, 1);
                call_rt(c, (void*)&nl_jit_obj_set);
                xor_rax(c.b);
            } else if (d == Dom::Dbl) {
                // *retobj = NObject(double), rax=0
                c.b.movsd_xmm_xmm(1, 0);
                c.b.mov_reg_mem_rbp(JitCtx::RET_SLOT, 1);
                call_rt(c, (void*)&nl_jit_of_dbl);
                xor_rax(c.b);
            }
        } else {
            xor_rax(c.b);
        }
        c.ret_jumps.push_back(c.b.jmp_rel32());
        return;
    }
    if (auto a = dynamic_cast<AssignmentStatement*>(
            n.get())) {
        const std::string* name = plain_var(a->getter);
        if (!name) { emit_stmt_exec(c, n); return; }
        if (c.is_int_var(*name)) {
            Dom d = emit_expr(c, a->value);
            if (d == Dom::Obj) {
                // промоушен: x = obj — домен вара -> obj,
                // как vars[x] в eval
                c.promote_obj(*name);
                int os = c.obj_slot_of(*name);
                mov_reg_rax(c.b, 2);
                lea_obj(c, 1, os);
                call_rt(c, (void*)&nl_jit_obj_set);
                return;
            }
            if (d == Dom::Dbl) {
                // x = 1.5 — int-var -> dbl-домен
                c.promote_dbl(*name);
                c.b.movsd_mem_xmm(0, c.slot_of(*name));
                return;
            }
            c.b.mov_mem_rbp_rax(c.slot_of(*name));
            return;
        }
        if (c.is_dbl_var(*name)) {
            Dom d = emit_expr(c, a->value);
            if (d == Dom::Obj) {
                c.promote_obj(*name);
                int os = c.obj_slot_of(*name);
                mov_reg_rax(c.b, 2);
                lea_obj(c, 1, os);
                call_rt(c, (void*)&nl_jit_obj_set);
                return;
            }
            if (d == Dom::Int) c.b.cvtsi2sd_xmm_rax(0);
            c.b.movsd_mem_xmm(0, c.slot_of(*name));
            return;
        }
        if (c.is_obj_var(*name)) {
            Dom d = emit_expr(c, a->value);
            if (d == Dom::Int) {
                int t = c.obj_slot();
                int_to_obj(c, t);
            } else if (d == Dom::Dbl) {
                int t = c.obj_slot();
                dbl_to_obj(c, t);
            }
            mov_reg_rax(c.b, 2);
            lea_obj(c, 1, c.obj_slot_of(*name));
            call_rt(c, (void*)&nl_jit_obj_set);
            return;
        }
        Dom d = emit_expr(c, a->value);
        if (d == Dom::Int) {
            c.b.mov_mem_rbp_rax(c.slot_of(*name));
        } else if (d == Dom::Dbl) {
            c.promote_dbl(*name);
            c.b.movsd_mem_xmm(0, c.slot_of(*name));
        } else {
            int os = c.obj_slot_of(*name);
            mov_reg_rax(c.b, 2);
            lea_obj(c, 1, os);
            call_rt(c, (void*)&nl_jit_obj_set);
        }
        return;
    }
    if (auto d = dynamic_cast<DeclarationStatement*>(
            n.get())) {
        // явный double — сразу dbl-домен (как тип в eval
        // смысла не меняет: vars[x] всё равно динамический)
        bool want_dbl = d->type == "double" &&
                        d->name[0] != '@';
        // vector<int>/vector<long long> — int-лист
        if ((d->type == "vector" || d->type == "list") &&
            !d->template_declaration_list.empty()) {
            const std::string& et =
                d->template_declaration_list[0];
            if (et == "int" || et == "long long" ||
                et == "int32" || et == "int64")
                c.int_lists.insert(d->name);
        }
        Dom dd = d->value ? emit_expr(c, d->value) : Dom::Int;
        if (want_dbl && dd != Dom::Obj &&
            !c.is_obj_var(d->name)) {
            c.promote_dbl(d->name);
            int off = c.slot_of(d->name);
            if (!d->value) {
                c.b.pxor_xmm(0, 0);
            } else if (dd == Dom::Int) {
                c.b.cvtsi2sd_xmm_rax(0);
            }
            c.b.movsd_mem_xmm(0, off);
            return;
        }
        // var с obj-доменом (promoted) пишется в obj-слот
        // даже при int-инициализации — как vars[x] в eval
        if (dd == Dom::Int && !c.is_obj_var(d->name) &&
            !c.is_dbl_var(d->name)) {
            int off = c.slot_of(d->name);
            if (!d->value) xor_rax(c.b);
            c.b.mov_mem_rbp_rax(off);
        } else if (c.is_dbl_var(d->name) && dd != Dom::Obj) {
            int off = c.slot_of(d->name);
            if (dd == Dom::Int) c.b.cvtsi2sd_xmm_rax(0);
            c.b.movsd_mem_xmm(0, off);
        } else {
            int os = c.obj_slot_of(d->name);
            if (dd == Dom::Int) {
                int t = c.obj_slot();
                int_to_obj(c, t);
            } else if (dd == Dom::Dbl) {
                int t = c.obj_slot();
                dbl_to_obj(c, t);
            }
            mov_reg_rax(c.b, 2);
            lea_obj(c, 1, os);
            call_rt(c, (void*)&nl_jit_obj_set);
        }
        return;
    }
    if (auto w = dynamic_cast<WhileStatement*>(n.get())) {
        c.loop_stack.emplace_back(std::vector<size_t>{},
                                  std::vector<size_t>{});
        size_t top = c.b.pos();
        Dom cd = emit_expr(c, w->expr);
        if (cd == Dom::Obj) obj_to_int(c);
        else if (cd == Dom::Dbl) {
            c.b.pxor_xmm(7, 7);
            c.b.ucomisd_xmm(0, 7);
            c.b.setcc_al(0x5);
            c.b.movzx_rax_al();
        }
        c.b.test_rax_rax();
        size_t jz = c.b.jcc_rel32(0x4);
        emit_block(c, w->block);
        c.b.jmp_abs(top);
        c.b.patch_here(jz);
        auto& lp = c.loop_stack.back();
        for (size_t p : lp.second)
            c.b.patch32(p, (int32_t)(top - (p + 4)));
        for (size_t p : lp.first)
            c.b.patch32(p, (int32_t)(c.b.pos() - (p + 4)));
        c.loop_stack.pop_back();
        return;
    }
    if (auto f = dynamic_cast<ForStatement*>(n.get())) {
        emit_stmt(c, f->decl);
        c.loop_stack.emplace_back(std::vector<size_t>{},
                                  std::vector<size_t>{});
        size_t top = c.b.pos();
        Dom cd = emit_expr(c, f->_while);
        if (cd == Dom::Obj) obj_to_int(c);
        else if (cd == Dom::Dbl) {
            c.b.pxor_xmm(7, 7);
            c.b.ucomisd_xmm(0, 7);
            c.b.setcc_al(0x5);
            c.b.movzx_rax_al();
        }
        c.b.test_rax_rax();
        size_t jz = c.b.jcc_rel32(0x4);
        emit_block(c, f->block);
        size_t do_pos = c.b.pos();
        emit_stmt(c, f->_do);
        c.b.jmp_abs(top);
        c.b.patch_here(jz);
        auto& lp = c.loop_stack.back();
        for (size_t p : lp.second)
            c.b.patch32(p, (int32_t)(do_pos - (p + 4)));
        for (size_t p : lp.first)
            c.b.patch32(p, (int32_t)(c.b.pos() - (p + 4)));
        c.loop_stack.pop_back();
        return;
    }
    if (auto fe = dynamic_cast<ForeachStatement*>(n.get())) {
        // for (x : list): i=0; while i<size { x=at(i);body;i++ }
        std::string iv = "@fe_i" + fe->value;
        std::string nv = "@fe_n" + fe->value;
        int ioff = c.slot_of(iv);
        int noff = c.slot_of(nv);
        int cont = c.obj_slot();
        Dom cd = emit_expr(c, fe->expr);
        if (cd == Dom::Int) int_to_obj(c, cont);
        else if (cd == Dom::Dbl) dbl_to_obj(c, cont);
        else {
            mov_reg_rax(c.b, 2);
            lea_obj(c, 1, cont);
            call_rt(c, (void*)&nl_jit_obj_set);
        }
        lea_obj(c, 1, cont);
        call_rt(c, (void*)&nl_jit_list_size);
        c.b.mov_mem_rbp_rax(noff);
        xor_rax(c.b);
        c.b.mov_mem_rbp_rax(ioff);
        // быстрый путь int-списка: data() один раз,
        // оффсеты variant — из layout-пробников
        int fe_doff = -1;
        int fe_ploff = (int)nl_jit_layout_ll_off();
        int fe_idxoff = (int)nl_jit_layout_idx_off();
        int fe_nsize = (int)nl_jit_layout_nobj_size();
        size_t fe_jok = 0;
        // типизированный vector<int> — x объявляем
        // int-варом: элемент по контракту типа — ll.
        // Если элемент оказался не-int — to_int (x
        // остаётся int-варом; документированное
        // расхождение на смешанном vector<int>)
        const std::string* cvar = plain_var(fe->expr);
        bool fe_typed = cvar && c.int_lists.count(*cvar) &&
            !c.is_obj_var(fe->value) &&
            !c.is_dbl_var(fe->value);
        if (fe_typed) c.slot_of(fe->value);
        if ((c.is_int_var(fe->value) || fe_typed) &&
            fe_ploff >= 0 && fe_idxoff >= 0) {
            fe_doff = c.slot_of("@fe_d" + fe->value);
            lea_obj(c, 1, cont);
            call_rt(c, (void*)&nl_jit_list_data);
            c.b.mov_mem_rbp_rax(fe_doff);
        }
        c.loop_stack.emplace_back(std::vector<size_t>{},
                                  std::vector<size_t>{});
        size_t top = c.b.pos();
        c.b.mov_rax_mem_rbp(noff);
        c.b.mov_rbx_rax();
        c.b.mov_rax_mem_rbp(ioff);
        c.b.cmp_rax_rbx();            // i - n
        size_t jge = c.b.jcc_rel32(0xD); // i>=n -> выход
        // быстрый путь: data() + inline-чтение int-элем.
        // дискриминант != ll -> медленный путь at_obj
        if (fe_doff >= 0) {
            c.b.mov_reg_mem_rbp(fe_doff, 3);  // rbx=data
            c.b.test_rbx_rbx();
            size_t jsl0 = c.b.jcc_rel32(0x4); // null -> slow
            c.b.mov_rax_mem_rbp(ioff);
            c.b.imul_rax_imm8(fe_nsize);
            c.b.add_rax_rbx();                // rax=&elem
            c.b.cmp_mem_rax_imm8(fe_idxoff, 2);
            size_t jsl1 = c.b.jcc_rel32(0x5); // !ll -> slow
            c.b.mov_rax_mem_rax(fe_ploff);    // payload
            c.b.mov_mem_rbp_rax(
                c.slot_of(fe->value));
            fe_jok = c.b.jmp_rel32();
            c.b.patch_here(jsl0);
            c.b.patch_here(jsl1);
        }
        lea_obj(c, 1, cont);
        c.b.mov_rax_mem_rbp(ioff);
        mov_reg_rax(c.b, 2);
        int et = c.obj_slot();
        lea_obj(c, 8, et);
        call_rt(c, (void*)&nl_jit_at_obj);
        if (c.is_int_var(fe->value)) {
            lea_obj(c, 1, et);
            call_rt(c, (void*)&nl_jit_to_int);
            c.b.mov_mem_rbp_rax(c.slot_of(fe->value));
            if (fe_doff >= 0) c.b.patch_here(fe_jok);
        } else if (c.is_dbl_var(fe->value)) {
            lea_obj(c, 1, et);
            call_rt(c, (void*)&nl_jit_to_dbl);
            c.b.movsd_mem_xmm(0, c.slot_of(fe->value));
        } else {
            int vs = c.obj_slot_of(fe->value);
            lea_obj(c, 2, et);
            lea_obj(c, 1, vs);
            call_rt(c, (void*)&nl_jit_obj_set);
        }
        emit_block(c, fe->block);
        size_t inc_pos = c.b.pos();
        c.b.mov_rax_mem_rbp(ioff);
        c.b.inc_rax();
        c.b.mov_mem_rbp_rax(ioff);
        c.b.jmp_abs(top);
        c.b.patch_here(jge);
        auto& lp = c.loop_stack.back();
        for (size_t p : lp.second)
            c.b.patch32(p, (int32_t)(inc_pos - (p + 4)));
        for (size_t p : lp.first)
            c.b.patch32(p, (int32_t)(c.b.pos() - (p + 4)));
        c.loop_stack.pop_back();
        return;
    }
    if (auto cond = dynamic_cast<ConditionalStatement*>(
            n.get())) {
        Dom cd = emit_expr(c, cond->cond);
        if (cd == Dom::Obj) obj_to_int(c);
        else if (cd == Dom::Dbl) {
            c.b.pxor_xmm(7, 7);
            c.b.ucomisd_xmm(0, 7);
            c.b.setcc_al(0x5);
            c.b.movzx_rax_al();
        }
        c.b.test_rax_rax();
        size_t jz = c.b.jcc_rel32(0x4);
        emit_block(c, cond->_if);
        if (cond->_else) {
            size_t jend = c.b.jmp_rel32();
            c.b.patch_here(jz);
            emit_block(c, cond->_else);
            c.b.patch_here(jend);
        } else {
            c.b.patch_here(jz);
        }
        return;
    }
    if (dynamic_cast<BreakStatement*>(n.get())) {
        if (c.loop_stack.empty())
            return; // вне цикла — вряд ли, но не падаем
        c.loop_stack.back().first.push_back(c.b.jmp_rel32());
        return;
    }
    if (dynamic_cast<ContinueStatement*>(n.get())) {
        if (c.loop_stack.empty()) return;
        c.loop_stack.back().second.push_back(c.b.jmp_rel32());
        return;
    }
    if (dynamic_cast<SkipStatement*>(n.get())) return;
    // выражение как оператор — эмитим нативно (внутри
    // emit_expr сложные подвыражения сами уходят в
    // escape hatch с синхронизацией)
    if (n->asVar() || n->asGetter() || n->asCall() ||
        n->asNum() || n->asStrExpr() || n->asArr() ||
        dynamic_cast<BinaryExpression*>(n.get()) ||
        dynamic_cast<ConditionalExpression*>(n.get()) ||
        dynamic_cast<ListExpression*>(n.get())) {
        emit_expr(c, n);
        return;
    }
    emit_stmt_exec(c, n);
}

// произвольный оператор — обратно в интерпретатор:
// node->eval() с пробросом return/break/continue кодами
static void emit_stmt_exec(JitCtx& c, NodePtr n) {
    emit_sync_out(c);
    c.b.mov_rcx_mem_rbp(JitCtx::VM_SLOT);
    mov_reg_imm64(c.b, 2, (int64_t)n.get());
    call_rt(c, (void*)&nl_jit_exec_stmt);
    emit_sync_in(c);
    // rax: 0=norm 1=return 2=break 3=continue
    cmp_rax_imm8(c.b, 1);
    size_t jret = c.b.jcc_rel32(0x4); // je -> return path
    cmp_rax_imm8(c.b, 2);
    if (!c.loop_stack.empty()) {
        c.loop_stack.back().first.push_back(
            c.b.jcc_rel32(0x4));
    }
    cmp_rax_imm8(c.b, 3);
    if (!c.loop_stack.empty()) {
        c.loop_stack.back().second.push_back(
            c.b.jcc_rel32(0x4));
    }
    // 4 = ошибка: выход как return-null — pending_error
    // перевбросит call() после возврата из jit-кода
    cmp_rax_imm8(c.b, 4);
    size_t jerr = c.b.jcc_rel32(0x4);
    size_t jend = c.b.jmp_rel32();
    // error-путь: rax=0, retobj не трогаем
    c.b.patch_here(jerr);
    xor_rax(c.b);
    c.ret_jumps.push_back(c.b.jmp_rel32());
    // return: retval -> *retobj
    c.b.patch_here(jret);
    int t = c.obj_slot();
    c.b.mov_rcx_mem_rbp(JitCtx::VM_SLOT);
    lea_obj(c, 2, t);
    call_rt(c, (void*)&nl_jit_retval_obj);
    lea_obj(c, 2, t);
    c.b.mov_reg_mem_rbp(JitCtx::RET_SLOT, 1);
    call_rt(c, (void*)&nl_jit_obj_set);
    xor_rax(c.b);
    c.ret_jumps.push_back(c.b.jmp_rel32());
    c.b.patch_here(jend);
}

// ---------------- двухпроходная компиляция ----------------

// проход эмиссии (dry — подсчёт layout, мусорный буфер).
// obj_init — сколько obj-слотов инициализировать/дестроить
// (на 2м проходе = dry-счёт, next_obj на момент пролога ещё 0)
static void compile_pass(JitCtx& c, int obj_init) {
    if (!c.dry) {
        c.b.push_rbp();
        c.b.mov_rbp_rsp();
        c.b.sub_rsp(0);
        c.subrsp_at = c.b.pos();   // pos ПОСЛЕ инсн(ов)
        c.b.mov_mem_rbp_reg(8, 3);
        c.b.mov_mem_rbp_reg(16, 7);
        c.b.mov_mem_rbp_reg(24, 6);
        c.b.mov_mem_rbp_reg(JitCtx::VM_SLOT, 1);
        c.b.mov_mem_rbp_reg(JitCtx::RET_SLOT, 8);
        for (size_t i = 0; i < c.fn->args.size(); ++i) {
            c.b.mov_rax_mem_rdx((int)(i * 8));
            c.b.mov_mem_rbp_rax(c.slots[c.fn->args[i].first]);
        }
        for (int i = 0; i < obj_init; ++i) {
            lea_obj(c, 1, i);
            call_rt(c, (void*)&nl_jit_obj_init);
        }
    }
    for (auto& s : c.fn->statements) emit_stmt(c, s);
    if (!c.dry) {
        size_t epilogue = c.b.pos();
        for (size_t j : c.ret_jumps)
            c.b.patch32(j, (int32_t)(epilogue - (j + 4)));
        // rax = возвращаемое int-значение — dtor-хелперы
        // затирают rax => спасаем в VM_SLOT (vm больше не нужен)
        c.b.mov_mem_rbp_rax(JitCtx::VM_SLOT);
        for (int i = obj_init - 1; i >= 0; --i) {
            lea_obj(c, 1, i);
            call_rt(c, (void*)&nl_jit_obj_dtor);
        }
        c.b.mov_rax_mem_rbp(JitCtx::VM_SLOT);
        c.b.ret_acc();            // arm64: acc -> x0
        c.b.mov_reg_mem_rbp(8, 3);
        c.b.mov_reg_mem_rbp(16, 7);
        c.b.mov_reg_mem_rbp(24, 6);
        c.b.add_rsp(0);
        c.addrsp_at = c.b.pos();
        c.b.pop_reg(5);
        c.b.ret();
    }
}

std::shared_ptr<JitCode> nl_jit_compile(NFunction* fn) {
    try {
        // проход 1: layout. Промоушен int->obj меняет ветки
        // эмиссии (is_int_var) — а значит и раскладку слотов.
        // Повторяем dry пока домены не стабилизируются —
        // oslots монотонно растёт => сходится.
        JitCtx dry;
        dry.fn = fn;
        dry.dry = true;
        for (auto& a : fn->args) dry.slot_of(a.first);
        compile_pass(dry, 0);
        while (true) {
            JitCtx d2;
            d2.fn = fn;
            d2.dry = true;
            for (auto& a : fn->args) d2.slot_of(a.first);
            d2.oslots = dry.oslots;
            d2.dset = dry.dset;
            d2.int_lists = dry.int_lists;
            compile_pass(d2, 0);
            bool stable = (d2.oslots == dry.oslots &&
                           d2.dset == dry.dset &&
                           d2.int_lists == dry.int_lists);
            dry.slots = std::move(d2.slots);
            dry.oslots = std::move(d2.oslots);
            dry.dset = std::move(d2.dset);
            dry.int_lists = std::move(d2.int_lists);
            dry.next_obj = d2.next_obj;
            dry.next_off = d2.next_off;
            dry.int_arg_used = d2.int_arg_used;
            dry.argv_used = d2.argv_used;
            if (stable) break;
        }

        // проход 2: настоящая эмиссия с готовым layout.
        // домены из dry: var, получивший obj-запись хоть раз,
        // obj-домен С НАЧАЛА функции — иначе промоушен
        // будет перечитывать устаревший int-слот в цикле
        JitCtx c;
        c.fn = fn;
        for (auto& a : fn->args) c.slot_of(a.first);
        c.oslots = dry.oslots;
        c.dset = dry.dset;
        c.int_lists = dry.int_lists;
        // next_obj=0: temp-индексы воспроизводятся в том же
        // порядке, obj_slot() пропускает занятые oslots-ами
        c.int_base = dry.next_off + 8;
        c.obj_base = (c.int_base + dry.int_arg_used + 15) & ~15;
        int obj_bytes = dry.next_obj * (int)sizeof(NObject);
        c.argv_base = c.obj_base + obj_bytes;
        int frame = (c.argv_base + dry.argv_used + 15) & ~15;
        int obj_total = dry.next_obj;

        compile_pass(c, obj_total);
        if (c.next_obj != obj_total ||
            c.int_arg_used != dry.int_arg_used ||
            c.argv_used != dry.argv_used ||
            c.next_off != dry.next_off) {
            // недетерминированная выдача слотов — не компилим
            return nullptr;
        }

        // ---- патчи ----
        int framev = frame;
        c.b.patch_frame(c.subrsp_at, framev);
        c.b.patch_frame(c.addrsp_at, framev);
        for (size_t at : c.self_calls)
            c.b.patch32(at, (int32_t)(0 - (at + 4)));
        // литералы — в конец буфера (за кодом)
        for (auto& lp : c.lit_patches) {
            size_t off = c.b.len;
            for (char ch : lp.second) c.b.u8((uint8_t)ch);
            c.b.u8(0);
            uint64_t addr = (uint64_t)(c.b.code + off);
            c.b.patch_lit(lp.first, addr);
        }

        if (!jit_make_exec(c.b)) return nullptr;

        if (std::getenv("NLANG_JIT_DEBUG")) {
            fprintf(stderr, "jit %s: %zu bytes, frame=%d, "
                    "obj=%d\n", fn->name.c_str(), c.b.len,
                    frame, obj_total);
            for (size_t i = 0; i < c.b.len; ++i)
                fprintf(stderr, "%02x", c.b.code[i]);
            fprintf(stderr, "\n");
        }

        auto out = std::make_shared<JitCode>();
        out->buf.code = c.b.code;
        out->buf.cap = c.b.cap;
        out->buf.len = c.b.len;
        c.b.code = nullptr;
        out->entry = (JitIntFn)out->buf.code;
        return out;
    } catch (...) {
        return nullptr;
    }
}

#else // !x64 — jit заглушка

std::shared_ptr<JitCode> nl_jit_compile(NFunction*) {
    return nullptr;
}

#endif
