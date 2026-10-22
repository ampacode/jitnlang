#pragma once
// Модуль quantum — порт quantum.py: кубиты, гейты, измерения
// + струнная физика (константы): KK-моды, Regge, натяжение.
#include <cmath>
#include <complex>
#include <vector>
#include <algorithm>

using cplx   = std::complex<double>;
using QState = std::vector<cplx>;       // вектор состояния |psi>
using QGate  = std::vector<std::vector<cplx>>;  // унитарный оператор

inline QState QKet0() { return {cplx(1), cplx(0)}; }
inline QState QKet1() { return {cplx(0), cplx(1)}; }
// базисное состояние n-кубитной системы |i>
inline QState QBasis(long i, long nqubits) {
    QState s(size_t(1) << nqubits, cplx(0));
    s[i] = cplx(1);
    return s;
}
// применить гейт: U|psi>
inline QState QApply(const QGate& g, const QState& s) {
    size_t n = g.size();
    QState out(n, cplx(0));
    for (size_t i = 0; i < n; ++i)
        for (size_t k = 0; k < n; ++k)
            out[i] += g[i][k] * s[k];
    return out;
}
// тензорное произведение состояний |a> ⊗ |b>
inline QState QTensor(const QState& a, const QState& b) {
    QState out;
    out.reserve(a.size() * b.size());
    for (auto& x : a)
        for (auto& y : b)
            out.push_back(x * y);
    return out;
}
// скалярное произведение <a|b>
inline cplx QInner(const QState& a, const QState& b) {
    cplx s(0);
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) s += std::conj(a[i]) * b[i];
    return s;
}
inline double QNorm(const QState& s) {
    return std::sqrt(std::norm(QInner(s, s)));
}
inline QState QNormalize(QState s) {
    double n = QNorm(s);
    for (auto& c : s) c /= n;
    return s;
}
// вероятность базиса i: |a_i|²
inline double QProb(const QState& s, long i) {
    return std::norm(s[i]);
}
// все вероятности
inline std::vector<double> QProbs(const QState& s) {
    std::vector<double> out;
    for (auto& c : s) out.push_back(std::norm(c));
    return out;
}
// матожидание измерения: sum d_i |a_i|²
inline double QExpect(const QState& s, const std::vector<double>& d) {
    double e = 0;
    for (size_t i = 0; i < s.size() && i < d.size(); ++i)
        e += d[i] * std::norm(s[i]);
    return e;
}
// гейты Паули/Адамар/фаза
inline QGate QX() { return {{0,1},{1,0}}; }
inline QGate QY() {
    return {{cplx(0),cplx(0,-1)},{cplx(0,1),cplx(0)}};
}
inline QGate QZ() { return {{1,0},{0,-1}}; }
inline QGate QH() {
    double s = 1.0 / std::sqrt(2.0);
    return {{cplx(s),cplx(s)},{cplx(s),cplx(-s)}};
}
inline QGate QPhase(double phi) {
    return {{cplx(1),0},{0,std::exp(cplx(0,phi))}};
}
// CNOT (2 кубита)
inline QGate QCNOT() {
    return {{1,0,0,0},{0,1,0,0},{0,0,0,1},{0,0,1,0}};
}
// вращения Блоха: Rx(theta) и т.д.
inline QGate QRx(double t) {
    cplx c(std::cos(t/2), 0), s(0, -std::sin(t/2));
    return {{c,s},{s,c}};
}
inline QGate QRy(double t) {
    double c = std::cos(t/2), s = std::sin(t/2);
    return {{cplx(c),cplx(-s)},{cplx(s),cplx(c)}};
}
inline QGate QRz(double t) {
    return {{std::exp(cplx(0,-t/2)),cplx(0)},
            {cplx(0),std::exp(cplx(0,t/2))}};
}
// коллапс: система переходит в состояние |i>
inline QState QCollapse(long i, long nqubits) {
    return QBasis(i, nqubits);
}

// ===== струнная физика =====
static const double SPlanckLen  = 1.616255e-35;   // м
static const double SPlanckMass = 2.176434e-8;    // кг
static const double SHbar = 1.054571817e-34;      // Дж·с
static const double SC = 299792458.0;             // м/с
// критическая размерность бозонной/супер-струны
inline int SDim(bool bosonic = false) { return bosonic ? 26 : 10; }
// натяжение струны: T = 1/(2π α')
inline double STension(double alpha) {
    return 1.0 / (2.0 * 3.14159265358979323846 * alpha);
}
// длина струны: l_s = sqrt(α')
inline double SLen(double alpha) { return std::sqrt(alpha); }
// спектр Regge: M² = (N - a)/α'
inline double SRegge(int n, double alpha, double a = 1.0) {
    return (n - a) / alpha;
}
// масса Regge: sqrt(M²)
inline double SReggeMass(int n, double alpha, double a = 1.0) {
    return std::sqrt(std::max(0.0, SRegge(n, alpha, a)));
}
// Калуца-Кляйн массы: m_n = n/R
inline double SKK(int n, double r) { return n / r; }
// KK-импульсы на компактифицированном измерении: p_n = n/R
inline double SMom(int n, double r) { return n / r; }
