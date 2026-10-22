#pragma once
// Порт nparse.py — NParser. Разбит по файлам:
//   statement.cpp  — statement-цепочка + block
//   expression.cpp — expression-цепочка + getter/primary
//   lambda.cpp     — лямбды + _typed_args + looks_like_*
#include "../type.h"
#include "../node.h"
#include <vector>

struct ParseError {
    std::string msg;
    int line = 0, col = 0;
};

class NParser {
public:
    std::vector<Token> tokens;
    size_t position = 0;
    bool skipLastDotcommaVar = false;
    bool allow_braces = false; // .nlb: C-стиль { } для блоков

    std::vector<NodePtr> program(const std::vector<Token>& toks);
    NodePtr expressionFrom(const std::vector<Token>& toks) {
        tokens = toks;
        position = 0;
        return expression();
    }

    size_t size() const { return tokens.size(); }

    // ---- statement-цепочка (statement.cpp) ----
    NodePtr statement();
    NodePtr scheming();
    NodePtr css_block();
    NodePtr using_directive();
    NodePtr funcdecl();
    NodePtr classdecl();
    NodePtr startsvariable();
    NodePtr return_statement();
    NodePtr conditional_statement();
    NodePtr continue_statement();
    NodePtr break_statement();
    NodePtr for_statement();
    NodePtr watch_statement();
    NodePtr while_statement();
    NodePtr vc_expression();
    std::vector<NodePtr> block(bool allowExplicitBrace = false);

    // ---- expression-цепочка (expression.cpp) ----
    NodePtr expression();
    NodePtr function();
    NodePtr newExpr();
    NodePtr funcexpr();
    NodePtr logicalor();
    NodePtr logicaland();
    NodePtr equality();
    NodePtr conditional();
    NodePtr additive();
    NodePtr multiplicative();
    NodePtr unary();
    NodePtr getter();
    NodePtr primary();

    // ---- лямбды (lambda.cpp) ----
    bool _looks_like_typed_lambda();
    bool _looks_like_paren_lambda();
    std::vector<std::pair<std::string,
        std::pair<std::string, NodePtr>>> _typed_args();

    // ---- helpers ----
    void next() { ++position; }
    bool match(int type, int rel = 0);
    bool look(int type, int rel = 0) const;
    const Token* get(int rel = 0) const;
    const Token& consume(int type, int rel = 0);
    bool listLook(std::initializer_list<int> types, int rel = 0);
    bool skipLastDotcomma();
    void _optional_semi();
    [[noreturn]] void fail(const std::string& msg);
};
