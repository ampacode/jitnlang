#include "lexer.h"
#include <cctype>
#include <unordered_map>

char Lexer::peek(int rel) const {
    size_t p = position + rel;
    return p < text.size() ? text[p] : '\0';
}
char Lexer::next() { ++position; return peek(); }
void Lexer::addToken(int type, const std::string& s) {
    Token t;
    t.type = type;
    t.text = s;
    t.line = line;
    t.col = (int)(position - line_start - s.size());
    if (t.col < 0) t.col = 0;
    tokens.push_back(std::move(t));
}

void Lexer::tokenizeNumber() {
    // 0x… — шестнадцатеричный литерал
    if (peek(0) == '0' && (peek(1) == 'x' || peek(1) == 'X') &&
        isxdigit((unsigned char)peek(2))) {
        std::string number = "0";
        number += next();
        char current = next();
        while (current && isxdigit((unsigned char)current)) {
            number += current;
            current = next();
        }
        addToken(HEX_NUMBER, number);
        return;
    }
    std::string number;
    char current = peek(0);
    while (isdigit((unsigned char)current) ||
           (current == '.' && isdigit((unsigned char)peek(1)))) {
        number += current;
        current = next();
    }
    // научная нотация: 5.972e24, 1e-3, 2E+10
    if ((current == 'e' || current == 'E') &&
        (isdigit((unsigned char)peek(1)) ||
         ((peek(1) == '+' || peek(1) == '-') &&
          isdigit((unsigned char)peek(2))))) {
        number += current;
        current = next();
        if (current == '+' || current == '-') {
            number += current;
            current = next();
        }
        while (isdigit((unsigned char)current)) {
            number += current;
            current = next();
        }
    }
    addToken(NUM, number);
}

void Lexer::tokenizeString(char first) {
    int start_line = line;
    int start_col = (int)(position - line_start - 1);
    std::string strval;
    char current = peek();
    while (current != first && current != '\0') {
        if (current == '\\') {          // escape (расширение над py-версией)
            char e = next();
            switch (e) {
                case 'n': strval += '\n'; break;
                case 't': strval += '\t'; break;
                case 'r': strval += '\r'; break;
                case '0': strval += '\0'; break;
                default: strval += e; break;
            }
            current = next();
            continue;
        }
        strval += current;
        current = next();
    }
    if (current != first)
        throw LexError{"незакрытая строка", start_line, start_col};
    next();
    addToken(STRING, strval);
}

void Lexer::tokenizeVariable(char) {
    std::string variable;
    char current = peek();
    while (isalnum((unsigned char)current) || current == '_' ||
           (unsigned char)current >= 0x80) {
        variable += current;
        current = next();
    }
    auto& kw = keywords();
    auto it = kw.find(variable);
    addToken(it != kw.end() ? it->second : VARIABLE, variable);
}

// как в py-версии: один символ -> один токен
void Lexer::tokenizeOperator() {
    static const std::unordered_map<char, int> ops = {
        {'+', PLUS}, {'-', MINUS}, {'*', STAR}, {'/', SLASH},
        {'(', LPAR}, {')', RPAR}, {'=', EQUAL}, {';', DOTCOMMA},
        {'{', LBRACE}, {'}', RBRACE}, {',', COMMA}, {'<', LT},
        {'>', GT}, {'#', SHARP}, {'&', AMP}, {'|', BAR},
        {'[', LQB}, {']', RQB}, {':', COLON}, {'.', DOT},
        {'@', AT}, {'?', QUESTION}, {'!', EXCL}, {'%', PERCENT},
        {'~', WAVE}, {'$', DOLLAR}, {'^', CARET},
    };
    char op = peek();
    next();
    auto it = ops.find(op);
    if (it != ops.end()) addToken(it->second, std::string(1, op));
}

std::vector<Token> Lexer::tokenize(const std::string& src, bool indent_blocks) {
    text = src + '\0';
    position = 0; line = 0; line_start = 0;
    tokens.clear();
    const std::string opchars = "+-*/()=;{},<>#&|[]:.@?!%~$^";
    while (position < text.size()) {
        char current = peek();
        if (current == '\0') break;
        if (current == '\n') {
            next();
            ++line;
            line_start = position;
        } else if (current == ' ' || current == '\t' || current == '\r') {
            next();
        } else if (isdigit((unsigned char)current)) {
            tokenizeNumber();
        } else if (current == '"' || current == '`' || current == '\'') {
            next();
            tokenizeString(current);
        } else if (isalpha((unsigned char)current) || current == '_' ||
                   (unsigned char)current >= 0x80) {
            tokenizeVariable(current);
        } else if (opchars.find(current) != std::string::npos) {
            tokenizeOperator();
        } else {
            next();
        }
    }
    if (indent_blocks)
        return apply_indent_blocks(tokens);
    return tokens;
}

// ================= apply_indent_blocks =================
// упрощённый порт: без vc-разметки и без DIMENSIONS/RECEPTORS
// (они у нас сразу VARIABLE)

static bool contFirst(int t) {
    switch (t) {
        case PLUS: case MINUS: case STAR: case SLASH: case PERCENT:
        case BAR: case DOT: case COMMA: case EQUAL: case EXCL:
        case QUESTION: case COLON: case TILDE: case CARET: case WAVE:
        case AT: case GT: case RPAR: case RQB:
            return true;
        default:
            return false;
    }
}
static bool openOk(int t) {
    switch (t) {
        case VARIABLE: case NUM: case STRING: case HEX_NUMBER:
        case RPAR: case RQB: case ELSE: case WATCH: case REVEAL:
            return true;
        default:
            return false;
    }
}

std::vector<Token> apply_indent_blocks(std::vector<Token>& in) {
    std::vector<Token> out;
    out.reserve(in.size() + 16);
    std::vector<int> stack;   // колонки авто-открытых '{'
    int depth = 0;            // ( [ { глубина
    int vc = 0;               // глубина vc-разметки (без рецепторов — всегда 0)
    const Token* prev_tok = nullptr;
    int logic_col = 0, prev_logic_col = 0;

    auto mkbrace = [&](char ch, const Token& ref) {
        Token t;
        t.type = ch == '{' ? LBRACE : RBRACE;
        t.text = std::string(1, ch);
        t.line = ref.line;
        t.col = ref.col;
        t.synthetic = true;
        out.push_back(std::move(t));
    };

    for (size_t i = 0; i < in.size(); ++i) {
        Token& tok = in[i];
        bool newline = !prev_tok || tok.line > prev_tok->line;
        int first_col = tok.col;
        if (newline && depth == 0 && vc == 0) {
            prev_logic_col = logic_col;
            logic_col = first_col;
        }

        if (newline && vc == 0 && depth == 0 && !contFirst(tok.type)) {
            if (tok.type == RBRACE) {
                while (!stack.empty() && stack.back() > tok.col) {
                    stack.pop_back();
                    mkbrace('}', tok);
                }
                if (!stack.empty() && stack.back() == tok.col)
                    stack.pop_back();
            } else {
                while (!stack.empty() && stack.back() >= tok.col) {
                    stack.pop_back();
                    mkbrace('}', tok);
                }
                // open_ok: '/' после '//' комментария-подобного паттерна
                // в py-версии — там проверка prev2/prev3 на ://;
                // здесь комментарии вырезаны до лексера, опускаем
                bool open_ok = prev_tok && openOk(prev_tok->type);
                if (open_ok && tok.col > prev_logic_col) {
                    stack.push_back(prev_logic_col);
                    mkbrace('{', tok);
                }
            }
        }

        out.push_back(tok);

        if (tok.type == LPAR || tok.type == LQB || tok.type == LBRACE) ++depth;
        else if (tok.type == RPAR || tok.type == RQB || tok.type == RBRACE)
            depth = depth > 0 ? depth - 1 : 0;

        prev_tok = &in[i];
    }

    static const Token eofRef{END, "", 0, 0};
    const Token& ref = in.empty() ? eofRef : in.back();
    while (!stack.empty()) {
        stack.pop_back();
        mkbrace('}', ref);
    }
    return out;
}
