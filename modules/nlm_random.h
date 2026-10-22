#pragma once
// Модуль random — генератор случайных чисел.
//
// Простыми словами: внутри — генератор «твистер» (mt19937),
// который из одного числа-семени строит длинную последовательность
// случайных на вид чисел. Одно семя = одна последовательность:
// удобно для воспроизводимых тестов. Для криптографии НЕ подходит —
// последовательность предсказуема.
#include <string>
#include <vector>

void nlrandom_seed(long long s);
void nlrandom_seedTime();      // семя из текущего времени
long long nlrandom_int(long long lo, long long hi); // [lo, hi] вкл.
double nlrandom_float();       // [0, 1)
bool nlrandom_bool();
