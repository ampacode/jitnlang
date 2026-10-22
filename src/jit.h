#pragma once
// mini-JIT "anti nlang" — аннигиляция интерпретатора:
// AST -> x64 машинный код напрямую, мимо g++ и temp.cpp.
// v1: int-подмножество (параметры/локальные int64,
// арифметика, сравнения, if/else, while, return,
// self-рекурсия как нативный call).
// Нет g++, нет ABI-дыр, промоушн за миллисекунды.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <memory>

struct NFunction;
class Nvm;

class NObject;
// (vm, int-args, retobj): int-результат в rax,
// объектный результат пишется в *retobj (тогда rax=0)
using JitIntFn = long long (*)(Nvm*, const long long*,
                               NObject*);

// исполняемый буфер + x64-эмиттер минимального набора
struct JitBuf {
    uint8_t* code = nullptr;
    size_t cap = 0, len = 0;

    JitBuf();
    ~JitBuf();
    JitBuf(const JitBuf&) = delete;
    JitBuf& operator=(const JitBuf&) = delete;

    void u8(uint8_t b);
    void u32(uint32_t v);
    void u64(uint64_t v);
    size_t pos() const { return len; }
    void patch32(size_t at, int32_t rel);

    // --- базовые инструкции ---
    void ret();                    // C3
    void push_rax();               // 50
    void push_reg(int r);          // 50+r
    void pop_rax();                // 58
    void pop_reg(int r);           // 58+r
    void mov_rax_imm64(int64_t v); // 48 B8 imm64
    void mov_rax_rbx();            // 48 89 D8
    void mov_rbx_rax();            // 48 89 C3
    void mov_rax_mem_rbp(int off); // 48 8B 85 off32
    void mov_mem_rbp_rax(int off); // 48 89 85 off32
    void mov_mem_rbp_reg(int off, int r); // [rbp-off] = r
    void mov_reg_mem_rbp(int off, int r); // r = [rbp-off]
    void mov_reg_rax(int r);       // 48 89 C0+r
    void add_rax_rbx();            // 48 01 D8
    void sub_rax_rbx();            // 48 29 D8
    void imul_rax_rbx();           // 48 0F AF C3
    void cqo_idiv_rbx();           // 48 99, 48 F7 FB (rdx->rem в rdx)
    void mov_rax_rdx();            // 48 89 D0
    void cmp_rax_rbx();            // 48 39 D8
    void test_rax_rax();           // 48 85 C0
    void setcc_al(int cc);         // 0F 9x C0
    void movzx_rax_al();           // 48 0F B6 C0
    void lea_rdx_rbp(int off);     // 48 8D 55 off8
    void mov_rcx_mem_rbp(int off); // 48 8B 4D off8

    // --- переходы с патчами ---
    size_t jmp_rel32();            // E9 rel32 -> pos патча
    size_t jcc_rel32(int cc);      // 0F 8x rel32
    void patch_here(size_t at);    // rel32 до текущего len

    // --- вызовы ---
    size_t call_rel32();           // E8 rel32 -> pos патча
    void sub_rsp(int n);           // 48 81 EC n32
    void add_rsp(int n);           // 48 81 C4 n32
    void mov_rbp_rsp();            // 48 89 E5
    void push_rbp();               // 55
    void mov_rax_rcx();            // 48 89 C8

    // --- sse2: double-домен (xmm0..7) ---
    void cvtsi2sd_xmm_rax(int x);   // xmm = (double)rax
    void cvtsi2sd_xmm_mem(int x, int off); // xmm = (double)[rbp-off]
    void cvttsd2si_rax_xmm(int x);  // rax = (ll)xmm (trunc)
    void movq_xmm_rax(int x);       // xmm = bits rax
    void movsd_xmm_mem(int x, int off);   // xmm = [rbp-off]
    void movsd_mem_xmm(int x, int off);   // [rbp-off] = xmm
    void movsd_xmm_xmm(int d, int s);     // xmm_d = xmm_s
    void sdsd_xmm(int op, int d, int s);  // 58 add 5C sub 59 mul 5E div
    void ucomisd_xmm(int a, int b);       // флаги по xmm_a-xmm_b
    void pxor_xmm(int a, int b);          // xmm_a ^= xmm_b (обнуление)

    // --- быстрый путь списков ---
    void imul_rax_imm8(int v);            // rax *= imm8
    void mov_rax_mem_rax(int d);          // rax = [rax+d]
    void mov_rax_mem_rbx(int d);          // rax = [rbx+d]
    void mov_rbx_mem_rax(int d);          // rbx = [rax+d]
    void mov_mem_rax_rdx(int d);          // [rax+d] = rdx
    void mov_mem_rax_rbx(int d);          // [rax+d] = rbx
    void mov_mem_rax_imm32(int d, int v); // [rax+d] = imm32
    void mov_mem_rbx_rax(int d);          // [rbx+d] = rax
    void cmp_rax_mem_rbx(int d);          // cmp rax, [rbx+d]
    void cmp_mem_rax_imm8(int d, int v);  // cmp byte[rax+d], imm8
    void mov_mem_rax_imm8(int d, int v);  // byte[rax+d] = imm8
    void add_rax_imm8(int v);             // rax += imm8

    // --- арх-нейтральные (обе архитектуры реализуют) ---
    void inc_rax();                       // rax += 1
    void dec_rax();                       // rax -= 1
    void test_rbx_rbx();                  // флаги по rbx
    void mov_rax_mem_rdx(int d);          // rax = [rdx+d] — пролив аргументов
    void jmp_abs(size_t target);          // jmp на АБСОЛЮТНУЮ pos в буфере
    void patch_frame(size_t at, int v);   // imm в sub/add rsp (at = pos ПОСЛЕ инсн)
    void patch_lit(size_t at, uint64_t addr); // адрес литерала в insn-последовательность
    void ret_acc();                       // результат acc -> return-рег (x86: no-op)
};

// держит исполняемый буфер + вход
struct JitCode {
    JitBuf buf;
    JitIntFn entry;
};

// компиляция функции в машинный код; nullptr — не tintable
// (остаётся интерпретируемой). tintable-подмножество см. jit.cpp
std::shared_ptr<JitCode> nl_jit_compile(NFunction* fn);
