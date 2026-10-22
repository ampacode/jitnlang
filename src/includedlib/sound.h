#pragma once
// Дескриптор модуля sound + eval-регистратор.
#include "../module.h"
#include "../nvm.h"
#include "../../modules/nlm_sound.h"

namespace includedlib {

inline void register_sound(Nvm& vm, const std::string& alias) {
    auto d = std::make_shared<std::map<std::string, NObject>>();
    auto add = [&](const char* n,
                   std::function<NObject(Nvm&, const NArgs&)> f) {
        auto fn = std::make_shared<NFunction>();
        fn->name = n;
        fn->stdimpl = std::move(f);
        (*d)[n] = NObject(fn);
    };
    add("beep", [](Nvm&, const NArgs& a) {
        nlsound_beep(a.pos.empty() ? 0 : a.pos[0].asDouble(),
                     a.pos.size() > 1 ? a.pos[1].asInt() : 0);
        return NObject(); });
    add("note", [](Nvm&, const NArgs& a) {
        nlsound_note(nstr(a.pos[0]).asStr(),
                     a.pos.size() > 1 ? a.pos[1].asInt() : 0);
        return NObject(); });
    add("nfreq", [](Nvm&, const NArgs& a) {
        return NObject(nlsound_freq(nstr(a.pos[0]).asStr())); });
    add("play", [](Nvm&, const NArgs& a) {
        nlsound_play(nstr(a.pos[0]).asStr());
        return NObject(); });
    add("stop", [](Nvm&, const NArgs&) {
        nlsound_stop();
        return NObject(); });
    // --- MCI-контроль текущего трека ---
    add("pause", [](Nvm&, const NArgs&) {
        nlsound_pause(); return NObject(); });
    add("resume", [](Nvm&, const NArgs&) {
        nlsound_resume(); return NObject(); });
    add("position", [](Nvm&, const NArgs&) {
        return NObject(nlsound_position()); });
    add("length", [](Nvm&, const NArgs&) {
        return NObject(nlsound_length()); });
    add("seek", [](Nvm&, const NArgs& a) {
        nlsound_seek(a.pos.empty() ? 0 : a.pos[0].asInt());
        return NObject(); });
    add("volume", [](Nvm&, const NArgs& a) {
        nlsound_volume(a.pos.empty() ? 50 : (int)a.pos[0].asInt());
        return NObject(); });
    add("mode", [](Nvm&, const NArgs&) {
        return NObject(nlsound_mode()); });
    add("wait", [](Nvm&, const NArgs& a) {
        nlsound_wait(a.pos.empty() ? 0 : a.pos[0].asInt());
        return NObject(); });
    // реальный спектр трека: nbins энергий 0..1 вокруг pos
    add("spectrum", [](Nvm&, const NArgs& a) {
        long long pos = a.pos.empty() ? 0 : a.pos[0].asInt();
        int bins = a.pos.size() > 1 ? (int)a.pos[1].asInt() : 24;
        auto l = std::make_shared<std::vector<NObject>>();
        for (double v : nlsound_spectrum(pos, bins))
            l->push_back(NObject(v));
        return NObject(l); });
    // доминирующая длина волны звука, метры (343/f)
    // скорость 1.0=норма, pitch по полутонам тоже через rate
    add("rate", [](Nvm&, const NArgs& a) {
        nlsound_rate(a.pos.empty() ? 1.0 : a.pos[0].asDouble());
        return NObject(); });
    // реверб-эхо 0..100 — второй WMP «мокрым» каналом,
    // на Android — PresetReverb на сессии плеера
    add("reverb", [](Nvm&, const NArgs& a) {
        nlsound_reverb(a.pos.empty() ? 0 : a.pos[0].asInt());
        return NObject(); });
    // тон в полутонах отдельно от скорости (Android
    // PlaybackParams.setPitch); на PC pitch идёт через rate
    add("pitch", [](Nvm&, const NArgs& a) {
        nlsound_pitch(a.pos.empty() ? 0.0 : a.pos[0].asDouble());
        return NObject(); });
    add("dbg", [](Nvm&, const NArgs&) {
#ifdef _WIN32
        auto& c = nlsound_pcm::cache();
        return NObject(std::string("reader=") +
            (c.rd ? "1" : "0") + " rate=" +
            std::to_string(c.rate));
#else
        return NObject(std::string("no pcm"));
#endif
    });
    add("wavelength", [](Nvm&, const NArgs& a) {
        long long pos = a.pos.empty() ? 0 : a.pos[0].asInt();
        return NObject(nlsound_wavelength(pos)); });
    vm.set(alias, NObject(d));
}

inline const ModuleDescriptor sound_desc = {
    "sound", "nlm_sound.h",
    {"nlmodules", "winmm", "strmiids", "quartz", "ole32",
     "oleaut32", "mfplat", "mfreadwrite", "mfuuid"},
    &register_sound, "sound", nullptr
};

} // namespace includedlib

