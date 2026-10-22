#pragma once
// Порт lexer.py — Lexer + apply_indent_blocks
#include <vector>
#include <string>
#include "type.h"

struct LexError {
    std::string msg;
    int line, col;
};

class Lexer {
public:
    std::vector<Token> tokenize(const std::string& text, bool indent_blocks);

private:
    std::string text;
    size_t position = 0;
    int line = 0;
    size_t line_start = 0;
    std::vector<Token> tokens;

    char peek(int rel = 0) const;
    char next();
    void addToken(int type, const std::string& text);
    void tokenizeNumber();
    void tokenizeString(char first);
    void tokenizeVariable(char first);
    void tokenizeOperator();
};

std::vector<Token> apply_indent_blocks(std::vector<Token>& tokens);
