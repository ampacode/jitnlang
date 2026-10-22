// helpers NParser — match/look/get/consume/_optional_semi
// (порт методов из конца nparse.py)
#include "nparse.h"
#include "../expressions/variable.h"
#include "../statements/block.h"
#include "../statements/conditional.h"
#include "../statements/_while.h"
#include "../statements/_for.h"
#include "../statements/foreach.h"
#include "../statements/_watch.h"
#include "../statements/function.h"

bool NParser::match(int type, int rel) {
    const Token* c = get(rel);
    if (!c || c->type != type) return false;
    next();
    return true;
}
bool NParser::look(int type, int rel) const {
    const Token* c = get(rel);
    return c && c->type == type;
}
const Token* NParser::get(int rel) const {
    long long p = (long long)position + rel;
    return p >= 0 && (size_t)p < tokens.size() ? &tokens[p] : nullptr;
}
const Token& NParser::consume(int type, int rel) {
    const Token* c = get(rel);
    if (!c || c->type != type)
        fail(std::string("ожидался токен ") + std::to_string(type) +
             ", получено '" + (c ? c->text : "<eof>") + "'");
    next();
    return *c;
}
bool NParser::listLook(std::initializer_list<int> types, int rel) {
    int i = 0;
    for (int t : types) {
        if (!look(t, rel + i)) return false;
        ++i;
    }
    return true;
}
bool NParser::skipLastDotcomma() {
    if (skipLastDotcommaVar) {
        skipLastDotcommaVar = false;
        return true;
    }
    return false;
}
void NParser::fail(const std::string& msg) {
    const Token* c = get(0);
    throw ParseError{msg + " ('" + (c ? c->text : "<eof>") + "')",
                     c ? c->line : 0, c ? c->col : 0};
}

void NParser::_optional_semi() {
    if (match(DOTCOMMA)) return;
    const Token* nxt = get(0);
    if (!nxt || nxt->type == RBRACE) return;
    const Token& last = tokens[position - 1];
    if (nxt->line > last.line) return;
    consume(DOTCOMMA); // та же строка — ; обязательна
}
