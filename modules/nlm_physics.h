#pragma once
// Модуль physics — порт physics.py: кинематика, динамика,
// энергия, гравитация, баллистика, колебания.
#include <cmath>
#include <vector>

static const double PG    = 9.80665;        // ускорение свободного падения, м/с²
static const double PGrav = 6.67430e-11;    // гравитационная постоянная
static const double PC    = 299792458.0;    // скорость света, м/с
static const double PH    = 6.62607015e-34; // постоянная Планка
static const double PK    = 1.380649e-23;   // постоянная Больцмана
static const double PNA   = 6.02214076e23;  // число Авогадро

// --- кинематика ---
// v(t) = v0 + a t
inline double PV(double v0, double a, double t) { return v0 + a * t; }
// s(t) = v0 t + a t²/2
inline double PS(double v0, double a, double t) {
    return v0 * t + 0.5 * a * t * t;
}
// время разгона до v: t = (v - v0)/a
inline double PT(double v, double v0, double a) {
    return a != 0 ? (v - v0) / a : 0;
}
// v² = v0² + 2as -> v после пути s
inline double PV2(double v0, double a, double s) {
    return std::sqrt(std::max(0.0, v0 * v0 + 2 * a * s));
}
// время падения с высоты h
inline double PFall(double h, double g = PG) {
    return std::sqrt(2 * h / g);
}

// --- динамика ---
// F = m a
inline double PF(double m, double a) { return m * a; }
// a = F/m
inline double PA(double f, double m) { return f / m; }
// импульс p = m v
inline double PMom(double m, double v) { return m * v; }
// работа W = F s cosφ
inline double PWork(double f, double s, double phi = 0) {
    return f * s * std::cos(phi);
}
// мощность P = W/t
inline double PPower(double w, double t) { return w / t; }

// --- энергия ---
inline double PKin(double m, double v) { return 0.5 * m * v * v; }
inline double PPot(double m, double h, double g = PG) {
    return m * g * h;
}
// скорость по кинетической энергии
inline double PVEnergy(double e, double m) {
    return std::sqrt(2 * e / m);
}

// --- гравитация ---
// F = G m1 m2 / r²
inline double PGravForce(double m1, double m2, double r) {
    return PGrav * m1 * m2 / (r * r);
}
// круговая орбита: v = sqrt(GM/r)
inline double POrbitV(double m, double r) {
    return std::sqrt(PGrav * m / r);
}
// вторая космическая: v = sqrt(2GM/r)
inline double PEscape(double m, double r) {
    return std::sqrt(2 * PGrav * m / r);
}
// период орбиты (кеплер): T = 2π sqrt(r³/GM)
inline double POrbitT(double m, double r) {
    return 2 * 3.14159265358979323846
           * std::sqrt(r * r * r / (PGrav * m));
}

// --- баллистика ---
// дальность: R = v² sin(2φ)/g
inline double PRange(double v, double phi, double g = PG) {
    return v * v * std::sin(2 * phi) / g;
}
// высота броска: H = v² sin²φ / 2g
inline double PHeight(double v, double phi, double g = PG) {
    double s = std::sin(phi);
    return v * v * s * s / (2 * g);
}
// время полёта: T = 2v sinφ/g
inline double PFlight(double v, double phi, double g = PG) {
    return 2 * v * std::sin(phi) / g;
}
// позиция (x, y) во время t
inline std::vector<double> PPos(double v, double phi, double t,
                                double g = PG) {
    return {v * std::cos(phi) * t,
            v * std::sin(phi) * t - 0.5 * g * t * t};
}

// --- колебания ---
// маятник: T = 2π sqrt(l/g)
inline double PPendulum(double l, double g = PG) {
    return 2 * 3.14159265358979323846 * std::sqrt(l / g);
}
// пружина: T = 2π sqrt(m/k)
inline double PSpringT(double m, double k) {
    return 2 * 3.14159265358979323846 * std::sqrt(m / k);
}
// гармоническое колебание: x(t) = A cos(ωt + φ)
inline double PHarm(double a, double w, double t, double phi = 0) {
    return a * std::cos(w * t + phi);
}
