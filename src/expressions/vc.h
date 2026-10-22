#pragma once
// vc-разметка: <ampa [cls] (id) {k=v}>...</gabaa>
//
// Рецепторы -> html-теги (как в py-версии). vc выражение —
// это не DOM и не js: на cpp-пути оно эмитится как сборка
// std::string, в eval — как конкатенация на лету.
// $(expr) — вставка значения; {tag="x"} — переопределение тега;
// [a b] — class="a b"; (name) — id="name"; {k=v} — атрибуты.
#include "../node.h"
#include "../nvm.h"
#include "../jsgen.h"
#include "string.h"
#include <map>
#include <vector>

// рецептор -> html-тег
inline const std::map<std::string, std::string>& vc_tags() {
    static const std::map<std::string, std::string> m = {
        {"ampa","div"}, {"gabaa","div"}, {"nmda","section"},
        {"glu","html"}, {"gabab","body"}, {"gaba","head"},
        {"d1","button"}, {"d2","span"}, {"d2l","form"},
        {"d2s","input"}, {"m1","a"}, {"m2","script"},
        {"ht2a","h1"}, {"ht2b","h2"}, {"ht2c","h3"},
        // f1..f6 — шкала размеров шрифта (как h1..h6):
        // резолвится в <font size=N> в resolve_tag()
        {"f1","font"}, {"f2","font"}, {"f3","font"},
        {"f4","font"}, {"f5","font"}, {"f6","font"},
    };
    return m;
}
inline bool vc_is_receptor(const std::string& s) {
    return vc_tags().count(s) != 0;
}
// допустимые закрывающие теги для открывающего рецептора
inline bool vc_close_ok(const std::string& open, const std::string& close) {
    if (open == "d1")  return close == "gabaa" || close == "d2s";
    if (open == "d2l") return close == "glu"   || close == "d2s";
    if (open == "d2s") return close == "gabaa" || close == "d1" ||
                              close == "d2l";
    if (open == "ampa" || open == "nmda") return close == "gabaa";
    return close == "gaba";
}
inline bool vc_is_void_tag(const std::string& tag) {
    static const char* voids[] = {"area","base","br","col","embed",
        "frame","hr","img","input","link","meta","param","source",
        "track","wbr"};
    for (auto v : voids) if (tag == v) return true;
    return false;
}
inline std::string vc_esc(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == '"') r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else r += c;
    }
    return r;
}

// $(expr) — вычислить и вставить
class VcOutputExpression : public Node {
public:
    NodePtr expr;
    explicit VcOutputExpression(NodePtr e) : expr(std::move(e)) {}
    Emitted cpp(Nvm& vm) override {
        return Emitted("nlStr(" + expr->cpp(vm).getCpp() + ")");
    }
    NObject eval(Nvm& vm) override;
};

class VcExpression : public Node {
public:
    std::string receptor;
    std::vector<std::string> classes;
    std::string id;
    // атрибуты: name -> expr (null = голый атрибут);
    // статические строки идут как StringExpression
    std::vector<std::pair<std::string, NodePtr>> attrs;
    std::vector<NodePtr> children;

    explicit VcExpression(std::string r) : receptor(std::move(r)) {}

    const std::string& html_tag() const { return resolved_tag; }
    void resolve_tag() {
        resolved_tag = vc_tags().count(receptor)
            ? vc_tags().at(receptor) : "div";
        // f1..f6 -> <font size="1..6">: рецептор сам задаёт
        // размер, атрибут size подставляем сами
        if (receptor.size() == 2 && receptor[0] == 'f' &&
            receptor[1] >= '1' && receptor[1] <= '6') {
            resolved_tag = "font";
            // size из рецептора — но явный {size=N} пользователя
            // важнее (не дублируем атрибут)
            bool has_size = false;
            for (auto& a : attrs)
                if (a.first == "size") has_size = true;
            if (!has_size)
                attrs.push_back({"size",
                    std::make_shared<StringExpression>(
                        std::string(1, receptor[1]))});
        }
        for (auto& a : attrs)
            if (a.first == "tag" && a.second) {
                // литерал-строка — вытаскиваем без eval
                if (auto se = std::dynamic_pointer_cast<
                        StringExpression>(a.second))
                    resolved_tag = se->value;
            }
    }

    Emitted cpp(Nvm& vm) override {
        std::string tag = resolved_tag;
        std::string lit = "\n<" + tag;
        if (!classes.empty()) {
            lit += " class=\"";
            for (size_t i = 0; i < classes.size(); ++i) {
                if (i) lit += " ";
                lit += classes[i];
            }
            lit += "\"";
        }
        if (!id.empty()) lit += " id=\"" + id + "\"";
        // конкатенация кусков: литералы + динамика
        std::vector<std::string> pieces{
            "std::string(\"" + vc_esc(lit) + "\")"};
        for (auto& a : attrs) {
            if (a.first == "tag") continue;
            if (!a.second) {
                pieces.push_back("std::string(\" " + a.first + "\")");
            } else {
                pieces.push_back("std::string(\" " + a.first +
                                 "=\\\"\")");
                // src=".../app.nlb" -> "/frontend/app.js" (как py
                // resolve_nlb_attr — .nlb компилируется в js)
                if (auto se = std::dynamic_pointer_cast<
                        StringExpression>(a.second);
                    se && se->value.size() > 4 &&
                    se->value.substr(se->value.size() - 4) == ".nlb") {
                    pieces.push_back("std::string(\"" +
                        nlang_resolve_nlb_attr(se->value, vm) +
                        "\")");
                } else {
                    pieces.push_back("nlStr(" +
                                     a.second->cpp(vm).getCpp() + ")");
                }
                pieces.push_back("std::string(\"\\\"\")");
            }
        }
        pieces.push_back("std::string(\">\")");
        if (!vc_is_void_tag(tag)) {
            for (auto& ch : children) {
                Emitted c = ch->cpp(vm);
                if (!c.getCpp().empty()) pieces.push_back(c.getCpp());
            }
            pieces.push_back("std::string(\"</" + tag + ">\")");
        }
        std::string code;
        for (size_t i = 0; i < pieces.size(); ++i) {
            if (i) code += " + ";
            code += pieces[i];
        }
        return Emitted(code);
    }

    NObject eval(Nvm& vm) override;

private:
    std::string resolved_tag;
};

// текст внутри vc
class VcTextExpression : public Node {
public:
    std::string text;
    explicit VcTextExpression(std::string t) : text(std::move(t)) {}
    Emitted cpp(Nvm&) override {
        return Emitted("std::string(\"" + vc_esc(text) + "\")");
    }
    NObject eval(Nvm&) override { return NObject(text); }
};

// ---- eval: сборка строки на лету ----
inline NObject VcOutputExpression::eval(Nvm& vm) {
    return NObject(nstr(expr->eval(vm)).asStr());
}
inline NObject VcExpression::eval(Nvm& vm) {
    std::string r = "\n<" + resolved_tag;
    if (!classes.empty()) {
        r += " class=\"";
        for (size_t i = 0; i < classes.size(); ++i) {
            if (i) r += " ";
            r += classes[i];
        }
        r += "\"";
    }
    if (!id.empty()) r += " id=\"" + id + "\"";
    for (auto& a : attrs) {
        if (a.first == "tag") continue;
        if (!a.second) { r += " " + a.first; continue; }
        std::string v = nstr(a.second->eval(vm)).asStr();
        if (v.size() > 4 && v.substr(v.size() - 4) == ".nlb")
            v = nlang_resolve_nlb_attr(v, vm);
        r += " " + a.first + "=\"" + v + "\"";
    }
    r += ">";
    if (vc_is_void_tag(resolved_tag)) return NObject(r);
    for (auto& ch : children) {
        NObject v = ch->eval(vm);
        if (!v.isNull()) r += nstr(v).asStr();
    }
    r += "</" + resolved_tag + ">";
    return NObject(r);
}
