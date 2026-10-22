#pragma once
// Порт statements/model.py — `model X` и `database db {...}`.
//
// model User
//     string name;
//     int age;
// генерирует struct User + ORM:
//   user.save()            — INSERT/UPDATE через g_defaultDb
//   user.update() user.remove()
//   User::all() User::where("cond") User::find(id)
//   User::order("f") User::orderDesc("f") User::count()
//   User.filter(name="x")  — query-builder (вложенные условия):
//     User.filter(name="x", age=30).orderDesc("id").limit(10).all()
//     User.filter(name="x").receive()   — первая запись или пустая
//     User.filter(name="x").count()     — число записей
//     User.filter(name="x").exists()    — bool
//
// database maindb { driver = "postgres" conninfo = "..." }
// database maindb { file = "app.db" }
// — открывает БД и делает её дефолтной (g_defaultDb).
#include "../node.h"
#include "../nvm.h"
#include "../ninclude.h"
#include "../expressions/getter.h"
#include "../expressions/variable.h"
#include "../expressions/string.h"

// привязка ORM-методов к dict-экземпляру (hot-swap: модель
// возвращается из dll как JSON -> dict без методов)
void nlang_orm_bind_instance(const std::string& model, NObject inst);

static void sub(std::string& s, const std::string& from,
                const std::string& to) {
    size_t p;
    while ((p = s.find(from)) != std::string::npos)
        s.replace(p, from.size(), to);
}

class ModelStatement : public Node {
public:
    std::string name;
    // [(cpp_type, field_name)] — cpp_type уже переведённый
    // (string -> std::string и т.п.)
    std::vector<std::pair<std::string, std::string>> fields;

    ModelStatement(std::string n,
        std::vector<std::pair<std::string, std::string>> f)
        : name(std::move(n)), fields(std::move(f)) {}

    // ---------- helpers для codegen ----------
    static std::string cpp_type(const std::string& t) {
        if (t == "string") return "std::string";
        if (t == "int" || t == "long") return "long long";
        if (t == "float" || t == "double") return "double";
        if (t == "bool") return "bool";
        return t;
    }
    static std::string sql_type(const std::string& t) {
        if (t == "string") return "TEXT";
        if (t == "int" || t == "long") return "INTEGER";
        if (t == "float" || t == "double") return "REAL";
        if (t == "bool") return "INTEGER";
        return "TEXT";
    }
    // 'text' -> "'" + esc(f) + "'", числа — to_string
    // fields[] хранит C++-типы — обратный маппинг в nlang-тип
    static std::string norm_type(const std::string& cppt) {
        if (cppt == "long long") return "long";
        if (cppt == "std::string") return "string";
        if (cppt == "double") return "double";
        return cppt;
    }
    static std::string sql_val(const std::string& t_,
                               const std::string& f,
                               const std::string& esc,
                               bool pg = false) {
        std::string t = norm_type(t_);
        if (pg && t == "bool")
            return "(" + f + " ? \"'t'\" : \"'f'\")";
        if (t == "int" || t == "long" || t == "float" ||
            t == "double" || t == "bool")
            return "std::to_string(" + f + ")";
        return "\"'\" + " + esc + "(" + f + ") + \"'\"";
    }
    // sqlite: выражение чтения колонки st[idx] по типу
    static std::string col_read_expr(const std::string& t_, size_t i) {
        std::string t = norm_type(t_);
        std::string ix = std::to_string(i);
        if (t == "string")
            return " = Db::colStr(st, " + ix + ");";
        if (t == "int" || t == "long" || t == "bool")
            return " = sqlite3_column_int64(st, " + ix +
                   (t == "bool" ? ") != 0;" : ");");
        if (t == "float" || t == "double")
            return " = sqlite3_column_double(st, " + ix + ");";
        return "";
    }
    // C++-чтение значения колонки для pg (r — vector<string>)
    std::string pg_col_read(const std::string& t_,
                            const std::string& f, int i) {
        std::string t = norm_type(t_);
        if (t == "string") return "m." + f + " = r[" +
                                 std::to_string(i) + "];";
        if (t == "int" || t == "long")
            return "m." + f + " = std::stoll(r[" +
                   std::to_string(i) + "]);";
        if (t == "float" || t == "double")
            return "m." + f + " = std::stod(r[" +
                   std::to_string(i) + "]);";
        if (t == "bool")
            return "m." + f + " = (r[" + std::to_string(i) +
                   "] == \"t\" || r[" + std::to_string(i) +
                   "] == \"1\");";
        return "";
    }

    // ---------- кодогенерация ----------
    Emitted cpp(Nvm& vm) override {
        vm.models[name] = fields;
        std::string n = name;
        // список колонок без id/rowid
        std::string cols;
        for (size_t i = 0; i < fields.size(); ++i) {
            if (i) cols += ", ";
            cols += "\\\"" + fields[i].second + "\\\"";
        }
        std::string decls;
        for (auto& f : fields)
            decls += "    " + f.first + " " + f.second + ";\n";

        std::string sqlite_code;
        if (vm.used_modules.count("sqlite")) {
            std::string vals, sets, reads;
            for (size_t i = 0; i < fields.size(); ++i) {
                if (i) { vals += " + \", \" + "; sets += " + \", \" + "; }
                vals += sql_val(fields[i].first, fields[i].second,
                                "Db::esc");
                sets += "std::string(\"" + fields[i].second +
                        " = \") + " + sql_val(fields[i].first,
                                              fields[i].second,
                                              "Db::esc");
                reads += "            m." + fields[i].second +
                         col_read_expr(fields[i].first, i + 1) + "\n";
            }
            sqlite_code = R"SQL(
    void save (Db &db) {
        if (rowid) { update(db); return; }
        std::string sql = "INSERT INTO \"%N%\" (%COLS%) VALUES(";
        sql += %VALS%;
        sql += ")";
        db.exec(sql);
        rowid = sqlite3_last_insert_rowid(db.h);
    }
    void update (Db &db) {
        std::string sql = "UPDATE \"%N%\" SET ";
        sql += %SETS%;
        sql += " WHERE rowid = " + std::to_string(rowid);
        db.exec(sql);
    }
    void remove (Db &db) {
        db.exec("DELETE FROM \"%N%\" WHERE rowid = "
            + std::to_string(rowid));
    }
    static std::vector<%N%> all (Db &db) {
        return where(db, "");
    }
    static std::vector<%N%> where (Db &db, const std::string &cond) {
        std::vector<%N%> out;
        sqlite3_stmt *st = nullptr;
        std::string sql = "SELECT rowid, %COLS% FROM \"%N%\"";
        if (!cond.empty()) sql += " WHERE " + cond;
        if (sqlite3_prepare_v2(db.h, sql.c_str(), -1, &st, nullptr)
                != SQLITE_OK) return out;
        while (sqlite3_step(st) == SQLITE_ROW) {
            %N% m;
            m.rowid = sqlite3_column_int64(st, 0);
%READS%            out.push_back(m);
        }
        sqlite3_finalize(st);
        return out;
    }
    static std::vector<%N%> order (Db &db, const std::string &f) {
        return where(db, "1=1 ORDER BY " + f);
    }
    static std::vector<%N%> orderDesc (Db &db, const std::string &f) {
        return where(db, "1=1 ORDER BY " + f + " DESC");
    }
    static std::vector<%N%> orderLim (Db &db, const std::string &f,
                                     int n) {
        return where(db, "1=1 ORDER BY " + f
                     + " LIMIT " + std::to_string(n));
    }
    static std::vector<%N%> filter (Db &db, const std::string &field,
                                    const std::string &val) {
        return where(db, field + "='" + Db::esc(val) + "'");
    }
    static std::vector<%N%> filterInt (Db &db, const std::string &field,
                                       long long v) {
        return where(db, field + "=" + std::to_string(v));
    }
    static %N% find (Db &db, long long rowid_) {
        auto v = where(db, "rowid=" + std::to_string(rowid_));
        return v.empty() ? %N%() : v[0];
    }
    static bool exists (Db &db, const std::string &field,
                        const std::string &val) {
        return db.count("SELECT COUNT(*) FROM \"%N%\" WHERE "
                        + field + "='" + Db::esc(val) + "'") > 0;
    }
    static long long count (Db &db) {
        return db.count("SELECT COUNT(*) FROM \"%N%\"");
    }
    static long long countFilter (Db &db, const std::string &field,
                                  const std::string &val) {
        return db.count("SELECT COUNT(*) FROM \"%N%\" WHERE "
                        + field + "='" + Db::esc(val) + "'");
    }
)SQL";
            sub(sqlite_code, "%N%", n);
            sub(sqlite_code, "%COLS%", cols);
            sub(sqlite_code, "%VALS%", vals);
            sub(sqlite_code, "%SETS%", sets);
            sub(sqlite_code, "%READS%", reads);
        }

        std::string pg_code;
        if (vm.used_modules.count("pg")) {
            std::string vals, sets, reads;
            for (size_t i = 0; i < fields.size(); ++i) {
                if (i) { vals += " + \", \" + "; sets += " + \", \" + "; }
                vals += sql_val(fields[i].first, fields[i].second,
                                "db.esc", true);
                sets += "std::string(\"" + fields[i].second +
                        " = \") + " + sql_val(fields[i].first,
                                              fields[i].second,
                                              "db.esc", true);
                reads += "            " +
                         pg_col_read(fields[i].first,
                                     fields[i].second,
                                     (int)i + 1) + "\n";
            }
            pg_code = R"PG(
    // ---------- postgresql ORM ----------
    void save (PgDb &db) {
        if (rowid) { update(db); return; }
        std::string sql = "INSERT INTO \"%N%\" (%COLS%) VALUES(";
        sql += %VALS%;
        sql += ") RETURNING id";
        std::string idv = db.one(sql);
        rowid = idv.empty() ? 0 : std::atoll(idv.c_str());
    }
    void update (PgDb &db) {
        std::string sql = "UPDATE \"%N%\" SET ";
        sql += %SETS%;
        sql += " WHERE id = " + std::to_string(rowid);
        db.exec(sql);
    }
    void remove (PgDb &db) {
        db.exec("DELETE FROM \"%N%\" WHERE id = "
            + std::to_string(rowid));
    }
    static std::vector<%N%> all (PgDb &db) {
        return where(db, "");
    }
    static std::vector<%N%> where (PgDb &db, const std::string &cond) {
        std::vector<%N%> out;
        std::string sql = "SELECT id, %COLS% FROM \"%N%\"";
        if (!cond.empty()) sql += " WHERE " + cond;
        for (auto &r : db.rows(sql)) {
            %N% m;
            m.rowid = std::atoll(r[0].c_str());
%READS%            out.push_back(m);
        }
        return out;
    }
    static std::vector<%N%> order (PgDb &db, const std::string &f) {
        return where(db, "1=1 ORDER BY " + f);
    }
    static std::vector<%N%> orderDesc (PgDb &db, const std::string &f) {
        return where(db, "1=1 ORDER BY " + f + " DESC");
    }
    static std::vector<%N%> orderLim (PgDb &db, const std::string &f,
                                     int lim) {
        return where(db, "1=1 ORDER BY " + f
                     + " LIMIT " + std::to_string(lim));
    }
    static std::vector<%N%> filter (PgDb &db, const std::string &field,
                                    const std::string &val) {
        return where(db, field + "='" + db.esc(val) + "'");
    }
    static std::vector<%N%> filterInt (PgDb &db, const std::string &field,
                                       long long v) {
        return where(db, field + "=" + std::to_string(v));
    }
    static %N% find (PgDb &db, long long id_) {
        auto v = where(db, "id=" + std::to_string(id_));
        return v.empty() ? %N%() : v[0];
    }
    static bool exists (PgDb &db, const std::string &field,
                        const std::string &val) {
        return db.count("SELECT COUNT(*) FROM \"%N%\" WHERE "
                        + field + "='" + db.esc(val) + "'") > 0;
    }
    static long long count (PgDb &db) {
        return db.count("SELECT COUNT(*) FROM \"%N%\"");
    }
    static long long countFilter (PgDb &db, const std::string &field,
                                  const std::string &val) {
        return db.count("SELECT COUNT(*) FROM \"%N%\" WHERE "
                        + field + "='" + db.esc(val) + "'");
    }
)PG";
            sub(pg_code, "%N%", n);
            sub(pg_code, "%COLS%", cols);
            sub(pg_code, "%VALS%", vals);
            sub(pg_code, "%SETS%", sets);
            sub(pg_code, "%READS%", reads);
        }

        // ---------- query-builder (filter(name="x").receive()) ----------
        // общий: работает и с PgDb, и с Db — тип подставляется
        // через шаблон Q<D>
        std::string qb_code;
        if (vm.used_modules.count("pg") ||
            vm.used_modules.count("sqlite")) {
            std::string defaultq, qvariants;
            if (vm.used_modules.count("pg"))
                qvariants += "    static Q<PgDb> q(PgDb &db) "
                             "{ return Q<PgDb>{&db}; }\n";
            if (vm.used_modules.count("sqlite"))
                qvariants += "    static Q<Db> q(Db &db) "
                             "{ return Q<Db>{&db}; }\n";
            if (vm.used_modules.count("pg"))
                defaultq =
                    "    // дефолтный: g_defaultDb\n"
                    "    static Q<PgDb> q() { "
                    "return Q<PgDb>{g_defaultDb}; }";
            else
                defaultq =
                    "    // дефолтный: g_defaultSqlDb\n"
                    "    static Q<Db> q() { "
                    "return Q<Db>{g_defaultSqlDb}; }";
            qb_code = R"QB(
    // ---------- query-builder: User.filter(name="x").receive() ----------
    template<typename D> struct Q {
        D* db;
        std::string cond;
        std::string ord;
        int lim = 0;
        Q& filter(const std::string& field, const std::string& v) {
            if (!cond.empty()) cond += " AND ";
            cond += field + "='" + esc(v) + "'";
            return *this;
        }
        Q& filter(const std::string& field, long long v) {
            if (!cond.empty()) cond += " AND ";
            cond += field + "=" + std::to_string(v);
            return *this;
        }
        Q& filter(const std::string& field, double v) {
            if (!cond.empty()) cond += " AND ";
            cond += field + "=" + std::to_string(v);
            return *this;
        }
        Q& filterLike(const std::string& field,
                      const std::string& v) {
            if (!cond.empty()) cond += " AND ";
            cond += field + " LIKE '%" + esc(v) + "%'";
            return *this;
        }
        Q& where(const std::string& c) {
            if (!cond.empty()) cond += " AND ";
            cond += "(" + c + ")";
            return *this;
        }
        Q& order(const std::string& f) {
            ord = " ORDER BY " + f;
            return *this;
        }
        Q& orderDesc(const std::string& f) {
            ord = " ORDER BY " + f + " DESC";
            return *this;
        }
        Q& limit(int n) { lim = n; return *this; }
        static std::string esc(const std::string& s) {
            std::string r;
            for (char c : s) {
                if (c == '\'') r += "''";
                else r += c;
            }
            return r;
        }
        std::string fullCond() const {
            std::string c = cond;
            if (!ord.empty()) {
                if (c.empty()) c = "1=1";
                c += ord;
            }
            if (lim > 0) {
                if (c.empty()) c = "1=1";
                c += " LIMIT " + std::to_string(lim);
            }
            return c;
        }
        std::vector<%N%> all() {
            return %N%::where(*db, fullCond());
        }
        %N% receive() {
            // первая запись — LIMIT 1
            Q q = *this;
            q.lim = 1;
            auto v = q.all();
            return v.empty() ? %N%() : v[0];
        }
        long long count() {
            return db->count("SELECT COUNT(*) FROM \"%N%\""
                + (cond.empty() ? "" : " WHERE " + cond));
        }
        bool exists() { return count() > 0; }
    };
%QVARIANTS%
%DEFAULTQ%
)QB";
            sub(qb_code, "%N%", n);
            sub(qb_code, "%DEFAULTQ%", defaultq);
            sub(qb_code, "%QVARIANTS%", qvariants);
        }

        // ---------- no-arg версии (g_defaultDb / g_defaultSqlDb) ----------
        std::string noarg_code;
        if (vm.used_modules.count("pg")) {
            noarg_code = R"NOARG(
    // ---------- no-arg: дефолтное соединение ----------
    void save () { save(*g_defaultDb); }
    void update () { update(*g_defaultDb); }
    void remove () { remove(*g_defaultDb); }
    static std::vector<%N%> all () { return all(*g_defaultDb); }
    static std::vector<%N%> where (const std::string &cond) {
        return where(*g_defaultDb, cond);
    }
    static std::vector<%N%> order (const std::string &f) {
        return order(*g_defaultDb, f);
    }
    static std::vector<%N%> orderDesc (const std::string &f) {
        return orderDesc(*g_defaultDb, f);
    }
    static std::vector<%N%> orderLim (const std::string &f, int lim) {
        return orderLim(*g_defaultDb, f, lim);
    }
    static std::vector<%N%> filter (const std::string &field,
                                    const std::string &val) {
        return filter(*g_defaultDb, field, val);
    }
    static std::vector<%N%> filterInt (const std::string &field,
                                       long long v) {
        return filterInt(*g_defaultDb, field, v);
    }
    static %N% find (long long id_) { return find(*g_defaultDb, id_); }
    static bool exists (const std::string &field,
                        const std::string &val) {
        return exists(*g_defaultDb, field, val);
    }
    static long long count () { return count(*g_defaultDb); }
    static long long countFilter (const std::string &field,
                                  const std::string &val) {
        return countFilter(*g_defaultDb, field, val);
    }
)NOARG";
            sub(noarg_code, "%N%", n);
        } else if (vm.used_modules.count("sqlite")) {
            // то же самое, но через g_defaultSqlDb
            noarg_code = R"NOARGSQL(
    // ---------- no-arg: дефолтное соединение ----------
    void save () { save(*g_defaultSqlDb); }
    void update () { update(*g_defaultSqlDb); }
    void remove () { remove(*g_defaultSqlDb); }
    static std::vector<%N%> all () { return all(*g_defaultSqlDb); }
    static std::vector<%N%> where (const std::string &cond) {
        return where(*g_defaultSqlDb, cond);
    }
    static std::vector<%N%> order (const std::string &f) {
        return order(*g_defaultSqlDb, f);
    }
    static std::vector<%N%> orderDesc (const std::string &f) {
        return orderDesc(*g_defaultSqlDb, f);
    }
    static std::vector<%N%> filter (const std::string &field,
                                    const std::string &val) {
        return filter(*g_defaultSqlDb, field, val);
    }
    static std::vector<%N%> filterInt (const std::string &field,
                                       long long v) {
        return filterInt(*g_defaultSqlDb, field, v);
    }
    static %N% find (long long id_) {
        return find(*g_defaultSqlDb, id_);
    }
    static bool exists (const std::string &field,
                        const std::string &val) {
        return exists(*g_defaultSqlDb, field, val);
    }
    static long long count () { return count(*g_defaultSqlDb); }
    static long long countFilter (const std::string &field,
                                  const std::string &val) {
        return countFilter(*g_defaultSqlDb, field, val);
    }
)NOARGSQL";
            sub(noarg_code, "%N%", n);
        }

        std::string code =
            "struct " + n + " {\n    long long rowid = 0;\n" +
            decls + sqlite_code + pg_code + qb_code + noarg_code +
            "};";
        Emitted e(code);
        e.is_function_declaration = true; // struct — уровень объявлений
        return e;
    }

    // ---------- eval ----------
    // Model-объект в eval — dict со статическими функциями.
    // Поля храним в vm.models; db — хендл g_dbs (sqlite) или
    // g_pgdbs (pg) — выбирается по vm.databases[name].
    NObject eval(Nvm& vm) override;
};

class DatabaseStatement : public Node {
public:
    std::string name;
    std::map<std::string, NodePtr> entries; // {key: expr}

    DatabaseStatement(std::string n,
        std::map<std::string, NodePtr> e)
        : name(std::move(n)), entries(std::move(e)) {}

    std::string entry_str(Nvm& vm, const char* k) {
        auto it = entries.find(k);
        if (it == entries.end()) return "";
        NodePtr e = nlang_unwrap_getter(it->second);
        auto s = std::dynamic_pointer_cast<StringExpression>(e);
        return s ? s->value : it->second->cpp(vm).getCpp();
    }

    Emitted cpp(Nvm& vm) override {
        std::string driver = entry_str(vm, "driver");
        if (driver.empty()) driver = "sqlite";
        if (driver == "postgres" || driver == "postgresql" ||
            driver == "pg") {
            std::string conn = entry_str(vm, "conninfo");
            std::string env = entry_str(vm, "conninfo_env");
            std::string code =
                "PgDb " + name + "([&]() -> std::string {";
            if (!env.empty())
                code += "const char* e = std::getenv(\"" + env +
                        "\"); if (e) return e; ";
            code += "return \"" + conn + "\"; }());";
            vm.databases[name] = "pg";
            Emitted e(code);
            e.is_function_declaration = true; // глобальная БД
            return e;
        }
        // sqlite
        std::string file = entry_str(vm, "file");
        vm.databases[name] = "sqlite";
        Emitted e("Db " + name + "(\"" + file + "\");");
        e.is_function_declaration = true;
        return e;
    }

    NObject eval(Nvm& vm) override;
};

// eval-сторона — в eval_impl/model_eval.cpp
NObject orm_instance_to_nobject(
    const std::string& model,
    const std::vector<std::pair<std::string, std::string>>& fields,
    const std::vector<std::string>& row);
