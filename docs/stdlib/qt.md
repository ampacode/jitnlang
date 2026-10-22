# Модуль `qt` — окна и виджеты Qt6 из nlang

`using <qt>` открывает eval-путь к Qt6 Widgets. Модуль
существует только в интерпретаторе: C++-путь программ Qt
не линкует — qt это «мост к десктопу», а не часть
скомпилированного кода.

## Хендлы

Каждый виджет — `long long`-хендл в таблице `widgets()`
внутри `nlm_qt.cpp`. Все функции принимают хендл:

```nlang
long long win = qt.window("app", 640, 480)
qt.vbox(win)
long long lbl = qt.label(win, "hello")
qt.show(win)
qt.exec()
```

## Виджеты

| функция | виджет |
|---|---|
| `qt.window(title, w, h)` | главное окно |
| `qt.panel(parent)` | панель (для hbox-строки кнопок) |
| `qt.label(parent, text)` | QLabel |
| `qt.button(parent, text)` | QPushButton |
| `qt.slider(parent, min, max, val)` | QSlider |
| `qt.playlist(parent)` | QListWidget |
| `qt.viz(parent, bars)` | эквалайзер + проходка кода |

## Состояние и стили

`qt.setText(h, s)`, `qt.setValue(h, v)` / `qt.value(h)`,
`qt.items(list, names)` (вектор строк), `qt.row(list)` /
`qt.setRow(list, i)`, `qt.setStyle(h, css)` —
Qt stylesheet на виджет.

## Сигналы

```nlang
qt.onClick(btn, on_click)          # ()
qt.onChange(slider, on_change)     # (value)
qt.onSelect(list, on_select)       # (row)
qt.onTimer(ms, on_tick)            # () периодически
qt.click(btn)                      # программный клик (тесты)
```

Колбэк — обычная nlang-функция. Исключения внутри неё
ловятся `qt_call` и печатаются с префиксом `[qt]` —
Qt-луп не рвётся.

## Файловый диалог

`qt.openFiles(title, filter)` → vector<string> выбранных
путей. Фильтр: `"audio (*.mp3 *.wav);;* (*.*)"`.

## Визуализация (`qt.viz`)

`VizWidget` — самоанимируемые полосы + «проходка кода»:

- `qt.vizActive(viz, true/false)` — полосы живут при playing;
- `qt.vizLevel(viz, 0..100)` — энергия (громкость);
- `qt.vizCode(viz, source_text)` — исходник, который
  печатается в виджет;
- `qt.vizPos(viz, posMs)` — позиция воспроизведения.
  Печать синхронизирована: за каждую мс трека допечатывается
  `CPS=38` символов/с — код идёт **ровно со скоростью
  проигрывания**; на паузе стоит, на seek назад — с начала;
- `qt.vizInfo(viz, ["pos 0:12", ...])` — живые числа,
  правый верхний угол.

Идея: виджет показывает код, который **реально выполняется
в цикле воспроизведения** — nlang-хэндлеры, eval-цикл
интерпретатора, звуковой бэкенд, jit — плюс настоящие числа
этого цикла (pos/len/mode/vol).

## Модуль `sound` — бэкенды файлов

`sound.play(path)` пробует по цепочке:

1. **WMP COM** (`wmp.dll`, `IWMPPlayer4`) — mp3/m4a/wma/wav
   без внешних кодеков. `put_url` сама стартует, когда медиа
   догрузится. Почему не MCI/DirectShow/MFPlay:
   `mpegvideo` на современном Windows отсутствует,
   DirectShow без mp3-декодера молчит, MFPlay требует
   message pump и в консоли мёртв.
2. **DirectShow** (`strmiids`/`quartz`) — старые кодеки.
3. **PlaySound** — только wav, без контроля позиции.

`pause/resume/stop`, `position()/length()` (мс), `seek(ms)`,
`volume(0..100)`, `mode()` -> `playing|paused|stopped|""`,
`wait([ms])`, `beep/note/nfreq` — синтез, как раньше.

### Реальный спектр и длина волны

`sound.spectrum(pos_ms, nbins)` — энергии полос 0..1,
`sound.wavelength(pos_ms)` — доминирующая **длина волны
звука в метрах** (λ = 343/f, f — доминирующая частота).
Файл декодируется в PCM через `IMFSourceReader`
(MF readwrite) синхронно при `play` — ~300мс на трек —
и полосы считаются Goertzel-окном вокруг текущей позиции.
Это настоящие сэмплы трека: `qt.vizWave(viz, bands)`
скармливает их полосам эквалайзера.

### Трассировка исполнения

`qt.execTrace()` включает `vm.exec_trace` — каждый
statement помечает свою строку (`exec_last[line]=now`),
подсветка в viz через `qt.vizHits(viz, qt.execTrace())`.
ВНИМАНИЕ: при включённой трассировке JIT-вызов обходится —
иначе построчность потеряется.

## ВАЖНО: транзакционные вызовы

Вызовы функций в nlang ведут себя «как в py» — записи в
глобальные переменные внутри функции **откатываются** на
выходе (`vm.lock`/`unlock`, `execFunctionBody`). Поэтому
состояние приложения нужно держать в **dict**:

```nlang
dict st = {"idx": -1, "playing": false}
void on_play()
    st["idx"] = 0      # переживёт выход из функции
    # idx = 0          # НЕ переживёт — rollback!
```

Мутации содержимого dict/list живут — откатывается только
привязка имён. Поэтому `files.push(p)` работает, а
`idx = 0` — нет.

См. `test/qtplayerui.nlc` — эталонный пример.
