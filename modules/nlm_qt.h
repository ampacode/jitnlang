#pragma once
// Модуль qt — окна и виджеты Qt6 для nlang (eval-путь).
//
// Виджеты адресуются целочисленными хендлами:
//   w = qt.window("title", 560, 420)
//   qt.vbox(w); l = qt.label(w, "hi"); qt.show(w); qt.exec()
//
// Сигналы принимают std::function — eval-слой
// (includedlib/qt.h) заворачивает в них NObject-функцию.
#include <string>
#include <vector>
#include <functional>

long long nlqt_window(const std::string& title,
                      long long w, long long h);
long long nlqt_panel(long long parent);
void      nlqt_vbox(long long handle);
void      nlqt_hbox(long long handle);
long long nlqt_label(long long parent, const std::string& text);
long long nlqt_button(long long parent, const std::string& text);
long long nlqt_ibtn(long long parent, const std::string& name);
long long nlqt_slider(long long parent,
                      long long min, long long max, long long val);
long long nlqt_playlist(long long parent);

void        nlqt_items(long long list,
                       const std::vector<std::string>& items);
long long   nlqt_row(long long list);
void        nlqt_setRow(long long list, long long row);
void        nlqt_setText(long long handle, const std::string& text);
long long   nlqt_value(long long handle);
void        nlqt_setValue(long long handle, long long v);
void        nlqt_setStyle(long long handle, const std::string& css);
void        nlqt_show(long long handle);
void        nlqt_close(long long handle);
long long   nlqt_exec();

// сигналы
void nlqt_onClick(long long handle, std::function<void()> fn);
void nlqt_onChange(long long handle, std::function<void(long long)> fn);
void nlqt_onSelect(long long handle, std::function<void(long long)> fn);
void nlqt_onTimer(long long ms, std::function<void()> fn);
void nlqt_onSeek(long long handle, std::function<void(long long)> fn);

// файловый диалог: фильтр формата "a (*.x *.y);;b (*.*)"
std::vector<std::string> nlqt_openFiles(const std::string& title,
                                        const std::string& filter);

// эквалайзер-виджет: полосы, анимируются сами (внутренний таймер).
// active — идёт воспроизведение; level 0..100 — общая энергия
// (громкость/позиция)
long long nlqt_viz(long long parent, long long bars);
void      nlqt_vizActive(long long handle, bool active);
void      nlqt_vizLevel(long long handle, long long level);
// vizCode � ����� ��������� ���� (�������� ��������������),
// vizInfo � ������ ����� � ������ �������
void      nlqt_vizCode(long long handle, const std::string& text);
void      nlqt_vizInfo(long long handle,
                       const std::vector<std::string>& lines);
void nlqt_click(long long handle);
void      nlqt_vizPos(long long handle, long long posMs);
void      nlqt_vizHits(long long handle, const std::vector<std::pair<long long,double>>& pairs);
void      nlqt_vizWave(long long handle, const std::vector<double>& bands);
void      nlqt_vizWaveLen(long long handle, double meters);
