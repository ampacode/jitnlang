#pragma once
// Дескриптор модуля qt + eval-регистратор.
//
// eval-путь only: сгенерированный C++-код Qt не линкует
// (qt — интерфейс интерпретатора, см. docs).
#include "../module.h"
#include "../nvm.h"
#include "../../modules/nlm_qt.h"
#include "str.h" // sarg

namespace includedlib {

// Вызов nlang-функции из Qt-слота: исключения не должны
// улетать внутрь Qt — ловим и показываем. Иначе падал весь
// процесс «включиться» переставало — и не видно почему.
inline void qt_call(Nvm& vm, const NObject& fn, NArgs& args) {
    try {
        fn.call(vm, args);
    } catch (const NLangError& e) {
        fprintf(stderr, "[qt] %s (line %d)\n", e.msg.c_str(), e.line);
    } catch (const std::exception& e) {
        fprintf(stderr, "[qt] %s\n", e.what());
    }
}

inline void register_qt(Nvm& vm, const std::string& alias) {
    auto d = std::make_shared<std::map<std::string, NObject>>();
    auto add = [&](const char* n,
                   std::function<NObject(Nvm&, const NArgs&)> f) {
        auto fn = std::make_shared<NFunction>();
        fn->name = n;
        fn->stdimpl = std::move(f);
        (*d)[n] = NObject(fn);
    };
    auto num = [](const NArgs& a, size_t i, long long d = 0) {
        return a.pos.size() > i ? a.pos[i].asInt() : d;
    };
    add("window", [](Nvm&, const NArgs& a) {
        return NObject(nlqt_window(sarg(a,0), a.pos.size()>1?a.pos[1].asInt():600,
                                   a.pos.size()>2?a.pos[2].asInt():400)); });
    add("panel", [num](Nvm&, const NArgs& a) {
        return NObject(nlqt_panel(num(a,0))); });
    add("vbox", [num](Nvm&, const NArgs& a) {
        nlqt_vbox(num(a,0)); return NObject(); });
    add("hbox", [num](Nvm&, const NArgs& a) {
        nlqt_hbox(num(a,0)); return NObject(); });
    add("label", [](Nvm&, const NArgs& a) {
        return NObject(nlqt_label(a.pos[0].asInt(), sarg(a,1))); });
    add("button", [](Nvm&, const NArgs& a) {
        return NObject(nlqt_button(a.pos[0].asInt(), sarg(a,1))); });
    add("ibtn", [](Nvm&, const NArgs& a) {
        return NObject(nlqt_ibtn(a.pos[0].asInt(), sarg(a,1))); });
    add("slider", [](Nvm&, const NArgs& a) {
        return NObject(nlqt_slider(a.pos[0].asInt(),
            a.pos.size()>1?a.pos[1].asInt():0,
            a.pos.size()>2?a.pos[2].asInt():100,
            a.pos.size()>3?a.pos[3].asInt():50)); });
    add("playlist", [num](Nvm&, const NArgs& a) {
        return NObject(nlqt_playlist(num(a,0))); });
    add("items", [num](Nvm&, const NArgs& a) {
        std::vector<std::string> items;
        if (a.pos.size() > 1 && a.pos[1].isList())
            for (auto& v : *a.pos[1].asList())
                items.push_back(nstr(v).asStr());
        nlqt_items(num(a,0), items);
        return NObject(); });
    add("row", [num](Nvm&, const NArgs& a) {
        return NObject(nlqt_row(num(a,0))); });
    add("setRow", [num](Nvm&, const NArgs& a) {
        nlqt_setRow(num(a,0), num(a,1)); return NObject(); });
    add("setText", [num](Nvm&, const NArgs& a) {
        nlqt_setText(num(a,0), sarg(a,1)); return NObject(); });
    add("value", [num](Nvm&, const NArgs& a) {
        return NObject(nlqt_value(num(a,0))); });
    add("setValue", [num](Nvm&, const NArgs& a) {
        nlqt_setValue(num(a,0), num(a,1)); return NObject(); });
    add("setStyle", [num](Nvm&, const NArgs& a) {
        nlqt_setStyle(num(a,0), sarg(a,1)); return NObject(); });
    add("show", [num](Nvm&, const NArgs& a) {
        nlqt_show(num(a,0)); return NObject(); });
    add("close", [num](Nvm&, const NArgs& a) {
        nlqt_close(num(a,0)); return NObject(); });
    add("exec", [](Nvm&, const NArgs&) {
        return NObject(nlqt_exec()); });

    // --- сигналы: fn — nlang-функция, зовётся fn.call ---
    add("click", [num](Nvm&, const NArgs& a) {
        nlqt_click(num(a,0)); return NObject(); });
    add("onClick", [num](Nvm& vm, const NArgs& a) {
        NObject fn = a.pos[1];
        nlqt_onClick(num(a,0), [&vm, fn]() mutable {
            NArgs args; qt_call(vm, fn, args); });
        return NObject(); });
    add("onChange", [num](Nvm& vm, const NArgs& a) {
        NObject fn = a.pos[1];
        nlqt_onChange(num(a,0), [&vm, fn](long long v) mutable {
            NArgs args; args.pos.push_back(NObject(v));
            qt_call(vm, fn, args); });
        return NObject(); });
    add("onSelect", [num](Nvm& vm, const NArgs& a) {
        NObject fn = a.pos[1];
        nlqt_onSelect(num(a,0), [&vm, fn](long long row) mutable {
            NArgs args; args.pos.push_back(NObject(row));
            qt_call(vm, fn, args); });
        return NObject(); });
    add("onTimer", [num](Nvm& vm, const NArgs& a) {
        NObject fn = a.pos[1];
        nlqt_onTimer(num(a,0), [&vm, fn]() mutable {
            NArgs args; qt_call(vm, fn, args); });
        return NObject(); });
    // один вызов по отпусканию слайдера — для seek
    add("onSeek", [num](Nvm& vm, const NArgs& a) {
        NObject fn = a.pos[1];
        nlqt_onSeek(num(a,0), [&vm, fn](long long v) mutable {
            NArgs args; args.pos.push_back(NObject(v));
            qt_call(vm, fn, args); });
        return NObject(); });
    add("openFiles", [](Nvm&, const NArgs& a) {
        auto l = std::make_shared<std::vector<NObject>>();
        for (auto& s : nlqt_openFiles(
                sarg(a,0), a.pos.size() > 1 ? sarg(a,1) : "* (*.*)"))
            l->push_back(NObject(s));
        return NObject(l); });
    add("viz", [num](Nvm&, const NArgs& a) {
        return NObject(nlqt_viz(num(a,0), num(a,1, 24))); });
    add("vizActive", [num](Nvm&, const NArgs& a) {
        nlqt_vizActive(num(a,0),
            a.pos.size() > 1 && a.pos[1].asInt() != 0);
        return NObject(); });
    add("vizLevel", [num](Nvm&, const NArgs& a) {
        nlqt_vizLevel(num(a,0), num(a,1, 60)); return NObject(); });
    add("vizCode", [num](Nvm&, const NArgs& a) {
        nlqt_vizCode(num(a,0), sarg(a,1)); return NObject(); });
    add("vizInfo", [num](Nvm&, const NArgs& a) {
        std::vector<std::string> l;
        if (a.pos.size() > 1 && a.pos[1].isList())
            for (auto& x : *a.pos[1].asList()) l.push_back(nstr(x).asStr());
        nlqt_vizInfo(num(a,0), l); return NObject(); });
    add("vizPos", [num](Nvm&, const NArgs& a) {
        nlqt_vizPos(num(a,0), num(a,1)); return NObject(); });
    // трассировка исполнения: включает vm.exec_trace и
    // возвращает [[line, age_ms], ...] — какие строки
    // скрипта исполнялись недавно
    add("execTrace", [](Nvm& vm, const NArgs&) {
        vm.exec_trace = true;
        auto l = std::make_shared<std::vector<NObject>>();
        long long now = Nvm::exec_now_ms();
        // снимок без итерации по живой мапе: реентрантные
        // вызовы (модальный диалог -> pump -> тик) пишут
        // в exec_last и ломали бы итератор рехешем.
        // После сбора сливаем свежие метки обратно.
        auto snap = std::move(vm.exec_last);
        vm.exec_last.clear();
        for (auto& kv : snap) {
            auto pair = std::make_shared<std::vector<NObject>>();
            pair->push_back(NObject((long long)kv.first));
            pair->push_back(NObject((double)(now - kv.second)));
            l->push_back(NObject(pair));
            auto it = vm.exec_last.find(kv.first);
            if (it == vm.exec_last.end() || it->second < kv.second)
                vm.exec_last[kv.first] = kv.second;
        }
        return NObject(l); });
    add("vizHits", [num](Nvm&, const NArgs& a) {
        std::vector<std::pair<long long, double>> pairs;
        if (a.pos.size() > 1 && a.pos[1].isList())
            for (auto& x : *a.pos[1].asList())
                if (x.isList() && x.asList()->size() >= 2)
                    pairs.emplace_back(
                        (*x.asList())[0].asInt(),
                        (*x.asList())[1].asDouble());
        nlqt_vizHits(num(a,0), pairs);
        return NObject(); });
    // реальный спектр музыки в полосы эквалайзера
    add("vizWave", [num](Nvm&, const NArgs& a) {
        std::vector<double> bands;
        if (a.pos.size() > 1 && a.pos[1].isList())
            for (auto& x : *a.pos[1].asList())
                bands.push_back(x.asDouble());
        nlqt_vizWave(num(a,0), bands);
        return NObject(); });
    add("vizWaveLen", [num](Nvm&, const NArgs& a) {
        nlqt_vizWaveLen(num(a,0),
            a.pos.size() > 1 ? a.pos[1].asDouble() : 0.0);
        return NObject(); });

    vm.set(alias, NObject(d));
}

inline const ModuleDescriptor qt_desc = {
    "qt", "", {"nlmodules"}, &register_qt, "qt", nullptr
};

} // namespace includedlib
