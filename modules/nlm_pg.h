#pragma once
// Модуль pg — PostgreSQL через libpq (ucrt64: libpq.dll.a,
// libpq-fe.h). Копия PgDb из py (includedlib/pg.py) —
// теперь используется тот же самый C++-код, что и в py.
//
// API:
//   db = pg.connect("host=.. port=.. user=.. dbname=.. password=..")
//   pg.ok(db) pg.err(db) pg.exec(db,sql) pg.query(db,sql)
//   pg.one(db,sql) pg.count(db,sql) pg.esc(db,s) pg.close(db)
#include <libpq-fe.h>
#include <string>
#include <vector>
#include <cstdlib>

struct PgDb;

// дефолтное соединение для ORM-вызовов (m.save(), X::all()).
// первое PgDb(...) авто-регистрирует себя тут.
static PgDb *g_defaultDb = nullptr;

struct PgDb {
    PGconn *h = nullptr;

    PgDb() {}
    explicit PgDb(const std::string &conninfo) {
        h = PQconnectdb(conninfo.c_str());
        // первое соединение — дефолтное для ORM (m.save(), X::all())
        if (!g_defaultDb) g_defaultDb = this;
    }
    PgDb(const PgDb&) = delete;
    PgDb(PgDb &&o) noexcept : h(o.h) {
        o.h = nullptr;
        if (g_defaultDb == &o) g_defaultDb = this;
    }
    PgDb &operator=(const PgDb&) = delete;
    ~PgDb() { if (h) PQfinish(h); }

    bool ok() const { return h && PQstatus(h) == CONNECTION_OK; }

    std::string err() const {
        if (!h) return "null connection";
        std::string e = PQerrorMessage(h);
        while (!e.empty() && (e.back() == '\n' || e.back() == '\r'))
            e.pop_back();
        return e;
    }

    void exec(const std::string &sql) {
        if (!h) return;
        PGresult *r = PQexec(h, sql.c_str());
        if (r) PQclear(r);
    }

    long long execRows(const std::string &sql) {
        if (!h) return -1;
        PGresult *r = PQexec(h, sql.c_str());
        ExecStatusType st = PQresultStatus(r);
        if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
            PQclear(r);
            return -1;
        }
        long long n = std::atoll(PQcmdTuples(r));
        PQclear(r);
        return n;
    }

    long long count(const std::string &sql) {
        std::string v = one(sql);
        return v.empty() ? 0 : std::atoll(v.c_str());
    }

    std::string one(const std::string &sql) {
        if (!h) return std::string();
        PGresult *r = PQexec(h, sql.c_str());
        std::string out;
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0
                && !PQgetisnull(r, 0, 0))
            out = PQgetvalue(r, 0, 0);
        PQclear(r);
        return out;
    }

    std::vector<std::vector<std::string>> rows(const std::string &sql) {
        std::vector<std::vector<std::string>> out;
        if (!h) return out;
        PGresult *r = PQexec(h, sql.c_str());
        if (PQresultStatus(r) != PGRES_TUPLES_OK) {
            PQclear(r);
            return out;
        }
        int n = PQntuples(r), f = PQnfields(r);
        for (int i = 0; i < n; i++) {
            std::vector<std::string> row;
            for (int j = 0; j < f; j++)
                row.push_back(PQgetisnull(r, i, j)
                              ? std::string() : PQgetvalue(r, i, j));
            out.push_back(row);
        }
        PQclear(r);
        return out;
    }

    std::string esc(const std::string &s) {
        if (!h) return s;
        std::vector<char> buf(s.size() * 2 + 1);
        int err = 0;
        size_t n = PQescapeStringConn(h, buf.data(), s.c_str(),
                                      s.size(), &err);
        return err ? s : std::string(buf.data(), n);
    }
};

inline PgDb nlpg_connect(const std::string &conninfo) {
    return PgDb(conninfo);
}
inline bool nlpg_ok(PgDb &db) { return db.ok(); }
inline std::string nlpg_err(PgDb &db) { return db.err(); }
inline long long nlpg_exec(PgDb &db, const std::string &sql) {
    return db.execRows(sql);
}
inline std::string nlpg_one(PgDb &db, const std::string &sql) {
    return db.one(sql);
}
inline std::vector<std::string> nlpg_query(PgDb &db,
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
inline long long nlpg_count(PgDb &db, const std::string &sql) {
    return db.count(sql);
}
inline std::string nlpg_esc(PgDb &db, const std::string &s) {
    return db.esc(s);
}
inline void nlpg_close(PgDb &db) {
    if (db.h) { PQfinish(db.h); db.h = nullptr; }
}
inline void nlpg_setDefault(PgDb &db) { g_defaultDb = &db; }
