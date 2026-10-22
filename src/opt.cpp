// Порт compiler/optimizations.py — паттерны -> AVX2 asm.
// Форма массива в нашем codegen: nlAt(a, (i)) — 1-индексация,
// поэтому индекс внутри nlAt без "-1".
#include "opt.h"
#include <regex>

namespace {

// для регулярок: имя массива + индекс-выражение внутри nlAt
// nlAt(a, (i))  nlAt(a, ((i) + (base)))
const std::regex re_decl(R"((\w+)\s+(\w+)\s*=\s*(.+))");
const std::regex re_cond(R"(\(\s*(\w+)\s*(<=?)\s*(.+)\))");
const std::regex re_incr(R"(\(\s*(\w+)\s*\+=\s*1\s*\)|\(\s*(\w+)\+\+\s*\))");
// тело — одна строка присваивания вида (acc += ...); или a[i] = ...
const std::regex re_dotprod(
    R"(^\s*\(?\s*(\w+)\s*\+=\s*\(?\s*nlAt\((\w+),\s*\((.+?)\)\)\s*\*\s*nlAt\((\w+),\s*\((.+?)\)\)\s*\)?\s*\)?\s*;?\s*$)");
const std::regex re_sum(
    R"(^\s*\(?\s*(\w+)\s*\+=\s*nlAt\((\w+),\s*\((\w+)\)\)\s*\)?\s*;?\s*$)");
const std::regex re_fill(
    R"(^\s*nlAt\((\w+),\s*\((\w+)\)\)\s*=\s*([^;]+)\s*;?\s*$)");
const std::regex re_copy(
    R"(^\s*nlAt\((\w+),\s*\((\w+)\)\)\s*=\s*nlAt\((\w+),\s*\(\2\)\)\s*;?\s*$)");
const std::regex re_maxmin(
    R"(^\s*if\s*\(\(nlAt\((\w+),\s*\((\w+)\)\)\s*(>|<)\s*(\w+)\)\)\s*\{?\s*\4\s*=\s*nlAt\(\1,\s*\(\2\)\)\s*;?\s*\}?\s*$)");
const std::regex re_scale(
    R"(^\s*\(?\s*nlAt\((\w+),\s*\((\w+)\)\)\s*(\*=|\+=|-=|/=)\s*([^;]+?)\s*\)?\s*;?\s*$)");
const std::regex re_var(
    R"(^\s*\(?\s*(\w+)\s*\+=\s*\(\(nlAt\((\w+),\s*\((\w+)\)\)\s*-\s*(\w+)\)\s*\*\s*\(nlAt\(\2,\s*\(\3\)\)\s*-\s*\4\)\)\s*\)?\s*;?\s*$)");
const std::regex re_ivar_word(R"(\bVAR\b)");

bool has_ivar(const std::string& s, const std::string& ivar) {
    return std::regex_search(s,
        std::regex("\\b" + ivar + "\\b"));
}

// idx вида `i` или `(... + i)` -> base-expr или "".
// база не должна содержать ivar.
bool split_index(std::string idx, const std::string& ivar,
                 std::string& base) {
    // trim
    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        size_t b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? "" : s.substr(a, b - a + 1);
    };
    idx = trim(idx);
    if (idx == ivar) { base = "0"; return true; }
    while (idx.size() >= 2 && idx.front() == '(' && idx.back() == ')')
        idx = trim(idx.substr(1, idx.size() - 2));
    std::smatch m;
    std::regex pat("(.+?)\\s*\\+\\s*" + ivar + "\\s*$");
    if (std::regex_match(idx, m, pat) && !has_ivar(m[1].str(), ivar)) {
        base = m[1].str();
        return true;
    }
    // индекс `i + base` — симметричный порядок
    std::regex pat2("^" + ivar + "\\s*\\+\\s*(.+?)$");
    if (std::regex_match(idx, m, pat2) && !has_ivar(m[1].str(), ivar)) {
        base = m[1].str();
        return true;
    }
    return false;
}

// ================= AVX2 эмиттеры (1:1 порт из py) =================

std::string emit_avx2_dotprod(const std::string& acc,
    const std::string& arrA, const std::string& arrB,
    const std::string& lo, const std::string& hi, bool incl,
    const std::string& baseA, const std::string& baseB) {
    std::string plus = incl ? " + 1" : "";
    std::string offA = baseA != "0" ? " + (" + baseA + ")" : "";
    std::string offB = baseB != "0" ? " + (" + baseB + ")" : "";
    return
"{\n"
"    long long _nl_n = (" + hi + ") - (" + lo + ")" + plus + ";\n"
"    const double *_nl_pa = &" + arrA + "[(" + lo + ")" + offA +
"], *_nl_pb = &" + arrB + "[(" + lo + ")" + offB + "];\n"
"    double _nl_acc = " + acc + ";\n"
"#if defined(__x86_64__) && defined(__AVX2__)\n"
"    long long _nl_k = _nl_n & ~31LL;\n"
"    /* 8 аккумуляторов: 2 FMA-порта x 4 такта латентности */\n"
"#define _NL_DP_BODY(pf) \\\n"
"    \"vxorpd %%ymm0, %%ymm0, %%ymm0\\n\\t\" \\\n"
"    \"vxorpd %%ymm1, %%ymm1, %%ymm1\\n\\t\" \\\n"
"    \"vxorpd %%ymm2, %%ymm2, %%ymm2\\n\\t\" \\\n"
"    \"vxorpd %%ymm3, %%ymm3, %%ymm3\\n\\t\" \\\n"
"    \"vxorpd %%ymm8, %%ymm8, %%ymm8\\n\\t\" \\\n"
"    \"vxorpd %%ymm9, %%ymm9, %%ymm9\\n\\t\" \\\n"
"    \"vxorpd %%ymm10, %%ymm10, %%ymm10\\n\\t\" \\\n"
"    \"vxorpd %%ymm11, %%ymm11, %%ymm11\\n\\t\" \\\n"
"    \"xorq %%rcx, %%rcx\\n\\t\" \\\n"
"    \"1:\\n\\t\" \\\n"
"    \"vmovupd (%[pa],%%rcx,8), %%ymm4\\n\\t\" \\\n"
"    \"vfmadd231pd (%[pb],%%rcx,8), %%ymm4, %%ymm0\\n\\t\" \\\n"
"    \"vmovupd 32(%[pa],%%rcx,8), %%ymm4\\n\\t\" \\\n"
"    \"vfmadd231pd 32(%[pb],%%rcx,8), %%ymm4, %%ymm1\\n\\t\" \\\n"
"    \"vmovupd 64(%[pa],%%rcx,8), %%ymm4\\n\\t\" \\\n"
"    \"vfmadd231pd 64(%[pb],%%rcx,8), %%ymm4, %%ymm2\\n\\t\" \\\n"
"    \"vmovupd 96(%[pa],%%rcx,8), %%ymm4\\n\\t\" \\\n"
"    \"vfmadd231pd 96(%[pb],%%rcx,8), %%ymm4, %%ymm3\\n\\t\" \\\n"
"    \"vmovupd 128(%[pa],%%rcx,8), %%ymm4\\n\\t\" \\\n"
"    \"vfmadd231pd 128(%[pb],%%rcx,8), %%ymm4, %%ymm8\\n\\t\" \\\n"
"    \"vmovupd 160(%[pa],%%rcx,8), %%ymm4\\n\\t\" \\\n"
"    \"vfmadd231pd 160(%[pb],%%rcx,8), %%ymm4, %%ymm9\\n\\t\" \\\n"
"    \"vmovupd 192(%[pa],%%rcx,8), %%ymm4\\n\\t\" \\\n"
"    \"vfmadd231pd 192(%[pb],%%rcx,8), %%ymm4, %%ymm10\\n\\t\" \\\n"
"    \"vmovupd 224(%[pa],%%rcx,8), %%ymm4\\n\\t\" \\\n"
"    \"vfmadd231pd 224(%[pb],%%rcx,8), %%ymm4, %%ymm11\\n\\t\" \\\n"
"    pf \\\n"
"    \"addq $32, %%rcx\\n\\t\" \\\n"
"    \"cmp %[k], %%rcx\\n\\t\" \\\n"
"    \"jb 1b\\n\\t\" \\\n"
"    \"vaddpd %%ymm1, %%ymm0, %%ymm0\\n\\t\" \\\n"
"    \"vaddpd %%ymm3, %%ymm2, %%ymm2\\n\\t\" \\\n"
"    \"vaddpd %%ymm9, %%ymm8, %%ymm8\\n\\t\" \\\n"
"    \"vaddpd %%ymm11, %%ymm10, %%ymm10\\n\\t\" \\\n"
"    \"vaddpd %%ymm2, %%ymm0, %%ymm0\\n\\t\" \\\n"
"    \"vaddpd %%ymm10, %%ymm8, %%ymm8\\n\\t\" \\\n"
"    \"vaddpd %%ymm8, %%ymm0, %%ymm0\\n\\t\" \\\n"
"    \"vextractf128 $1, %%ymm0, %%xmm1\\n\\t\" \\\n"
"    \"vaddpd %%xmm1, %%xmm0, %%xmm0\\n\\t\" \\\n"
"    \"vpermilpd $1, %%xmm0, %%xmm1\\n\\t\" \\\n"
"    \"vaddsd %%xmm1, %%xmm0, %%xmm0\\n\\t\" \\\n"
"    \"vaddsd %[out], %%xmm0, %%xmm0\\n\\t\" \\\n"
"    \"vmovsd %%xmm0, %[out]\\n\\t\"\n"
"    /* данные > L2 — подтягиваем заранее; маленькие — без pf */\n"
"    /* do-while: >=1 итерации — при _nl_k==0 был бы OOB-read */\n"
"    if (_nl_k == 0) {} else if (_nl_n > 65536) {\n"
"        __asm__ volatile(\n"
"            _NL_DP_BODY(\n"
"                \"prefetcht0 1536(%[pa],%%rcx,8)\\n\\t\"\n"
"                \"prefetcht0 1536(%[pb],%%rcx,8)\\n\\t\")\n"
"            : [out] \"+m\" (_nl_acc)\n"
"            : [pa] \"r\" (_nl_pa), [pb] \"r\" (_nl_pb), [k] \"m\" (_nl_k)\n"
"            : \"rcx\", \"ymm0\", \"ymm1\", \"ymm2\", \"ymm3\", \"ymm4\",\n"
"              \"ymm8\", \"ymm9\", \"ymm10\", \"ymm11\", \"cc\", \"memory\");\n"
"    } else {\n"
"        __asm__ volatile(\n"
"            _NL_DP_BODY(\"\")\n"
"            : [out] \"+m\" (_nl_acc)\n"
"            : [pa] \"r\" (_nl_pa), [pb] \"r\" (_nl_pb), [k] \"m\" (_nl_k)\n"
"            : \"rcx\", \"ymm0\", \"ymm1\", \"ymm2\", \"ymm3\", \"ymm4\",\n"
"              \"ymm8\", \"ymm9\", \"ymm10\", \"ymm11\", \"cc\", \"memory\");\n"
"    }\n"
"#undef _NL_DP_BODY\n"
"    for (long long i = _nl_k; i < _nl_n; i++) _nl_acc += _nl_pa[i] * _nl_pb[i];\n"
"#else\n"
"    for (long long i = 0; i < _nl_n; i++) _nl_acc += _nl_pa[i] * _nl_pb[i];\n"
"#endif\n"
"    " + acc + " = _nl_acc;\n"
"}";
}

std::string emit_avx2_sum(const std::string& acc,
    const std::string& arr, const std::string& lo,
    const std::string& hi, bool incl) {
    std::string plus = incl ? " + 1" : "";
    return
"{\n"
"    long long _nl_n = (" + hi + ") - (" + lo + ")" + plus + ";\n"
"    const double *_nl_pa = &" + arr + "[(" + lo + ");\n"
"    double _nl_acc = " + acc + ";\n"
"#if defined(__x86_64__) && defined(__AVX2__)\n"
"    long long _nl_k = _nl_n & ~31LL;\n"
"    if (_nl_k > 0)\n"
"    __asm__ volatile(\n"
"        \"vxorpd %%ymm0, %%ymm0, %%ymm0\\n\\t\"\n"
"        \"vxorpd %%ymm1, %%ymm1, %%ymm1\\n\\t\"\n"
"        \"vxorpd %%ymm2, %%ymm2, %%ymm2\\n\\t\"\n"
"        \"vxorpd %%ymm3, %%ymm3, %%ymm3\\n\\t\"\n"
"        \"vxorpd %%ymm8, %%ymm8, %%ymm8\\n\\t\"\n"
"        \"vxorpd %%ymm9, %%ymm9, %%ymm9\\n\\t\"\n"
"        \"vxorpd %%ymm10, %%ymm10, %%ymm10\\n\\t\"\n"
"        \"vxorpd %%ymm11, %%ymm11, %%ymm11\\n\\t\"\n"
"        \"xorq %%rcx, %%rcx\\n\\t\"\n"
"        \"1:\\n\\t\"\n"
"        \"vaddpd (%[pa],%%rcx,8), %%ymm0, %%ymm0\\n\\t\"\n"
"        \"vaddpd 32(%[pa],%%rcx,8), %%ymm1, %%ymm1\\n\\t\"\n"
"        \"vaddpd 64(%[pa],%%rcx,8), %%ymm2, %%ymm2\\n\\t\"\n"
"        \"vaddpd 96(%[pa],%%rcx,8), %%ymm3, %%ymm3\\n\\t\"\n"
"        \"vaddpd 128(%[pa],%%rcx,8), %%ymm8, %%ymm8\\n\\t\"\n"
"        \"vaddpd 160(%[pa],%%rcx,8), %%ymm9, %%ymm9\\n\\t\"\n"
"        \"vaddpd 192(%[pa],%%rcx,8), %%ymm10, %%ymm10\\n\\t\"\n"
"        \"vaddpd 224(%[pa],%%rcx,8), %%ymm11, %%ymm11\\n\\t\"\n"
"        \"addq $32, %%rcx\\n\\t\"\n"
"        \"cmp %[k], %%rcx\\n\\t\"\n"
"        \"jb 1b\\n\\t\"\n"
"        \"vaddpd %%ymm1, %%ymm0, %%ymm0\\n\\t\"\n"
"        \"vaddpd %%ymm3, %%ymm2, %%ymm2\\n\\t\"\n"
"        \"vaddpd %%ymm9, %%ymm8, %%ymm8\\n\\t\"\n"
"        \"vaddpd %%ymm11, %%ymm10, %%ymm10\\n\\t\"\n"
"        \"vaddpd %%ymm2, %%ymm0, %%ymm0\\n\\t\"\n"
"        \"vaddpd %%ymm10, %%ymm8, %%ymm8\\n\\t\"\n"
"        \"vaddpd %%ymm8, %%ymm0, %%ymm0\\n\\t\"\n"
"        \"vextractf128 $1, %%ymm0, %%xmm1\\n\\t\"\n"
"        \"vaddpd %%xmm1, %%xmm0, %%xmm0\\n\\t\"\n"
"        \"vpermilpd $1, %%xmm0, %%xmm1\\n\\t\"\n"
"        \"vaddsd %%xmm1, %%xmm0, %%xmm0\\n\\t\"\n"
"        \"vaddsd %[out], %%xmm0, %%xmm0\\n\\t\"\n"
"        \"vmovsd %%xmm0, %[out]\\n\\t\"\n"
"        : [out] \"+m\" (_nl_acc)\n"
"        : [pa] \"r\" (_nl_pa), [k] \"m\" (_nl_k)\n"
"        : \"rcx\", \"ymm0\", \"ymm1\", \"ymm2\", \"ymm3\", \"ymm8\",\n"
"          \"ymm9\", \"ymm10\", \"ymm11\", \"cc\", \"memory\");\n"
"    for (long long i = _nl_k; i < _nl_n; i++) _nl_acc += _nl_pa[i];\n"
"#else\n"
"    for (long long i = 0; i < _nl_n; i++) _nl_acc += _nl_pa[i];\n"
"#endif\n"
"    " + acc + " = _nl_acc;\n"
"}";
}

std::string emit_avx2_fill(const std::string& arr,
    const std::string& val, const std::string& lo,
    const std::string& hi, bool incl) {
    std::string plus = incl ? " + 1" : "";
    return
"{\n"
"    long long _nl_n = (" + hi + ") - (" + lo + ")" + plus + ";\n"
"    double *_nl_pa = &" + arr + "[(" + lo + ");\n"
"    double _nl_v = (" + val + ");\n"
"#if defined(__x86_64__) && defined(__AVX2__)\n"
"    long long _nl_k = _nl_n & ~3LL;\n"
"    if (_nl_k > 0)\n"
"    __asm__ volatile(\n"
"        \"vbroadcastsd %[v], %%ymm0\\n\\t\"\n"
"        \"xorq %%rcx, %%rcx\\n\\t\"\n"
"        \"1:\\n\\t\"\n"
"        \"vmovupd %%ymm0, (%[pa],%%rcx,8)\\n\\t\"\n"
"        \"addq $4, %%rcx\\n\\t\"\n"
"        \"cmp %[k], %%rcx\\n\\t\"\n"
"        \"jb 1b\\n\\t\"\n"
"        :\n"
"        : [v] \"m\" (_nl_v), [pa] \"r\" (_nl_pa), [k] \"m\" (_nl_k)\n"
"        : \"rcx\", \"ymm0\", \"cc\", \"memory\");\n"
"    for (long long i = _nl_k; i < _nl_n; i++) _nl_pa[i] = _nl_v;\n"
"#else\n"
"    for (long long i = 0; i < _nl_n; i++) _nl_pa[i] = _nl_v;\n"
"#endif\n"
"}";
}

std::string emit_avx2_copy(const std::string& dst,
    const std::string& src, const std::string& lo,
    const std::string& hi, bool incl) {
    std::string plus = incl ? " + 1" : "";
    return
"{\n"
"    long long _nl_n = (" + hi + ") - (" + lo + ")" + plus + ";\n"
"    double *_nl_pa = &" + dst + "[(" + lo + ");\n"
"    const double *_nl_pb = &" + src + "[(" + lo + ");\n"
"#if defined(__x86_64__) && defined(__AVX2__)\n"
"    long long _nl_k = _nl_n & ~3LL;\n"
"    if (_nl_k > 0)\n"
"    __asm__ volatile(\n"
"        \"xorq %%rcx, %%rcx\\n\\t\"\n"
"        \"1:\\n\\t\"\n"
"        \"vmovupd (%[pb],%%rcx,8), %%ymm0\\n\\t\"\n"
"        \"vmovupd %%ymm0, (%[pa],%%rcx,8)\\n\\t\"\n"
"        \"addq $4, %%rcx\\n\\t\"\n"
"        \"cmp %[k], %%rcx\\n\\t\"\n"
"        \"jb 1b\\n\\t\"\n"
"        :\n"
"        : [pa] \"r\" (_nl_pa), [pb] \"r\" (_nl_pb), [k] \"m\" (_nl_k)\n"
"        : \"rcx\", \"ymm0\", \"cc\", \"memory\");\n"
"    for (long long i = _nl_k; i < _nl_n; i++) _nl_pa[i] = _nl_pb[i];\n"
"#else\n"
"    for (long long i = 0; i < _nl_n; i++) _nl_pa[i] = _nl_pb[i];\n"
"#endif\n"
"}";
}

std::string emit_avx2_maxmin(const std::string& acc,
    const std::string& arr, char op, const std::string& lo,
    const std::string& hi, bool incl) {
    std::string ins = op == '>' ? "vmaxpd" : "vminpd";
    std::string ins_s = op == '>' ? "vmaxsd" : "vminsd";
    std::string fn = op == '>' ? "std::max" : "std::min";
    std::string plus = incl ? " + 1" : "";
    return
"{\n"
"    long long _nl_n = (" + hi + ") - (" + lo + ")" + plus + ";\n"
"    const double *_nl_pa = &" + arr + "[(" + lo + ");\n"
"    double _nl_acc = " + acc + ";\n"
"#if defined(__x86_64__) && defined(__AVX2__)\n"
"    long long _nl_k = _nl_n & ~3LL;\n"
"    if (_nl_k > 0)\n"
"    __asm__ volatile(\n"
"        \"vbroadcastsd %[out], %%ymm0\\n\\t\"\n"
"        \"xorq %%rcx, %%rcx\\n\\t\"\n"
"        \"1:\\n\\t\"\n"
"        \"" + ins + " (%[pa],%%rcx,8), %%ymm0, %%ymm0\\n\\t\"\n"
"        \"addq $4, %%rcx\\n\\t\"\n"
"        \"cmp %[k], %%rcx\\n\\t\"\n"
"        \"jb 1b\\n\\t\"\n"
"        \"vextractf128 $1, %%ymm0, %%xmm1\\n\\t\"\n"
"        \"" + ins + " %%xmm1, %%xmm0, %%xmm0\\n\\t\"\n"
"        \"vpermilpd $1, %%xmm0, %%xmm1\\n\\t\"\n"
"        \"" + ins_s + " %%xmm1, %%xmm0, %%xmm0\\n\\t\"\n"
"        \"" + ins_s + " %[out], %%xmm0, %%xmm0\\n\\t\"\n"
"        \"vmovsd %%xmm0, %[out]\\n\\t\"\n"
"        : [out] \"+m\" (_nl_acc)\n"
"        : [pa] \"r\" (_nl_pa), [k] \"m\" (_nl_k)\n"
"        : \"rcx\", \"ymm0\", \"ymm1\", \"cc\", \"memory\");\n"
"    for (long long i = _nl_k; i < _nl_n; i++)\n"
"        _nl_acc = " + fn + "(_nl_acc, _nl_pa[i]);\n"
"#else\n"
"    for (long long i = 0; i < _nl_n; i++)\n"
"        _nl_acc = " + fn + "(_nl_acc, _nl_pa[i]);\n"
"#endif\n"
"    " + acc + " = _nl_acc;\n"
"}";
}

std::string emit_avx2_scale(const std::string& arr, char op,
    const std::string& val, const std::string& lo,
    const std::string& hi, bool incl) {
    std::string ins, tail;
    switch (op) {
        case '*': ins = "vmulpd"; tail = "*="; break;
        case '+': ins = "vaddpd"; tail = "+="; break;
        case '-': ins = "vsubpd"; tail = "-="; break;
        default:  ins = "vdivpd"; tail = "/="; break;
    }
    std::string plus = incl ? " + 1" : "";
    return
"{\n"
"    long long _nl_n = (" + hi + ") - (" + lo + ")" + plus + ";\n"
"    double *_nl_pa = &" + arr + "[(" + lo + ");\n"
"    double _nl_v = (" + val + ");\n"
"#if defined(__x86_64__) && defined(__AVX2__)\n"
"    long long _nl_k = _nl_n & ~3LL;\n"
"    if (_nl_k > 0)\n"
"    __asm__ volatile(\n"
"        \"vbroadcastsd %[v], %%ymm1\\n\\t\"\n"
"        \"xorq %%rcx, %%rcx\\n\\t\"\n"
"        \"1:\\n\\t\"\n"
"        \"" + ins + " (%[pa],%%rcx,8), %%ymm1, %%ymm0\\n\\t\"\n"
"        \"vmovupd %%ymm0, (%[pa],%%rcx,8)\\n\\t\"\n"
"        \"addq $4, %%rcx\\n\\t\"\n"
"        \"cmp %[k], %%rcx\\n\\t\"\n"
"        \"jb 1b\\n\\t\"\n"
"        :\n"
"        : [v] \"m\" (_nl_v), [pa] \"r\" (_nl_pa), [k] \"m\" (_nl_k)\n"
"        : \"rcx\", \"ymm0\", \"ymm1\", \"cc\", \"memory\");\n"
"    for (long long i = _nl_k; i < _nl_n; i++) _nl_pa[i] " + tail +
" _nl_v;\n"
"#else\n"
"    for (long long i = 0; i < _nl_n; i++) _nl_pa[i] " + tail +
" _nl_v;\n"
"#endif\n"
"}";
}

std::string emit_avx2_var(const std::string& acc,
    const std::string& arr, const std::string& mean,
    const std::string& lo, const std::string& hi, bool incl) {
    std::string plus = incl ? " + 1" : "";
    return
"{\n"
"    long long _nl_n = (" + hi + ") - (" + lo + ")" + plus + ";\n"
"    const double *_nl_pa = &" + arr + "[(" + lo + ");\n"
"    double _nl_acc = " + acc + ";\n"
"    double _nl_m = (" + mean + ");\n"
"#if defined(__x86_64__) && defined(__AVX2__)\n"
"    long long _nl_k = _nl_n & ~31LL;\n"
"    if (_nl_k > 0)\n"
"    __asm__ volatile(\n"
"        \"vbroadcastsd %[m], %%ymm5\\n\\t\"\n"
"        \"vxorpd %%ymm0, %%ymm0, %%ymm0\\n\\t\"\n"
"        \"vxorpd %%ymm1, %%ymm1, %%ymm1\\n\\t\"\n"
"        \"vxorpd %%ymm2, %%ymm2, %%ymm2\\n\\t\"\n"
"        \"vxorpd %%ymm3, %%ymm3, %%ymm3\\n\\t\"\n"
"        \"vxorpd %%ymm8, %%ymm8, %%ymm8\\n\\t\"\n"
"        \"vxorpd %%ymm9, %%ymm9, %%ymm9\\n\\t\"\n"
"        \"vxorpd %%ymm10, %%ymm10, %%ymm10\\n\\t\"\n"
"        \"vxorpd %%ymm11, %%ymm11, %%ymm11\\n\\t\"\n"
"        \"xorq %%rcx, %%rcx\\n\\t\"\n"
"        \"1:\\n\\t\"\n"
"        \"vmovupd (%[pa],%%rcx,8), %%ymm4\\n\\t\"\n"
"        \"vsubpd %%ymm5, %%ymm4, %%ymm4\\n\\t\"\n"
"        \"vfmadd231pd %%ymm4, %%ymm4, %%ymm0\\n\\t\"\n"
"        \"vmovupd 32(%[pa],%%rcx,8), %%ymm4\\n\\t\"\n"
"        \"vsubpd %%ymm5, %%ymm4, %%ymm4\\n\\t\"\n"
"        \"vfmadd231pd %%ymm4, %%ymm4, %%ymm1\\n\\t\"\n"
"        \"vmovupd 64(%[pa],%%rcx,8), %%ymm4\\n\\t\"\n"
"        \"vsubpd %%ymm5, %%ymm4, %%ymm4\\n\\t\"\n"
"        \"vfmadd231pd %%ymm4, %%ymm4, %%ymm2\\n\\t\"\n"
"        \"vmovupd 96(%[pa],%%rcx,8), %%ymm4\\n\\t\"\n"
"        \"vsubpd %%ymm5, %%ymm4, %%ymm4\\n\\t\"\n"
"        \"vfmadd231pd %%ymm4, %%ymm4, %%ymm3\\n\\t\"\n"
"        \"vmovupd 128(%[pa],%%rcx,8), %%ymm4\\n\\t\"\n"
"        \"vsubpd %%ymm5, %%ymm4, %%ymm4\\n\\t\"\n"
"        \"vfmadd231pd %%ymm4, %%ymm4, %%ymm8\\n\\t\"\n"
"        \"vmovupd 160(%[pa],%%rcx,8), %%ymm4\\n\\t\"\n"
"        \"vsubpd %%ymm5, %%ymm4, %%ymm4\\n\\t\"\n"
"        \"vfmadd231pd %%ymm4, %%ymm4, %%ymm9\\n\\t\"\n"
"        \"vmovupd 192(%[pa],%%rcx,8), %%ymm4\\n\\t\"\n"
"        \"vsubpd %%ymm5, %%ymm4, %%ymm4\\n\\t\"\n"
"        \"vfmadd231pd %%ymm4, %%ymm4, %%ymm10\\n\\t\"\n"
"        \"vmovupd 224(%[pa],%%rcx,8), %%ymm4\\n\\t\"\n"
"        \"vsubpd %%ymm5, %%ymm4, %%ymm4\\n\\t\"\n"
"        \"vfmadd231pd %%ymm4, %%ymm4, %%ymm11\\n\\t\"\n"
"        \"addq $32, %%rcx\\n\\t\"\n"
"        \"cmp %[k], %%rcx\\n\\t\"\n"
"        \"jb 1b\\n\\t\"\n"
"        \"vaddpd %%ymm1, %%ymm0, %%ymm0\\n\\t\"\n"
"        \"vaddpd %%ymm3, %%ymm2, %%ymm2\\n\\t\"\n"
"        \"vaddpd %%ymm9, %%ymm8, %%ymm8\\n\\t\"\n"
"        \"vaddpd %%ymm11, %%ymm10, %%ymm10\\n\\t\"\n"
"        \"vaddpd %%ymm2, %%ymm0, %%ymm0\\n\\t\"\n"
"        \"vaddpd %%ymm10, %%ymm8, %%ymm8\\n\\t\"\n"
"        \"vaddpd %%ymm8, %%ymm0, %%ymm0\\n\\t\"\n"
"        \"vextractf128 $1, %%ymm0, %%xmm1\\n\\t\"\n"
"        \"vaddpd %%xmm1, %%xmm0, %%xmm0\\n\\t\"\n"
"        \"vpermilpd $1, %%xmm0, %%xmm1\\n\\t\"\n"
"        \"vaddsd %%xmm1, %%xmm0, %%xmm0\\n\\t\"\n"
"        \"vaddsd %[out], %%xmm0, %%xmm0\\n\\t\"\n"
"        \"vmovsd %%xmm0, %[out]\\n\\t\"\n"
"        : [out] \"+m\" (_nl_acc)\n"
"        : [m] \"m\" (_nl_m), [pa] \"r\" (_nl_pa), [k] \"m\" (_nl_k)\n"
"        : \"rcx\", \"ymm0\", \"ymm1\", \"ymm2\", \"ymm3\", \"ymm4\", \"ymm5\",\n"
"          \"ymm8\", \"ymm9\", \"ymm10\", \"ymm11\", \"cc\", \"memory\");\n"
"    for (long long i = _nl_k; i < _nl_n; i++)\n"
"        _nl_acc += (_nl_pa[i] - _nl_m) * (_nl_pa[i] - _nl_m);\n"
"#else\n"
"    for (long long i = 0; i < _nl_n; i++)\n"
"        _nl_acc += (_nl_pa[i] - _nl_m) * (_nl_pa[i] - _nl_m);\n"
"#endif\n"
"    " + acc + " = _nl_acc;\n"
"}";
}

} // namespace

std::string nl_try_optimize_for(const std::string& decl_txt,
                                const std::string& cond_txt,
                                const std::string& incr_txt,
                                const std::string& body) {
    std::smatch md, mc, mi;
    if (!std::regex_search(decl_txt, md, re_decl)) return "";
    if (!std::regex_match(cond_txt, mc, re_cond)) return "";
    if (!std::regex_match(incr_txt, mi, re_incr)) return "";
    std::string ivar = md[2].str();
    if (mc[1].str() != ivar) return "";
    if (!(mi[1].matched && mi[1].str() == ivar) &&
        !(mi[2].matched && mi[2].str() == ivar)) return "";
    std::string lo = md[3].str(), hi = mc[3].str();
    bool incl = mc[2].str() == "<=";

    std::smatch m;
    if (std::regex_match(body, m, re_dotprod)) {
        std::string bA, bB;
        if (split_index(m[3].str(), ivar, bA) &&
            split_index(m[5].str(), ivar, bB))
            return emit_avx2_dotprod(m[1].str(), m[2].str(),
                                     m[4].str(), lo, hi, incl, bA, bB);
    }
    if (std::regex_match(body, m, re_copy)) {
        if (m[2].str() == ivar && m[1].str() != m[3].str())
            return emit_avx2_copy(m[1].str(), m[3].str(), lo, hi, incl);
    }
    if (std::regex_match(body, m, re_fill)) {
        if (m[2].str() == ivar && !has_ivar(m[3].str(), ivar))
            return emit_avx2_fill(m[1].str(), m[3].str(), lo, hi, incl);
    }
    if (std::regex_match(body, m, re_maxmin)) {
        if (m[2].str() == ivar)
            return emit_avx2_maxmin(m[4].str(), m[1].str(),
                                    m[3].str()[0], lo, hi, incl);
    }
    if (std::regex_match(body, m, re_scale)) {
        if (m[2].str() == ivar && !has_ivar(m[4].str(), ivar))
            return emit_avx2_scale(m[1].str(), m[3].str()[0],
                                   m[4].str(), lo, hi, incl);
    }
    if (std::regex_match(body, m, re_var)) {
        if (m[3].str() == ivar && !has_ivar(m[4].str(), ivar))
            return emit_avx2_var(m[1].str(), m[2].str(), m[4].str(),
                                 lo, hi, incl);
    }
    if (std::regex_match(body, m, re_sum)) {
        if (m[3].str() == ivar)
            return emit_avx2_sum(m[1].str(), m[2].str(), lo, hi, incl);
    }
    return "";
}
