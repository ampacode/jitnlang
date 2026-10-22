#pragma once
// Модуль str — строковые функции.
#include <string>
#include <vector>

std::string nlstr_upper(const std::string& s);
std::string nlstr_lower(const std::string& s);
std::string nlstr_trim(const std::string& s);
std::vector<std::string> nlstr_split(const std::string& s,
                                     const std::string& delim);
std::string nlstr_join(const std::vector<std::string>& parts,
                       const std::string& delim);
bool nlstr_contains(const std::string& s, const std::string& sub);
bool nlstr_startsWith(const std::string& s, const std::string& prefix);
bool nlstr_endsWith(const std::string& s, const std::string& suffix);
std::string nlstr_replace(const std::string& s, const std::string& from,
                          const std::string& to);
std::string nlstr_repeat(const std::string& s, long long n);
long long nlstr_indexOf(const std::string& s, const std::string& sub,
                        long long from);
// substr(s, pos[, len]) — позиции с 0; len < 0 или за край — до конца
std::string nlstr_substr(const std::string& s, long long pos,
                         long long len);
// at(s, i) — один символ строкой; за границей — пусто
std::string nlstr_at(const std::string& s, long long i);
long long nlstr_toInt(const std::string& s);
double nlstr_toDouble(const std::string& s);
