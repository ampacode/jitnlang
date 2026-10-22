// Порт nparse.py — statement-цепочка + program + block
#include "nparse.h"
#include "../expressions/vc.h"
#include "../statements/assignment.h"
#include "../statements/block.h"
#include "../statements/conditional.h"
#include "../statements/_while.h"
#include "../statements/_for.h"
#include "../statements/foreach.h"
#include "../statements/_watch.h"
#include "../statements/_class.h"
#include "../statements/_return.h"
#include "../statements/using.h"
#include "../statements/function.h"
#include "../statements/wavelen.h"
#include "../statements/model.h"
#include "../expressions/getter.h"
#include "../expressions/variable.h"
#include "../expressions/binary.h"

std::vector<NodePtr> NParser::program(const std::vector<Token>& toks) {
    tokens = toks;
    position = 0;
    std::vector<NodePtr> statements;
    while (position < size() && !look(END)) {
        // проставляем стейтменту номер строки — нужно
        // трассировке (qt.execTrace) и будущим ошибкам
        int stmt_line = get(0) ? get(0)->line : 0;
        NodePtr st = statement();
        if (st) { st->line = stmt_line; statements.push_back(st); }
        skipLastDotcommaVar = false;
    }
    return statements;
}

NodePtr NParser::statement() {
    if (look(DOTCOMMA)) {
        if (allow_braces) { // .nlb — C-стиль: ';' как разделитель
            next();
            if (!get(0) || look(DOTCOMMA) || look(RBRACE))
                return nullptr;
            return statement();
        }
        fail("';' запрещена — операторы разделяются переводом строки");
    }
    return scheming();
}

NodePtr NParser::scheming() {
    // $-throught — позже; css/palette — в css.cpp
    return css_block();
}

NodePtr NParser::using_directive() {
    // wavelen fn 16 | wavelen on.pulse 60hz — периодика ядра:
    // привязка функции к таймеру (читается рантаймом)
    if (look(VARIABLE) && get(0)->text == "wavelen") {
        next();
        std::string fn = consume(VARIABLE).text;
        if (match(DOT)) fn += "." + consume(VARIABLE).text;
        int ms = 16;
        if (look(NUM)) {
            double period = std::stod(get(0)->text);
            next();
            std::string unit;
            if (look(VARIABLE) &&
                (get(0)->text == "hz" || get(0)->text == "ms" ||
                 get(0)->text == "Hz")) {
                unit = get(0)->text;
                next();
            }
            ms = (unit == "hz" || unit == "Hz")
                 ? (int)(1000.0 / period) : (int)period;
        }
        return std::make_shared<WavelenStatement>(fn, ms);
    }
    // vc-блок как statement: <ampa>...</gabaa>
    if (look(LT) && look(VARIABLE, 1) &&
        vc_is_receptor(get(1)->text))
        return vc_expression();
    if (match(USING)) {
        if (match(LT)) {
            skipLastDotcommaVar = true;
            std::string name;
            while (!match(GT)) {
                const Token* t = get(0);
                if (!t) fail("using: незакрытый <");
                name += t->text;
                next();
            }
            // .nlc/.nlb — nlang-исходники (инклудятся/перезагружаются);
            // .nls — стили (vc-путь, пока пропускаем)
            if (name.find(".nlc") != std::string::npos ||
                name.find(".nlb") != std::string::npos)
                return std::make_shared<NlangUsingStatement>(name);
            return std::make_shared<UsingStatement>(name);
        }
        if (match(NAMESPACE)) {
            std::string ns = get(0)->text;
            next();
            return std::make_shared<NamespaceStatement>(ns);
        }
    }
    // model X / database db {...} — ORM-декларации
    if (look(VARIABLE) && (get(0)->text == "model" ||
                           get(0)->text == "database")) {
        std::string kw = get(0)->text;
        next();
        std::string dname = consume(VARIABLE).text;
        if (kw == "model") {
            // model User\n    type field; — блок отступами,
            // ';' внутри допустима
            consume(LBRACE);
            std::vector<std::pair<std::string, std::string>> fields;
            while (!match(RBRACE)) {
                if (match(DOTCOMMA)) continue;
                std::string t = consume(VARIABLE).text;
                std::string f = consume(VARIABLE).text;
                match(DOTCOMMA);
                fields.push_back({
                    ModelStatement::cpp_type(t), f});
            }
            return std::make_shared<ModelStatement>(dname,
                                                    std::move(fields));
        }
        // database name { key = "value"; ... } — явные {}
        consume(LBRACE);
        std::map<std::string, NodePtr> entries;
        while (!match(RBRACE)) {
            if (match(DOTCOMMA) || match(COMMA)) continue;
            std::string key = consume(VARIABLE).text;
            consume(EQUAL);
            entries[key] = expression();
            match(DOTCOMMA);
        }
        match(DOTCOMMA); // `};` в py-синтаксисе
        return std::make_shared<DatabaseStatement>(dname,
                                                   std::move(entries));
    }
    return funcdecl(); // elocation/css — позже
}

// тип с шаблонными аргументами и &-квалификаторами:
//   int / vector<int> / list<map> / Foo&
static bool is_funcdecl_start(NParser& P) {
    // Type<args>? name ( — декларация функции
    if (!P.look(VARIABLE)) return false;
    // точечное имя: anti on.pulse( — type name.name(
    if (P.look(VARIABLE, 1) && P.look(DOT, 2) &&
        P.look(VARIABLE, 3))
        return P.look(LPAR, 4);
    // многословный тип: long long f( / unsigned int f(
    if (P.look(VARIABLE, 1) && P.look(VARIABLE, 2)) {
        const std::string& t0 = P.get(0)->text;
        if (t0 == "long" || t0 == "unsigned" || t0 == "short" ||
            t0 == "signed" || t0 == "struct")
            return P.look(LPAR, 3);
    }
    if (P.look(VARIABLE, 1)) return P.look(LPAR, 2);
    if (P.look(LT, 1)) {
        size_t j = P.position + 2;
        int depth = 1;
        while (j < P.size() && depth > 0) {
            int t = P.tokens[j].type;
            if (t == LT) ++depth;
            else if (t == GT) --depth;
            ++j;
        }
        return j + 1 < P.size() && P.tokens[j].type == VARIABLE &&
               P.tokens[j + 1].type == LPAR;
    }
    return false;
}

NodePtr NParser::funcdecl() {
    if (is_funcdecl_start(*this)) {
        // int f(int n) { — FunctionDeclaration: после ')' идёт '{'
        // иначе это может быть вызов с двумя именами? — как в py,
        // VARIABLE VARIABLE LPAR всегда декларация
        // тип с шаблонными параметрами: vector<int>/list<...>
        auto parse_type = [&]() -> std::string {
            std::string t = get(0)->text;
            next();
            if (look(LT)) {
                int depth = 0;
                while (position < size()) {
                    const Token* tk = get(0);
                    t += tk->text;
                    next();
                    if (tk->type == LT) ++depth;
                    else if (tk->type == GT && --depth == 0) break;
                }
            }
            return t;
        };
        std::string typeofret = parse_type();
        // многословный тип: long long fibo(...) — 'long long' как тип
        if (look(VARIABLE) && look(VARIABLE, 1) && look(LPAR, 2) &&
            (typeofret == "long" || typeofret == "unsigned" ||
             typeofret == "short" || typeofret == "signed" ||
             typeofret == "struct")) {
            typeofret += " " + get(0)->text;
            next();
        }
        std::string funcname = consume(VARIABLE).text;
        // точечное имя обработчика: anti on.pulse(...)
        if (match(DOT))
            funcname += "." + consume(VARIABLE).text;
        consume(LPAR);
        std::vector<std::pair<std::string,
            std::pair<std::string, NodePtr>>> args;
        while (!match(RPAR)) {
            std::string type, name;
            // type name / type& name / vector<T> name —
            // или одиночное слово = anti-параметр
            if (look(VARIABLE) &&
                (look(VARIABLE, 1) || look(AMP, 1) ||
                 look(STAR, 1) || look(COLON, 1) || look(LT, 1))) {
                type = parse_type();
                // многословный тип: long long x
                while (look(VARIABLE) && look(VARIABLE, 1) &&
                       (type == "long" || type == "unsigned" ||
                        type == "short" || type == "signed")) {
                    type += " " + get(0)->text;
                    next();
                }
                while (look(AMP) || look(STAR) || look(COLON)) {
                    type += get(0)->text;
                    next();
                }
                name = consume(VARIABLE).text;
            } else {
                type = "anti";
                name = consume(VARIABLE).text;
            }
            NodePtr base;
            if (match(EQUAL)) base = expression();
            args.push_back({name, {type, base}});
            match(COMMA);
        }
        auto stmts = block();
        skipLastDotcommaVar = true;
        auto f = std::make_shared<FunctionDeclaration>(
            typeofret, funcname, args, stmts);
        f->line = typeofret.empty() ? 0 : get(-1) ? get(-1)->line : 0;
        return f;
    }
    return classdecl();
}

NodePtr NParser::classdecl() {
    if (!look(CLASS)) return startsvariable();
    next();
    std::string cname = consume(VARIABLE).text;
    consume(LBRACE);
    std::vector<ClassMember> members;
    while (!match(RBRACE)) {
        // access-модификатор (необязателен, default public)
        std::string access = "public";
        if (look(VARIABLE) &&
            (get(0)->text == "public" || get(0)->text == "private" ||
             get(0)->text == "protected")) {
            access = get(0)->text;
            next();
        }
        bool is_static = look(VARIABLE) && get(0)->text == "static";
        if (is_static) next();

        auto ctor_typed_args = [&]() {
            consume(LPAR);
            auto a = _typed_args();
            return a;
        };

        // конструктор: Name(args)
        if (look(VARIABLE) && get(0)->text == cname && look(LPAR, 1)) {
            next();
            auto args = ctor_typed_args();
            auto st = block();
            auto m = std::make_shared<FunctionDeclaration>(
                "", cname, args, st);
            m->is_ctor = true;
            m->is_static = is_static;
            members.push_back({access, m});
        }
        // метод: type name(args)
        else if (look(VARIABLE) && look(VARIABLE, 1) && look(LPAR, 2)) {
            std::string type = get(0)->text;
            next();
            std::string mname = consume(VARIABLE).text;
            auto args = ctor_typed_args();
            auto st = block();
            auto m = std::make_shared<FunctionDeclaration>(
                type, mname, args, st);
            m->is_static = is_static;
            members.push_back({access, m});
        }
        // поле: type name [= expr]
        else if (look(VARIABLE) && look(VARIABLE, 1)) {
            auto d = std::make_shared<DeclarationStatement>();
            d->type = get(0)->text;
            next();
            d->name = consume(VARIABLE).text;
            if (match(EQUAL)) d->value = expression();
            if (is_static) d->type = "static " + d->type;
            members.push_back({access, d});
        }
        else fail("class: ожидалось поле или метод");
    }
    return std::make_shared<ClassStatement>(cname, members);
}

NodePtr NParser::startsvariable() {
    if ((look(VARIABLE) && look(VARIABLE, 1)) ||
        (look(VARIABLE) && look(LT, 1))) {
        // Декларация: TYPE NAME = expr / list<T> name = expr
        std::string type = get(0)->text;
        next();
        // многословный тип: long long x / unsigned int x / struct S x
        while (look(VARIABLE) && look(VARIABLE, 1) &&
               (type == "long" || type == "unsigned" ||
                type == "short" || type == "signed" ||
                type == "struct")) {
            type += " " + get(0)->text;
            next();
        }
        std::vector<std::string> template_declaration_list;
        if (look(LT)) {
            next();
            int depth = 1;
            std::string cur;
            while (position < size() && depth > 0) {
                const Token* t = get(0);
                next();
                if (t->type == GT) {
                    if (--depth == 0) break;
                    cur += '>';
                } else if (t->type == LT) {
                    ++depth;
                    cur += '<';
                } else if (t->type == COMMA && depth == 1) {
                    template_declaration_list.push_back(cur);
                    cur.clear();
                    // trim
                    if (!template_declaration_list.back().empty() &&
                        template_declaration_list.back().back() == ' ')
                        template_declaration_list.back().pop_back();
                } else {
                    cur += t->text;
                }
            }
            if (!cur.empty()) {
                while (!cur.empty() && cur.back() == ' ') cur.pop_back();
                template_declaration_list.push_back(cur);
            }
        }
        std::string name = consume(VARIABLE).text;

        // C-массив: type name[N]
        NodePtr array_size;
        if (match(LQB)) {
            array_size = expression();
            consume(RQB);
        }
        // инициализатор — только при '='; иначе съедается
        // следующая строка (Empty e -> Empty e = print(...))
        bool has_eq = match(EQUAL);
        NodePtr expr;
        if (has_eq) expr = expression();

        auto d = std::make_shared<DeclarationStatement>();
        d->type = type;
        d->name = name;
        d->value = expr;
        d->template_declaration_list = template_declaration_list;
        d->array_size = array_size;
        return d;
    }
    return return_statement();
}

NodePtr NParser::return_statement() {
    if (match(RETURN)) {
        // голый return — в конце строки/блока/файла
        const Token* nxt = get(0);
        const Token* ret = get(-1);
        if (!nxt || nxt->type == RBRACE ||
            (ret && nxt->line > ret->line))
            return std::make_shared<ReturnStatement>(nullptr);
        return std::make_shared<ReturnStatement>(expression());
    }
    if (look(VARIABLE)) {
        NodePtr g = getter();
        if (match(EQUAL)) {
            return std::make_shared<AssignmentStatement>(g, expression());
        }
        // составные присваивания — как в py: BinaryExpression
        auto compound = [&](int op2, int opid) -> NodePtr {
            if (look(op2) && look(EQUAL, 1)) {
                next(); next();
                return std::make_shared<BinaryExpression>(
                    opid, g, expression());
            }
            return nullptr;
        };
        if (NodePtr r = compound(STAR, STAREQ)) return r;
        if (NodePtr r = compound(SLASH, SLASHEQ)) return r;
        if (NodePtr r = compound(PLUS, PLUSEQ)) return r;
        if (NodePtr r = compound(MINUS, MINUSEQ)) return r;
        if (look(PERCENT) && look(EQUAL, 1)) {
            next(); next();
            return std::make_shared<BinaryExpression>(PERCENTEQ, g, expression());
        }
        if (look(PLUS) && look(PLUS, 1)) {
            next(); next();
            return std::make_shared<BinaryExpression>(INCREMENT, g, nullptr);
        }
        if (look(MINUS) && look(MINUS, 1)) {
            next(); next();
            return std::make_shared<BinaryExpression>(DECREMENT, g, nullptr);
        }
        return g; // выражение как statement
    }
    return conditional_statement();
}

// if/elif (cond) block — общий хвост (elif = else { if })
// скобки условия обязательны
static NodePtr cond_tail(NParser& P) {
    P.consume(LPAR);
    NodePtr cond = P.expression();
    P.consume(RPAR);
    auto ifst = std::make_shared<BlockStatement>(P.block());
    NodePtr elsest;
    if (P.match(ELSE)) {
        if (P.look(IF))
            elsest = std::make_shared<BlockStatement>(
                std::vector<NodePtr>{P.conditional_statement()});
        else
            elsest = std::make_shared<BlockStatement>(P.block());
    } else if (P.match(ELIF)) {
        // elif == else { if } — рекурсивный хвост
        elsest = std::make_shared<BlockStatement>(
            std::vector<NodePtr>{cond_tail(P)});
    }
    return std::make_shared<ConditionalStatement>(cond, ifst, elsest);
}

NodePtr NParser::conditional_statement() {
    if (match(IF)) return cond_tail(*this);
    return continue_statement();
}

NodePtr NParser::continue_statement() {
    if (match(CONTINUE)) return std::make_shared<ContinueStatement>();
    return break_statement();
}
NodePtr NParser::break_statement() {
    if (match(BREAK)) return std::make_shared<BreakStatement>();
    return for_statement();
}

NodePtr NParser::for_statement() {
    if (match(FOR)) {
        consume(LPAR);
        const Token* t0 = get(0);
        const Token* t1 = get(1);
        if (t0 && t0->type == VARIABLE &&
            (t1->type == COMMA || t1->type == COLON)) {
            std::string key, value = consume(VARIABLE).text;
            if (match(COMMA)) {
                key = value;
                value = consume(VARIABLE).text;
            }
            consume(COLON);
            NodePtr expr = getter();
            consume(RPAR);
            auto blk = std::make_shared<BlockStatement>(block());
            return std::make_shared<ForeachStatement>(key, value, expr, blk);
        }
        NodePtr decl = startsvariable();
        consume(DOTCOMMA);
        NodePtr w = expression();
        consume(DOTCOMMA);
        NodePtr d = statement();
        consume(RPAR);
        auto blk = std::make_shared<BlockStatement>(block());
        return std::make_shared<ForStatement>(decl, w, d, blk);
    }
    return watch_statement();
}

NodePtr NParser::watch_statement() {
    if (match(WATCH)) {
        auto body = std::make_shared<BlockStatement>(block());
        auto w = std::make_shared<WatchStatement>();
        w->body = body;
        while (match(REVEAL)) {
            WatchStatement::Catch c;
            if (match(LPAR)) {
                if (look(VARIABLE) && look(VARIABLE, 1)) {
                    c.type = get(0)->text;
                    next();
                    c.var = consume(VARIABLE).text;
                } else {
                    c.var = consume(VARIABLE).text;
                }
                consume(RPAR);
            }
            c.block = std::make_shared<BlockStatement>(block());
            w->catches.push_back(c);
        }
        if (w->catches.empty())
            fail("watch без reveal — нет catch-блока");
        return w;
    }
    return while_statement();
}

NodePtr NParser::while_statement() {
    if (match(WHILE)) {
        consume(LPAR);
        NodePtr expr = expression();
        consume(RPAR);
        auto blk = std::make_shared<BlockStatement>(block());
        return std::make_shared<WhileStatement>(expr, blk);
    }
    if ((get(0) && (get(0)->type == NUM || get(0)->type == STRING) &&
         look(DOT, 1)) ||
        (get(0) && get(0)->type == LBRACE)) {
        return getter();
    }
    fail(std::string("нераспознано: \"") +
         (get(0) ? get(0)->text : "<eof>") + "\"");
}

// Блок — только '{' из indent-блоков (synthetic).
// Реальные '{' разрешены только у лямбд/vc-шаблонов
// (allowExplicitBrace) и словарных литералов.
std::vector<NodePtr> NParser::block(bool allowExplicitBrace) {
    const Token* brace = get(0);
    if (!brace || brace->type != LBRACE)
        fail("ожидался блок (отступ)");
    if (!brace->synthetic && !allowExplicitBrace && !allow_braces)
        fail("'{ }' для блоков запрещены — используй отступ");
    next();
    std::vector<NodePtr> statements;
    while (!match(RBRACE)) {
        if (position >= size()) fail("незакрытый блок");
        int stmt_line = get(0) ? get(0)->line : 0;
        NodePtr st = statement();
        if (st) { st->line = stmt_line; statements.push_back(st); }
        skipLastDotcommaVar = false;
    }
    return statements;
}
