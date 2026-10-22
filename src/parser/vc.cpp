// Порт vc-разметки из nparse.py (ta_representation).
//   <ampa [cls1 cls2] (id) {k=v, k2=$(expr)}> дети </gabaa>
// Дети: вложенные vc, "строки", $(expr), выражения-primary,
// statements (eval-only, для побочных эффектов).
#include "nparse.h"
#include "../expressions/vc.h"
#include "../expressions/string.h"
#include "../expressions/variable.h"

// старт vc-ноды: < receptor
static bool is_vc_open(NParser& P) {
    return P.look(LT) && P.look(VARIABLE, 1) &&
           vc_is_receptor(P.get(1)->text);
}

// порт is_vc_statement_start: внутри vc statement —
// то, что не начинается как контент (< / " / $ / primary)
static bool is_vc_statement_start(NParser& P) {
    const Token* t = P.get(0);
    if (!t) return false;
    switch (t->type) {
        case IF: case FOR: case WHILE: case CLASS:
        case RETURN: case BREAK: case CONTINUE:
            return true;
        case VARIABLE: {
            const Token* nxt = P.get(1);
            if (!nxt) return false;
            if (nxt->type == STRING || nxt->type == LT ||
                nxt->type == DOLLAR)
                return false;
            return true;
        }
        case NUM: case LPAR: case EXCL: case MINUS: case PLUS:
            return true;
        default:
            return false;
    }
}

// конфигурация: [classes] (id) {k = v, ...}
static void vc_config(NParser& P, VcExpression* vc) {
    // [cls1 cls2 ...] — классы; '-' склеивает (greeting-form)
    if (P.match(LQB)) {
        std::string cur;
        auto flush = [&] {
            if (!cur.empty()) { vc->classes.push_back(cur); cur.clear(); }
        };
        while (!P.match(RQB)) {
            const Token* t = P.get(0);
            if (!t) P.fail("vc: незакрытый [");
            if (t->type == MINUS) { cur += "-"; P.next(); continue; }
            if (t->type == VARIABLE) {
                if (!cur.empty() && cur.back() == '-')
                    cur += t->text;
                else { flush(); cur = t->text; }
                P.next(); continue;
            }
            P.next();
        }
        flush();
    }
    // (id) — идентификатор
    if (P.look(LPAR) && P.look(VARIABLE, 1) && P.look(RPAR, 2)) {
        P.next();
        vc->id = P.consume(VARIABLE).text;
        P.consume(RPAR);
    }
    // {k = v, k2 = $(expr), bare}
    if (P.match(LBRACE)) {
        while (!P.match(RBRACE)) {
            const Token* t = P.get(0);
            if (!t) P.fail("vc: незакрытый {");
            if (t->type == COMMA) { P.next(); continue; }
            std::string name = P.consume(VARIABLE).text;
            NodePtr val;
            if (P.match(EQUAL)) {
                if (P.match(DOLLAR)) {
                    P.match(LPAR);
                    val = P.expression();
                    P.match(RPAR);
                } else if (P.look(STRING)) {
                    val = std::make_shared<StringExpression>(
                        P.consume(STRING).text);
                } else if (P.look(NUM)) {
                    // число как строковый литерал
                    val = std::make_shared<StringExpression>(
                        P.consume(NUM).text);
                } else {
                    val = P.expression();
                }
            }
            vc->attrs.push_back({name, val});
        }
    }
}

NodePtr NParser::vc_expression() {
    consume(LT);
    std::string receptor = consume(VARIABLE).text;
    auto vc = std::make_shared<VcExpression>(receptor);
    vc_config(*this, vc.get());
    consume(GT);
    vc->resolve_tag();

    while (true) {
        const Token* t = get(0);
        if (!t) fail("vc: незакрытый <" + receptor + ">");
        // закрывающий тег
        if (t->type == LT && look(SLASH, 1)) break;
        // вложенный vc
        if (is_vc_open(*this)) {
            vc->children.push_back(vc_expression());
            continue;
        }
        // $(expr)
        if (t->type == DOLLAR) {
            next();
            match(LPAR);
            vc->children.push_back(
                std::make_shared<VcOutputExpression>(expression()));
            match(RPAR);
            continue;
        }
        // "строка"
        if (t->type == STRING) {
            vc->children.push_back(std::make_shared<VcTextExpression>(
                std::dynamic_pointer_cast<StringExpression>(
                    std::make_shared<StringExpression>(
                        consume(STRING).text))->value));
            continue;
        }
        // statement внутри vc
        if (is_vc_statement_start(*this)) {
            vc->children.push_back(statement());
            _optional_semi();
            continue;
        }
        // прочее выражение — выводимое значение
        NodePtr e = primary();
        if (!e) break;
        vc->children.push_back(
            std::make_shared<VcOutputExpression>(e));
    }

    // </close>
    consume(LT);
    consume(SLASH);
    std::string close = consume(VARIABLE).text;
    if (!vc_close_ok(receptor, close))
        fail("vc: <" + receptor + "> закрыт </" + close + ">");
    consume(GT);
    return vc;
}
