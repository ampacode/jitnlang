#pragma once
// Порт expressions/string.py — escape-декодирование + ${}-интерполяция
#include "../node.h"
#include "../nvm.h"
#include "../jsgen.h"

class StringExpression : public Node {
public:
    void* asStrExpr() override { return this; }
    std::string value;

    explicit StringExpression(const std::string& s) {
        // \xNN сначала, потом остальные escapes (как в py)
        std::string out;
        for (size_t i = 0; i < s.size();) {
            if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] == 'x' &&
                isxdigit((unsigned char)s[i + 2]) &&
                isxdigit((unsigned char)s[i + 3])) {
                out += (char)std::stoi(s.substr(i + 2, 2), nullptr, 16);
                i += 4;
            } else {
                out += s[i++];
            }
        }
        std::string r;
        for (size_t i = 0; i < out.size();) {
            if (out[i] == '\\' && i + 1 < out.size()) {
                char e = out[i + 1];
                if (e == '\\') { r += '\x01'; i += 2; continue; }
                if (e == 'r') { r += '\r'; i += 2; continue; }
                if (e == 't') { r += '\t'; i += 2; continue; }
                if (e == 'n') { r += '\n'; i += 2; continue; }
                if (e == 's') { r += '\x20'; i += 2; continue; }
            }
            r += out[i++];
        }
        for (auto& c : r) if (c == '\x01') c = '\\';
        value = std::move(r);
    }

    NObject eval(Nvm&) override { return NObject(value); }
    NObject antieval(Nvm&) override { return NObject(value); }

    static std::string esc(const std::string& s) {
        std::string r;
        for (char c : s) {
            switch (c) {
                case '\\': r += "\\\\"; break;
                case '"': r += "\\\""; break;
                case '\n': r += "\\n"; break;
                case '\r': r += "\\r"; break;
                case '\t': r += "\\t"; break;
                default: r += c;
            }
        }
        return r;
    }

    Emitted cpp(Nvm& vm) override;
    Emitted javascript(Nvm&) override {
        return Emitted(js::str_lit(value));
    }
};
