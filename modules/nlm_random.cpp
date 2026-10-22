// Модуль random — mt19937 + равномерные распределения.
//
// Идея распределения: генератор даёт числа от 0 до большого числа N;
// мы делим диапазон на полоски нужной длины и смотрим, куда упало.
#include "nlm_random.h"
#include <random>
#include <chrono>

static std::mt19937& generator() {
    static std::mt19937 g((unsigned)std::chrono::steady_clock::now()
                              .time_since_epoch().count());
    return g;
}

void nlrandom_seed(long long s) { generator().seed((unsigned)s); }
void nlrandom_seedTime() {
    generator().seed((unsigned)std::chrono::steady_clock::now()
                         .time_since_epoch().count());
}
long long nlrandom_int(long long lo, long long hi) {
    std::uniform_int_distribution<long long> d(lo, hi);
    return d(generator());
}
double nlrandom_float() {
    std::uniform_real_distribution<double> d(0.0, 1.0);
    return d(generator());
}
bool nlrandom_bool() {
    std::uniform_int_distribution<int> d(0, 1);
    return d(generator()) != 0;
}
