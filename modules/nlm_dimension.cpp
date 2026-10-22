// Модуль dimension — реализация доменов.
#include "nlm_dimension.h"
#include <unordered_map>

DimEcache dim_ecaches[64];

static std::unordered_map<std::string,
    std::unordered_map<std::string, JValue>> dim_store[64];
static std::mutex dim_store_locks[64];
static std::vector<std::thread> dim_threads;
static std::mutex dim_threads_lock;

void* dimAlloc(int dim, size_t n) {
    auto& e = dim_ecaches[dim];
    if (!e.buf) e.buf = (char*)std::malloc(DimEcache::SIZE);
    std::lock_guard<std::mutex> g(e.lock);
    if (e.off + n > DimEcache::SIZE) return nullptr;
    void* p = e.buf + e.off;
    e.off += n;
    return p;
}
void* dimAllocFree(int dim, size_t n) {
    auto& e = dim_ecaches[dim];
    if (!e.buf) e.buf = (char*)std::malloc(DimEcache::SIZE);
    if (e.off + n > DimEcache::SIZE) return nullptr;
    void* p = e.buf + e.off;
    e.off += n;
    return p;
}
void dimReset(int dim) {
    auto& e = dim_ecaches[dim];
    std::lock_guard<std::mutex> g(e.lock);
    e.off = 0;
}

std::unordered_map<std::string, JValue>& dimSlot(
        int dim, const std::string& var) {
    std::lock_guard<std::mutex> g(dim_store_locks[dim]);
    return dim_store[dim][var];
}
std::mutex& dimSlotLock(int dim) {
    return dim_store_locks[dim];
}

void dimSpawn(std::function<void()> fn) {
    std::lock_guard<std::mutex> g(dim_threads_lock);
    dim_threads.emplace_back(std::move(fn));
}
void dimJoinAll() {
    for (;;) {
        std::lock_guard<std::mutex> g(dim_threads_lock);
        bool alive = false;
        for (auto& t : dim_threads)
            if (t.joinable()) { alive = true; t.join(); }
        if (!alive) {
            dim_threads.clear();
            return;
        }
    }
}
