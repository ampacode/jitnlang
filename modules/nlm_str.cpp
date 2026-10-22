// Модуль str — строковые функции с понятными именами.
#include "nlm_str.h"

// ASCII + кириллица UTF-8 (двухбайтные D0 xx / D1 xx):
//   А-Я = D0 90..AF, Ё = D0 81 -> а-я = D0 B0..CF, ё = D1 91
std::string nlstr_upper(const std::string& s) {
    std::string r = s;
    for (size_t i = 0; i < r.size(); ++i) {
        unsigned char c = (unsigned char)r[i];
        if (c == 0xD0 && i + 1 < r.size()) {
            unsigned char d = (unsigned char)r[i + 1];
            if (d >= 0xB0 && d <= 0xCF) { r[i + 1] = (char)(d - 0x20); ++i; }
        } else if (c == 0xD1 && i + 1 < r.size()) {
            unsigned char d = (unsigned char)r[i + 1];
            if (d >= 0x80 && d <= 0x8F) { r[i] = (char)0xD0; r[i + 1] = (char)(d + 0x20); ++i; }
            else if (d == 0x91)         { r[i] = (char)0xD0; r[i + 1] = (char)0x81; ++i; }
        } else if (c < 0x80) {
            r[i] = (char)toupper(c);
        }
    }
    return r;
}
std::string nlstr_lower(const std::string& s) {
    std::string r = s;
    for (size_t i = 0; i < r.size(); ++i) {
        unsigned char c = (unsigned char)r[i];
        if (c == 0xD0 && i + 1 < r.size()) {
            unsigned char d = (unsigned char)r[i + 1];
            if (d >= 0x90 && d <= 0xAF) { r[i + 1] = (char)(d + 0x20); ++i; }
            else if (d == 0x81)         { r[i] = (char)0xD1; r[i + 1] = (char)0x91; ++i; }
        } else if (c == 0xD1 && i + 1 < r.size()) {
            // D1 80..8F = п..я уже строчные — пропускаем
            ++i;
        } else if (c < 0x80) {
            r[i] = (char)tolower(c);
        }
    }
    return r;
}
std::string nlstr_trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\n\r");
    size_t b = s.find_last_not_of(" \t\n\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
std::vector<std::string> nlstr_split(const std::string& s,
                                     const std::string& d) {
    std::vector<std::string> r;
    if (d.empty()) { r.push_back(s); return r; }
    size_t p = 0, f;
    while ((f = s.find(d, p)) != std::string::npos) {
        r.push_back(s.substr(p, f - p));
        p = f + d.size();
    }
    r.push_back(s.substr(p));
    return r;
}
std::string nlstr_join(const std::vector<std::string>& parts,
                       const std::string& d) {
    std::string r;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) r += d;
        r += parts[i];
    }
    return r;
}
bool nlstr_contains(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}
bool nlstr_startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
bool nlstr_endsWith(const std::string& s, const std::string& x) {
    return s.size() >= x.size() &&
           s.compare(s.size() - x.size(), x.size(), x) == 0;
}
std::string nlstr_replace(const std::string& s, const std::string& from,
                          const std::string& to) {
    if (from.empty()) return s;
    std::string r = s;
    size_t p = 0;
    while ((p = r.find(from, p)) != std::string::npos) {
        r.replace(p, from.size(), to);
        p += to.size();
    }
    return r;
}
std::string nlstr_repeat(const std::string& s, long long n) {
    std::string r;
    for (long long i = 0; i < n; ++i) r += s;
    return r;
}
long long nlstr_indexOf(const std::string& s, const std::string& sub,
                        long long from) {
    if (from < 0) from = 0;
    size_t p = s.find(sub, (size_t)from);
    return p == std::string::npos ? -1 : (long long)p;
}
long long nlstr_toInt(const std::string& s) {
    try { return std::stoll(nlstr_trim(s)); }
    catch (...) { return 0; }
}
double nlstr_toDouble(const std::string& s) {
    try { return std::stod(nlstr_trim(s)); }
    catch (...) { return 0.0; }
}
std::string nlstr_substr(const std::string& s, long long pos,
                         long long len) {
    if (pos < 0) pos = 0;
    if (pos >= (long long)s.size()) return "";
    size_t p = (size_t)pos;
    size_t n = len < 0 || (size_t)len > s.size() - p
               ? s.size() - p : (size_t)len;
    return s.substr(p, n);
}
std::string nlstr_at(const std::string& s, long long i) {
    if (i < 0 || i >= (long long)s.size()) return "";
    return std::string(1, s[(size_t)i]);
}
