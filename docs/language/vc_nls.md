# vc и nls

## vc — разметка

vc — способ писать HTML прямо в коде, с рецепторами вместо
HTML-тегов. Это не DOM и не JavaScript — на cpp-пути vc
компилируется в сборку `std::string`, в eval конкатенируется на лету.

```nlang
string name = "Vlad"
auto page = <nmda [page]>
    <ht2a>"Hello"</gaba>
    <ampa [card] (main-card) {data-x = "1"}>
        <d2>"user: "$(name)</gaba>
        <m1 {href = "/about"}>"link"</gaba>
    </gabaa>
</gabaa>
```

| рецептор | тег | рецептор | тег |
|----------|-----|----------|-----|
| ampa/gabaa | div | d1 | button |
| nmda | section | d2 | span |
| glu | html | d2l | form |
| gabab | body | d2s | input |
| gaba | head | m1 | a |
| m2 | script | ht2a/b/c | h1/h2/h3 |
| | | f1..f6 | font size=1..6 |

`f1`..`f6` — шкала размеров шрифта: рецептор сам задаёт размер,
резолвится в `<font size="N">`. Явный `{size=N}` переопределяет:

```nlang
<f1>"заголовок"</gaba>          // <font size="1">заголовок</font>
<f4 {size=7}>"больше"</gaba>    // <font size="7">больше</font>
```

- `[a b]` → `class="a b"`; `x-y` склеивается: `[admin - sidebar]` →
  `admin-sidebar`
- `(id)` → `id="id"`
- `{k = v}` → атрибут; `v` может быть `$(expr)`
- `{tag = "video"}` → переопределить html-тег
- `$(expr)` — вставка значения
- void-теги (`input`, `br`...) не закрываются
- statements внутри vc исполняются для побочных эффектов (eval-only)

Закрывающий тег — рецепторная пара, как в py:
`ampa→gabaa`, `d1→gabaa|d2s`, `d2l→glu|d2s`, остальные → `gaba`.

### view("name")

`view("hello")` читает `views/hello.vc` (относительно каталога
исходника) и возвращает строку html. В eval файл парсится при каждом
вызове — правки видны сразу (как hot-reload у `using`).

## nls — стили

`style("site")` читает `styles/site.nls` и возвращает
`<style>...</style>`:

```nlang
css site {
    int gap = 16;
    ampa.card {
        padding = gap px;      // переменные + юниты
        background = gabaa;    // рецептор -> цвет палитры
        margin = gap / 2 px;   // арифметика
        &:hover { opacity = 90 %; }
    }
    (header) { font-size = 14 px; }  // -> #header
}
```

- селекторные рецепторы мапятся в html-теги (`ampa.card` →
  `div.card`, `d1.cta` → `button.cta`)
- палитра: ampa `#7a5cff`, nmda `#2f6fed`, gaba `#111118`,
  gabaa `#1c1c26`, gabab `#2a2a38`, d1 `#ff4d6d`, d2 `#ffa657`,
  m1 `#e8e8f0`, m2 `#9a9ab0`
- `palette Name { ampa ~ client = 0x112233; }` — переопределение
  client-палитры; `~ programmer` — программерская сторона
- `$(expr |> prop)` — биндинг значения в свойство
- `&media`/`&screen`/`&keyframes`/`raw`/`import` — поддержаны
- значения: hex `0x..` и `0 x7a5cff`, слова `system-ui`,
  `f("arg")`, `! important` → `!important`
- `style("x.css")` вернёт содержимое файла как есть

**Отличие от py:** `import` с `is_file` пока не резолвит файл в css
(документировано), `makemigrations`/`pg`-ORM не портируется.

## nlb

`.nlb` — nlang-файл для браузерного кода. Два пути:

- `using <x.nlb>` — инклудится интерпретатором как `.nlc`
  (hot-reload).
- `<m2 {src="/frontend/x.nlb"}>` в vc — компилируется в
  `static/frontend/x.js` через `Node::javascript`, ссылка
  становится `/frontend/x.js` (обслуживается `net.staticDir`).

Синтаксис nlb — C-стиль: `{}` и `;` разрешены (`allow_braces`).
Комментарии — только `#` (`//` не поддерживается, `/` —
лексерская ошибка; внутри строк `//` безопасен).

Особенности nlang→JS трансляции:

- `auto x`/`int x` → `let x`; `new X(args)` → `new X(args)`;
  `for (x : c)` → `for (const x of c)`; `==` → `===`.
- **Индексация 1-базная**: `arr[1]` → `arr[0]` в JS.
  `arr[i]` в JS-коде пишется как `arr[i + 1]`.
- `stod`/`stoi` → `parseFloat`/`parseInt`; `log`/`print` →
  `console.log`; `.size()` → `.length`.
- DOM: `document.getElementById`, `canvas.getContext("2d")`,
  `addEventListener`, `requestAnimationFrame`, `AudioContext` —
  работают как есть (getter-цепочки проходят в JS).

Пример — `test/mars.nlc`: аудио-плеер + канвас-рендер
красной планеты (`views/mars.vc`, `styles/mars.nls`,
`frontend/mars.nlb`).
