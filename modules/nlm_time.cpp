// Модуль time — часы и паузы.
#include "nlm_time.h"
#include <chrono>
#include <thread>
#include <ctime>
#include <cstdio>

double nltime_now() {
    using namespace std::chrono;
    return (double)duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}
double nltime_nowSec() { return nltime_now() / 1000.0; }
double nltime_unix() {
    using namespace std::chrono;
    return (double)duration_cast<seconds>(
        system_clock::now().time_since_epoch()).count();
}
void nltime_sleep(double ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds((long long)ms));
}
std::string nltime_date() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[16];
    std::snprintf(b, 16, "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return b;
}
std::string nltime_clock() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[16];
    std::snprintf(b, 16, "%02d:%02d:%02d",
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return b;
}
