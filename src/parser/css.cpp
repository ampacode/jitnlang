// Порт css-грамматики из nparse.py: css_block/palette_block/css_rule.
//   css site { int gap = 16; ampa.card { padding = gap px; } }
#include "nparse.h"
#include "../statements/css.h"
#include "../expressions/vc.h"

// части селектора -> текст; рецепторы после , > + ~ (и в начале)
// превращаются в html-теги; (id) -> #id
static std::string css_selector_text(std::vector<std::string> parts) {
    std::string prev = ",";
    for (auto& p : parts) {
        if (vc_tags().count(p) &&
            (prev == "," || prev == ">" || prev == "+" ||
             prev == "~"))
            p = vc_tags().at(p);
        prev = p;
    }
    std::string text;
    for (auto& p : parts) text += p;
    if (text.size() >= 2 && text.front() == '(' && text.back() == ')')
        return "#" + text.substr(1, text.size() - 2);
    return text;
}

// токены до голого токена stop (для $(.. |> ..))
static std::vector<Token> css_value_tokens_until(NParser& P, int stop) {
    std::vector<Token> toks;
    while (!P.listLook({stop})) {
        const Token* t = P.get(0);
        if (!t) P.fail("css: неожиданный конец");
        toks.push_back(*t);
        P.next();
    }
    return toks;
}

// токены значения до ';'
static std::vector<Token> css_value_tokens(NParser& P) {
    std::vector<Token> toks;
    while (!P.match(DOTCOMMA)) {
        const Token* t = P.get(0);
        if (!t) P.fail("css: неожиданный конец");
        toks.push_back(*t);
        P.next();
    }
    return toks;
}

// мера внутри [a..b]: текст до DOT/DOTDOT/RQB
static std::string css_measure(NParser& P) {
    std::string text;
    while (!P.listLook({DOT, DOTDOT, RQB})) {
        const Token* t = P.get(0);
        if (!t) P.fail("css: неожиданный конец в [..]");
        text += t->text;
        P.next();
    }
    return text;
}

std::shared_ptr<CssNode> css_rule(NParser& P) {
    // import "url";
    if (P.look(VARIABLE) && P.get(0)->text == "import") {
        P.next();
        std::string url = P.consume(STRING).text;
        P.consume(DOTCOMMA);
        auto im = std::make_shared<CssImport>();
        im->url = url;
        im->is_file = url.size() >= 4 &&
            url.substr(url.size() - 4) == ".css" &&
            url.rfind("http://", 0) != 0 &&
            url.rfind("https://", 0) != 0;
        return im;
    }
    // raw { ... }
    if (P.look(VARIABLE) && P.get(0)->text == "raw") {
        P.next();
        P.consume(LBRACE);
        int depth = 1;
        std::string text;
        while (P.position < P.size()) {
            const Token* t = P.get(0);
            if (t->type == LBRACE) { ++depth; text += t->text; P.next(); }
            else if (t->type == RBRACE) {
                if (--depth == 0) { P.next(); break; }
                text += t->text; P.next();
            } else { text += t->text; P.next(); }
        }
        auto r = std::make_shared<CssRaw>();
        r->text = text;
        return r;
    }
    // $(expr |> prop)
    if (P.listLook({DOLLAR, LPAR})) {
        P.next(); P.next();
        auto toks = css_value_tokens_until(P, BAR);
        P.consume(BAR);
        P.consume(GT);
        std::string prop;
        while (!P.match(RPAR)) {
            const Token* t = P.get(0);
            if (!t) P.fail("css: незакрытый $(...|>)");
            prop += t->text;
            P.next();
        }
        auto b = std::make_shared<CssBinding>();
        b->prop = prop;
        b->toks = std::move(toks);
        return b;
    }
    // &keyframes name { ... }
    if (P.look(AMP) && P.get(1) && P.get(1)->text == "keyframes") {
        P.next(); P.next();
        std::string name;
        while (!P.match(LBRACE)) {
            name += P.get(0)->text;
            P.next();
        }
        auto k = std::make_shared<CssKeyframes>();
        k->name = name;
        while (!P.match(RBRACE)) k->children.push_back(css_rule(P));
        return k;
    }
    // &media (cond) { ... }
    if (P.look(AMP) && P.get(1) && P.get(1)->text == "media" &&
        P.get(2) && P.get(2)->type == LPAR) {
        P.next(); P.next();
        P.consume(LPAR);
        std::string cond;
        while (!P.match(RPAR)) { cond += P.get(0)->text; P.next(); }
        P.consume(LBRACE);
        auto m = std::make_shared<CssMediaExpr>();
        m->condition = cond;
        while (!P.match(RBRACE)) m->children.push_back(css_rule(P));
        return m;
    }
    // &screen [a .. b] { ... }
    if (P.look(AMP) && P.get(1) && P.get(1)->text == "screen") {
        P.next(); P.next();
        P.consume(LQB);
        std::string low = css_measure(P);
        P.consume(DOT); P.consume(DOT);
        std::string high = css_measure(P);
        P.consume(RQB);
        P.consume(LBRACE);
        auto m = std::make_shared<CssMedia>();
        m->low = low; m->high = high;
        while (!P.match(RBRACE)) m->children.push_back(css_rule(P));
        return m;
    }

    // селектор { } или prop = значение;
    std::vector<std::string> parts;
    int brackets = 0;
    while (true) {
        const Token* t = P.get(0);
        if (!t) P.fail("css: неожиданный конец");
        if (t->type == LBRACE || t->type == RBRACE) break;
        if (t->type == EQUAL && brackets == 0) break;
        if (t->type == LQB) ++brackets;
        else if (t->type == RQB) --brackets;
        parts.push_back(t->text);
        P.next();
    }
    if (P.match(EQUAL)) {
        if (parts.size() >= 2 &&
            (parts[0] == "int" || parts[0] == "float" ||
             parts[0] == "double" || parts[0] == "long" ||
             parts[0] == "short")) {
            auto d = std::make_shared<CssVarDecl>();
            d->name = parts[1];
            d->toks = css_value_tokens(P);
            return d;
        }
        auto pr = std::make_shared<CssProperty>();
        for (auto& p : parts) pr->name += p;
        pr->toks = css_value_tokens(P);
        return pr;
    }
    P.consume(LBRACE);
    auto s = std::make_shared<CssSelector>();
    s->selector = css_selector_text(parts);
    while (!P.match(RBRACE)) s->children.push_back(css_rule(P));
    return s;
}

NodePtr NParser::css_block() {
    // palette Name { rec ~ side = value; }
    if (look(VARIABLE) && get(0)->text == "palette") {
        next();
        auto p = std::make_shared<PaletteStatement>();
        p->name = consume(VARIABLE).text;
        consume(LBRACE);
        while (!match(RBRACE)) {
            std::string receptor = get(0)->text;
            next();
            std::string side = "client";
            if (get(0) && (get(0)->type == WAVE ||
                           get(0)->type == TILDE)) {
                next();
                side = get(0)->text;
                next();
            }
            consume(EQUAL);
            p->entries[receptor][side] = css_value_tokens(*this);
        }
        return p;
    }
    // css Name { ... }
    if (look(VARIABLE) && get(0)->text == "css") {
        next();
        auto st = std::make_shared<CssStatement>();
        st->name = consume(VARIABLE).text;
        consume(LBRACE);
        while (!match(RBRACE))
            st->nodes.push_back(css_rule(*this));
        return st;
    }
    return using_directive();
}
