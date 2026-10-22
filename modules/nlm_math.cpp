// Модуль math — обёртки над <cmath> с понятными именами.
#include "nlm_math.h"
#include <cmath>

double nlmath_floor(double x) { return std::floor(x); }
double nlmath_ceil(double x) { return std::ceil(x); }
double nlmath_round(double x) { return std::round(x); }
double nlmath_sqrt(double x) { return std::sqrt(x); }
double nlmath_pow(double b, double e) { return std::pow(b, e); }
double nlmath_abs(double x) { return std::fabs(x); }
long long nlmath_iabs(long long x) { return x < 0 ? -x : x; }
double nlmath_sin(double x) { return std::sin(x); }
double nlmath_cos(double x) { return std::cos(x); }
double nlmath_tan(double x) { return std::tan(x); }
double nlmath_log(double x) { return std::log(x); }
double nlmath_exp(double x) { return std::exp(x); }
double nlmath_min(double a, double b) { return a < b ? a : b; }
double nlmath_max(double a, double b) { return a > b ? a : b; }
double nlmath_imin(long long a, long long b) { return a < b ? a : b; }
double nlmath_imax(long long a, long long b) { return a > b ? a : b; }
