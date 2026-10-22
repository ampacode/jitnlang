#pragma once
// Рекурсивный JSON-парсер в header'е — нужен и сгенерированным
// hot-swap dll (они не линкуются к libnlmodules), и модулям.
// Функции nljs_parse/nljs_export; nlm_json.cpp — тонкие обёртки.
#include "nlrt.h"
#include <cctype>
#include <stdexcept>

namespace nljs_detail {

struct Parser {
    const std::string& s;
    size_t pos = 0;

    [[noreturn]] void fail(const std::string& what) {
        throw std::runtime_error("json: " + what +
                                 " (позиция " + std::to_string(pos) + ")");
    }
    void skipSpaces() {
        while (pos < s.size() &&
               (s[pos] == ' ' || s[pos] == '\t' ||
                s[pos] == '\n' || s[pos] == '\r'))
            ++pos;
    }
    char peek() {
        skipSpaces();
        return pos < s.size() ? s[pos] : '\0';
    }
    void expect(char c) {
        if (peek() != c) fail(std::string("ожидался '").append(1, c) + "'");
        ++pos;
    }

    JValue parseValue() {
        char c = peek();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return JValue(parseString());
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
        if (s.compare(pos, 4, "true") == 0) { pos += 4; return JValue(true); }
        if (s.compare(pos, 5, "false") == 0) { pos += 5; return JValue(false); }
        if (s.compare(pos, 4, "null") == 0) { pos += 4; return JValue(); }
        fail("неожиданный символ");
    }
    JValue parseObject() {
        JValue::Object obj;
        expect('{');
        if (peek() == '}') { ++pos; return JValue(std::move(obj)); }
        for (;;) {
            std::string key = parseString();
            expect(':');
            obj[key] = parseValue();
            char c = peek();
            if (c == ',') { ++pos; continue; }
            if (c == '}') { ++pos; break; }
            fail("ожидалась ',' или '}'");
        }
        return JValue(std::move(obj));
    }
    JValue parseArray() {
        JValue::Array arr;
        expect('[');
        if (peek() == ']') { ++pos; return JValue(std::move(arr)); }
        for (;;) {
            arr.push_back(parseValue());
            char c = peek();
            if (c == ',') { ++pos; continue; }
            if (c == ']') { ++pos; break; }
            fail("ожидалась ',' или ']'");
        }
        return JValue(std::move(arr));
    }
    std::string parseString() {
        expect('"');
        std::string out;
        while (pos < s.size()) {
            char c = s[pos++];
            if (c == '"') return out;
            if (c == '\\' && pos < s.size()) {
                char e = s[pos++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': { // \uXXXX -> UTF-8
                        if (pos + 4 > s.size()) fail("обрыв \\u");
                        unsigned cp = (unsigned)std::stoi(
                            s.substr(pos, 4), nullptr, 16);
                        pos += 4;
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 63));
                        } else {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 63));
                            out += (char)(0x80 | (cp & 63));
                        }
                        break;
                    }
                    default: out += e;
                }
            } else out += c;
        }
        fail("незакрытая строка");
    }
    JValue parseNumber() {
        size_t start = pos;
        if (s[pos] == '-') ++pos;
        while (pos < s.size() &&
               (isdigit((unsigned char)s[pos]) || s[pos] == '.' ||
                s[pos] == 'e' || s[pos] == 'E' ||
                s[pos] == '+' || s[pos] == '-'))
            ++pos;
        std::string num = s.substr(start, pos - start);
        if (num.find('.') != std::string::npos ||
            num.find('e') != std::string::npos ||
            num.find('E') != std::string::npos)
            return JValue(std::stod(num));
        return JValue(std::stoll(num));
    }
};

inline std::string escape(const std::string& s) {
    std::string r;
    for (char c : s) {
        switch (c) {
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default: r += c;
        }
    }
    return r;
}

inline void exportValue(const JValue& v, std::string& out,
                        int indent, int depth) {
    auto pad = [&](int d) {
        if (indent > 0) out += std::string(indent * d, ' ');
    };
    if (v.isObject()) {
        out += "{";
        bool first = true;
        for (auto& kv : std::get<JValue::Object>(v.v)) {
            if (!first) out += ",";
            first = false;
            if (indent > 0) out += "\n";
            pad(depth + 1);
            out += "\"" + escape(kv.first) + "\":";
            if (indent > 0) out += " ";
            exportValue(kv.second, out, indent, depth + 1);
        }
        if (indent > 0 && !std::get<JValue::Object>(v.v).empty()) {
            out += "\n";
            pad(depth);
        }
        out += "}";
        return;
    }
    if (v.isArray()) {
        out += "[";
        bool first = true;
        for (auto& e : std::get<JValue::Array>(v.v)) {
            if (!first) out += ",";
            first = false;
            if (indent > 0) out += "\n";
            pad(depth + 1);
            exportValue(e, out, indent, depth + 1);
        }
        if (indent > 0 && !std::get<JValue::Array>(v.v).empty()) {
            out += "\n";
            pad(depth);
        }
        out += "]";
        return;
    }
    if (std::holds_alternative<std::string>(v.v)) {
        out += "\"" + escape(std::get<std::string>(v.v)) + "\"";
        return;
    }
    if (std::holds_alternative<std::monostate>(v.v)) {
        out += "null";
        return;
    }
    out += v.asString();
}

} // namespace nljs_detail

inline JValue nljs_parse(const std::string& text) {
    nljs_detail::Parser p{text};
    JValue v = p.parseValue();
    p.skipSpaces();
    if (p.pos != text.size())
        p.fail("лишние символы после конца JSON");
    return v;
}
inline std::string nljs_export(const JValue& value) {
    std::string out;
    nljs_detail::exportValue(value, out, 0, 0);
    return out;
}
inline std::string nljs_exportPretty(const JValue& value,
                                     long long indent) {
    std::string out;
    nljs_detail::exportValue(value, out, (int)indent, 0);
    return out;
}
