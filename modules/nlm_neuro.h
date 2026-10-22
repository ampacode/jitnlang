#pragma once
// Модуль neuro — порт neuro.py: фармакокинетика —
// полураспад, концентрации, dose-response.
#include <cmath>

// полураспад: amount(t) = x0 * 2^(-t/t12)
inline double NDecay(double x0, double t12, double t) {
    return x0 * std::pow(0.5, t / t12);
}
// начальная концентрация: C0 = bio*dose/V
inline double NConc(double dose, double v, double bio = 1.0) {
    return bio * dose / v;
}
// концентрация к моменту t: C(t) = C0 * 2^(-t/t12)
inline double NConcT(double dose, double v, double t12, double t,
                     double bio = 1.0) {
    return NConc(dose, v, bio) * std::pow(0.5, t / t12);
}
// клиренс -> t12: t12 = ln2 * V / CL
inline double NT12(double cl, double v) {
    return 0.6931471805599453 * v / cl;
}
// клиренс -> концентрация к моменту t: C = C0 e^(-CL t/V)
inline double NClear(double c0, double cl, double v, double t) {
    return c0 * std::exp(-cl * t / v);
}
// занятость рецепторов (Хилл): d^n/(EC50^n + d^n)
inline double NOcc(double dose, double ec50, double n = 1.0) {
    double dn = std::pow(dose, n), en = std::pow(ec50, n);
    if (en + dn <= 0) return 0;
    return dn / (en + dn);
}
// Emax dose-response: E0 + Emax * occ
inline double NEff(double e0, double emax, double dose,
                   double ec50, double n = 1.0) {
    return e0 + emax * NOcc(dose, ec50, n);
}
// стационарная концентрация: Css = bio*rate/CL
inline double NCss(double rate, double cl, double bio = 1.0) {
    return bio * rate / cl;
}
// загрузочная доза по Css: LD = Css * V / bio
inline double NLoad(double css, double v, double bio = 1.0) {
    return css * v / bio;
}
// поддерживающая доза: MD = Css * CL / bio
inline double NInf(double css, double cl, double bio = 1.0) {
    return css * cl / bio;
}
// площадь под кривой: AUC = bio*dose/CL
inline double NAuc(double dose, double cl, double bio = 1.0) {
    return bio * dose / cl;
}
// отношение риска: occup / target (>1 — выше цели)
inline double NRisk(double occ, double target) {
    return target > 0 ? occ / target : 0;
}
// поправка Шильда на EC50 (конкурентный ингибитор): EC50' = EC50*(1+[I]/Ki)
inline double NSchild(double ec50, double inhibitor, double ki) {
    return ec50 * (1.0 + inhibitor / ki);
}
