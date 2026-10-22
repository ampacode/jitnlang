// StringExpression::cpp — ${}-интерполяция (порт string.py cppExpression)
#include "string.h"
#include "../lexer.h"
#include "../parser/nparse.h"

Emitted StringExpression::cpp(Nvm& vm) {
    const std::string& v = value;
    if (v.find("${") == std::string::npos)
        return Emitted("\"" + esc(v) + "\"");

    // "a ${expr} b" -> ("a " + nlStr(expr) + " b")
    std::vector<std::string> segs;
    size_t pos = 0;
    for (;;) {
        size_t p = v.find("${", pos);
        if (p == std::string::npos) {
            if (pos < v.size()) segs.push_back("\"" + esc(v.substr(pos)) + "\"");
            break;
        }
        if (p > pos) segs.push_back("\"" + esc(v.substr(pos, p - pos)) + "\"");
        int depth = 1;
        size_t i = p + 2;
        for (; i < v.size() && depth; ++i) {
            if (v[i] == '{') ++depth;
            else if (v[i] == '}') --depth;
        }
        std::string code = v.substr(p + 2, i - 1 - (p + 2));
        Lexer lx;
        NParser sub;
        auto toks = lx.tokenize(code, false);
        NodePtr e = sub.expressionFrom(toks);
        segs.push_back("nlStr((" + e->cpp(vm).getCpp() + "))");
        pos = i;
    }
    if (segs.empty()) segs.push_back("\"\"");
    std::string out = "(";
    for (size_t i = 0; i < segs.size(); ++i) {
        if (i) out += " + ";
        out += segs[i];
    }
    return Emitted(out + ")");
}
