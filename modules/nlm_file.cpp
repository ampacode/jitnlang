// Модуль file — чтение/запись файлов.
#include "nlm_file.h"
#include <fstream>
#include <sstream>
#include <cstdio>
#include <sys/stat.h>
#include <filesystem>
#include <vector>

// android: content:// uri (SAF-пикер) — настоящее имя
// живёт в MediaStore._display_name, резолвим через
// ContentResolver.query.
#if defined(__ANDROID__) && defined(NLANG_HAS_QT)
#include <QJniObject>
#include <QJniEnvironment>
#include <QtCore/QCoreApplication>
namespace nlfile_a {
inline std::string display_name(const std::string& path) {
    static const QJniObject ctx{
        QNativeInterface::QAndroidApplication::context()};
    QJniEnvironment env;
    if (!ctx.isValid()) return "";
    QJniObject uri = QJniObject::callStaticObjectMethod(
        "android/net/Uri", "parse",
        "(Ljava/lang/String;)Landroid/net/Uri;",
        QJniObject::fromString(QString::fromStdString(path))
            .object<jstring>());
    if (env.checkAndClearExceptions() || !uri.isValid())
        return "";
    QJniObject resolver = ctx.callObjectMethod(
        "getContentResolver",
        "()Landroid/content/ContentResolver;");
    if (env.checkAndClearExceptions() || !resolver.isValid())
        return "";
    QJniObject cursor = resolver.callObjectMethod(
        "query",
        "(Landroid/net/Uri;[Ljava/lang/String;"
        "Ljava/lang/String;[Ljava/lang/String;"
        "Ljava/lang/String;)Landroid/database/Cursor;",
        uri.object(), jarray(nullptr), jstring(nullptr),
        jarray(nullptr), jstring(nullptr));
    if (env.checkAndClearExceptions() || !cursor.isValid())
        return "";
    jboolean has = cursor.callMethod<jboolean>(
        "moveToFirst", "()Z");
    if (env.checkAndClearExceptions() || !has) {
        cursor.callMethod<void>("close", "()V");
        env.checkAndClearExceptions();
        return "";
    }
    // сначала тег TITLE из MediaStore (имя по метаданным,
    // не по файлу), запасной вариант — _display_name
    jint col = cursor.callMethod<jint>("getColumnIndex",
        "(Ljava/lang/String;)I",
        QJniObject::fromString("title").object<jstring>());
    if (env.checkAndClearExceptions() || col < 0)
        col = cursor.callMethod<jint>("getColumnIndex",
            "(Ljava/lang/String;)I",
            QJniObject::fromString("_display_name")
                .object<jstring>());
    std::string name;
    if (!env.checkAndClearExceptions() && col >= 0) {
        QJniObject js = cursor.callObjectMethod(
            "getString", "(I)Ljava/lang/String;", col);
        if (!env.checkAndClearExceptions() && js.isValid())
            name = js.toString().toStdString();
    }
    cursor.callMethod<void>("close", "()V");
    env.checkAndClearExceptions();
    return name;
}
} // namespace nlfile_a
#endif

std::string nlfile_read(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
void nlfile_write(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary);
    f << data;
}
void nlfile_append(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary | std::ios::app);
    f << data;
}
bool nlfile_exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}
std::vector<std::string> nlfile_lines(const std::string& path) {
    std::vector<std::string> r;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line))
        r.push_back(line);
    return r;
}
long long nlfile_size(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 ? (long long)st.st_size : -1;
}
bool nlfile_remove(const std::string& path) {
    return std::remove(path.c_str()) == 0;
}
// листинг каталога: file.list("dir") -> vector<string> имён
std::vector<std::string> nlfile_list(const std::string& path) {
    std::vector<std::string> r;
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(path, ec))
        r.push_back(e.path().filename().string());
    return r;
}
// имя файла без каталога: file.name("a/b/c.mp3") -> "c.mp3"
std::string nlfile_name(const std::string& path) {
#if defined(__ANDROID__) && defined(NLANG_HAS_QT)
    if (path.rfind("content://", 0) == 0) {
        std::string n = nlfile_a::display_name(path);
        if (!n.empty()) return n;
        // fallback: хвост uri без кодирования
        std::string t = path.substr(path.find_last_of('/') + 1);
        // %3A -> ':' : берём после последнего ':'
        auto pc = t.find_last_of(':');
        if (pc != std::string::npos) t = t.substr(pc + 1);
        return t;
    }
#endif
    return std::filesystem::path(path).filename().string();
}
