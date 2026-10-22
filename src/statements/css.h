#pragma once
// Порт css.py — nlang css (`.nls` / `css name { ... }`).
//
// Формат:
//   css site {
//       int gap = 16;                 // переменные
//       ampa.card, m1.link {          // селекторы (рецептор -> тег)
//           padding = gap px;         // свойство = значение unit
//           background = gabaa;       // рецептор -> цвет палитры
//           margin = gap * 2 px;      // арифметика
//           &:hover { opacity = 1; }  // вложенность
//       }
//   }
//
// ~ side = Debounce Attention: client (цвет) / programmer (имя стороны).
// style("name") -> "<style>...</style>".
#include "../node.h"
#include "../nvm.h"
#include "../type.h"
#include <map>
#include <vector>
#include <string>

// палитра по умолчанию — рецептор -> цвет (как в py)
inline const std::map<std::string, long long>& css_default_palette() {
    static const std::map<std::string, long long> p = {
        {"ampa",  0x7a5cff}, {"nmda",  0x2f6fed},
        {"gaba",  0x111118}, {"gabaa", 0x1c1c26},
        {"gabab", 0x2a2a38}, {"d1",    0xff4d6d},
        {"d2",    0xffa657}, {"m1",    0xe8e8f0},
        {"m2",    0x9a9ab0},
    };
    return p;
}

inline bool css_is_unit(const std::string& s) {
    static const char* u[] = {"px","em","rem","vh","vw","vmin",
        "vmax","pt","pc","cm","mm","in","q","ch","ex","lh",
        "deg","fr","ms","s","%"};
    for (auto x : u) if (s == x) return true;
    return false;
}

inline std::string css_hexfmt(long long v) {
    char b[16];
    snprintf(b, sizeof(b), "#%06llx",
             (unsigned long long)(v < 0 ? 0 : v > 0xffffff ? 0xffffff : v));
    return b;
}
inline std::string css_numfmt(double v) {
    if (v == (long long)v) return std::to_string((long long)v);
    char b[32]; snprintf(b, sizeof(b), "%g", v); return b;
}

// ---------- парсер значений (порт _CssValueParser) ----------
// term := arith unit? ; атомы: числа, hex, рецепторы-цвета,
// переменные css-блока, литеральные слова (system-ui...), f(...)
struct CssValueParser {
    std::vector<Token> toks;
    size_t i = 0;
    std::map<std::string, double>& vars;
    const std::map<std::string, long long>& palette;

    CssValueParser(std::vector<Token> t,
                   std::map<std::string, double>& v,
                   const std::map<std::string, long long>& p)
        : toks(std::move(t)), vars(v), palette(p) {}

    const Token* peek() const {
        return i < toks.size() ? &toks[i] : nullptr;
    }
    bool is_unit_tok(const Token* t) const {
        if (!t) return false;
        if (t->type == PERCENT) return true;
        return t->type == VARIABLE && css_is_unit(t->text);
    }

    // значение -> {число|строка, тип: number|color|literal}
    std::pair<double, int> atom() {
        const Token* t = peek();
        if (!t) return {0, 0};
        if (t->text == "-") {
            ++i;
            auto v = atom();
            return {-v.first, v.second};
        }
        if (t->type == LPAR) {
            ++i;
            auto v = addsub();
            if (peek() && peek()->type == RPAR) ++i;
            return v;
        }
        if (t->type == NUM || t->type == HEX_NUMBER) {
            const Token* nxt = i + 1 < toks.size() ? &toks[i + 1] : nullptr;
            // '0' 'x7a5cff' -> 0x7a5cff
            if (t->text == "0" && nxt && nxt->type == VARIABLE &&
                nxt->text.size() > 1 && nxt->text[0] == 'x') {
                i += 2;
                return {(double)std::stoll(nxt->text.substr(1),
                                         nullptr, 16), 1};
            }
            ++i;
            if (t->type == HEX_NUMBER ||
                t->text.rfind("0x", 0) == 0)
                return {(double)std::stoll(t->text, nullptr, 16), 1};
            return {t->text.find('.') != std::string::npos
                    ? std::stod(t->text) : (double)std::stoll(t->text), 0};
        }
        if (t->type == VARIABLE && palette.count(t->text)) {
            ++i;
            return {(double)palette.at(t->text), 1};
        }
        if (t->type == VARIABLE) {
            ++i;
            if (vars.count(t->text)) return {vars[t->text], 0};
            // слово с дефисами: system-ui (пока следующая часть не var)
            std::string word = t->text;
            while (i + 1 < toks.size() && toks[i].text == "-" &&
                   isalpha((unsigned char)toks[i + 1].text[0]) &&
                   !vars.count(toks[i + 1].text)) {
                word += "-" + toks[i + 1].text;
                i += 2;
            }
            if (i < toks.size() && toks[i].type == LPAR)
                return func_call(word);
            lit_ = word;
            return {0, 2};
        }
        if (t->type == STRING) {
            ++i;
            lit_ = "\"" + t->text + "\"";
            return {0, 2};
        }
        ++i;
        lit_ = t->text;
        return {0, 2};
    }

    std::pair<double, int> func_call(const std::string& name) {
        ++i; // '('
        if (peek() && peek()->type == STRING) {
            std::string s = name + "(" + peek()->text + ")";
            i += 2; // string + ')'
            lit_ = s;
            return {0, 2};
        }
        int depth = 1;
        std::string parts;
        while (i < toks.size() && depth > 0) {
            const Token& t = toks[i];
            if (t.type == LPAR) { ++depth; parts += t.text; }
            else if (t.type == RPAR) {
                if (--depth == 0) { ++i; break; }
                parts += t.text;
            } else parts += t.text;
            ++i;
        }
        lit_ = name + "(" + parts + ")";
        return {0, 2};
    }

    std::string lit_; // литеральный текст последнего атома

    std::pair<double, int> mult() {
        auto v = atom();
        while (peek() && (peek()->text == "*" || peek()->text == "/")) {
            std::string op = peek()->text;
            ++i;
            auto r = atom();
            if (v.second == 2 || r.second == 2) return {0, 2};
            v.first = op == "*" ? v.first * r.first : v.first / r.first;
            if (v.second == 1 || r.second == 1) v.second = 1;
        }
        return v;
    }
    std::pair<double, int> addsub() {
        auto v = mult();
        while (peek() && (peek()->text == "+" || peek()->text == "-")) {
            const Token* nxt = i + 1 < toks.size() ? &toks[i + 1] : nullptr;
            if (nxt && v.second == 2) break; // '-' — часть слова
            std::string op = peek()->text;
            ++i;
            auto r = mult();
            if (v.second == 2 || r.second == 2) return {0, 2};
            v.first = op == "+" ? v.first + r.first : v.first - r.first;
            if (v.second == 1 || r.second == 1) v.second = 1;
        }
        return v;
    }
    std::string term() {
        auto v = addsub();
        if (v.second == 2) return lit_;
        if (is_unit_tok(peek())) {
            std::string u = peek()->type == PERCENT ? "%" : peek()->text;
            ++i;
            if (v.second == 1) return css_hexfmt((long long)v.first);
            return css_numfmt(v.first) + u;
        }
        if (v.second == 1) return css_hexfmt((long long)v.first);
        return css_numfmt(v.first);
    }
    std::string parse() {
        std::vector<std::string> terms;
        while (i < toks.size()) terms.push_back(term());
        // "! important" -> "!important"
        std::string r;
        for (size_t k = 0; k < terms.size(); ++k) {
            if (terms[k] == "!" && k + 1 < terms.size() &&
                terms[k + 1] == "important") { r += "!important"; ++k; }
            else { if (!r.empty()) r += " "; r += terms[k]; }
        }
        return r;
    }
};

// ---------- AST css-узлов ----------
struct CssNode { virtual ~CssNode() = default; };
struct CssVarDecl : CssNode {
    std::string name; std::vector<Token> toks;
};
struct CssProperty : CssNode {
    std::string name; std::vector<Token> toks;
};
struct CssBinding : CssNode { // $(expr |> prop)
    std::string prop; std::vector<Token> toks;
};
struct CssSelector : CssNode {
    std::string selector; std::vector<std::shared_ptr<CssNode>> children;
};
struct CssMedia : CssNode {
    std::string low, high;
    std::vector<std::shared_ptr<CssNode>> children;
};
struct CssMediaExpr : CssNode {
    std::string condition;
    std::vector<std::shared_ptr<CssNode>> children;
};
struct CssKeyframes : CssNode {
    std::string name;
    std::vector<std::shared_ptr<CssNode>> children;
};
struct CssImport : CssNode { std::string url; bool is_file = false; };
struct CssRaw : CssNode { std::string text; };

// эмиссия css-текста (порт CssStatement.emit_nodes)
inline void css_emit(const std::vector<std::shared_ptr<CssNode>>& nodes,
              std::vector<std::string> parents,
              std::map<std::string, double>& vars,
              const std::map<std::string, long long>& palette,
              std::string& out,
              const std::map<std::string,std::string>& prog_palette) {
    std::string sel = parents.empty() ? ":root" : "";
    for (size_t k = 0; k < parents.size(); ++k) {
        if (k) sel += " ";
        sel += parents[k];
    }
    std::vector<std::string> props;
    std::vector<std::shared_ptr<CssNode>> deferred;
    for (auto& n : nodes) {
        if (auto d = std::dynamic_pointer_cast<CssVarDecl>(n)) {
            CssValueParser p(d->toks, vars, palette);
            vars[d->name] = p.addsub().first;
        } else if (auto pr = std::dynamic_pointer_cast<CssProperty>(n)) {
            CssValueParser p(pr->toks, vars, palette);
            props.push_back(pr->name + ": " + p.parse());
        } else if (auto b = std::dynamic_pointer_cast<CssBinding>(n)) {
            CssValueParser p(b->toks, vars, palette);
            props.push_back(b->prop + ": " + p.parse());
        } else if (auto r = std::dynamic_pointer_cast<CssRaw>(n)) {
            out += r->text;
            if (r->text.empty() || r->text.back() != '\n') out += "\n";
        } else if (auto im = std::dynamic_pointer_cast<CssImport>(n)) {
            out += "@import url(\"" + im->url + "\");\n";
        } else {
            deferred.push_back(n);
        }
    }
    if (!props.empty()) {
        out += sel + " {\n";
        for (auto& p : props) out += "    " + p + ";\n";
        out += "}\n";
    }
    for (auto& n : deferred) {
        if (auto s = std::dynamic_pointer_cast<CssSelector>(n)) {
            auto np = parents;
            if (!s->selector.empty() && s->selector[0] == '&' &&
                !parents.empty())
                np.back() += s->selector.substr(1);
            else
                np.push_back(s->selector);
            css_emit(s->children, np, vars, palette, out, prog_palette);
        } else if (auto m = std::dynamic_pointer_cast<CssMedia>(n)) {
            std::string inner;
            css_emit(m->children, {}, vars, palette, inner, prog_palette);
            out += "@media (min-width: " + m->low +
                   ") and (max-width: " + m->high + ") {\n" + inner + "}\n";
        } else if (auto m = std::dynamic_pointer_cast<CssMediaExpr>(n)) {
            std::string inner;
            css_emit(m->children, {}, vars, palette, inner, prog_palette);
            out += "@media (" + m->condition + ") {\n" + inner + "}\n";
        } else if (auto k = std::dynamic_pointer_cast<CssKeyframes>(n)) {
            std::string inner;
            css_emit(k->children, {}, vars, palette, inner, prog_palette);
            out += "@keyframes " + k->name + " {\n" + inner + "}\n";
        }
    }
}

// css name { ... } — statement: рендерится и кладётся в vm.css_blocks
class CssStatement : public Node {
public:
    std::string name;
    std::vector<std::shared_ptr<CssNode>> nodes;
    std::string rendered; // заполняется при eval/compile

    std::string render(Nvm& vm) const {
        std::map<std::string, double> vars;
        // client-палитра = default + переопределения из palette-блоков
        auto pal = css_default_palette();
        for (auto& kv : vm.css_client_palette) pal[kv.first] = kv.second;
        std::string out;
        css_emit(nodes, {}, vars, pal, out, vm.css_prog_palette);
        return out;
    }
    Emitted cpp(Nvm& vm) override {
        vm.css_blocks["css:" + name] = render(vm);
        return Emitted("");
    }
    NObject eval(Nvm& vm) override {
        rendered = render(vm);
        vm.css_blocks["css:" + name] = rendered;
        return NObject();
    }
};

// palette Name { ampa ~ client = 0x...; } — обновляет палитру
class PaletteStatement : public Node {
public:
    std::string name;
    // receptor -> {client: tokens, programmer: text}
    std::map<std::string,
             std::map<std::string, std::vector<Token>>> entries;
    void apply(Nvm& vm) {
        std::map<std::string, double> novars;
        for (auto& e : entries)
            for (auto& s : e.second) {
                if (s.first == "client") {
                    CssValueParser p(s.second, novars,
                                     css_default_palette());
                    auto v = p.atom();
                    if (v.second != 2)
                        vm.css_client_palette[e.first] =
                            (long long)v.first;
                } else {
                    std::string w;
                    for (auto& t : s.second)
                        w += (w.empty() ? "" : " ") + t.text;
                    vm.css_prog_palette[e.first] = w;
                }
            }
    }
    Emitted cpp(Nvm& vm) override { apply(vm); return Emitted(""); }
    NObject eval(Nvm& vm) override { apply(vm); return NObject(); }
};
