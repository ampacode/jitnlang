// Реализация инклуда nlang-файлов.
#include "ninclude.h"
#include "nvm.h"
#include "lexer.h"
#include "run.h" // stripComments
#include "parser/nparse.h"
#include "statements/function.h"
#include "statements/_class.h"
#include "statements/model.h"
#include "expressions/vc.h"
#include "expressions/string.h"
#include "expressions/getter.h"
#include "statements/css.h"
#include <fstream>
#include <sstream>
#include <filesystem>

long long nlang_mtime(const std::string& path) {
    // «штамп» файла: FNV-хэш содержимого. stat/last_write_time
    // на этом MinGW возвращают закешированное значение —
    // содержимое надёжнее и файлы маленькие.
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;
    unsigned long long h = 14695981039346656037ull;
    char buf[4096];
    while (f) {
        f.read(buf, sizeof(buf));
        std::streamsize n = f.gcount();
        for (std::streamsize i = 0; i < n; ++i) {
            h ^= (unsigned char)buf[i];
            h *= 1099511628211ull;
        }
    }
    return (long long)h;
}

std::vector<NodePtr> nlang_parse_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw NLangError{"не найден файл '" + path + "'"};
    std::stringstream ss;
    ss << f.rdbuf();
    Lexer lexer;
    NParser parser;
    auto tokens = lexer.tokenize(stripComments(ss.str()), true);
    return parser.program(tokens);
}

// имена функций/классов, которые определит файл — для hot-меток
static void mark_hot(Nvm& vm, const std::vector<NodePtr>& stmts,
                     const std::string& path, long long mtime) {
    for (auto& st : stmts) {
        std::string name;
        if (auto f = std::dynamic_pointer_cast<FunctionDeclaration>(st))
            name = f->name;
        else if (auto c = std::dynamic_pointer_cast<ClassStatement>(st))
            name = c->name;
        else if (auto m = std::dynamic_pointer_cast<ModelStatement>(st))
            name = m->name; // model-фабрика тоже hot-метится
        if (name.empty()) continue;
        auto it = vm.vars.find(name);
        if (it != vm.vars.end() && it->second.isFunc()) {
            it->second.asFunc()->hot_path = path;
            it->second.asFunc()->hot_mtime = mtime;
        }
    }
}

static bool file_exists(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return f.good();
}

// разрешение пути: относительный -> каталог входного файла
static std::string resolve(Nvm& vm, const std::string& path) {
    if (file_exists(path)) return path;
    if (!vm.source_dir.empty()) {
        std::string p = vm.source_dir + "/" + path;
        if (file_exists(p)) return p;
    }
    return path;
}

void nlang_include_eval(Nvm& vm, const std::string& path) {
    std::string p = resolve(vm, path);
    auto stmts = nlang_parse_file(p);
    for (auto& st : stmts) { vm.exec_note(st->line); st->eval(vm); }
    mark_hot(vm, stmts, p, nlang_mtime(p));
}

std::string nlang_include_cpp(Nvm& vm, const std::string& path) {
    auto stmts = nlang_parse_file(resolve(vm, path));
    std::string code;
    for (auto& st : stmts) {
        // прототипы функций инклуда — чтобы их видели дальше
        if (auto fd = std::dynamic_pointer_cast<FunctionDeclaration>(st))
            vm.module_declarations += fd->signature(vm) + ";\n";
        Emitted r = st->cpp(vm);
        if (r.anticpp) continue;
        // функции/классы — на уровень объявлений файла,
        // исполняемые — инлайн на месте using
        if (r.is_function_declaration)
            vm.module_declarations += r.getCpp() + "\n";
        else
            code += cpp_pad(vm) + r.getCpp() + ";\n";
    }
    return code;
}

// ================= view() — views/<name>.vc =================

// .vc файл — последовательность vc-блоков; несколько блоков
// собираются в "фрагмент" (div-обёртка не нужна — конкатенация)
class VcFragment : public Node {
public:
    std::vector<NodePtr> children;
    Emitted cpp(Nvm& vm) override {
        std::string code;
        bool first = true;
        for (auto& ch : children) {
            Emitted c = ch->cpp(vm);
            if (c.getCpp().empty()) continue;
            code += first ? c.getCpp() : " + " + c.getCpp();
            first = false;
        }
        return Emitted(code.empty() ? "std::string(\"\")" : code);
    }
    NObject eval(Nvm& vm) override {
        std::string r;
        for (auto& ch : children) {
            NObject v = ch->eval(vm);
            if (!v.isNull()) r += nstr(v).asStr();
        }
        return NObject(r);
    }
};

static std::string view_path(Nvm& vm, const std::string& name) {
    std::string rel = "views/" + name + ".vc";
    if (file_exists(rel)) return rel;
    if (!vm.source_dir.empty()) {
        std::string p = vm.source_dir + "/" + rel;
        if (file_exists(p)) return p;
    }
    return rel;
}

NodePtr nlang_parse_view(Nvm& vm, const std::string& name) {
    std::string path = view_path(vm, name);
    std::ifstream f(path, std::ios::binary);
    if (!f) throw NLangError{"view: нет файла '" + path + "'"};
    std::stringstream ss;
    ss << f.rdbuf();
    Lexer lexer;
    NParser parser;
    auto tokens = lexer.tokenize(ss.str(), false);
    parser.tokens = tokens;
    parser.position = 0;
    auto frag = std::make_shared<VcFragment>();
    while (parser.position < parser.size() &&
           parser.look(LT) && parser.look(VARIABLE, 1)) {
        frag->children.push_back(parser.vc_expression());
    }
    return frag;
}

// eval-натив: парсится при каждом вызове — изменения .vc
// видны сразу (как hot-reload у .nlc)
static NObject bi_view(Nvm& vm, const NArgs& a) {
    if (a.pos.empty()) return NObject(std::string());
    NodePtr v = nlang_parse_view(vm, a.pos[0].asStr());
    return v->eval(vm);
}

static NObject bi_style(Nvm&, const NArgs&);

void nlang_register_view(Nvm& vm) {
    auto f = std::make_shared<NFunction>();
    f->name = "view";
    f->impl.store(&bi_view, std::memory_order_release);
    vm.set("view", NObject(f));

    auto s = std::make_shared<NFunction>();
    s->name = "style";
    s->impl.store(&bi_style, std::memory_order_release);
    vm.set("style", NObject(s));
}

// ================= style() — styles/<name>.nls =================

// style("name") -> "<style>css</style>" или <link> для .css
// .nls парсится при каждом вызове — изменения видны сразу
std::string nlang_style_css(Nvm& vm, const std::string& name) {
    std::string base = name;
    if (base.size() > 4 &&
        (base.substr(base.size() - 4) == ".nls" ||
         base.substr(base.size() - 4) == ".css"))
        base = base.substr(0, base.size() - 4);
    std::string nls = "styles/" + base + ".nls";
    if (!file_exists(nls) && !vm.source_dir.empty())
        nls = vm.source_dir + "/" + nls;
    if (file_exists(nls)) {
        std::ifstream f(nls, std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        Lexer lexer;
        NParser parser;
        auto toks = lexer.tokenize(stripComments(ss.str()), false);
        auto stmts = parser.program(toks);
        std::string css;
        for (auto& st : stmts) {
            if (auto c = std::dynamic_pointer_cast<CssStatement>(st))
                css += c->render(vm);
            else
                st->eval(vm); // palette и прочее
        }
        return css;
    }
    std::string cssf = "styles/" + base + ".css";
    if (!file_exists(cssf) && !vm.source_dir.empty())
        cssf = vm.source_dir + "/" + cssf;
    if (file_exists(cssf)) {
        std::ifstream f(cssf, std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        return ss.str();
    }
    return "/* no such style: " + base + " */";
}

static NObject bi_style(Nvm& vm, const NArgs& a) {
    if (a.pos.empty()) return NObject(std::string());
    return NObject("<style>" +
                   nlang_style_css(vm, a.pos[0].asStr()) + "</style>");
}

NodePtr nlang_unwrap_getter(NodePtr n) {
    if (auto g = std::dynamic_pointer_cast<GetterExpression>(n))
        if (g->list.size() == 1) return g->list[0];
    return n;
}
