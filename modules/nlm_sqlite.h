#pragma once
// Модуль sqlite — Db поверх sqlite3 (ucrt64: libsqlite3.dll.a).
// Копия структуры из py (includedlib/sqlite.py):
//   Db filename.db; db.exec(sql); db.count(sql); db.column(sql)
//   db.rows(sql); db.esc(s); db.err()
#include <sqlite3.h>
#include <string>
#include <vector>
#include <iostream>
#include <ctime>
#include <cstdlib>

struct Db;

// дефолтное соединение для ORM (m.save(), X::all()).
// первое Db(...) авто-регистрирует себя тут.
static Db *g_defaultSqlDb = nullptr;

struct Db {
    sqlite3 *h = nullptr;
    Db() {}
    explicit Db(const std::string &path) {
        open(path);
        if (!g_defaultSqlDb) g_defaultSqlDb = this;
    }
    Db(const Db&) = delete;
    Db(Db &&o) noexcept : h(o.h) {
        o.h = nullptr;
        if (g_defaultSqlDb == &o) g_defaultSqlDb = this;
    }
    Db &operator=(const Db&) = delete;
    ~Db() { if (h) sqlite3_close(h); }

    void open(const std::string &path) {
        if (sqlite3_open(path.c_str(), &h) != SQLITE_OK) {
            std::cerr << "sqlite open fail: "
                      << (h ? sqlite3_errmsg(h) : "?") << std::endl;
        }
    }

    bool ok() const { return h != nullptr; }
    std::string err() const {
        return h ? sqlite3_errmsg(h) : "null db";
    }

    void exec(const std::string &sql) {
        char *err = nullptr;
        if (sqlite3_exec(h, sql.c_str(), nullptr, nullptr, &err)
                != SQLITE_OK) {
            std::cerr << "sql err: " << (err ? err : "?") << std::endl;
            if (err) sqlite3_free(err);
        }
    }

    // количество изменённых строк после exec-подобного запроса
    long long execRows(const std::string &sql) {
        exec(sql);
        return sqlite3_changes64(h);
    }

    long long count(const std::string &sql) {
        sqlite3_stmt *st = nullptr;
        long long r = 0;
        if (sqlite3_prepare_v2(h, sql.c_str(), -1, &st, nullptr)
                == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW)
                r = sqlite3_column_int64(st, 0);
        }
        if (st) sqlite3_finalize(st);
        return r;
    }

    std::string one(const std::string &sql) {
        sqlite3_stmt *st = nullptr;
        std::string out;
        if (sqlite3_prepare_v2(h, sql.c_str(), -1, &st, nullptr)
                == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW)
                out = colStr(st, 0);
        }
        if (st) sqlite3_finalize(st);
        return out;
    }

    // список значений первой колонки
    std::vector<std::string> column(const std::string &sql) {
        std::vector<std::string> out;
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(h, sql.c_str(), -1, &st, nullptr)
                == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW)
                out.push_back(colStr(st, 0));
        }
        if (st) sqlite3_finalize(st);
        return out;
    }

    // полные строки
    std::vector<std::vector<std::string>>
    rows(const std::string &sql) {
        std::vector<std::vector<std::string>> out;
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(h, sql.c_str(), -1, &st, nullptr)
                == SQLITE_OK) {
            int nf = sqlite3_column_count(st);
            while (sqlite3_step(st) == SQLITE_ROW) {
                std::vector<std::string> row;
                for (int i = 0; i < nf; ++i)
                    row.push_back(colStr(st, i));
                out.push_back(row);
            }
        }
        if (st) sqlite3_finalize(st);
        return out;
    }

    static std::string esc(const std::string &s) {
        std::string r;
        for (char c : s) {
            if (c == '\'') r += "''";
            else r += c;
        }
        return r;
    }

    static std::string colStr(sqlite3_stmt *st, int i) {
        const unsigned char *t = sqlite3_column_text(st, i);
        return t ? (const char*)t : "";
    }
};

// обёртки для эмиссии nlsqlite_* в сгенерированном коде
inline Db nlsqlite_open(const std::string &path) { return Db(path); }
inline bool nlsqlite_ok(Db &db) { return db.ok(); }
inline std::string nlsqlite_err(Db &db) { return db.err(); }
inline long long nlsqlite_exec(Db &db, const std::string &sql) {
    return db.execRows(sql);
}
inline long long nlsqlite_count(Db &db, const std::string &sql) {
    return db.count(sql);
}
inline std::string nlsqlite_one(Db &db, const std::string &sql) {
    return db.one(sql);
}
inline std::vector<std::string> nlsqlite_column(Db &db,
                                              const std::string &sql) {
    return db.column(sql);
}
inline std::vector<std::string> nlsqlite_query(Db &db,
                                             const std::string &sql) {
    std::vector<std::string> out;
    for (auto &row : db.rows(sql)) {
        std::string s;
        for (size_t j = 0; j < row.size(); j++) {
            if (j) s += '\x1f';
            s += row[j];
        }
        out.push_back(s);
    }
    return out;
}
inline std::string nlsqlite_esc(Db &db, const std::string &s) {
    return Db::esc(s);
}
