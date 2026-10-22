#pragma once
// Модуль math — математические функции.
// Прототипы попадают в сгенерированный код через #include "nlm_math.h".
inline double nlmath_pi() { return 3.14159265358979323846; }
inline double nlmath_e()  { return 2.71828182845904523536; }
double nlmath_floor(double x);
double nlmath_ceil(double x);
double nlmath_round(double x);
double nlmath_sqrt(double x);
double nlmath_pow(double base, double exp);
double nlmath_abs(double x);
long long nlmath_iabs(long long x);
double nlmath_sin(double x);
double nlmath_cos(double x);
double nlmath_tan(double x);
double nlmath_log(double x);
double nlmath_exp(double x);
double nlmath_min(double a, double b);
double nlmath_max(double a, double b);
double nlmath_imin(long long a, long long b);
double nlmath_imax(long long a, long long b);
