// Порт nparse.py — expression-цепочка:
// expression -> function -> new -> funcexpr -> logicalor ->
// logicaland -> equality -> conditional -> additive ->
// multiplicative -> unary -> getter -> primary
#include "nparse.h"
#include "../expressions/number.h"
#include "../expressions/string.h"
#include "../expressions/variable.h"
#include "../expressions/unary.h"
#include "../expressions/conditional.h"
#include "../expressions/binary.h"
#include "../expressions/list.h"
#include "../expressions/_dict.h"
#include "../expressions/calling.h"
#include "../expressions/getter.h"
#include "../expressions/function.h"
#include "../expressions/new.h"
#include "../expressions/vc.h"

NodePtr NParser::expression() { return function(); }

NodePtr NParser::newExpr() {
    if (match(NEW))
        return std::make_shared<NewExpression>(getter());
    return funcexpr();
}
NodePtr NParser::funcexpr() { return logicalor(); }

NodePtr NParser::logicalor() {
    NodePtr result = logicaland();
    for (;;) {
        // || как два BAR; одиночный '|' — тоже ИЛИ (но не |= и не |>)
        if (look(BAR) && look(BAR, 1)) {
            next(); next();
            result = std::make_shared<ConditionalExpression>(
                BARBAR, result, logicaland());
            continue;
        }
        if (look(BAR) && !look(EQUAL, 1) && !look(GT, 1)) {
            next();
            result = std::make_shared<ConditionalExpression>(
                BARBAR, result, logicaland());
            continue;
        }
        break;
    }
    return result;
}

NodePtr NParser::logicaland() {
    NodePtr result = equality();
    for (;;) {
        if (look(AMP) && look(AMP, 1)) {
            next(); next();
            result = std::make_shared<ConditionalExpression>(
                AMPAMP, result, equality());
            continue;
        }
        if (look(AMP) && !look(EQUAL, 1)) {
            next();
            result = std::make_shared<ConditionalExpression>(
                AMPAMP, result, equality());
            continue;
        }
        break;
    }
    return result;
}

NodePtr NParser::equality() {
    NodePtr result = conditional();
    for (;;) {
        if (look(EQUAL) && look(EQUAL, 1)) {
            next(); next();
            result = std::make_shared<ConditionalExpression>(
                EQEQ, result, conditional());
            continue;
        }
        if (look(EXCL) && look(EQUAL, 1)) {
            next(); next();
            result = std::make_shared<ConditionalExpression>(
                EXCLEQ, result, additive());
            continue;
        }
        break;
    }
    return result;
}

NodePtr NParser::conditional() {
    NodePtr result = additive();
    for (;;) {
        if (look(LT) && look(EQUAL, 1)) {
            next(); next();
            result = std::make_shared<ConditionalExpression>(
                LTEQ, result, additive());
            continue;
        }
        if (look(GT) && look(EQUAL, 1)) {
            next(); next();
            result = std::make_shared<ConditionalExpression>(
                GTEQ, result, additive());
            continue;
        }
        if (match(LT)) {
            result = std::make_shared<ConditionalExpression>(
                LT, result, additive());
            continue;
        }
        if (match(GT)) {
            result = std::make_shared<ConditionalExpression>(
                GT, result, additive());
            continue;
        }
        break;
    }
    return result;
}

NodePtr NParser::additive() {
    NodePtr result = multiplicative();
    for (;;) {
        if (match(PLUS)) {
            result = std::make_shared<BinaryExpression>(
                PLUS, result, multiplicative());
            continue;
        }
        if (match(MINUS)) {
            result = std::make_shared<BinaryExpression>(
                MINUS, result, multiplicative());
            continue;
        }
        break;
    }
    return result;
}

NodePtr NParser::multiplicative() {
    NodePtr result = unary();
    for (;;) {
        if (look(STAR) && look(STAR, 1)) {
            next(); next();
            result = std::make_shared<BinaryExpression>(
                STARSTAR, result, unary());
            continue;
        }
        if (look(SLASH) && look(SLASH, 1)) {
            next(); next();
            result = std::make_shared<BinaryExpression>(
                DOUBLESLASH, result, unary());
            continue;
        }
        if (match(STAR)) {
            result = std::make_shared<BinaryExpression>(
                STAR, result, unary());
            continue;
        }
        if (match(SLASH)) {
            result = std::make_shared<BinaryExpression>(
                SLASH, result, unary());
            continue;
        }
        if (match(PERCENT)) {
            result = std::make_shared<BinaryExpression>(
                PERCENT, result, unary());
            continue;
        }
        break;
    }
    return result;
}

NodePtr NParser::unary() {
    if (match(MINUS))
        return std::make_shared<UnaryExpression>(MINUS, getter());
    if (match(PLUS))
        return getter();
    return getter();
}

NodePtr NParser::getter() {
    std::vector<NodePtr> getterarr;
    NodePtr first = primary();
    if (match(LPAR)) {
        auto call = std::make_shared<CallingExpression>(first,
                    std::vector<NodePtr>{});
        while (!match(RPAR)) {
            if (look(VARIABLE) && look(EQUAL, 1)) {
                std::string name = consume(VARIABLE).text;
                match(EQUAL);
                call->named_args.push_back({name, expression()});
            } else {
                call->pos_args.push_back(expression());
            }
            match(COMMA);
        }
        getterarr.push_back(call);
    } else {
        getterarr.push_back(first);
        if (match(LQB)) {
            std::vector<NodePtr> access{expression()};
            consume(RQB);
            while (match(LQB)) {
                access.push_back(expression());
                consume(RQB);
            }
            getterarr.push_back(
                std::make_shared<ArrayAccessExpression>(access));
        }
    }
    while (match(DOT)) {
        if (look(VARIABLE) && look(LPAR, 1)) {
            NodePtr var = primary();
            match(LPAR);
            auto call = std::make_shared<CallingExpression>(var,
                        std::vector<NodePtr>{});
            while (!match(RPAR)) {
                if (look(VARIABLE) && look(EQUAL, 1)) {
                    std::string name = consume(VARIABLE).text;
                    match(EQUAL);
                    call->named_args.push_back({name, expression()});
                } else {
                    call->pos_args.push_back(expression());
                }
                match(COMMA);
            }
            getterarr.push_back(call);
        } else {
            getterarr.push_back(primary());
        }
    }
    return std::make_shared<GetterExpression>(getterarr);
}

NodePtr NParser::primary() {
    const Token* current = get(0);
    if (!current) fail("неожиданный конец файла");

    // vc-выражение как значение: = <ampa>...</gabaa>
    if (current->type == LT && look(VARIABLE, 1) &&
        vc_is_receptor(get(1)->text))
        return vc_expression();

    if (match(NUM)) return std::make_shared<NumberExpression>(current->text);
    if (match(HEX_NUMBER))
        return std::make_shared<NumberExpression>(current->text);
    if (match(STRING))
        return std::make_shared<StringExpression>(current->text);
    if (match(VARIABLE))
        return std::make_shared<VariableExpression>(current->text);
    if (match(LQB)) {
        std::vector<NodePtr> array;
        while (!match(RQB)) {
            array.push_back(expression());
            match(COMMA);
            match(DOTCOMMA);
        }
        return std::make_shared<ListExpression>(array);
    }
    if (match(EXCL))
        return std::make_shared<ExcludeExpression>(expression());
    if (match(LBRACE)) {
        auto d = std::make_shared<DictExpression>();
        while (!match(RBRACE)) {
            NodePtr name = primary();
            consume(COLON);
            NodePtr val = expression();
            d->dictionary.push_back({name, val});
            match(COMMA);
            match(DOTCOMMA);
        }
        return d;
    }
    if (match(LPAR)) {
        NodePtr result = expression();
        match(RPAR);
        return result;
    }
    fail(std::string("неправильное выражение: \"") + current->text + "\"");
}
