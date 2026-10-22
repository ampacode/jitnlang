#pragma once
// Порт type.py — ядро. Лексер, как и в Python-версии, выдаёт
// ОДНОСИМВОЛЬНЫЕ операторы; составные (==, >=, ++) парсер
// собирает через match(X); match(X).
#include <string>
#include <unordered_map>

enum {
    END = 0, NUM, HEX_NUMBER, STRING, VARIABLE,
    IF, ELIF, ELSE, WHILE, FOR, BREAK, CONTINUE, RETURN,
    FUNCTION, CLASS, NEW, USING, NAMESPACE, WATCH, REVEAL, SKIP,
    // односимвольные операторы
    PLUS, MINUS, STAR, SLASH, PERCENT, AT, EQUAL, EXCL,
    LT, GT, TILDE, CARET, BAR, AMP, QUESTION, COLON,
    LPAR, RPAR, LQB, RQB, LBRACE, RBRACE,
    COMMA, DOT, DOTCOMMA, SHARP, DOLLAR, WAVE,
    // виртуальные типы операторов для AST (как в type.py)
    EQEQ, EXCLEQ, LTEQ, GTEQ, AMPAMP, BARBAR,
    PLUSEQ, MINUSEQ, STAREQ, SLASHEQ, PERCENTEQ,
    INCREMENT, DECREMENT, DOUBLESLASH, STARSTAR,
    LTLT, GTGT, DOTDOT, QUESTIONCOLON, COLONCOLON,
};

struct Token {
    int type = END;
    std::string text;
    int line = 0;
    int col = 0;
    bool synthetic = false; // '{'/'}', вставленные indent-блоками
};

inline const std::unordered_map<std::string, int>& keywords() {
    static const std::unordered_map<std::string, int> kw = {
        {"end", END},
        {"if", IF}, {"elif", ELIF}, {"else", ELSE},
        {"while", WHILE}, {"for", FOR},
        {"break", BREAK}, {"continue", CONTINUE},
        {"return", RETURN},
        {"function", FUNCTION}, {"func", FUNCTION}, {"def", FUNCTION},
        {"fn", FUNCTION}, {"lambda", FUNCTION}, {"define", FUNCTION},
        {"indef", FUNCTION}, {"redef", FUNCTION}, {"redefine", FUNCTION},
        {"cdef", FUNCTION}, {"cpdef", FUNCTION},
        {"class", CLASS}, {"new", NEW},
        {"using", USING}, {"namespace", NAMESPACE},
        {"watch", WATCH}, {"reveal", REVEAL}, {"skip", SKIP},
    };
    return kw;
}
