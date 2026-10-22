// Порт nparse.py — лямбды: function(), _typed_args(),
// _looks_like_typed_lambda(), _looks_like_paren_lambda()
#include "nparse.h"
#include "../expressions/function.h"
#include "../expressions/variable.h"
#include <set>

static const std::set<std::string> LAMBDA_RTYPE_BARE = {
    "anti", "auto", "func",
    "int", "long", "double", "float", "short", "char",
    "unsigned", "size_t", "intptr_t", "uintptr_t",
    "int64_t", "uint64_t", "long long",
    "bool", "void", "string", "std::string",
    "widget", "vector", "list", "map", "json", "JValue",
};

NodePtr NParser::function() {
    // безтиповая лямбда: (args) { body }
    if (look(LPAR) && _looks_like_paren_lambda()) {
        next(); // '('
        auto args = _typed_args();
        auto stmts = block(true);
        return std::make_shared<FunctionExpression>(args, stmts);
    }
    // типизированная лямбда: int(a) { } / anti(anti a) { }
    if (look(VARIABLE) && look(LPAR, 1) && _looks_like_typed_lambda()) {
        std::string rtype = get(0)->text;
        next();
        consume(LPAR);
        auto args = _typed_args();
        auto stmts = block(true);
        return std::make_shared<FunctionExpression>(args, stmts, rtype);
    }
    if (match(FUNCTION)) {
        consume(LPAR);
        auto args = _typed_args();
        auto stmts = block(true);
        return std::make_shared<FunctionExpression>(args, stmts);
    }
    return newExpr();
}

bool NParser::_looks_like_typed_lambda() {
    size_t j = position + 2;
    std::vector<int> arg_words;
    int cur = 0;
    while (j < size()) {
        int t = tokens[j].type;
        if (t == VARIABLE) {
            if (++cur > 2) return false;
        } else if (t == AMP || t == STAR || t == COLON) {
            // & * :: — часть типа аргумента
        } else if (t == COMMA) {
            if (cur == 0) return false;
            arg_words.push_back(cur);
            cur = 0;
        } else if (t == RPAR) {
            if (cur == 0) return false;
            arg_words.push_back(cur);
            if (!(j + 1 < size() && tokens[j + 1].type == LBRACE))
                return false;
            bool all_two = true;
            for (int w : arg_words) if (w != 2) all_two = false;
            if (all_two) return true;
            return LAMBDA_RTYPE_BARE.count(get(0)->text) != 0;
        } else {
            return false;
        }
        ++j;
    }
    return false;
}

bool NParser::_looks_like_paren_lambda() {
    size_t j = position + 1;
    int depth = 0;
    bool prev_amp = false;
    while (j < size()) {
        int t = tokens[j].type;
        if (t == LPAR) {
            ++depth;
        } else if (t == RPAR) {
            if (depth == 0)
                return j + 1 < size() && tokens[j + 1].type == LBRACE;
            --depth;
        } else if (depth == 0) {
            if (t == VARIABLE || t == COMMA || t == STAR || t == COLON) {
                prev_amp = false;
            } else if (t == AMP) {
                if (prev_amp) return false; // && — выражение
                prev_amp = true;
            } else {
                return false; // оператор/число — группировка
            }
        }
        ++j;
    }
    return false;
}

std::vector<std::pair<std::string,
    std::pair<std::string, NodePtr>>> NParser::_typed_args() {
    std::vector<std::pair<std::string,
        std::pair<std::string, NodePtr>>> args;
    while (!match(RPAR)) {
        std::string type, name;
        bool typed = look(VARIABLE) &&
            (look(VARIABLE, 1) || look(AMP, 1) ||
             look(STAR, 1) || look(COLON, 1));
        if (typed) {
            type = get(0)->text;
            next();
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
    return args;
}
