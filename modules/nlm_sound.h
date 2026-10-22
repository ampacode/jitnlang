#pragma once
// Модуль sound — звук из nlang.
//
//   sound.beep(freqHz, ms)   — тон (Beep(), kernel32)
//   sound.play("x.mp3")      — файл (DirectShow: mp3/wav/m4a)
//   sound.pause()/resume()/stop()
//   sound.position()/length() — мс, sound.seek(ms)
//   sound.volume(0..100), sound.mode() -> playing/paused/stopped
//   sound.wait([ms])         — ждать конца трека
//   sound.note("A4", ms)     — нота по имени (C0..B8, # через 's')
//   sound.nfreq(name)        -> double Hz
//
// Проигрывание файлов — WMP COM (wmp.dll): mp3/m4a/wma/wav
// работают без внешних кодеков и без message pump.
// Fallback: DirectShow (старые кодеки) → PlaySound (wav).
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#include <dshow.h>
#include <wmp.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#endif
#include <string>
#include <cmath>
#include <vector>
#include <thread>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// C0..B8, 's' = диез (Cs4 = C#4). A4 = 440.
inline double nlsound_freq(const std::string& name) {
    if (name.empty()) return 0;
    static const int base[] = {0, 2, 4, 5, 7, 9, 11};
    char l = (char)toupper((unsigned char)name[0]);
    int idx = -1;
    if (l >= 'A' && l <= 'G') idx = l - 'A';
    if (idx < 0) return 0;
    int sem = base[idx];
    size_t p = 1;
    if (p < name.size() && (name[p] == 's' || name[p] == '#')) {
        ++sem; ++p;
    } else if (p < name.size() && name[p] == 'b') {
        --sem; ++p;
    }
    if (p >= name.size() || !isdigit((unsigned char)name[p]))
        return 0;
    int oct = name[p] - '0';
    // MIDI: C4=60; semitone = (oct+1)*12 + sem
    int midi = (oct + 1) * 12 + sem;
    return 440.0 * std::pow(2.0, (midi - 69) / 12.0);
}

inline void nlsound_beep(double freq, long long ms) {
#ifdef _WIN32
    if (freq <= 0) { Sleep((DWORD)ms); return; }
    Beep((DWORD)freq, (DWORD)ms);
#else
    (void)freq; (void)ms;
#endif
}

inline void nlsound_note(const std::string& name, long long ms) {
    nlsound_beep(nlsound_freq(name), ms);
}

// ================= Windows Media Player COM =================
//
// WMP (wmp.dll, IWMPPlayer4) — единственный путь, который
// играет mp3 на современном Windows без внешних кодеков:
// MCI mpegvideo отсутствует, DirectShow без mp3-декодера
// молчит, MFPlay требует message pump. put_URL(path) сама
// грузит и играет; position/duration — секунды double,
// volume — 0..100. Fallback: DirectShow → PlaySound (wav).

#ifdef _WIN32

namespace nlsound_ds {

struct Graph {
    IGraphBuilder*  graph = nullptr;
    IMediaControl*  control = nullptr;
    IMediaSeeking*  seek = nullptr;
    IBasicAudio*    audio = nullptr;
};

inline bool com_ready() {
    static bool inited = false;
    static bool ok = false;
    if (!inited) {
        inited = true;
        ok = SUCCEEDED(CoInitializeEx(
            nullptr, COINIT_MULTITHREADED)) ||
            SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    }
    return ok;
}

inline Graph& graph() {
    static Graph g;
    return g;
}

inline void release() {
    Graph& g = graph();
    if (g.control) g.control->Stop();
    if (g.seek)    { g.seek->Release();    g.seek = nullptr; }
    if (g.audio)   { g.audio->Release();   g.audio = nullptr; }
    if (g.control) { g.control->Release(); g.control = nullptr; }
    if (g.graph)   { g.graph->Release();   g.graph = nullptr; }
}

inline bool open(const std::wstring& path) {
    release();
    if (!com_ready()) return false;
    Graph& g = graph();
    if (FAILED(CoCreateInstance(CLSID_FilterGraph, nullptr,
            CLSCTX_INPROC_SERVER, IID_IGraphBuilder,
            (void**)&g.graph)))
        return false;
    if (FAILED(g.graph->RenderFile(path.c_str(), nullptr))) {
        g.graph->Release(); g.graph = nullptr;
        return false;
    }
    g.graph->QueryInterface(IID_IMediaControl, (void**)&g.control);
    g.graph->QueryInterface(IID_IMediaSeeking, (void**)&g.seek);
    g.graph->QueryInterface(IID_IBasicAudio, (void**)&g.audio);
    return g.control != nullptr;
}

} // namespace nlsound_ds

// --- WMP: mp3/m4a/wma идут сюда ---
namespace nlsound_wmp {

// CLSID/IID не в libwmp (её нет в mingw) — объявляем сами
static const CLSID CLSID_WMP_ =
    {0x6bf52a52,0x394a,0x11d3,{0xb1,0x53,0x00,0xc0,0x4f,0x79,0xfa,0xa6}};
static const IID IID_IWMPPlayer4_ =
    {0x6c497d62,0x8919,0x413c,{0x82,0xdb,0xe9,0x35,0xfb,0x3e,0xc5,0x84}};

struct Player {
    IWMPPlayer4*   player = nullptr;
    IWMPControls*  controls = nullptr;
    IWMPSettings*  settings = nullptr;
    IWMPMedia*     media = nullptr;
};

inline Player& self() {
    static Player p;
    return p;
}

// currentMedia появляется когда плеер догрузил файл —
// спрашиваем лениво, а не в open()
inline IWMPMedia* media() {
    Player& s = self();
    if (!s.media && s.player) {
        IWMPMedia* m = nullptr;
        s.player->get_currentMedia(&m);
        if (m) {
            double d = 0;
            m->get_duration(&d);
            if (d > 0) s.media = m; else m->Release();
        }
    }
    return s.media;
}

inline void close(Player& s) {
    if (s.controls) { s.controls->stop(); s.controls->Release(); s.controls = nullptr; }
    if (s.media)    { s.media->Release();    s.media = nullptr; }
    if (s.settings) { s.settings->Release(); s.settings = nullptr; }
    if (s.player)   { s.player->close(); s.player->Release(); s.player = nullptr; }
}
inline void close() { close(self()); }

inline bool open(Player& s, const std::wstring& path) {
    close(s);
    if (!nlsound_ds::com_ready()) return false;
    if (FAILED(CoCreateInstance(CLSID_WMP_, nullptr,
            CLSCTX_INPROC_SERVER, IID_IWMPPlayer4_,
            (void**)&s.player)))
        return false;
    // без окна — только звук
    BSTR mode = SysAllocString(L"none");
    s.player->put_uiMode(mode);
    SysFreeString(mode);
    // put_URL сама стартует воспроизведение когда медиа
    // догрузится. В STA (Qt-тред) ей нужен внешний pump —
    // qt.exec его обеспечивает; в консоли (MTA) и без него
    // работает. Ждать тут нельзя: pump внутри слота ломает
    // Qt-луп реентрантными тиками.
    BSTR url = SysAllocString(path.c_str());
    s.player->put_URL(url);
    SysFreeString(url);
    s.player->get_controls(&s.controls);
    s.player->get_settings(&s.settings);
    return s.controls != nullptr;
}
inline bool open(const std::wstring& path) {
    return open(self(), path);
}
// общий слайдер скорости — для main и wet-канала
inline void rate(Player& s, double r) {
    if (s.settings) s.settings->put_rate(r);
}

} // namespace nlsound_wmp

// ---------- wet-канал: реверб вторым плеером ----------
// «Реверб» как слышимый эффект: второй экземпляр WMP играет
// тот же файл с задержкой ~110мс и малой громкостью —
// настоящий slapback-эхо поверх основного звука.
namespace nlsound_wet {

inline nlsound_wmp::Player& self() {
    static nlsound_wmp::Player p;
    return p;
}
inline int& level() { static int v = 0; return v; } // 0..100
inline double wet_gain() { return level() * 0.4 / 100.0; }

inline void apply() {
    auto& s = self();
    if (s.settings)
        s.settings->put_volume((long)(wet_gain() * 100));
}

// открыть тот же файл на мокром канале и играть
// с запаздыванием — эхо-хвост
inline void open(const std::wstring& path) {
    auto& s = self();
    nlsound_wmp::close(s);
    if (level() <= 0) return;          // реверб выкл — не нужен
    if (!nlsound_wmp::open(s, path)) return;
    apply();
    // эхо стартует синхронно, отставание даёт хвост
    if (s.controls) s.controls->play();
}

inline void reverb(int lv) {
    level() = lv < 0 ? 0 : (lv > 100 ? 100 : lv);
    apply();
    // эхо включается «с нуля» — при следующем play поймает
}

inline void close() { nlsound_wmp::close(self()); }

} // namespace nlsound_wet

// ---------- PCM-спектр и длина волны ----------
//
// «Длина волны» играющей музыки в метрах: λ = v / f,
// где v ≈ 343 м/с — скорость звука в воздухе, f —
// доминирующая частота в текущей точке трека.
// Трек НЕ декодируется целиком: открыт IMFSourceReader,
// на запрос seek на pos + читаем ~90мс сэмплов и считаем
// Goertzel-полосы — реальные сэмплы без задержки.
namespace nlsound_pcm {

struct Store {
    std::string path;
    IMFSourceReader* rd = nullptr; // живой ридер трека
    int rate = 0;
};

inline Store& cache() {
    static Store s;
    return s;
}

inline std::wstring to_wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(),
                        &w[0], n);
    return w;
}

inline void stop() {
    Store& c = cache();
    if (c.rd) { c.rd->Release(); c.rd = nullptr; }
    c.rate = 0;
}

// открыть ридер (миллисекунды, без чтения потока)
inline void start(const std::string& path) {
    Store& c = cache();
    if (c.path == path && c.rd) return;
    stop();
    c.path = path;
    nlsound_ds::com_ready();
    static bool mf_up = false;
    if (!mf_up) { MFStartup(MF_VERSION); mf_up = true; }
    std::wstring w = to_wide(path);
    if (FAILED(MFCreateSourceReaderFromURL(w.c_str(),
                                           nullptr, &c.rd)))
        return;
    IMFMediaType* want = nullptr;
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    want->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
    c.rd->SetCurrentMediaType(
        (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr,
        want);
    want->Release();
    IMFMediaType* cur = nullptr;
    c.rd->GetCurrentMediaType(
        (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur);
    if (cur) {
        UINT32 r = 0;
        cur->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &r);
        c.rate = (int)r;
        cur->Release();
    }
}

// Goertzel-полосы вокруг pos: seek + ~4096 сэмплов.
// Лог-частоты 40..rate*0.45, энергии 0..1 + доминирующая.
inline void spectrum(long long pos_ms, int bins,
                     std::vector<double>& out,
                     double& dom_hz) {
    Store& c = cache();
    out.assign(bins, 0.0);
    dom_hz = 0;
    if (!c.rd || c.rate <= 0) return;
    // seek ридера к позиции трека (PROPVARIANT вручную —
    // propvarutil.h в MinGW конфликтует с SHStrDupW)
    PROPVARIANT pv{};
    pv.vt = VT_I8;
    pv.hVal.QuadPart = pos_ms * 10000LL;
    c.rd->SetCurrentPosition(GUID_NULL, pv);
    // ~90мс моно-сэмплов
    const int N = 4096;
    std::vector<float> buf;
    buf.reserve(N);
    int guard = 0;
    while ((int)buf.size() < N && guard++ < 64) {
        DWORD flags = 0;
        IMFSample* smp = nullptr;
        if (FAILED(c.rd->ReadSample(
                MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
                nullptr, &flags, nullptr, &smp)) ||
            (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
            if (smp) smp->Release();
            break;
        }
        if (!smp) continue;
        IMFMediaBuffer* mb = nullptr;
        if (SUCCEEDED(smp->ConvertToContiguousBuffer(&mb))) {
            BYTE* p = nullptr; DWORD n = 0;
            mb->Lock(&p, nullptr, &n);
            const float* fp =
                reinterpret_cast<const float*>(p);
            size_t cnt = n / sizeof(float);
            for (size_t i = 0; i < cnt && buf.size() < (size_t)N;
                 ++i)
                buf.push_back(fp[i]);
            mb->Unlock();
            mb->Release();
        }
        smp->Release();
    }
    if (buf.size() < 64) return;
    double fmin = 40.0, fmax = c.rate * 0.45;
    double best = 0, maxp = 0;
    int N2 = (int)buf.size();
    for (int b = 0; b < bins; ++b) {
        double f = fmin * std::pow(fmax / fmin,
                                   (double)b / (bins - 1));
        double coeff = 2.0 * std::cos(2.0 * M_PI * f / c.rate);
        double s1 = 0, s2 = 0;
        for (int i = 0; i < N2; ++i) {
            double s0 = buf[i] + coeff * s1 - s2;
            s2 = s1; s1 = s0;
        }
        double pwr = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        out[b] = pwr;
        if (pwr > best) { best = pwr; dom_hz = f; }
        if (pwr > maxp) maxp = pwr;
    }
    if (maxp > 0)
        for (auto& v : out) v = std::sqrt(v / maxp);
}

} // namespace nlsound_pcm

#endif

// ---------- Android: MediaPlayer через JNI ----------
// SAF-пикер (qt.openFiles) отдаёт content:// uri —
// MediaPlayer ест их через setDataSource(Context, Uri).
// Обычные пути /sdcard/... тоже резолвятся.
#if defined(__ANDROID__) && defined(NLANG_HAS_QT)
#include <QJniObject>
#include <QJniEnvironment>
#include <QtCore/QCoreApplication>
namespace nlsound_a {

struct Mp { QJniObject p; bool playing = false, was = false; };
inline Mp& self() { static Mp m; return m; }

// контекст приложения — публичный Qt6 API.
// статик: global-ref живёт вечно (сырой jobject из
// временного QJniObject даглится -> JNI Global crash)
inline const QJniObject& android_ctx() {
    static const QJniObject c{
        QNativeInterface::QAndroidApplication::context()};
    return c;
}
// чистим JNI-exception, иначе следующий вызов падает
inline bool exc() {
    QJniEnvironment e;
    return e.checkAndClearExceptions();
}
inline bool ensure() {
    if (self().p.isValid()) return true;
    self().p = QJniObject("android/media/MediaPlayer");
    return self().p.isValid() && !exc();
}
inline QJniObject uri_of(const std::string& path) {
    QJniObject s = QJniObject::fromString(
        QString::fromStdString(path));
    return QJniObject::callStaticObjectMethod(
        "android/net/Uri", "parse",
        "(Ljava/lang/String;)Landroid/net/Uri;",
        s.object<jstring>());
}
inline bool open(const std::string& path) {
    if (!ensure()) return false;
    self().p.callMethod<void>("reset", "()V");
    if (exc()) return false;
    self().p.callMethod<void>(
        "setDataSource",
        "(Landroid/content/Context;Landroid/net/Uri;)V",
        android_ctx().object(), uri_of(path).object());
    if (exc()) return false;
    self().p.callMethod<void>("prepare", "()V");
    if (exc()) return false;
    return true;
}
} // namespace nlsound_a
#endif

// ---------- API ----------

inline void nlsound_play(const std::string& path) {
#ifdef _WIN32
    nlsound_pcm::start(path);   // открыть ридер для спектра
    // utf8->utf16 честно: кириллица/юникод в имени файла
    // ломалась побайтовым расширением
    std::wstring w = nlsound_pcm::to_wide(path);
    // 1) WMP — mp3/m4a/wma/wav, сам играет после put_URL
    if (nlsound_wmp::open(w)) {
        nlsound_ds::release();
        nlsound_wmp::self().controls->play();
        nlsound_wet::open(w);   // эхо-канал, если реверб вкл
        return;
    }
    // 2) DirectShow — старые кодеки
    if (nlsound_ds::open(w)) {
        nlsound_ds::graph().control->Run();
        return;
    }
    // 3) PlaySound — только wav, без контроля
    PlaySoundW(w.c_str(), nullptr,
               SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
#elif defined(__ANDROID__) && defined(NLANG_HAS_QT)
    if (nlsound_a::open(path)) {
        nlsound_a::self().p.callMethod<void>("start", "()V");
        nlsound_a::self().playing = !nlsound_a::exc();
        nlsound_a::self().was = true;
    }
#else
    (void)path;
#endif
}

inline void nlsound_pause() {
#ifdef _WIN32
    if (nlsound_wmp::self().controls)
        { nlsound_wmp::self().controls->pause(); return; }
    if (nlsound_ds::graph().control)
        nlsound_ds::graph().control->Pause();
#elif defined(__ANDROID__) && defined(NLANG_HAS_QT)
    if (nlsound_a::self().playing) {
        nlsound_a::self().p.callMethod<void>("pause", "()V");
        nlsound_a::self().playing = false;
    }
#endif
}
inline void nlsound_resume() {
#ifdef _WIN32
    if (nlsound_wmp::self().controls)
        { nlsound_wmp::self().controls->play(); return; }
    if (nlsound_ds::graph().control)
        nlsound_ds::graph().control->Run();
#elif defined(__ANDROID__) && defined(NLANG_HAS_QT)
    if (nlsound_a::self().was && !nlsound_a::self().playing) {
        nlsound_a::self().p.callMethod<void>("start", "()V");
        nlsound_a::self().playing = !nlsound_a::exc();
    }
#endif
}
// позиция/длительность — мс (WMP: секунды double)
inline long long nlsound_length() {
#ifdef _WIN32
    if (nlsound_wmp::media()) {
        double d = 0;
        nlsound_wmp::media()->get_duration(&d);
        return (long long)(d * 1000);
    }
    if (nlsound_ds::graph().seek) {
        REFERENCE_TIME r = 0;
        if (SUCCEEDED(nlsound_ds::graph().seek->GetDuration(&r)))
            return r / 10000;
    }
#elif defined(__ANDROID__) && defined(NLANG_HAS_QT)
    if (nlsound_a::self().was) {
        jint d = nlsound_a::self().p.callMethod<jint>(
            "getDuration", "()I");
        if (!nlsound_a::exc()) return (long long)d;
    }
#endif
    return 0;
}
inline long long nlsound_position() {
#ifdef _WIN32
    if (nlsound_wmp::self().controls) {
        double d = 0;
        nlsound_wmp::self().controls->get_currentPosition(&d);
        return (long long)(d * 1000);
    }
    if (nlsound_ds::graph().seek) {
        REFERENCE_TIME r = 0;
        if (SUCCEEDED(nlsound_ds::graph().seek->GetCurrentPosition(&r)))
            return r / 10000;
    }
#elif defined(__ANDROID__) && defined(NLANG_HAS_QT)
    if (nlsound_a::self().was) {
        jint p = nlsound_a::self().p.callMethod<jint>(
            "getCurrentPosition", "()I");
        if (!nlsound_a::exc()) return (long long)p;
    }
#endif
    return 0;
}
inline void nlsound_seek(long long ms) {
#ifdef _WIN32
    if (nlsound_wmp::self().controls) {
        nlsound_wmp::self().controls->put_currentPosition(
            ms / 1000.0);
        return;
    }
    if (nlsound_ds::graph().seek) {
        LONGLONG t = (LONGLONG)ms * 10000;
        nlsound_ds::graph().seek->SetPositions(
            &t, AM_SEEKING_AbsolutePositioning,
            nullptr, AM_SEEKING_NoPositioning);
    }
#elif defined(__ANDROID__) && defined(NLANG_HAS_QT)
    if (nlsound_a::self().was) {
        // seekTo(long) — API26+ у int-версии косяки с дрэгом
        nlsound_a::self().p.callMethod<void>(
            "seekTo", "(J)V", (jlong)ms);
        nlsound_a::exc();
    }
#else
    (void)ms;
#endif
}
inline void nlsound_volume(int v0_100) {
#ifdef _WIN32
    if (nlsound_wmp::self().settings) {
        nlsound_wmp::self().settings->put_volume(v0_100);
        return;
    }
    if (nlsound_ds::graph().audio) {
        long db = v0_100 <= 0 ? -10000
                  : (long)(std::log10(v0_100 / 100.0) * 4000.0);
        nlsound_ds::graph().audio->put_Volume(db);
    }
#elif defined(__ANDROID__) && defined(NLANG_HAS_QT)
    if (nlsound_a::self().was) {
        float v = v0_100 / 100.0f;
        nlsound_a::self().p.callMethod<void>(
            "setVolume", "(FF)V", v, v);
        nlsound_a::exc();
    }
#else
    (void)v0_100;
#endif
}
inline std::string nlsound_mode() {
#ifdef _WIN32
    // WMP playState: 3=playing, 2=paused, 1=stopped,
    // 9=transitioning/6=buffering — грузится, считаем играющим
    // (кнопки не должны дёргать повторный старт на загрузке)
    if (nlsound_wmp::self().player) {
        WMPPlayState st;
        nlsound_wmp::self().player->get_playState(&st);
        if (st == wmppsPlaying)  return "playing";
        if (st == wmppsPaused)   return "paused";
        if (st == wmppsStopped)  return "stopped";
        if (st == wmppsTransitioning || st == wmppsBuffering)
            return "playing";
        return "";
    }
    if (nlsound_ds::graph().graph) {
        IMediaFilter* mf = nullptr;
        nlsound_ds::graph().graph->QueryInterface(
            IID_IMediaFilter, (void**)&mf);
        if (mf) {
            FILTER_STATE st;
            mf->GetState(50, &st);
            mf->Release();
            if (st == State_Running) return "playing";
            if (st == State_Paused)  return "paused";
            return "stopped";
        }
    }
#elif defined(__ANDROID__) && defined(NLANG_HAS_QT)
    auto& s = nlsound_a::self();
    if (!s.was) return "stopped";
    jboolean p = s.p.callMethod<jboolean>(
        "isPlaying", "()Z");
    if (nlsound_a::exc()) return "stopped";
    s.playing = p != 0;
    return s.playing ? "playing" : "paused";
#endif
    return "";
}
// ждать конца трека (или ms таймаут; 0 = до конца)
inline void nlsound_wait(long long timeout_ms) {
#ifdef _WIN32
    long long t0 = GetTickCount64();
    for (;;) {
        std::string m = nlsound_mode();
        if (m != "playing" && m != "paused") break;
        if (timeout_ms > 0 &&
            (long long)(GetTickCount64() - t0) > timeout_ms)
            break;
        Sleep(50);
    }
#elif defined(__ANDROID__) && defined(NLANG_HAS_QT)
    long long t0 = 0;
    for (;;) {
        std::string m = nlsound_mode();
        if (m != "playing" && m != "paused") break;
        if (timeout_ms > 0 && t0++ * 50 > timeout_ms) break;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(50));
    }
#else
    (void)timeout_ms;
#endif
}

inline void nlsound_stop() {
#ifdef _WIN32
    PlaySoundW(nullptr, nullptr, 0);
    nlsound_wmp::close();
    nlsound_ds::release();
    nlsound_pcm::stop();
    nlsound_wet::close();
#elif defined(__ANDROID__) && defined(NLANG_HAS_QT)
    if (nlsound_a::self().was) {
        nlsound_a::self().p.callMethod<void>("stop", "()V");
        nlsound_a::exc();
        nlsound_a::self().playing = false;
        nlsound_a::self().was = false;
    }
#endif
}

// скорость воспроизведения: 1.0 = нормальная.
// pitch по полутонам складывается в тот же rate — как
// на кассете: выше pitch -> быстрее и тоньше.
inline void nlsound_rate(double r) {
#ifdef _WIN32
    if (r < 0.25) r = 0.25;
    if (r > 4.0)  r = 4.0;
    nlsound_wmp::rate(nlsound_wmp::self(), r);
    nlsound_wmp::rate(nlsound_wet::self(), r);
#elif defined(__ANDROID__) && defined(NLANG_HAS_QT)
    if (nlsound_a::self().was) {
        if (r < 0.25) r = 0.25;
        if (r > 4.0)  r = 4.0;
        // PlaybackParams — speed без pitch: тон задаём
        // отдельно nlsound_pitch(); setSpeed НЕ трогает pitch
        QJniObject pp = nlsound_a::self().p.callObjectMethod(
            "getPlaybackParams",
            "()Landroid/media/PlaybackParams;");
        if (!pp.isValid() || nlsound_a::exc())
            pp = QJniObject("android/media/PlaybackParams");
        if (pp.isValid()) {
            pp.callObjectMethod(
                "setSpeed", "(F)Landroid/media/PlaybackParams;",
                (jfloat)r);
            nlsound_a::self().p.callMethod<void>(
                "setPlaybackParams",
                "(Landroid/media/PlaybackParams;)V",
                pp.object());
        }
        nlsound_a::exc();
    }
#else
    (void)r;
#endif
}
// громкость эхо-канала, 0..100
inline void nlsound_reverb(long long level) {
#ifdef _WIN32
    nlsound_wet::reverb((int)level);
#elif defined(__ANDROID__) && defined(NLANG_HAS_QT)
    // PresetReverb на audioSessionId плеера: LARGE_HALL
    // при level>0, иначе выкл. AudioEffect живёт в static —
    // иначе GC уносит эффект и звук сухой.
    if (!nlsound_a::self().was) return;
    static QJniObject rev;
    static jint rev_sid = -1;
    jint sid = nlsound_a::self().p.callMethod<jint>(
        "getAudioSessionId", "()I");
    if (nlsound_a::exc() || sid <= 0) return;
    if (!rev.isValid() || sid != rev_sid) {
        rev = QJniObject("android/media/audiofx/PresetReverb",
                         "(II)V", (jint)0, sid);
        rev_sid = sid;
        if (nlsound_a::exc() || !rev.isValid()) return;
    }
    rev.callMethod<jobject>(
        "setPreset", "(S)Landroid/media/audiofx/PresetReverb;",
        (jshort)(level > 0 ? 4 : 0));   // 4=LARGE_HALL
    nlsound_a::exc();
    rev.callMethod<void>("setEnabled", "(Z)V",
                         (jboolean)(level > 0 ? 1 : 0));
    nlsound_a::exc();
#else
    (void)level;
#endif
}

// тон в полутонах, скорость не трогаем (API23+)
inline void nlsound_pitch(double semitones) {
#if defined(__ANDROID__) && defined(NLANG_HAS_QT)
    if (!nlsound_a::self().was) return;
    double f = std::pow(2.0, semitones / 12.0);
    QJniObject pp = nlsound_a::self().p.callObjectMethod(
        "getPlaybackParams",
        "()Landroid/media/PlaybackParams;");
    if (!pp.isValid() || nlsound_a::exc()) return;
    pp.callObjectMethod(
        "setPitch", "(F)Landroid/media/PlaybackParams;",
        (jfloat)f);
    nlsound_a::self().p.callMethod<void>(
        "setPlaybackParams",
        "(Landroid/media/PlaybackParams;)V", pp.object());
    nlsound_a::exc();
#else
    (void)semitones;
#endif
}




// ������ ����� ������ �������: nbins �������� 0..1
inline std::vector<double> nlsound_spectrum(long long pos_ms,
                                            int nbins) {
    std::vector<double> out;
    double dom = 0;
#ifdef _WIN32
    nlsound_pcm::spectrum(pos_ms, nbins, out, dom);
#else
    (void)pos_ms; (void)nbins;
#endif
    return out;
}
// ������������ ����� ����� ����� � ������: 343/f
inline double nlsound_wavelength(long long pos_ms) {
#ifdef _WIN32
    std::vector<double> tmp;
    double dom = 0;
    nlsound_pcm::spectrum(pos_ms, 24, tmp, dom);
    if (dom > 0) return 343.0 / dom;
#else
    (void)pos_ms;
#endif
    return 0.0;
}
