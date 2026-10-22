#pragma once
// Порт expressions/number.py
#include "../node.h"
#include "../nvm.h"

class NumberExpression : public Node {
public:
    void* asNum() override { return this; }
    long long ivalue = 0;
    double dvalue = 0.0;
    bool is_float = false;

    explicit NumberExpression(const std::string& value) {
        std::string v = value;
        std::string low = v;
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        if (low.rfind("0x", 0) == 0) {
            ivalue = std::stoll(v, nullptr, 16);
        } else if (v.find('.') != std::string::npos ||
                   v.find('e') != std::string::npos ||
                   v.find('E') != std::string::npos) {
            is_float = true;
            dvalue = std::stod(v);
        } else {
            ivalue = std::stoll(v);
        }
    }

    Emitted cpp(Nvm&) override {
        return Emitted(is_float ? fmt(dvalue) : std::to_string(ivalue));
    }
    Emitted javascript(Nvm&) override {
        return Emitted(is_float ? fmt(dvalue) : std::to_string(ivalue));
    }
    NObject eval(Nvm&) override {
        return is_float ? NObject(dvalue) : NObject(ivalue);
    }

    static std::string fmt(double d) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.17g", d);
        // C++-литерал double: всегда с точкой или экспонентой
        std::string s = buf;
        if (s.find('.') == std::string::npos &&
            s.find('e') == std::string::npos) s += ".0";
        return s;
    }
};
