// Модуль qt — реализация на Qt6 Widgets.
//
// Как устроено простым языком:
//  - один глобальный QApplication создаётся при первом окне;
//  - каждый виджет получает номер (хендл) в таблице `widgets`;
//    все функции модуля работают по номеру — так nlang-сторона
//    держит виджеты как обычные числа;
//  - сигналы цепляются через QObject::connect на лямбды —
//    класс-обёртка с moc не нужен.
#include "nlm_qt.h"

// Qt есть только на десктопной сборке; без Qt файл
// компилируется пустым — хендлы qt.* не регистрируются.
#ifdef NLANG_HAS_QT

#include <QApplication>
#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QSlider>
#include <QListWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTimer>
#include <QObject>
#include <QSignalBlocker>
#include <QFileDialog>
#include <QPainter>
#include <cmath>

#include <map>

namespace {

// таблица хендлов -> указатели
std::map<long long, QWidget*>& widgets() {
    static std::map<long long, QWidget*> m;
    return m;
}
long long next_handle = 1;
QObject* ctx() {
    static QObject* o = new QObject;
    return o;
}

QApplication* app() {
    static QApplication* a = nullptr;
    if (!a) {
        static int argc = 1;
        static char nm[] = "nlang";
        static char* argv[] = { nm };
        a = new QApplication(argc, argv);
    }
    return a;
}

QWidget* w(long long h) {
    auto it = widgets().find(h);
    return it == widgets().end() ? nullptr : it->second;
}

template<class T> T* as(long long h) {
    return qobject_cast<T*>(w(h));
}

long long put(QWidget* p) {
    widgets()[next_handle] = p;
    return next_handle++;
}

// у виджета либо есть лэйаут (vbox/hbox), либо он внутри
// лэйаута родителя — ребёнок автоматом в него кладётся
void adopt(long long parent, QWidget* child) {
    QWidget* p = w(parent);
    if (p && p->layout())
        p->layout()->addWidget(child);
}

} // namespace

// ---------- окна и лэйауты ----------

long long nlqt_window(const std::string& title,
                      long long width, long long height) {
    app();
    auto* win = new QWidget;
    win->setWindowTitle(QString::fromStdString(title));
    win->resize((int)width, (int)height);
    return put(win);
}
long long nlqt_panel(long long parent) {
    auto* p = new QWidget;
    adopt(parent, p);
    return put(p);
}
void nlqt_vbox(long long handle) {
    QWidget* p = w(handle);
    if (p) p->setLayout(new QVBoxLayout);
}
void nlqt_hbox(long long handle) {
    QWidget* p = w(handle);
    if (p) p->setLayout(new QHBoxLayout);
}

// ---------- виджеты ----------

long long nlqt_label(long long parent, const std::string& text) {
    auto* l = new QLabel(QString::fromStdString(text));
    adopt(parent, l);
    return put(l);
}
long long nlqt_button(long long parent, const std::string& text) {
    auto* b = new QPushButton(QString::fromStdString(text));
    adopt(parent, b);
    return put(b);
}
// кнопка-иконка: QStyle::standardIcon на android рисует
// ТЁМНЫЕ иконки — на тёмном фоне их не видно. Поэтому
// юникод-глифы как текст — цвет задаётся стилем скрипта.
long long nlqt_ibtn(long long parent, const std::string& name) {
    static const std::map<std::string, const char*> ic = {
        {"play",  "\xE2\x96\xB6"},          // ▶
        {"pause", "\xE2\x8F\xB8"},          // ⏸
        {"stop",  "\xE2\x96\xA0"},          // ■
        {"next",  "\xE2\x96\xB6|"},         // ▶|
        {"prev",  "|\xE2\x97\x80"},         // |◀
        {"open",  "+"},
        {"del",   "\xF0\x9F\x97\x91"},      // 🗑
        {"close", "\xE2\x9C\x95"},          // ✕
    };
    auto it = ic.find(name);
    auto* b = new QPushButton(
        QString::fromUtf8(it != ic.end() ? it->second
                                         : name.c_str()));
    b->setToolTip(QString::fromStdString(name));
    b->setMinimumSize(54, 54);
    b->setStyleSheet(
        "QPushButton{font-size:22px;color:#ff6a45;"
        "background:#1c1c26;border:1px solid #2a1a12;"
        "border-radius:10px;}"
        "QPushButton:pressed{background:#ff5a33;color:#0a0503;}");
    adopt(parent, b);
    return put(b);
}
long long nlqt_slider(long long parent,
                      long long mn, long long mx, long long val) {
    auto* s = new QSlider(Qt::Horizontal);
    s->setRange((int)mn, (int)mx);
    s->setValue((int)val);
    adopt(parent, s);
    return put(s);
}
long long nlqt_playlist(long long parent) {
    auto* l = new QListWidget;
    // длинные имена — перенос на следующую строку
    l->setWordWrap(true);
    l->setTextElideMode(Qt::ElideNone);
    adopt(parent, l);
    return put(l);
}

// ---------- свойства ----------

void nlqt_items(long long list,
                const std::vector<std::string>& items) {
    if (auto* l = as<QListWidget>(list)) {
        // перезаливка списка не должна дёргать on_select —
        // иначе первый трек сам стартует при refresh
        QSignalBlocker block(l);
        l->clear();
        for (auto& s : items)
            l->addItem(QString::fromStdString(s));
    }
}
long long nlqt_row(long long list) {
    if (auto* l = as<QListWidget>(list)) return l->currentRow();
    return -1;
}
void nlqt_setRow(long long list, long long row) {
    if (auto* l = as<QListWidget>(list)) l->setCurrentRow((int)row);
}
void nlqt_setText(long long handle, const std::string& text) {
    QString t = QString::fromStdString(text);
    if (auto* l = as<QLabel>(handle)) { l->setText(t); return; }
    if (auto* b = as<QPushButton>(handle)) { b->setText(t); return; }
}
long long nlqt_value(long long handle) {
    if (auto* s = as<QSlider>(handle)) return s->value();
    return 0;
}
void nlqt_setValue(long long handle, long long v) {
    if (auto* s = as<QSlider>(handle))
        // пока палец тащит ручку — тикер не перезаписывает,
        // иначе драг срывается и seek «не работает»
        if (!s->isSliderDown()) s->setValue((int)v);
}
void nlqt_setStyle(long long handle, const std::string& css) {
    QWidget* p = w(handle);
    if (p) p->setStyleSheet(QString::fromStdString(css));
}
void nlqt_show(long long handle) {
    QWidget* p = w(handle);
    if (p) {
        p->show();
        // без активации окно под андроидом не получает
        // мышь/тач/клаву — события уходят старому окну
        p->raise();
        p->activateWindow();
    }
}
void nlqt_close(long long handle) {
    QWidget* p = w(handle);
    if (p) p->close();
}
long long nlqt_exec() {
    return app()->exec();
}

// ---------- сигналы ----------

void nlqt_onClick(long long handle, std::function<void()> fn) {
    if (auto* b = as<QPushButton>(handle))
        QObject::connect(b, &QPushButton::clicked, ctx(),
                         [fn](bool){ fn(); });
}
void nlqt_onChange(long long handle,
                   std::function<void(long long)> fn) {
    if (auto* s = as<QSlider>(handle))
        QObject::connect(s, &QSlider::valueChanged, ctx(),
                         [fn](int v){ fn(v); });
}
void nlqt_onSelect(long long handle,
                   std::function<void(long long)> fn) {
    if (auto* l = as<QListWidget>(handle))
        QObject::connect(l, &QListWidget::currentRowChanged,
                         ctx(), [fn](int row){ fn(row); });
}
void nlqt_onTimer(long long ms, std::function<void()> fn) {
    auto* t = new QTimer(ctx());
    QObject::connect(t, &QTimer::timeout, ctx(),
                     [fn](void){ fn(); });
    t->start((int)ms);
}
// seek-слайдер: один вызов на ОТПУСКАНИИ пальца —
// valueChanged в процессе драга долбил seekTo каждый тик
// и заикал воспроизведение.
void nlqt_onSeek(long long handle,
                 std::function<void(long long)> fn) {
    if (auto* s = as<QSlider>(handle))
        QObject::connect(s, &QAbstractSlider::sliderReleased,
                         ctx(), [s, fn]{ fn(s->value()); });
}
// программный клик — нужен для автотестов UI
void nlqt_click(long long handle) {
    if (auto* b = as<QPushButton>(handle)) b->click();
}

// ---------- файловый диалог ----------

std::vector<std::string> nlqt_openFiles(const std::string& title,
                                        const std::string& filter) {
    QStringList fs;
#ifdef __ANDROID__
    // SAF-проводник: name-filter мапится в mime и всё
    // кроме аудио-мима серое. На андроиде просим */* —
    // юзер и так понимает что выбрать.
    QFileDialog dlg(app()->activeWindow(),
                    QString::fromStdString(title));
    dlg.setFileMode(QFileDialog::ExistingFiles);
    dlg.setMimeTypeFilters({QStringLiteral("*/*")});
    if (dlg.exec() == QDialog::Accepted)
        fs = dlg.selectedFiles();
#else
    // parent = активное окно — диалог не уходит под главное
    fs = QFileDialog::getOpenFileNames(
        app()->activeWindow(), QString::fromStdString(title),
        QString(), QString::fromStdString(filter));
#endif
    std::vector<std::string> r;
    for (auto& f : fs) r.push_back(f.toStdString());
    return r;
}

// ---------- эквалайзер + «проходка кода» ----------

// VizWidget — полосы + печать кода, РАБОТАЮЩЕГО в цикле
// воспроизведения. Печать синхронизирована с позицией:
// codePos растёт ровно на дельту posMs × cps — стоп на
// паузе, отскок на seek. Текст — реальный код, который
// исполняется: nlang-хэндлеры + eval + звук + jit.
class VizWidget : public QWidget {
public:
    int bars = 24;
    bool active = false;
    double level = 0.6;              // 0..1
    std::vector<double> heights, phase, speed;
    double t = 0.0;
    QStringList codeLines;           // исходник плеера (nlang)
    int codePos = 0;
    QStringList infoLines;           // живые числа
    // реальный спектр трека (Goertzel-полосы из PCM) —
    // если задан, полосы показывают его, а не sin-анимацию
    std::vector<double> wave;
    // трассировка: строка -> сколько мс назад исполнялась
    QMap<int, double> hits;
    int freshLine = -1;              // самая свежая исполненная
    double scrollCur = -1;           // плавная прокрутка
    double waveLambda = 0;           // λ доминирующей частоты, м

    void setHits(const QMap<int, double>& h) {
        hits = h;
        freshLine = -1;
        double best = 1e18;
        for (auto it = h.begin(); it != h.end(); ++it)
            if (it.value() < best) { best = it.value(); freshLine = it.key(); }
    }
    void setLambda(double m) { waveLambda = m; }

    explicit VizWidget(int n, QWidget* p = nullptr)
        : QWidget(p), bars(n) {
        heights.assign(bars, 0.0);
        phase.resize(bars);
        speed.resize(bars);
        for (int i = 0; i < bars; ++i) {
            phase[i] = i * 0.7;
            speed[i] = 0.8 + (i % 5) * 0.35;
        }
        setMinimumHeight(110);
        auto* tm = new QTimer(this);
        connect(tm, &QTimer::timeout, this, [this] {
            t += 0.05;
            for (int i = 0; i < bars; ++i) {
                double target = 0.0;
                if (active) {
                    if ((int)wave.size() == bars)
                        // реальный спектр играющего трека
                        target = 0.1 + wave[i] * 0.9;
                    else
                        target = (0.15 + level * 0.85) *
                            std::fabs(std::sin(t * speed[i] +
                                               phase[i]));
                }
                heights[i] += (target - heights[i]) * 0.25;
            }
            // плавная прокрутка к свежей строке —
            // без скачков при «прыжках» кода
            if (freshLine >= 0) {
                if (scrollCur < 0) scrollCur = freshLine;
                scrollCur += (freshLine - scrollCur) * 0.18;
            }
            update();
        });
        tm->start(40);
    }

    void setCode(const QString& text) {
        codeLines = text.split('\n');
        codePos = 0;
    }
    void setInfo(const QStringList& l) { infoLines = l; }
    // позиция трека хранится — печать шла по ней, теперь
    // позицию движет трассировка exec_last; оставлено для API
    void setPosMs(long long ms) { (void)ms; }

    // суммарная длина кода с переносами — для цикла печати
    int codeTotal() const {
        int n = 0;
        for (auto& s : codeLines) n += s.size() + 1;
        return n;
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor(10, 10, 20));

        // --- код: окно вокруг свежей исполненной строки;
        // строка светится по давности исполнения ---
        if (!codeLines.isEmpty()) {
            // высота строки — от высоты виджета: 10px на
            // телефоне (high-dpi) — микроскоп. ~1/16 высоты.
            int mono_h = qMax(16, height() / 14);
            int show = qMax(1, (height() - 40) / mono_h);
            int anchor = scrollCur >= 0 ? (int)scrollCur
                        : (freshLine >= 0 ? freshLine : 0);
            int first = qMax(0, anchor - show / 2);
            int last = qMin(codeLines.size(), first + show);
            if (last - first < show) first = qMax(0, last - show);
            QFont f("Monospace");
            f.setStyleHint(QFont::TypeWriter);
            f.setPixelSize(mono_h - 5);
            p.setFont(f);
            int y = mono_h;
            for (int i = first; i < last; ++i, y += mono_h) {
                double age = hits.value(i, 1e9);   // line — 0-based
                int glow = 0;
                if (age < 1500)
                    glow = 255 - (int)(age / 1500.0 * 200.0);
                if (i == freshLine) {
                    p.fillRect(0, y - 10, width(), mono_h,
                               QColor(255, 90, 51, 70));
                    p.setPen(QColor(255, 110, 70));
                    p.drawText(6, y, codeLines[i]);
                } else if (glow > 0) {
                    p.setPen(QColor(80 + glow / 2,
                                    80 + glow / 5,
                                    120 + glow / 3));
                    p.drawText(6, y, codeLines[i]);
                } else {
                    p.setPen(QColor(70, 75, 105, 190));
                    p.drawText(6, y, codeLines[i]);
                }
            }
        }

        // --- живые числа ---
        if (!infoLines.isEmpty()) {
            QFont f("Consolas"); f.setPixelSize(11);
            f.setBold(true);
            p.setFont(f);
            p.setPen(QColor(255, 90, 51));
            int y = 14;
            for (auto& s : infoLines) {
                p.drawText(width() - 8 - (int)s.size() * 7, y, s);
                y += 13;
            }
        }

        // --- полосы эквалайзера ---
        double bw = width() / (double)bars;
        double base = height() - 26;
        for (int i = 0; i < bars; ++i) {
            double h = heights[i] * 22;
            int r = 255, g = 90 + (int)(heights[i] * 60), b = 40;
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(r, g, b, 210));
            double x = i * bw + 2;
            p.drawRoundedRect(QRectF(x, base - h, bw - 4, h), 2, 2);
        }

        // --- длина волны: синус с периодом ∝ λ (метры),
        // бегущий по нижнему краю; длиннее λ -> шире волна
        if (waveLambda > 0.01) {
            double period = 14.0 + waveLambda * 26.0;
            if (period > width()) period = width();
            double amp = 16.0;
            double yc = base + 13;
            p.setPen(QPen(QColor(120, 180, 255, 220), 2));
            QPointF prev;
            for (int x = 0; x <= width(); x += 3) {
                double yy = yc +
                    amp * std::sin(x / period * 2.0 * M_PI -
                                   t * 4.0);
                QPointF pt(x, yy);
                if (x > 0) p.drawLine(prev, pt);
                prev = pt;
            }
        }
    }
    static int fm_w(const QPainter& p) {
        return p.fontMetrics().horizontalAdvance("M");
    }
};

long long nlqt_viz(long long parent, long long bars) {
    auto* v = new VizWidget((int)bars);
    adopt(parent, v);
    return put(v);
}
void nlqt_vizActive(long long handle, bool active) {
    auto* v = dynamic_cast<VizWidget*>(w(handle));
    if (v) v->active = active;
}
void nlqt_vizLevel(long long handle, long long level) {
    auto* v = dynamic_cast<VizWidget*>(w(handle));
    if (v) v->level = level / 100.0;
}
void nlqt_vizCode(long long handle, const std::string& text) {
    auto* v = dynamic_cast<VizWidget*>(w(handle));
    if (v) v->setCode(QString::fromUtf8(text));
}
void nlqt_vizInfo(long long handle,
                  const std::vector<std::string>& lines) {
    auto* v = dynamic_cast<VizWidget*>(w(handle));
    if (!v) return;
    QStringList l;
    for (auto& s : lines) l << QString::fromStdString(s);
    v->setInfo(l);
}
void nlqt_vizPos(long long handle, long long posMs) {
    auto* v = dynamic_cast<VizWidget*>(w(handle));
    if (v) v->setPosMs(posMs);
}
// реальный спектр (число полос = числу bars виджета)
void nlqt_vizWave(long long handle,
                  const std::vector<double>& bands) {
    auto* v = dynamic_cast<VizWidget*>(w(handle));
    if (v) v->wave = bands;
}
// длина волны доминирующей частоты, метры
void nlqt_vizWaveLen(long long handle, double meters) {
    auto* v = dynamic_cast<VizWidget*>(w(handle));
    if (v) v->setLambda(meters);
}
// строки, исполнявшиеся недавно: (line, age_ms)
void nlqt_vizHits(long long handle,
                  const std::vector<std::pair<long long,
                                  double>>& pairs) {
    auto* v = dynamic_cast<VizWidget*>(w(handle));
    if (!v) return;
    QMap<int, double> h;
    for (auto& p : pairs) h[(int)p.first] = p.second;
    v->setHits(h);
}


#endif // NLANG_HAS_QT
