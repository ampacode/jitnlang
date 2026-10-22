#pragma once
// Порт compiler/optimizations.py — распознавание горячих
// for-паттернов в сгенерированном C++ и замена на AVX2
// __asm__ volatile (dot product, sum, fill, copy, max/min,
// scale, variance) + скалярный фолбэк без __AVX2__.
#include <string>
#include <vector>
#include <memory>

// Возвращает оптимизированный C++ или "" если паттерн
// не распознан. decl/cond/incr/body — текст эмиссии частей for.
std::string nl_try_optimize_for(const std::string& decl_txt,
                                const std::string& cond_txt,
                                const std::string& incr_txt,
                                const std::string& body_txt);
