# Стандартная библиотека

Подключение: `using <имя>` в начале файла. Объект модуля —
`<имя>.<функция>(args)`.

## math

Числа. Всё — обёртки над математикой процессора, работают
на скорости C++.

```nlang
math.floor(3.7)     # 3 — округление вниз
math.ceil(3.2)      # 4 — вверх
math.round(3.5)     # 4 — к ближайшему
math.sqrt(16.0)     # 4
math.pow(2.0, 8.0)  # 256 — возведение в степень
math.abs(-5.0)      # 5
math.sin/cos/tan(x) # тригонометрия (x в радианах)
math.log(x)         # натуральный логарифм
math.exp(x)         # e в степени x
math.min(a, b), math.max(a, b)
math.pi(), math.e() # константы
```

## str

Строки.

```nlang
str.upper(s)              # "ABC"
str.lower(s)              # "abc"
str.trim(s)               # обрезка пробелов по краям
str.split(s, ",")         # строка -> список
str.join(list, "+")       # список -> строка
str.contains(s, "x")      # есть ли подстрока
str.startsWith(s, "pre")  # начинается с
str.endsWith(s, "end")    # заканчивается на
str.replace(s, "a", "b")  # заменить все вхождения
str.repeat(s, 3)          # повторить n раз
str.indexOf(s, "x")       # позиция или -1
```

## file

Файлы.

```nlang
file.read(path)        # весь файл -> строка
file.write(path, str)  # записать (перезаписать)
file.append(path, str) # дописать в конец
file.exists(path)      # существует ли
file.lines(path)       # список строк
file.size(path)        # байт или -1
file.remove(path)      # удалить -> успех
```

## time

```nlang
time.now()     # миллисекунды от старта ОС (для замеров)
time.nowSec()  # то же в секундах
time.unix()    # unix-время (секунды от 1970)
time.sleep(ms) # пауза
time.date()    # "ГГГГ-ММ-ДД"
time.clock()   # "ЧЧ:ММ:СС"
```

`now()` — монотонные часы: идут только вперёд и не скачут от
перевода системного времени. Используй для бенчмарков.
`unix()` — настенные часы: для меток времени в логах/файлах.

## random

```nlang
random.seed(42)        # фиксированное семя — повторяемые тесты
random.seedTime()      # семя из времени — «настоящая» случайность
random.int(1, 100)     # целое [1, 100] включительно
random.float()         # дробное [0, 1)
random.bool()          # true/false
```

**Простыми словами:** внутри — генератор «твистер» (mt19937).
Из одного числа-семени он раскручивает длинную последовательность
чисел, которая выглядит случайной. Одно семя — одна и та же
последовательность: поэтому `seed(42)` делает «случайность»
воспроизводимой в тестах.

**Где нельзя:** пароли, токены, ключи. Последовательность
предсказуема — для криптографии нужен другой генератор.

## json

```nlang
json j = json.import("{\"a\": 1, \"b\": [1, 2]}")
j["a"]            # чтение поля
j["b"][0]         # вложенный список (индекс с 0)
print(json.export(j))          # в строку одной строкой
print(json.exportPretty(j, 2)) # красиво с отступами
```

**Простыми словами:** JSON — текстовый формат «словарей и списков».
`import` читает текст слева направо и строит `JValue` (объект/массив/
число/строка/логическое), `export` печатает обратно.

**Где нельзя:** гигабайтные файлы — весь документ читается в память
целиком, потокового разбора нет.

## dimension

Домены-потоки. `dimN` — не переменные, а псевдоимена доменов:
`dim1` — основной поток, `dim2`…`dim63` — фоновые.

```nlang
using <dimension>

void worker()
    dim2_result["done"] = 42

dim2.worker()           # запуск worker() в потоке 2
dim1.compute()          # dim1 — синхронный вызов
dim2.alloc(1024)        # быстрая арена домена 2
dim2.reset()            # сброс арены
dim2_data["key"] = v    # общая память домена (JValue)
dim2.joinAll()          # дождаться потоков
```

**Простыми словами:** каждый домен — отдельный поток ОС. Функция
запускается «в стороне», а результат кладётся в общее хранилище
`dimN_имя["ключ"]`. `alloc` — bump-арена: выделение памяти это
просто сдвиг указателя (очень быстро), но освобождать можно
только всё разом через `reset`.

**Где нельзя:** арена — не для долгоживущих объектов (reset сотрёт
всё); потоки делят хранилище — записи в одну и ту же `dimN_var`
из разных потоков лучше делать по разным ключам.

## internet

Встроенный HTTP-сервер. Объект — `net`.

```nlang
using <internet>

net.get("/hello", (req) {
    return ActivationContext("<h1>привет</h1>")
})

net.get("/user/{id}", (req) {
    # {id} в пути попадает в req.get("id")
    return ActivationContext("user=" + req.get("id"))
})

net.post("/echo", (req) {
    r = ActivationContext("")
    r.body = "ты прислал: " + req.body
    return r
})

net.get("/api", (req) {
    r = ActivationContext("")
    r.json = "{\"ok\": true}"   # ответ JSON + правильный Content-Type
    return r
})

net.staticDir("static")       # раздать файлы из каталога
net.listen(8080)              # запуск (блокирующий)
```

**Рецепторы.** Вместо имён методов можно писать нейромедиаторные
алиасы — та же конвенция, что в python-версии:

```nlang
net.ampa("/hello", h)   # GET    — быстрый вход, чтение
net.nmda("/save", h)    # POST   — запись
net.d2("/item", h)      # PUT    — замена
net.gabab("/item", h)   # DELETE — подавление
net.gabaa("/item", h)   # PATCH  — точечная правка
net.m1("/ping", h)      # HEAD   — только заголовки
net.m2("/info", h)      # OPTIONS
```

**wire — комбинированные протоколы.** Рецептор из набора
задаёт и метод, и Content-Type (checkWiresProtocolds из py):

```nlang
net.wire(["ampa", "d2l", "d2s"], "/api", h)  # GET + application/json
```

| комбинация | Content-Type |
|-----------|--------------|
| d2l + d2s | application/json |
| d1 + d2s  | application/x-www-form-urlencoded |
| d1        | application/octet-stream |
| d2l       | multipart/form-data |

**Параметры запроса — имперские акцессоры.** `req` (requisition)
несёт три словаря распоряжений, у каждого метод `order(имя)`:

```nlang
anti officer(anti requisition)
    requisition.order("q")         # любой параметр (слитно)
    requisition.orders.order("q")  # адресные: path + query (?q=)
    requisition.writs.order("f")   # письменные: form-urlencoded тело
```

- `orders` — `/{file}`-захваты пути и `?a=b` строки запроса
- `writs` — поля `application/x-www-form-urlencoded` тела (POST-формы)
- `order` — слитный вид (path + query + form); при совпадении имён
  приоритет: path > form > query
- Совместимо: `req.get("q")` и `req.get("params").get("q")` работают
  по-прежнему

Маршруты: литеральные сегменты приоритетнее `{params}` —
`/login` важнее `/{file}` независимо от порядка регистрации.

## courier — исходящий HTTP-клиент (имперский диалект http)

```nlang
using <courier>

dispatch d = courier.order("https://site")      # GET
dispatch d = courier.deliver(url, body)         # POST
dispatch d = courier.supplant(url, body)        # PUT
dispatch d = courier.amend(url, body)           # PATCH
dispatch d = courier.purge(url)                 # DELETE
```

Ответ — **депеша (dispatch)**:

```nlang
d.body      # тело ответа
d.status    # код (200/404/...)
d.error     # строка ошибки, "" если ok
d.ok        # status 200..399
d.headers   # dict заголовков ответа
```

**Курьер-сессия** — исходящий клиент с состоянием: cookie jar
(вход/регистрация живут между запросами) и свои заголовки:

```nlang
dispatch c = courier.session()
c.header("Authorization", "Bearer t")
c.order("https://api/login")    # Set-Cookie сохранится
c.order("https://api/whoami")   # Cookie уедет сама
```

`courier.header(n, v)` / `clearHeaders()` / `timeout(sec)` — то же
для глобального курьера. Параметр `headers` у вызовов — строка
`"Name: v\nName2: v2"`.

**Хранилище сеанса на сервере** (`using <holonet>`/`internet`):
у `requisition` есть `vault` — серверное key-value, живёт между
запросами по куке `sid`:

```nlang
requisition.vault.set("login", user)
requisition.vault.get("login")   # null если пусто
requisition.vault.erase("login")
```

## crypto

```nlang
using <crypto>

crypto.b64encode("abc")                    # base64
crypto.b64decode("YWJj")
crypto.randomBytes(32)                     # CSPRNG (BCryptGenRandom)
crypto.keygen()                            # b64-ключ 256 бит
crypto.toHex("ab")                         # "6162"
crypto.aes256gcmEncrypt(k, "текст")        # b64(iv||ct||tag)
crypto.aes256gcmDecrypt(k, blobB64)        # "текст" | "" (неверный тег)
crypto.hkdfSha256(ikm, salt, info, 32)     # hex-ключ
```

Без OpenSSL — чистые реализации SHA-256 (RFC 6234),
HMAC/HKDF (RFC 2104/5869), AES-256 в GCM (NIST SP 800-38D).
Неверный ключ/тег → `""`, не исключение — как в py.

## regex

```nlang
using <regex>

re.match("abc123", "[a-z]+[0-9]+")    # true — совпадение целиком
re.search("abc123", "[0-9]+")         # true — где-то внутри
re.group("abc123", "([a-z]+)([0-9]+)", 2)  # "123"
re.groups("abc123", "([a-z]+)([0-9]+)")    # ["abc","123"] (с 0-м match)
re.findAll("a1 b2", "[a-z][0-9]")     # ["a1","b2"]
re.replace("hello", "o", "0")         # "hell0"
re.split("a,b;c", "[,;]")             # ["a","b","c"]
re.test("[a-z]+")                     # true — валидность паттерна
```

Под капотом — `std::regex` (ECMAScript-диалект). Невалидный
паттерн возвращает false/""/пустой список — не исключение.

`ActivationContext` — контекст запроса и ответа одновременно:
читаешь из него `req.get("имя")` (query + path + form-параметры),
`req.body`, `req.getHeader(k)`, `req.getCookie(k)`; пишешь
`r.html` / `r.body` / `r.json` / `r.statusCode` / `r.redirectUrl`,
`r.setHeader`, `r.setCookie`, `r.sessionSet/Get/Erase` (сессии
по куке `sid`, хранятся на сервере).

**Простыми словами:** сервер ждёт соединение на порту, читает
строку запроса (`GET /user/42?x=1`), сопоставляет путь с шаблонами
маршрутов (`{id}` захватывает кусок пути), вызывает твою функцию
и упаковывает результат в HTTP-ответ. Каждое соединение — свой
поток.

**Где нельзя:** публичный продакшен под нагрузкой — нет пула потоков,
TLS/HTTPS, HTTP/2, таймаутов. Это приближённая замена drogon для
учебных и внутренних сервисов.

## physics / neuro / quantum

���������� ������ � ���������� �������, ��� � py:

```nlang
using <physics>
print(PV(10, 2, 3))        # v(t) = v0 + a t
print(POrbitV(5.972e24, 6371000))   # ����������� ��������
print(PPendulum(1))        # ������ ��������

using <neuro>
print(NDecay(100, 2, 4))   # ����������: 100 -> 25
print(NOcc(1, 1, 1))       # ��������� ���������� (����)

using <quantum>
print(SDim(false))         # 10 � ����������� �����������
# QX/QH/QApply/QBasis/QProbs... � ������ � ����� (cpp-�����,
# ����������� ���� � eval �� ��������)
```

������ ������� � � `compiler/includedlib/{physics,neuro,quantum}.py`
(����� ��������� 1-�-1: `PT`, `NConc`, `QTensor`...). ��������� �
`PG`, `PGrav`, `PC`, `PH`, `PK`, `PNA`, `SPlanckLen`, `SHbar`.

## tcp

```nlang
using <tcp>

int l = TcpListen(8199)
int c = TcpConnect("127.0.0.1", 8199)
int s = TcpAccept(l)
TcpSend(c, "ping\n")
print(TcpRecvLine(s))      # "ping"
print(TcpPeerIp(s))        # "127.0.0.1"
TcpClose(c)
```

`TcpListen/TcpAccept/TcpConnect` > ����� `intptr_t` (0 = ������).
`TcpSend`, `TcpRecvLine` (�� \n), `TcpRecvAll`, `TcpSetTimeout`,
`TcpSleepMs`, `TcpPeerIp`. ����������� ������ � ���
�������� � �������� ��������� `internet` (HTTP) ��� ���� ���.

## pg (PostgreSQL)

������ **��� libpq** � ������ wire-�������� v3 ������ `tcp`:

```nlang
using <pg>

auto db = pg.connect("host=127.0.0.1 port=5432 user=u dbname=d password=p")
print(pg.ok(db))
print(pg.err(db))
pg.exec(db, "create table if not exists t(id int)")
print(pg.count(db, "select count(*) from t"))
print(pg.one(db, "select version()"))
auto rows = pg.query(db, "select id,name from t")   # "\x1f"-join �������
pg.close(db)
```

� cpp-������ `pg.connect` ���������� ��������� `PgDb` (��� � py),
� eval � �������� �����.

**Auth:** cleartext (R3), MD5 (R5) � **SCRAM-SHA-256** (R10�R12,
�������� PG10+; PBKDF2-HMAC-SHA256 � �� ����� ���������� `nlhash.h`).
�������: `T` RowDescription > `db.cols`, `D` DataRow (NULL > `""`),
`C` CommandComplete > `execRows`, `E` > `err()`.
���������: ��� TLS (���������� �� localhost/���������� ����),
������ ��������� ������ ��������, async-����������� ������������.

## sqlite

`Db` ������ sqlite3 (ucrt64 `libsqlite3.dll.a`):

```nlang
using <sqlite>
auto db = sqlite.open("app.db")   # ":memory:" � � ������
sqlite.exec(db, "create table t(id integer, name text)")
sqlite.exec(db, "insert into t values (1, 'alice')")
print(sqlite.count(db, "select count(*) from t"))
print(sqlite.one(db, "select name from t where id=1"))
sqlite.column(db, "select name from t")   # ������ �������
sqlite.query(db, "select id,name from t") # "\x1f"-join
sqlite.esc(db, "it's")                    # ' -> ''
```

� cpp-������ `open` ���������� `Db` (��������� �� py), � eval �
�����.

## ORM: model / database

`database` ��������� ���������� � ������ ��� ���������
(`g_defaultDb` ��� pg, `g_defaultSqlDb` ��� sqlite):

```nlang
using <sqlite>            # ��� using <pg>
database maindb {
    file = "app.db"                    # sqlite
    # driver = "postgres"
    # conninfo = "host=... dbname=..." # pg
    # conninfo_env = "PG_URL"          # pg �� env
}
```

`model` ���������� struct � ORM:

```nlang
model User
    string name
    int age
```

### Query-builder (��� .objects)

```nlang
user = User.filter(name = "��������").receive()  # ������ ������
user.name = "��������2"
user.save()           # INSERT ��� UPDATE (�� rowid/id)

User.filter(name = "x", age = 30).all()
User.filter(name = "x").count()
User.filter(name = "x").exists()
User.filterLike("title", "%amp%").orderDesc("id").limit(10).all()
User.where("age > 18").all()
```

### ����������� ������ (py-�����, � ����� ��)

```nlang
User.all()                       # ��������� ��
User.all(maindb)                 # �����
User.find(42)                    # �� id/rowid
User.exists("email", e)
User.order("ts") User.orderDesc("ts") User.orderLim("ts", 10)
```

`m.save()/update()/remove()` � no-arg ����� ��������� ��.
� eval `database` ���������� dict-������ � `exec/one/count/query/esc`,
������ � ���������� ������� + `_this`-�������� ��������.

## ���������������� ������

`r.json`/`r.body`/`r.html` � ������, �� ����� ����������� dict/list
������� � �� ������������� � JSON �������������:

```nlang
r = ActivationContext("")
r.json = {"ok": true, "n": 42}   # = r.json = {...}.dump()
```

cpp-�����: `(JObj(...)).dump()`; eval: `nljson_export`.

## logic � ��������

������ `logic` �� py (Brain.ask / Prolog-������) ����������� ��
�������� � nlang-cpp � ������� ������� ��� ������������ �������:
������� �� ������������ � ������� ������� � �� �������� ���
������� ��������� �������������+����������.
