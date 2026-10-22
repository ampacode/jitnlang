#include "nlm_regex.h"
#include <regex>

bool nlregex_test(const std::string& pat) {
    try { std::regex r(pat); (void)r; return true; }
    catch (const std::regex_error&) { return false; }
}
bool nlregex_match(const std::string& s, const std::string& pat) {
    try { return std::regex_match(s, std::regex(pat)); }
    catch (const std::regex_error&) { return false; }
}
bool nlregex_search(const std::string& s, const std::string& pat) {
    try { return std::regex_search(s, std::regex(pat)); }
    catch (const std::regex_error&) { return false; }
}
std::string nlregex_group(const std::string& s,
                          const std::string& pat, long long i) {
    try {
        std::smatch m;
        if (std::regex_search(s, m, std::regex(pat)) &&
            i >= 0 && (size_t)i < m.size())
            return m[(size_t)i].str();
    } catch (const std::regex_error&) {}
    return std::string();
}
std::vector<std::string> nlregex_groups(const std::string& s,
                                        const std::string& pat) {
    std::vector<std::string> out;
    try {
        std::smatch m;
        if (std::regex_search(s, m, std::regex(pat)))
            for (size_t i = 0; i < m.size(); ++i)
                out.push_back(m[i].str());
    } catch (const std::regex_error&) {}
    return out;
}
std::vector<std::string> nlregex_findAll(const std::string& s,
                                         const std::string& pat) {
    std::vector<std::string> out;
    try {
        std::regex r(pat);
        for (std::sregex_iterator it(s.begin(), s.end(), r), e;
             it != e; ++it)
            out.push_back(it->str());
    } catch (const std::regex_error&) {}
    return out;
}
std::string nlregex_replace(const std::string& s,
                            const std::string& pat,
                            const std::string& rep) {
    try { return std::regex_replace(s, std::regex(pat), rep); }
    catch (const std::regex_error&) { return s; }
}
std::vector<std::string> nlregex_split(const std::string& s,
                                       const std::string& pat) {
    std::vector<std::string> out;
    try {
        std::regex r(pat);
        std::sregex_token_iterator it(s.begin(), s.end(), r, -1), e;
        for (; it != e; ++it) out.push_back(it->str());
    } catch (const std::regex_error&) {}
    return out;
}
