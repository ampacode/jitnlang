#pragma once
// Модуль dimension — домены-потоки dimN.
//
// Простыми словами: программа делится на «домены» dim1..dim63 —
// это независимые потоки выполнения. dim1 = основной поток
// (вызов синхронный), dim2+ = фоновые потоки.
//
//   dim2.compute()      — запустить compute() в потоке 2
//   dim2.alloc(n)       — быстрая память домена 2 (bump-арена)
//   dim2.reset()        — сбросить арену домена 2
//   dim2_data["k"] = v  — общая память домена 2 (JValue-хранилище)
//   dimJoinAll()        — дождаться всех потоков
//
// Арены: bump-allocator — выделение = сдвиг указателя, очень
// быстро, но память не освобождается по одному куску —
// только вся арена целиком (reset). Не для долгоживущих
// переменных — для временных вычислений внутри потока.
#include <string>
#include <unordered_map>
#include <vector>
#include <thread>
#include <mutex>
#include <cstdlib>
#include "nlrt.h"

// ---------- dim ecache: bump-арена 64 МБ на домен ----------
struct DimEcache {
    static constexpr size_t SIZE = 64u * 1024u * 1024u;
    char* buf = nullptr;
    size_t off = 0;
    std::mutex lock;
    ~DimEcache() { if (buf) std::free(buf); }
};

extern DimEcache dim_ecaches[64];

void* dimAlloc(int dim, size_t n);       // с мьютексом
void* dimAllocFree(int dim, size_t n);   // без блокировки
void dimReset(int dim);

// ---------- dim shared store ----------
// dimN_var["key"] — JValue-хранилище домена
std::unordered_map<std::string, JValue>& dimSlot(
    int dim, const std::string& var);
std::mutex& dimSlotLock(int dim);

// ---------- потоки ----------
void dimSpawn(std::function<void()> fn);
void dimJoinAll();
