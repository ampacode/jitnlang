#pragma once
// Модуль regex — порт regex.py: std::regex поверх тех же имён.
//   reMatch(s, p)   -> bool   — совпадение целиком
//   reSearch(s, p)  -> bool   — где-то внутри
//   reGroup(s,p,i)  -> string — i-я захваченная группа первого match
//   reGroups(s,p)   -> list   — все группы первого match
//   reFindAll(s,p)  -> list   — все совпадения
//   reReplace(s,p,r)-> string
//   reSplit(s,p)    -> list
//   reTest(p)       -> bool   — валидность паттерна
#include <string>
#include <vector>

bool nlregex_test(const std::string& pat);
bool nlregex_match(const std::string& s, const std::string& pat);
bool nlregex_search(const std::string& s, const std::string& pat);
std::string nlregex_group(const std::string& s,
                          const std::string& pat, long long i);
std::vector<std::string> nlregex_groups(const std::string& s,
                                        const std::string& pat);
std::vector<std::string> nlregex_findAll(const std::string& s,
                                         const std::string& pat);
std::string nlregex_replace(const std::string& s,
                            const std::string& pat,
                            const std::string& rep);
std::vector<std::string> nlregex_split(const std::string& s,
                                       const std::string& pat);
