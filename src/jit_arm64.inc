// jit_arm64.inc — бэкенд mini-JIT для aarch64 (Android).
// Включается внутрь jit.cpp (те же static-имена что у
// x86-бэкенда, но arm64-кодировки — проверены через
// llvm-objdump NDK).
//
// Регистровая карта x86->arm:
//   rax (acc)    -> x9
//   rbx (scratch)-> x10
//   rcx,rdx,r8,r9 (аргументы хелперов) -> x0,x1,x2,x3
//   rdi,rsi (сохраняемые в кадре)      -> x19,x20
//   rbp          -> x29 (fp), rsp -> sp
//   xmm N        -> dN  (v0..v7)
//   временные: x11 (остаток div), x12 (sdiv tmp),
//   x16 (адресация больших оффсетов / цель blr)
//
// Кадр — тот же по семантике: локали [x29-off], результат
// call_rt перекладывается x0 -> x9.

#include <sys/mman.h>

// ---------- JitBuf — arm64 ----------

JitBuf::JitBuf() {
    len = 0; cap = 256 * 1024;
    // RW сначала — mprotect -> RX в jit_make_exec
    void* p = mmap(nullptr, cap, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    code = p == MAP_FAILED ? nullptr : (uint8_t*)p;
}
JitBuf::~JitBuf() {
    if (code) munmap(code, cap);
}

// x86-код регистра -> номер Xn
static int a64_reg(int r) {
    switch (r) {
    case 0: return 9;   // rax
    case 1: return 0;   // rcx
    case 2: return 1;   // rdx
    case 3: return 10;  // rbx
    case 6: return 20;  // rsi
    case 7: return 19;  // rdi
    case 8: return 2;   // r8
    case 9: return 3;   // r9
    }
    return r;           // внутренние x11/x12/x16
}

// x86 cc -> arm cond. Беззнаковые коды (B/AE/BE/A) у нас
// встречаются только после fcmp — там они и есть fp-условия.
static int a64_cc(int cc) {
    switch (cc & 0xF) {
    case 0x4: return 0;   // E  -> EQ
    case 0x5: return 1;   // NE -> NE
    case 0x2:             // B  -> LT   (fp <)
    case 0xC: return 11;  // L  -> LT
    case 0x3:             // AE -> GE   (fp >=)
    case 0xD: return 10;  // GE -> GE
    case 0x6:             // BE -> LE   (fp <=)
    case 0xE: return 13;  // LE -> LE
    case 0x7:             // A  -> GT   (fp >)
    case 0xF: return 12;  // G  -> GT
    }
    return 0;
}

// ---------- формы загрузки/выгрузки ----------

// movz+movk: полная 64-бит константа в Xn
static void a64_mov_imm(JitBuf& b, int rn, uint64_t v) {
    b.u32(0xD2800000 | ((v & 0xFFFF) << 5) | rn);
    b.u32(0xF2A00000 | (((v >> 16) & 0xFFFF) << 5) | rn);
    b.u32(0xF2C00000 | (((v >> 32) & 0xFFFF) << 5) | rn);
    b.u32(0xF2E00000 | (((v >> 48) & 0xFFFF) << 5) | rn);
}
static uint32_t a64_imm9(int off) {
    return (uint32_t)(off & 0x1FF) << 12;
}
// [x29, -off]: ldur при off<=256, иначе x16=-off + reg-off
static void a64_ld_frame(JitBuf& b, int rn, int off) {
    if (off <= 256)
        b.u32(0xF8400000 | a64_imm9(-off) | (29 << 5) | rn);
    else {
        a64_mov_imm(b, 16, (uint64_t)(-(int64_t)off));
        b.u32(0xF8606800 | (16 << 16) | (29 << 5) | rn);
    }
}
static void a64_st_frame(JitBuf& b, int rn, int off) {
    if (off <= 256)
        b.u32(0xF8000000 | a64_imm9(-off) | (29 << 5) | rn);
    else {
        a64_mov_imm(b, 16, (uint64_t)(-(int64_t)off));
        b.u32(0xF8206800 | (16 << 16) | (29 << 5) | rn);
    }
}
// [xN, +d]: ldur <=255, ldr scaled (mult 8), иначе x16=d
static void a64_ld(JitBuf& b, int rd, int rn, int d) {
    if (d >= 0 && d <= 255)
        b.u32(0xF8400000 | ((d & 0x1FF) << 12) |
              (rn << 5) | rd);
    else if (d % 8 == 0 && d / 8 <= 4095)
        b.u32(0xF9400000 | ((d / 8) << 10) |
              (rn << 5) | rd);
    else {
        a64_mov_imm(b, 16, (uint64_t)d);
        b.u32(0xF8606800 | (16 << 16) | (rn << 5) | rd);
    }
}
static void a64_st(JitBuf& b, int rt, int rn, int d) {
    if (d >= 0 && d <= 255)
        b.u32(0xF8000000 | ((d & 0x1FF) << 12) |
              (rn << 5) | rt);
    else if (d % 8 == 0 && d / 8 <= 4095)
        b.u32(0xF9000000 | ((d / 8) << 10) |
              (rn << 5) | rt);
    else {
        a64_mov_imm(b, 16, (uint64_t)d);
        b.u32(0xF8206800 | (16 << 16) | (rn << 5) | rt);
    }
}
// [x29, -off] для fp-регистров (d)
static void a64_ld_d(JitBuf& b, int rd, int off) {
    if (off <= 256)
        b.u32(0xFC400000 | a64_imm9(-off) | (29 << 5) | rd);
    else {
        a64_mov_imm(b, 16, (uint64_t)(-(int64_t)off));
        b.u32(0xFC706800 | (16 << 16) | (29 << 5) | rd);
    }
}
static void a64_st_d(JitBuf& b, int rt, int off) {
    if (off <= 256)
        b.u32(0xFC000000 | a64_imm9(-off) | (29 << 5) | rt);
    else {
        a64_mov_imm(b, 16, (uint64_t)(-(int64_t)off));
        b.u32(0xFC306800 | (16 << 16) | (29 << 5) | rt);
    }
}

// ---------- рамка ----------

void JitBuf::push_rbp() {                 // stp x29,x30,[sp,#-16]!
    u32(0xA9BF7BFD);
}
void JitBuf::mov_rbp_rsp() {              // mov x29,sp
    u32(0x910003FD);
}
void JitBuf::ret() { u32(0xD65F03C0); }   // ret (x30 уж спасён)

// размер кадра — две инсн: hi<<12 + lo12 (patch_frame
// пишет imm в обе). Кодируем фиктивный #0 на эмиссии.
void JitBuf::sub_rsp(int n) {
    u32(0xD14003FF | (((n >> 12) & 0xFFF) << 10));
    u32(0xD10003FF | ((n & 0xFFF) << 10));
}
void JitBuf::add_rsp(int n) {
    u32(0x914003FF | (((n >> 12) & 0xFFF) << 10));
    u32(0x910003FF | ((n & 0xFFF) << 10));
}
// patch_frame(at=pos после инснов): переписать imm пары
void JitBuf::patch_frame(size_t at, int v) {
    uint32_t* p = (uint32_t*)(code + at - 8);
    uint32_t hi = p[0] & 0xFFC00000;      // op-часть слова 0
    p[0] = hi | (((v >> 12) & 0xFFF) << 10) | 0x3FF;
    p[1] = (hi & ~(1u << 22)) |
           ((v & 0xFFF) << 10) | 0x3FF;
}

void JitBuf::push_rax() {
    u32(0xD10043FF);                      // sub sp,sp,#16
    u32(0xF90003E0 | 9);                  // str x9,[sp]
}
void JitBuf::pop_rax() {
    u32(0xF94003E0 | 9);                  // ldr x9,[sp]
    u32(0x910043FF);                      // add sp,sp,#16
}
void JitBuf::push_reg(int r) {
    if (r == 5) { push_rbp(); return; }
    u32(0xD10043FF);
    u32(0xF90003E0 | a64_reg(r));
}
void JitBuf::pop_reg(int r) {
    if (r == 5) {                         // эпилог: пара fp/lr
        u32(0xA8C17BFD);                  // ldp x29,x30,[sp],#16
        return;
    }
    u32(0xF94003E0 | a64_reg(r));
    u32(0x910043FF);
}

// ---------- целочисленные ----------

void JitBuf::mov_rax_imm64(int64_t v) {
    a64_mov_imm(*this, 9, (uint64_t)v);
}
void JitBuf::mov_rax_rbx() { u32(0xAA0A03E9); } // x9<-x10
void JitBuf::mov_rbx_rax() { u32(0xAA0903EA); } // x10<-x9
void JitBuf::mov_rax_rdx() { u32(0xAA0B03E9); } // x9<-x11 (rem)
void JitBuf::mov_rax_rcx() { u32(0xAA0003E9); } // x9<-x0
void JitBuf::mov_rax_mem_rbp(int off) { a64_ld_frame(*this, 9, off); }
void JitBuf::mov_mem_rbp_rax(int off) { a64_st_frame(*this, 9, off); }
void JitBuf::mov_rcx_mem_rbp(int off) { a64_ld_frame(*this, 0, off); }
void JitBuf::lea_rdx_rbp(int off) {
    if (off <= 4095)
        u32(0xD1000000 | (off << 10) | (29 << 5) | 1);
    else {
        a64_mov_imm(*this, 16, (uint64_t)off);
        u32(0xCB1003A1);                  // sub x1,x29,x16
    }
}
void JitBuf::mov_mem_rbp_reg(int off, int r) {
    a64_st_frame(*this, a64_reg(r), off);
}
void JitBuf::mov_reg_mem_rbp(int off, int r) {
    a64_ld_frame(*this, a64_reg(r), off);
}
void JitBuf::add_rax_rbx() { u32(0x8B0A0129); }   // add x9,x9,x10
void JitBuf::sub_rax_rbx() { u32(0xCB0A0129); }   // sub x9,x9,x10
void JitBuf::imul_rax_rbx() { u32(0x9B0A7D29); }  // mul x9,x9,x10
// частное -> x9 (acc), остаток -> x11 (rdx-map)
void JitBuf::cqo_idiv_rbx() {
    u32(0x9ACA0D2C);                      // sdiv x12,x9,x10
    u32(0x9B0AA58B);                      // msub x11,x12,x10,x9
    u32(0xAA0C03E9);                      // mov x9,x12
}
void JitBuf::cmp_rax_rbx() { u32(0xEB0A013F); }   // cmp x9,x10
void JitBuf::test_rax_rax() { u32(0xF100013F); }  // cmp x9,#0
void JitBuf::test_rbx_rbx() { u32(0xF100015F); }  // cmp x10,#0
void JitBuf::inc_rax() { u32(0x91000529); }       // add x9,x9,#1
void JitBuf::dec_rax() { u32(0xD1000529); }       // sub x9,x9,#1
void JitBuf::mov_rax_mem_rdx(int d) { a64_ld(*this, 9, 1, d); }
void JitBuf::setcc_al(int cc) {
    // cset x9,cc = csinc x9,xzr,xzr,!cc
    u32(0x9A9F07E0 | ((a64_cc(cc) ^ 1) << 12) | 9);
}
void JitBuf::movzx_rax_al() {}  // cset даёт чистый 0/1 в x9

// ---------- переходы / вызовы ----------
// at — НАЧАЛО слова инструкции. patch32 принимает rel в
// стиле x86 (от at+4) и кладёт его в imm19/imm26.
size_t JitBuf::jmp_rel32() {
    size_t at = len; u32(0x14000000); return at;    // b
}
size_t JitBuf::jcc_rel32(int cc) {
    size_t at = len;
    u32(0x54000000 | a64_cc(cc));                   // b.cond
    return at;
}
size_t JitBuf::call_rel32() {
    size_t at = len;
    u32(0x94000000);                      // bl
    u32(0xAA0003E9);                      // mov x9,x0 — результат -> acc
    return at;
}
void JitBuf::ret_acc() { u32(0xAA0903E0); } // mov x0,x9 — acc -> ret
void JitBuf::jmp_abs(size_t target) {
    u32(0x14000000 | ((uint32_t)((int64_t)target - len) >> 2)
                    & 0x3FFFFFF);
}
void JitBuf::patch_here(size_t at) {
    patch32(at, (int32_t)(len - (at + 4)));
}
void JitBuf::patch32(size_t at, int32_t rel) {
    int32_t w = (rel >> 2) + 1;   // x86-rel -> слова
    uint32_t* p = (uint32_t*)(code + at);
    // b.cond: верхние 8 бит 0x54, биты 0..3 — cond (не трогаем),
    // imm19 в битах 23..5. b/bl: верхние 6 бит, imm26.
    if ((*p & 0xFF000010) == 0x54000000)
        *p = (*p & ~0xFFFFE0u) | ((w & 0x7FFFF) << 5);
    else
        *p = (*p & 0xFC000000) | (w & 0x3FFFFFF);
}
// литерал: адрес в imm16 полях movz+movk*3
void JitBuf::patch_lit(size_t at, uint64_t addr) {
    uint32_t* p = (uint32_t*)(code + at);
    for (int i = 0; i < 4; ++i)
        p[i] = (p[i] & ~0x1FFFE0u) |
               ((uint32_t)((addr >> (16 * i)) & 0xFFFF) << 5);
}

// ---------- SSE-аналоги (fp) ----------

void JitBuf::cvtsi2sd_xmm_rax(int x) {    // dX = (double)x9
    u32(0x9E620000 | (9 << 5) | x);       // scvtf dX,x9
}
void JitBuf::cvtsi2sd_xmm_mem(int x, int off) {
    a64_ld_frame(*this, 16, off);
    u32(0x9E620000 | (16 << 5) | x);      // scvtf dX,x16
}
void JitBuf::cvttsd2si_rax_xmm(int x) {   // x9 = (ll)dX
    u32(0x9E780000 | (x << 5) | 9);       // fcvtzs x9,dX
}
void JitBuf::movq_xmm_rax(int x) {        // dX = биты x9
    u32(0x9E670000 | (9 << 5) | x);       // fmov dX,x9
}
void JitBuf::movsd_xmm_mem(int d, int off) {
    a64_ld_d(*this, d, off);
}
void JitBuf::movsd_mem_xmm(int s, int off) {
    a64_st_d(*this, s, off);
}
void JitBuf::movsd_xmm_xmm(int d, int s) {  // fmov dD,dS
    u32(0x1E604000 | (s << 5) | d);
}
void JitBuf::sdsd_xmm(int op, int d, int s) {
    uint32_t base = op == 0x58 ? 0x1E602800 :  // fadd
                    op == 0x5C ? 0x1E603800 :  // fsub
                    op == 0x59 ? 0x1E600800 :  // fmul
                                 0x1E601800;   // fdiv
    u32(base | (s << 16) | (d << 5) | d);
}
void JitBuf::ucomisd_xmm(int a, int b) {      // fcmp dA,dB
    u32(0x1E602000 | (b << 16) | (a << 5));
}
void JitBuf::pxor_xmm(int a, int) {           // dA = 0.0
    u32(0x9E670000 | (31 << 5) | a);          // fmov dA,xzr
}

// ---------- быстрый путь списков ----------

void JitBuf::imul_rax_imm8(int v) {
    u32(0xD2800000 | ((v & 0xFFFF) << 5) | 16);  // movz x16,#v
    u32(0x9B107D29);                             // mul x9,x9,x16
}
void JitBuf::mov_rax_mem_rax(int d) { a64_ld(*this, 9, 9, d); }
void JitBuf::mov_rax_mem_rbx(int d) { a64_ld(*this, 9, 10, d); }
void JitBuf::mov_rbx_mem_rax(int d) { a64_ld(*this, 10, 9, d); }
void JitBuf::mov_mem_rax_rdx(int d) { a64_st(*this, 1, 9, d); }
void JitBuf::mov_mem_rax_rbx(int d) { a64_st(*this, 10, 9, d); }
void JitBuf::mov_mem_rbx_rax(int d) { a64_st(*this, 9, 10, d); }
void JitBuf::mov_mem_rax_imm32(int d, int v) { // 8-байт запись
    a64_mov_imm(*this, 16, (uint64_t)(int64_t)v);
    a64_st(*this, 16, 9, d);
}
void JitBuf::cmp_rax_mem_rbx(int d) {
    a64_ld(*this, 16, 10, d);
    u32(0xEB10013F);                           // cmp x9,x16
}
void JitBuf::cmp_mem_rax_imm8(int d, int v) {  // byte
    u32(0x39400000 | ((d & 0xFFF) << 10) |
        (9 << 5) | 16);                        // ldrb w16,[x9,#d]
    u32(0x7100001F | ((v & 0xFFF) << 10) |
        (16 << 5));                            // cmp w16,#v
}
void JitBuf::mov_mem_rax_imm8(int d, int v) {  // byte
    u32(0x52800000 | ((v & 0xFFFF) << 5) | 16); // movz w16,#v
    u32(0x39000000 | ((d & 0xFFF) << 10) |
        (9 << 5) | 16);                        // strb w16,[x9,#d]
}
void JitBuf::add_rax_imm8(int v) {
    u32(0x91000000 | ((v & 0xFFF) << 10) | (9 << 5) | 9);
}

// ---------- низкоуровневые хелперы ----------
// Те же имена что у x86-бэкенда. Аргументы хелперов:
// rcx=1,rdx=2,r8=8,r9=9 -> x0,x1,x2,x3 (AAPCS64).

static void mov_reg_imm64(JitBuf& b, int r, int64_t v) {
    a64_mov_imm(b, a64_reg(r), (uint64_t)v);
}
static void mov_reg_rax(JitBuf& b, int r) {   // r = acc
    b.u32(0xAA0903E0 | a64_reg(r));           // mov xR,x9
}
static void lea_reg_rbp(JitBuf& b, int r, int off) {
    int x = a64_reg(r);
    if (off <= 4095)
        b.u32(0xD1000000 | (off << 10) | (29 << 5) | x);
    else {
        a64_mov_imm(b, 16, (uint64_t)off);
        b.u32(0xCB1003A0 | x);                // sub xR,x29,x16
    }
}
static void xor_rax(JitBuf& b) {
    b.u32(0xCA090129);                        // eor x9,x9,x9
}
static void cmp_rax_imm8(JitBuf& b, int v) {
    b.u32(0xF100001F | ((v & 0xFFF) << 10) | (9 << 5));
}
// вызов рантайм-помощника: x16=&fn, blr; результат
// x0 перекладываем в acc (x9). SP 16-выравнен кадром.
static void call_rt(JitCtx& c, void* fn) {
    mov_reg_imm64(c.b, 16, (int64_t)fn);
    c.b.u32(0xD63F0200);                      // blr x16
    c.b.u32(0xAA0003E9);                      // mov x9,x0
}

// сделать буфер исполняемым + icache flush
static bool jit_make_exec(JitBuf& b) {
    if (mprotect(b.code, b.cap, PROT_READ | PROT_EXEC) != 0)
        return false;
    __builtin___clear_cache((char*)b.code,
                            (char*)b.code + b.len);
    return true;
}
