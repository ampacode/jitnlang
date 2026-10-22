#pragma once
// Модуль time — время и паузы.
#include <string>

double nltime_now();          // миллисекунды от старта системы
double nltime_nowSec();       // секунды от старта системы
double nltime_unix();         // unix-время, секунды
void nltime_sleep(double ms); // пауза в миллисекундах
std::string nltime_date();    // "ГГГГ-ММ-ДД"
std::string nltime_clock();   // "ЧЧ:ММ:СС"
