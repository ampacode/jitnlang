// Модуль gl — реализация: QOpenGLWidget + lit/space-шейдеры.
//
// Как устроено простым языком:
//  - NGLWindow — виджет с QTimer 16 мс; каждый update зовёт
//    nlang-функцию кадра (std::function, ставится nlgl_onFrame);
//  - меши — VAO/VBO с вершиной [pos(3) normal(3) color(3) uv(2)];
//  - lit-шейдер: солнце + полусфера-ambient + точечный свет +
//    туман + мягкий тонмап; текстуры из файлов или uv;
//  - небо — отдельный шейдер: процедурные звёзды, туманность и
//    чёрная дыра с гравитационным линзированием;
//  - «энергопланета» — сфера, цвет которой считает спектр:
//    каждая полоса = стоячая волна по широте на бегущую по
//    долготе; юниформы задаются nlgl_eband*;
//  - nlgl_transmit — полноэкранный треугольник с оверлеем
//    «передачи через гиперпространство».
#include "nlm_gl.h"

// Qt/OpenGL есть только на десктопной сборке с
// Qt6::OpenGLWidgets; без неё файл компилируется пустым.
#ifdef NLANG_HAS_QTGL

#include <QApplication>
#include <QOpenGLWidget>
#include <QOpenGLExtraFunctions>
#include <QOpenGLPaintDevice>
#include <QPainter>
#include <QTimer>
#include <QSurfaceFormat>
#include <QOpenGLContext>
#include <QMatrix4x4>
#include <QVector3D>
#include <QtMath>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QCursor>
#include <QElapsedTimer>
#include <unordered_map>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cmath>
#include <cstdlib>
#include <iostream>

using glwin = intptr_t;

namespace {

// ---------- globals ----------
glwin g_current_win = 0;
glwin g_current_shader = 0;
bool g_sky_enabled = true;

QApplication* gl_app() {
    static QApplication* a = nullptr;
    if (!a) {
        a = qobject_cast<QApplication*>(QApplication::instance());
        if (!a) {
            static int argc = 1;
            static char nm[] = "nlang";
            static char* argv[] = { nm };
            a = new QApplication(argc, argv);
        }
    }
    return a;
}

QOpenGLExtraFunctions* glf() {
    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    return ctx ? ctx->extraFunctions() : nullptr;
}

QElapsedTimer g_elapsed;
double gl_time() {
    if (!g_elapsed.isValid()) g_elapsed.start();
    return g_elapsed.elapsed() / 1000.0;
}

// ---------- camera ----------
struct NGLCamera {
    QVector3D pos = QVector3D(0, 1.7, 24);
    float yaw = 0;          // градусы; 0 -> смотрим -Z
    float pitch = 0;
    float roll = 0;
    float far_plane = 800.0f;
    float speed = 5.0f;
    float sprint = 9.0f;
    float sens = 0.12f;
    float last_mdx = 0, last_mdy = 0;
    float height = 1.7f;
    bool fly = true;        // свободный полёт (космос)
    bool locked = false;

    QVector3D lookDir() const {
        float yr = qDegreesToRadians(yaw), pr = qDegreesToRadians(pitch);
        return QVector3D(-sin(yr) * cos(pr), sin(pr), -cos(yr) * cos(pr));
    }
    QVector3D fwd2d() const {
        float yr = qDegreesToRadians(yaw);
        return QVector3D(-sin(yr), 0, -cos(yr));
    }
    QVector3D right2d() const {
        float yr = qDegreesToRadians(yaw);
        return QVector3D(cos(yr), 0, -sin(yr));
    }
    QMatrix4x4 getView() const {
        QVector3D f = lookDir();
        QVector3D up = QVector3D(0, 1, 0);
        if (roll != 0.0f) {
            QMatrix4x4 rm;
            rm.rotate(-roll, f.x(), f.y(), f.z());
            up = rm * up;
        }
        QMatrix4x4 m;
        m.lookAt(pos, pos + f, up);
        return m;
    }
    QMatrix4x4 getProj(float w, float h) const {
        QMatrix4x4 m;
        m.perspective(72.0f, w / std::max(h, 1.0f), 0.3f, far_plane);
        return m;
    }
};

// ---------- mesh ----------
struct NGLMesh {
    GLuint vao = 0, vbo = 0, ebo = 0;
    GLsizei count = 0, icount = 0;
    float alpha = 1.0f;
    GLuint tex = 0;
    float tex_scale = 0.2f;
    int use_uv = 0;
    std::string tex_path;
    bool pending = false;
    std::vector<float> pv;
    std::vector<unsigned> pi;
};

bool gl_upload_mesh(NGLMesh* m);

NGLMesh* gl_make_mesh(const std::vector<float>& verts,
                      const std::vector<unsigned>* idx) {
    auto* f = glf();
    NGLMesh* m = new NGLMesh();
    if (!f) {
        // контекста ещё нет — меш загрузится при первой отрисовке
        m->pending = true;
        m->pv = verts;
        if (idx) m->pi = *idx;
        m->count = idx && !idx->empty() ? 0 : (GLsizei)(verts.size() / 11);
        m->icount = idx ? (GLsizei)idx->size() : 0;
        return m;
    }
    f->glGenVertexArrays(1, &m->vao);
    f->glGenBuffers(1, &m->vbo);
    f->glBindVertexArray(m->vao);
    f->glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    f->glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float),
                    verts.data(), GL_STATIC_DRAW);
    if (idx && !idx->empty()) {
        f->glGenBuffers(1, &m->ebo);
        f->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ebo);
        f->glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                        idx->size() * sizeof(unsigned),
                        idx->data(), GL_STATIC_DRAW);
        m->icount = (GLsizei)idx->size();
    }
    // 0=pos 1=normal 2=color 3=uv, stride 11 floats
    size_t st = 11 * sizeof(float);
    f->glEnableVertexAttribArray(0);
    f->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, (void*)0);
    f->glEnableVertexAttribArray(1);
    f->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, st, (void*)(3 * sizeof(float)));
    f->glEnableVertexAttribArray(2);
    f->glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, st, (void*)(6 * sizeof(float)));
    f->glEnableVertexAttribArray(3);
    f->glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, st, (void*)(9 * sizeof(float)));
    f->glBindVertexArray(0);
    m->count = idx && !idx->empty() ? 0 : (GLsizei)(verts.size() / 11);
    return m;
}

bool gl_upload_mesh(NGLMesh* m) {
    if (!m || !m->pending) return m != nullptr;
    auto* f = glf(); if (!f) return false;
    f->glGenVertexArrays(1, &m->vao);
    f->glGenBuffers(1, &m->vbo);
    f->glBindVertexArray(m->vao);
    f->glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    f->glBufferData(GL_ARRAY_BUFFER, m->pv.size() * sizeof(float),
                    m->pv.data(), GL_STATIC_DRAW);
    if (!m->pi.empty()) {
        f->glGenBuffers(1, &m->ebo);
        f->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ebo);
        f->glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                        m->pi.size() * sizeof(unsigned),
                        m->pi.data(), GL_STATIC_DRAW);
    }
    size_t st = 11 * sizeof(float);
    f->glEnableVertexAttribArray(0);
    f->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, (void*)0);
    f->glEnableVertexAttribArray(1);
    f->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, st, (void*)(3 * sizeof(float)));
    f->glEnableVertexAttribArray(2);
    f->glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, st, (void*)(6 * sizeof(float)));
    f->glEnableVertexAttribArray(3);
    f->glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, st, (void*)(9 * sizeof(float)));
    f->glBindVertexArray(0);
    m->pending = false;
    m->pv.clear(); m->pi.clear();
    return true;
}

void gl_push_vert(std::vector<float>& v, float x, float y, float z,
                  float nx, float ny, float nz, float r, float g, float b,
                  float u = 0.0f, float vv = 0.0f) {
    float tmp[11] = {x, y, z, nx, ny, nz, r, g, b, u, vv};
    v.insert(v.end(), tmp, tmp + 11);
}

// ---------- shader ----------
struct NGLShader { GLuint prog; };

NGLShader* gl_create_shader(const std::string& vsrc,
                            const std::string& fsrc) {
    auto* f = glf();
    if (!f) { std::cout << "glf null" << std::endl; return nullptr; }
    auto compile = [&](GLenum type, const char* src, const char* tag) -> GLuint {
        GLuint s = f->glCreateShader(type);
        f->glShaderSource(s, 1, &src, nullptr);
        f->glCompileShader(s);
        GLint ok; f->glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            GLint len; f->glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
            std::vector<char> log(len);
            f->glGetShaderInfoLog(s, len, nullptr, log.data());
            std::cout << tag << " shader error: " << log.data() << std::endl;
            f->glDeleteShader(s);
            return 0;
        }
        return s;
    };
    GLuint vs = compile(GL_VERTEX_SHADER, vsrc.c_str(), "VS");
    GLuint fs = compile(GL_FRAGMENT_SHADER, fsrc.c_str(), "FS");
    if (!vs || !fs) { std::cout << "shader compile failed" << std::endl; return nullptr; }
    GLuint p = f->glCreateProgram();
    f->glAttachShader(p, vs);
    f->glAttachShader(p, fs);
    f->glLinkProgram(p);
    f->glDeleteShader(vs);
    f->glDeleteShader(fs);
    GLint ok; f->glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len; f->glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(len);
        f->glGetProgramInfoLog(p, len, nullptr, log.data());
        std::cout << "link error: " << log.data() << std::endl;
        f->glDeleteProgram(p);
        return nullptr;
    }
    return new NGLShader{p};
}

// ---------- lights / fog ----------
QVector3D g_sun_dir(0.5, 0.8, 0.3);
QVector3D g_sun_color(1.5, 1.4, 1.3);
QVector3D g_hemi_sky(0.2, 0.25, 0.35);
QVector3D g_hemi_ground(0.08, 0.05, 0.04);
QVector3D g_fog_color(0.016, 0.031, 0.094);
float g_fog_start = 60, g_fog_end = 220;
QVector3D g_point_pos(0, 0, 0), g_point_color(0, 0, 0);

// ---------- textures ----------
GLuint gl_load_tex(const std::string& path) {
    auto* f = glf(); if (!f) return 0;
    QImage img(QString::fromStdString(path));
    if (img.isNull()) {
        printf("[gl] texture not found: %s", path.c_str());
        fflush(stdout);
        return 0;
    }
    img = img.convertToFormat(QImage::Format_RGB888);
    GLuint t = 0;
    f->glGenTextures(1, &t);
    f->glBindTexture(GL_TEXTURE_2D, t);
    f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, img.width(), img.height(),
                    0, GL_RGB, GL_UNSIGNED_BYTE, img.bits());
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    f->glGenerateMipmap(GL_TEXTURE_2D);
    float aniso = 0;
    f->glGetFloatv(0x84FF, &aniso);   // MAX_TEXTURE_MAX_ANISOTROPY
    if (aniso > 0)
        f->glTexParameterf(GL_TEXTURE_2D, 0x84FE, std::min(aniso, 8.0f));
    return t;
}

// ---------- lit shader ----------
glwin gl_default_shader() {
    if (g_current_shader) return g_current_shader;
    std::string vs = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;
layout(location = 3) in vec2 aUV;
uniform mat4 u_viewProj;
uniform mat4 u_model;
uniform float u_uvScale;
uniform float u_useUV;
out vec3 vColor;
out vec3 vNormal;
out vec3 vWorld;
out vec2 vUV;
void main(){
    vec4 wp = u_model * vec4(aPos, 1.0);
    vWorld = wp.xyz;
    vNormal = mat3(u_model) * aNormal;
    vColor = aColor;
    vUV = u_useUV > 0.5 ? aUV : wp.xz * u_uvScale;
    gl_Position = u_viewProj * wp;
})";
    std::string fs = R"(#version 330 core
in vec3 vColor;
in vec3 vNormal;
in vec3 vWorld;
in vec2 vUV;
uniform vec3 u_sunDir;
uniform vec3 u_sunColor;
uniform vec3 u_hemiSky;
uniform vec3 u_hemiGround;
uniform vec3 u_fogColor;
uniform vec2 u_fog;
uniform vec3 u_camPos;
uniform vec3 u_pointPos;
uniform vec3 u_pointColor;
uniform float u_alpha;
uniform sampler2D u_tex;
uniform float u_hasTex;
uniform float u_useUV;
uniform float u_uvScale;
out vec4 FragColor;
void main(){
    vec3 N = normalize(vNormal);
    vec3 L = normalize(u_sunDir);
    float diff = max(dot(N, L), 0.0);
    vec3 amb = mix(u_hemiGround, u_hemiSky, N.y * 0.5 + 0.5);
    vec3 base = vColor;
    if (u_hasTex > 0.5) {
        vec3 tc;
        if (u_useUV > 0.5) {
            tc = texture(u_tex, vUV).rgb;
        } else {
            // трипланарная проекция — стены без растяжения uv
            vec3 an = abs(N);
            if (an.y >= an.x && an.y >= an.z) tc = texture(u_tex, vWorld.xz * u_uvScale).rgb;
            else if (an.x >= an.z)            tc = texture(u_tex, vWorld.zy * u_uvScale).rgb;
            else                              tc = texture(u_tex, vWorld.xy * u_uvScale).rgb;
        }
        base = tc * vColor * 2.0;
    }
    vec3 col = base * (amb + u_sunColor * diff);
    // specular Blinn-Phong — только где есть солнце
    vec3 V = normalize(u_camPos - vWorld);
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), 48.0) * 0.30;
    col += u_sunColor * spec;
    // точечный свет
    vec3 toL = u_pointPos - vWorld;
    float dl = length(toL);
    if (dl < 30.0) {
        float att = 1.0 / (1.0 + 0.35 * dl + 0.2 * dl * dl);
        col += base * u_pointColor * max(dot(N, normalize(toL)), 0.0) * att;
    }
    float dist = length(u_camPos - vWorld);
    float fogF = clamp((dist - u_fog.x) / max(u_fog.y - u_fog.x, 0.001), 0.0, 1.0);
    col = mix(col, u_fogColor, fogF);
    // HDR: без тонмапа — яркое (>1) уходит в bloom и ACES
    FragColor = vec4(col, u_alpha);
})";
    g_current_shader = (glwin)gl_create_shader(vs, fs);
    return g_current_shader;
}

// пост-пайплайн — определён ниже; paintGL зовёт его после кадра
void gl_post_ensure(int w, int h);
void gl_post_run(int w, int h);
extern GLuint g_fbo_ms;
extern int    g_ms_w, g_ms_h;

// ---------- window ----------
class NGLWindow : public QOpenGLWidget {
public:
    NGLWindow(const std::string& title, int w, int h) {
        QSurfaceFormat fmt;
        fmt.setRenderableType(QSurfaceFormat::OpenGL);
        fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
        fmt.setVersion(3, 3);
        fmt.setDepthBufferSize(24);
        setFormat(fmt);
        setWindowTitle(QString::fromStdString(title));
        resize(w, h);
        setMouseTracking(true);
        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, [this]{ update(); });
        m_timer->start(16);
    }
    void setFrame(std::function<void()> fn) { m_frame = fn; }
    NGLCamera& camera() { return m_camera; }
    bool keyDown(int k) const {
        auto it = m_keys.find(k); return it != m_keys.end() && it->second;
    }
    // мёртвая зона ±0.6px — дрожь курсора не дрейфует камеру
    float mouseDX() {
        m_accX += m_dx; m_dx = 0;
        if (fabs(m_accX) < 0.6f) return 0;
        float d = m_accX; m_accX = 0; return d;
    }
    float mouseDY() {
        m_accY += m_dy; m_dy = 0;
        if (fabs(m_accY) < 0.6f) return 0;
        float d = m_accY; m_accY = 0; return d;
    }
    void lockMouse() {
        m_camera.locked = true;
        setCursor(Qt::BlankCursor);
        grabMouse();
        m_first = true;
        m_lastG = mapToGlobal(QPoint(width() / 2, height() / 2));
        QCursor::setPos(m_lastG);
    }
protected:
    void initializeGL() override {}
    void paintGL() override {
        auto* f = glf();
        if (!f) return;
        int w = width(), h = height();
        // сцена — в HDR MSAA-цель; пост прогоняет её на экран
        gl_post_ensure(w, h);
        f->glBindFramebuffer(GL_FRAMEBUFFER, g_fbo_ms);
        f->glViewport(0, 0, w, h);
        if (m_frame) {
            try { m_frame(); }
            catch (const std::exception& e) {
                fprintf(stderr, "[gl] frame: %s\n", e.what());
            }
            catch (...) {
                fprintf(stderr, "[gl] frame: unknown error\n");
            }
        }
        gl_post_run(w, h);
    }
    void keyPressEvent(QKeyEvent* e) override {
        m_keys[e->nativeVirtualKey()] = true;  // физический код — раскладка не важна
        if (e->nativeVirtualKey() == 0x1B && m_camera.locked) {  // Esc
            m_camera.locked = false;
            unsetCursor();
            releaseMouse();
        }
    }
    void keyReleaseEvent(QKeyEvent* e) override {
        m_keys[e->nativeVirtualKey()] = false;
    }
    void mousePressEvent(QMouseEvent*) override {
        if (!m_camera.locked) lockMouse();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        QPoint gp = e->globalPosition().toPoint();
        if (m_camera.locked) {
            if (m_first) { m_first = false; m_lastG = gp; return; }
            m_dx += qBound(-300.0f, (float)(gp.x() - m_lastG.x()), 300.0f);
            m_dy += qBound(-300.0f, (float)(gp.y() - m_lastG.y()), 300.0f);
            m_lastG = gp;
            // курсор всегда в центр — FPS-стиль, не убегает за окно
            QPoint center = mapToGlobal(QPoint(width() / 2, height() / 2));
            if (gp != center) {
                QCursor::setPos(center);
                m_lastG = center;
            }
        } else {
            if (m_first) { m_lastG = gp; m_first = false; }
            m_dx += gp.x() - m_lastG.x();
            m_dy += gp.y() - m_lastG.y();
            m_lastG = gp;
        }
    }
private:
    QTimer* m_timer = nullptr;
    std::function<void()> m_frame;
    NGLCamera m_camera;
    std::unordered_map<int, bool> m_keys;
    bool m_first = true;
    QPoint m_lastG;
    float m_dx = 0, m_dy = 0;
    float m_accX = 0, m_accY = 0;
};

NGLWindow* win_of(long long h) { return (NGLWindow*)h; }
NGLWindow* cur_win() { return (NGLWindow*)g_current_win; }

// ---------- 2D QPainter ----------
QPainter* g_painter = nullptr;
QOpenGLPaintDevice* g_dev = nullptr;

// ---------- scene ----------
struct NGLObj { NGLMesh* m; QMatrix4x4 model; };
std::vector<NGLObj> g_scene;
std::unordered_map<long long, std::vector<NGLObj>> g_scene_cache;

void gl_draw_one(QOpenGLExtraFunctions* f, NGLMesh* m,
                 const QMatrix4x4& model, NGLShader* s,
                 float vpw, float vph) {
    if (!m || !s) return;
    if (m->pending && !gl_upload_mesh(m)) return;
    f->glUseProgram(s->prog);
    auto* win = cur_win();
    QMatrix4x4 view = win->camera().getView();
    QMatrix4x4 proj = win->camera().getProj(vpw, vph);
    QMatrix4x4 vp = proj * view;
    f->glUniformMatrix4fv(f->glGetUniformLocation(s->prog, "u_viewProj"),
                        1, GL_FALSE, vp.constData());
    f->glUniformMatrix4fv(f->glGetUniformLocation(s->prog, "u_model"),
                        1, GL_FALSE, model.constData());
    QVector3D cp = win->camera().pos;
    f->glUniform3f(f->glGetUniformLocation(s->prog, "u_camPos"), cp.x(), cp.y(), cp.z());
    f->glUniform3f(f->glGetUniformLocation(s->prog, "u_sunDir"), g_sun_dir.x(), g_sun_dir.y(), g_sun_dir.z());
    f->glUniform3f(f->glGetUniformLocation(s->prog, "u_sunColor"), g_sun_color.x(), g_sun_color.y(), g_sun_color.z());
    f->glUniform3f(f->glGetUniformLocation(s->prog, "u_hemiSky"), g_hemi_sky.x(), g_hemi_sky.y(), g_hemi_sky.z());
    f->glUniform3f(f->glGetUniformLocation(s->prog, "u_hemiGround"), g_hemi_ground.x(), g_hemi_ground.y(), g_hemi_ground.z());
    f->glUniform3f(f->glGetUniformLocation(s->prog, "u_fogColor"), g_fog_color.x(), g_fog_color.y(), g_fog_color.z());
    f->glUniform2f(f->glGetUniformLocation(s->prog, "u_fog"), g_fog_start, g_fog_end);
    f->glUniform3f(f->glGetUniformLocation(s->prog, "u_pointPos"), g_point_pos.x(), g_point_pos.y(), g_point_pos.z());
    f->glUniform3f(f->glGetUniformLocation(s->prog, "u_pointColor"), g_point_color.x(), g_point_color.y(), g_point_color.z());
    f->glUniform1f(f->glGetUniformLocation(s->prog, "u_alpha"), m->alpha);
    if (!m->tex && !m->tex_path.empty()) m->tex = gl_load_tex(m->tex_path);
    if (m->tex) {
        f->glActiveTexture(GL_TEXTURE0);
        f->glBindTexture(GL_TEXTURE_2D, m->tex);
        f->glUniform1i(f->glGetUniformLocation(s->prog, "u_tex"), 0);
        f->glUniform1f(f->glGetUniformLocation(s->prog, "u_hasTex"), 1.0f);
        f->glUniform1f(f->glGetUniformLocation(s->prog, "u_uvScale"), m->tex_scale);
        f->glUniform1f(f->glGetUniformLocation(s->prog, "u_useUV"),
                       m->use_uv ? 1.0f : 0.0f);
    } else {
        f->glUniform1f(f->glGetUniformLocation(s->prog, "u_hasTex"), 0.0f);
        f->glUniform1f(f->glGetUniformLocation(s->prog, "u_uvScale"), 1.0f);
        f->glUniform1f(f->glGetUniformLocation(s->prog, "u_useUV"), 0.0f);
    }
    bool alpha = m->alpha < 0.999f;
    if (alpha) {
        f->glEnable(GL_BLEND);
        f->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        f->glDepthMask(GL_FALSE);
    }
    f->glBindVertexArray(m->vao);
    if (m->ebo) f->glDrawElements(GL_TRIANGLES, m->icount, GL_UNSIGNED_INT, 0);
    else f->glDrawArrays(GL_TRIANGLES, 0, m->count);
    f->glBindVertexArray(0);
    if (alpha) {
        f->glDepthMask(GL_TRUE);
        f->glDisable(GL_BLEND);
    }
}

// ---------- sky ----------
glwin g_sky_shader = 0;
glwin g_sky_mesh = 0;
QVector3D g_bh_dir(0.55f, 0.25f, -0.7f);   // направление на чёрную дыру

glwin gl_sky_shader() {
    if (g_sky_shader) return g_sky_shader;
    std::string vs = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 u_viewProj;
uniform mat4 u_model;
uniform vec3 u_camPos;
out vec3 vDir;
void main(){
    vec4 wp = u_model * vec4(aPos, 1.0);
    vDir = normalize(wp.xyz - u_camPos);
    gl_Position = u_viewProj * wp;
})";
    std::string fs = R"(#version 330 core
in vec3 vDir;
uniform float uTime;
uniform vec3 uBH_Pos;
out vec4 FragColor;
float hash(vec3 p){
    p = fract(p * 0.3183099 + vec3(0.1, 0.2, 0.3));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float hash1(float n){ return fract(sin(n) * 43758.5453); }
float starField(vec3 dir){
    vec3 grid = floor(dir * 400.0);
    float h = hash(grid);
    float star = step(0.9985, h);
    float bright = 0.6 + 0.4 * hash(grid + vec3(1.0, 0.0, 0.0));
    return star * bright;
}
void main(){
    vec3 dir = normalize(vDir);
    float h = dir.y;
    // космос почти чёрный — едва заметный градиент
    vec3 zenithColor  = vec3(0.006, 0.010, 0.028);
    vec3 horizonColor = vec3(0.002, 0.004, 0.014);
    float t = smoothstep(-0.3, 0.7, h);
    vec3 col = mix(horizonColor, zenithColor, t);
    col += vec3(0.010, 0.018, 0.045) * (1.0 - abs(h)) * 0.3;
    float n1 = hash(dir * 2.0);
    float n2 = hash(dir * 5.0 + 1.0);
    float nebula = smoothstep(0.55, 0.85, n1 * 0.6 + n2 * 0.4);
    col += vec3(0.020, 0.030, 0.075) * nebula * 0.18;
    // ===== чёрная дыра: аккреционный диск, марш лучей =====
    vec3 bhPos = normalize(uBH_Pos) * 480.0;
    float rs = 16.0;
    vec3 diskN = normalize(vec3(0.10, 0.85, -0.30));
    float diskR0 = rs * 3.2;
    float diskR1 = rs * 9.0;
    vec3 v = dir;
    vec3 pos = vec3(0.0);
    vec3 prev = pos;
    vec3 disk = vec3(0.0);
    float diskA = 0.0;
    float minR = 1e9;
    bool captured = false;
    int crossN = 0;
    float stp;
    if (dot(dir, uBH_Pos) > -0.2) {
        for (int i = 0; i < 220; i++) {
            vec3 diff = bhPos - pos;
            float r = length(diff);
            if (r < minR) minR = r;
            if (r < rs) { captured = true; break; }
            // адаптивный шаг — мелкий у горизонта
            stp = clamp(r * 0.08, 0.5, 8.0);
            v += normalize(diff) * (2.2 * rs) / (r * r) * stp;
            v = normalize(v);
            pos += v * stp;
            vec3 rel = pos - bhPos;
            float dn1 = dot(prev - bhPos, diskN);
            float dn2 = dot(rel, diskN);
            if (dn1 * dn2 < 0.0) {
                // бисекция — точная точка пересечения диска
                vec3 a2 = prev - bhPos, b2 = rel;
                float d1 = dn1;
                for (int j = 0; j < 6; j++) {
                    vec3 mid = (a2 + b2) * 0.5;
                    float dm = dot(mid, diskN);
                    if (dm * d1 > 0.0) { a2 = mid; } else { b2 = mid; }
                }
                vec3 hit = (a2 + b2) * 0.5;
                float rh = length(hit);
                if (rh > diskR0 - 8.0 && rh < diskR1 + 18.0) {
                    float edge = smoothstep(diskR0 - 6.0, diskR0 + 6.0, rh)
                               * smoothstep(diskR1 + 16.0, diskR1 - 10.0, rh);
                    float tn = 1.0 - (rh - diskR0) / (diskR1 - diskR0);
                    tn = clamp(tn, 0.0, 1.0);
                    // температура: жёлто-белый внутри → тёмно-красный снаружи
                    vec3 hc = mix(vec3(0.32, 0.07, 0.015), vec3(1.0, 0.88, 0.62),
                                  pow(tn, 1.4));
                    // кеплеровское вращение + полосы
                    vec3 ax1 = normalize(cross(diskN, vec3(0.0, 0.0, 1.0)));
                    vec3 ax2 = cross(diskN, ax1);
                    float ang = atan(dot(hit, ax1), dot(hit, ax2));
                    ang += uTime * (0.25 + 2.4 * tn * tn);
                    float streak = 0.85 + 0.15 * sin(ang * 4.0 + sin(rh * 0.12) * 3.0)
                                       * sin(ang * 2.0 - rh * 0.08);
                    // доплер — приближающаяся сторона ярче
                    vec3 tang = normalize(cross(diskN, normalize(hit)));
                    float dop = 1.0 + dot(-v, tang) * 0.85;
                    float fade = pow(0.3, float(crossN));
                    crossN++;
                    float graze = clamp(abs(dot(v, diskN)) * 5.0, 0.0, 1.0);
                    float al = pow(tn, 0.6) * streak * smoothstep(0.0, 0.1, tn)
                             * fade * edge * graze;
                    disk += hc * dop * al * (1.0 - diskA) * 1.9;
                    diskA += al * (1.0 - diskA);
                    if (diskA > 0.97) break;
                }
            }
            prev = pos;
            if (dot(pos, pos) > 300000.0) break;
        }
    }
    // звёзды по изогнутому лучу — гравитационное линзирование
    float stars = starField(normalize(v));
    col += vec3(1.0, 1.0, 1.0) * stars * 1.5;
    // фотонное кольцо
    if (minR < 1e8) {
        float pring = smoothstep(rs * 2.1, rs * 1.05, minR);
        col += vec3(0.95, 0.75, 0.45) * pring * 0.6;
    }
    // гасим диск у фотонной сферы
    if (minR < rs * 3.0) {
        float k = smoothstep(rs * 1.05, rs * 2.4, minR);
        disk *= k * k * k * k;
    }
    col = col * (1.0 - clamp(diskA, 0.0, 1.0)) + disk;
    if (captured) col = vec3(0.0);
    FragColor = vec4(col, 1.0);
})";
    g_sky_shader = (glwin)gl_create_shader(vs, fs);
    return g_sky_shader;
}

// ---------- energoviewer (pseudoworld) ----------
// 8 полос спектра: частота, амплитуда, фаза, скорость дрейфа
float g_ebands[8][4] = {
    { 1.5f, 0.90f, 0.0f, 0.10f},
    { 3.0f, 0.60f, 1.2f, 0.16f},
    { 5.0f, 0.45f, 2.4f, 0.22f},
    { 8.0f, 0.35f, 0.5f, 0.30f},
    {13.0f, 0.25f, 3.1f, 0.45f},
    {21.0f, 0.18f, 1.7f, 0.60f},
    {34.0f, 0.12f, 0.3f, 0.80f},
    {55.0f, 0.08f, 2.0f, 1.10f},
};
float g_ebcol[8][3] = {   // палитра низкие→высокие
    {0.05f,0.10f,0.30f}, {0.05f,0.30f,0.55f}, {0.10f,0.55f,0.70f},
    {0.20f,0.75f,0.55f}, {0.45f,0.85f,0.30f}, {0.85f,0.75f,0.25f},
    {0.90f,0.45f,0.20f}, {0.85f,0.20f,0.40f},
};
QVector3D g_ebase(0.02f, 0.03f, 0.08f);
QVector3D g_eatmo(0.30f, 0.55f, 0.95f);
float g_eatmo_k = 0.9f;

glwin g_epl_shader = 0;
glwin g_epl_mesh = 0;

// Каждая полоса спектра — октава рельефа: частота, вес, сдвиг,
// дрейф. Вращаем позицию выборки на свой угол — октавы не
// выстраиваются в решётку по осям.
const char* EPL_NOISE = R"(
float eh(vec3 p){
    p = fract(p * 0.3183099 + vec3(0.11, 0.17, 0.13));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}
// value-noise 3D: 8 углов, плавная trilinear
float evn(vec3 p){
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = eh(i),                n100 = eh(i + vec3(1,0,0));
    float n010 = eh(i + vec3(0,1,0)),  n110 = eh(i + vec3(1,1,0));
    float n001 = eh(i + vec3(0,0,1)),  n101 = eh(i + vec3(1,0,1));
    float n011 = eh(i + vec3(0,1,1)),  n111 = eh(i + vec3(1,1,1));
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
               mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}
mat3 erot(float a, float b){
    float ca = cos(a), sa = sin(a), cb = cos(b), sb = sin(b);
    return mat3(cb, 0, -sb,  0, 1, 0,  sb, 0, cb) *
           mat3(1, 0, 0,  0, ca, -sa,  0, sa, ca);
}
)";

glwin gl_epl_shader() {
    if (g_epl_shader) return g_epl_shader;
    std::string vs = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNrm;
uniform mat4 u_viewProj;
uniform mat4 u_model;
out vec3 vN;
out vec3 vW;
out vec3 vLocal;
void main(){
    vec4 wp = u_model * vec4(aPos, 1.0);
    vW = wp.xyz;
    vN = normalize(mat3(u_model) * aNrm);
    vLocal = normalize(aPos);          // сфера единичная — pos = dir
    gl_Position = u_viewProj * wp;
})";
    std::string fs = std::string(R"(#version 330 core
in vec3 vN;
in vec3 vW;
in vec3 vLocal;
uniform vec4 u_bands[8];
uniform vec3 u_bcol[8];
uniform vec3 u_base;
uniform vec3 u_atmo;
uniform float u_atmoK;
uniform vec3 u_sunDir;
uniform vec3 u_sunColor;
uniform vec3 u_camPos;
uniform float uTime;
out vec4 FragColor;
)") + EPL_NOISE + R"(
// высота мира = сумма полос-октав: каждая крутит своё
// направление и дрейфует со своей скоростью
float eheight(vec3 p){
    float h = 0.0, an = 0.0;
    for (int i = 0; i < 8; i++) {
        vec4 b = u_bands[i];
        if (b.y < 0.002) continue;
        vec3 q = erot(float(i) * 1.31, float(i) * 0.73 + b.z) * p;
        h += evn(q * b.x + vec3(uTime * b.w * 0.05)) * b.y;
        an += b.y;
    }
    return h / max(an, 1e-3) * 2.0 - 1.0;   // -1..1
}
void main(){
    vec3 n = normalize(vLocal);          // сферическая нормаль
    vec3 vdir = normalize(u_camPos - vW);
    vec3 L = normalize(u_sunDir);
    float h = eheight(n * 2.7 + u_base);   // seed = u_base
    float sea = 0.0;                       // уровень «моря»

    // ---------- биомы по высоте/широте ----------
    vec3 deepSea  = vec3(0.012, 0.045, 0.14);
    vec3 shallow  = vec3(0.03, 0.19, 0.30);
    vec3 sand     = vec3(0.36, 0.30, 0.16);
    vec3 lowland  = vec3(0.08, 0.17, 0.07);
    vec3 highland = vec3(0.30, 0.24, 0.18);
    vec3 snow     = vec3(0.75, 0.80, 0.85);
    vec3 col;
    float isSea = 0.0;
    if (h < sea) {
        float d = clamp(-h / 0.45, 0.0, 1.0);
        col = mix(shallow, deepSea, d);
        isSea = 1.0;
    } else {
        float e = h - sea;
        col = mix(sand, lowland, smoothstep(0.0, 0.08, e));
        col = mix(col, highland, smoothstep(0.10, 0.30, e));
        col = mix(col, snow, smoothstep(0.32, 0.5, e));
        col *= 0.9 + 0.2 * evn(n * 31.0);   // микродеталь
    }
    // полярные шапки: широта + лёгкий шум
    float ice = smoothstep(0.68, 0.78, abs(n.y)
                          + (evn(n * 9.0) - 0.5) * 0.10);
    col = mix(col, snow, ice);

    // ---------- освещение: терминатор + ночь ----------
    float ndl = dot(n, L);
    float day = smoothstep(-0.08, 0.18, ndl);
    // рельефная нормаль: градиент высоты по касательной
    vec3 t1 = normalize(cross(n, vec3(0, 1, 0.001)));
    vec3 t2 = cross(n, t1);
    float e = 0.015;
    float hx = eheight(n * 2.7 + u_base + t1 * e) - h;
    float hz = eheight(n * 2.7 + u_base + t2 * e) - h;
    vec3 nr = normalize(n - (t1 * hx + t2 * hz) * 14.0);
    float dif = max(dot(nr, L), 0.0);
    vec3 amb = u_atmo * 0.10;              // рассеянный свет неба

    vec3 lit = col * (amb + u_sunColor * dif * 1.15);

    // океанский блик (спекуляр) — только вода, днём
    if (isSea > 0.5) {
        vec3 hv = normalize(L + vdir);
        float spec = pow(max(dot(nr, hv), 0.0), 220.0);
        lit += u_sunColor * spec * 1.6;
    }

    // тёплая полоса терминатора — рассеяние на закате
    float term = exp(-abs(ndl) * 6.0) * smoothstep(-0.3, 0.1, ndl);
    lit += vec3(1.0, 0.32, 0.10) * term * 0.35;

    // облака: крупный дрейфующий шум, белые поля над поверхностью
    float cl = evn(n * 4.0 + vec3(uTime * 0.008, 0.0, 0.0));
    cl = smoothstep(0.56, 0.78, cl) * 0.55;
    lit = mix(lit, vec3(0.75, 0.80, 0.88) * (amb + u_sunColor * dif * 1.1),
              cl);

    // ---------- энергетические трещины ----------
    // вены только в коре (не в океане), точечно — по маске
    // шума покрытие ломается на сгустки, не решётка
    float night = 1.0 - day;
    vec3 veins = vec3(0.0);
    if (isSea < 0.5) {
        for (int i = 0; i < 8; i++) {
            vec4 b = u_bands[i];
            if (b.y < 0.002) continue;
            vec3 q = erot(float(i) * 1.31, float(i) * 0.73 + b.z) * n;
            float w = evn(q * b.x * 2.0 + vec3(uTime * b.w * 0.05));
            float vein = smoothstep(0.018, 0.0, abs(w - 0.5));
            // сгустковая маска — разломы кучкуются, не сетью
            vein *= smoothstep(0.45, 0.75, evn(n * 6.0 + vec3(float(i) * 3.1)));
            veins += u_bcol[i] * vein * b.y;
        }
    }
    lit += veins * (0.10 + 1.6 * night);   // ночью разломы яркие

    col = lit;

    // ---------- атмосфера (приближение рассеяния) ----------
    // узкая тёмная кромка; светится только дневной стороной
    float fr = pow(1.0 - abs(dot(n, vdir)), 4.5);
    col += u_atmo * fr * u_atmoK * (0.08 + 0.7 * day);
    // тёплый ободок на терминаторе
    col += vec3(1.0, 0.4, 0.12) * fr * term * 0.9;

    FragColor = vec4(col, 1.0);
})";
    g_epl_shader = (glwin)gl_create_shader(vs, fs);
    return g_epl_shader;
}

// ---------- post pipeline ----------
// Сцена рисуется в MSAA-FBO (RGBA16F, 4x) — HDR-кадр.
// Потом: resolve -> bright-pass -> blur ping-pong ->
// композит: ACES-тонмап + bloom + виньетка + зерно +
// хроматическая аберрация. Всё «фотографическое» живёт тут.
float g_post_exposure = 1.15f;
float g_post_bloom = 0.85f;
float g_post_grain = 0.045f;
float g_post_aberr = 0.6f;      // сила хроматической аберрации, px
int   g_post_samples = 4;

GLuint g_fbo_ms = 0, g_ms_col = 0, g_ms_depth = 0;
int    g_ms_w = 0, g_ms_h = 0;
struct GLPostBuf { GLuint fbo = 0, tex = 0; int w = 0, h = 0; };
GLPostBuf g_pb_scene, g_pb_b0, g_pb_b1;
glwin g_sh_bright = 0, g_sh_blur = 0, g_sh_comp = 0;
GLuint g_tx_vao = 0;

void gl_post_ensure(int w, int h) {
    auto* f = glf(); if (!f) return;
    if (g_ms_w == w && g_ms_h == h) return;
    g_ms_w = w; g_ms_h = h;
    // старые буферы
    if (g_fbo_ms) f->glDeleteFramebuffers(1, &g_fbo_ms);
    if (g_ms_col) f->glDeleteRenderbuffers(1, &g_ms_col);
    if (g_ms_depth) f->glDeleteRenderbuffers(1, &g_ms_depth);
    // MSAA-FBO: цвет RGBA16F + depth, оба multisampled
    f->glGenFramebuffers(1, &g_fbo_ms);
    f->glBindFramebuffer(GL_FRAMEBUFFER, g_fbo_ms);
    f->glGenRenderbuffers(1, &g_ms_col);
    f->glBindRenderbuffer(GL_RENDERBUFFER, g_ms_col);
    f->glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_post_samples,
                                        GL_RGBA16F, w, h);
    f->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                 GL_RENDERBUFFER, g_ms_col);
    f->glGenRenderbuffers(1, &g_ms_depth);
    f->glBindRenderbuffer(GL_RENDERBUFFER, g_ms_depth);
    f->glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_post_samples,
                                        GL_DEPTH_COMPONENT24, w, h);
    f->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                 GL_RENDERBUFFER, g_ms_depth);
    f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // текстурные цели: сцена (полный размер), bloom (половина)
    auto mk = [](GLPostBuf& b, int bw, int bh) {
        auto* f2 = glf();
        if (b.fbo) { f2->glDeleteFramebuffers(1, &b.fbo);
                     f2->glDeleteTextures(1, &b.tex); }
        f2->glGenTextures(1, &b.tex);
        f2->glBindTexture(GL_TEXTURE_2D, b.tex);
        f2->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, bw, bh, 0,
                         GL_RGBA, GL_FLOAT, nullptr);
        f2->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        f2->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        f2->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        f2->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        f2->glGenFramebuffers(1, &b.fbo);
        f2->glBindFramebuffer(GL_FRAMEBUFFER, b.fbo);
        f2->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, b.tex, 0);
        b.w = bw; b.h = bh;
    };
    mk(g_pb_scene, w, h);
    mk(g_pb_b0, std::max(1, w / 2), std::max(1, h / 2));
    mk(g_pb_b1, std::max(1, w / 2), std::max(1, h / 2));
    f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

glwin gl_bright_shader() {
    if (g_sh_bright) return g_sh_bright;
    std::string vs = R"(#version 330 core
void main(){
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";
    std::string fs = R"(#version 330 core
uniform sampler2D uTex;
uniform float uThresh;
out vec4 FragColor;
void main(){
    vec3 c = texelFetch(uTex, ivec2(gl_FragCoord.xy) * 2, 0).rgb;
    // мягкое колено — граница порога не видна
    float br = max(c.r, max(c.g, c.b));
    float soft = clamp(br - uThresh + 0.5, 0.0, 1.0);
    soft = soft * soft / max(br, 1e-4);
    FragColor = vec4(c * max(soft, step(uThresh, br)), 1.0);
})";
    g_sh_bright = (glwin)gl_create_shader(vs, fs);
    return g_sh_bright;
}

glwin gl_blur_shader() {
    if (g_sh_blur) return g_sh_blur;
    std::string vs = R"(#version 330 core
void main(){
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";
    std::string fs = R"(#version 330 core
uniform sampler2D uTex;
uniform vec2 uDir;         // направление размытия: (1,0) или (0,1)
out vec4 FragColor;
void main(){
    vec2 ts = 1.0 / vec2(textureSize(uTex, 0));
    vec2 uv = gl_FragCoord.xy * ts;
    vec2 o1 = uDir * ts * 1.3846153, o2 = uDir * ts * 3.2307692;
    vec3 c = texture(uTex, uv).rgb * 0.227027;
    c += (texture(uTex, uv + o1).rgb + texture(uTex, uv - o1).rgb) * 0.3162162;
    c += (texture(uTex, uv + o2).rgb + texture(uTex, uv - o2).rgb) * 0.0702703;
    FragColor = vec4(c, 1.0);
})";
    g_sh_blur = (glwin)gl_create_shader(vs, fs);
    return g_sh_blur;
}

glwin gl_comp_shader() {
    if (g_sh_comp) return g_sh_comp;
    std::string vs = R"(#version 330 core
void main(){
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";
    std::string fs = R"(#version 330 core
uniform sampler2D uTex;
uniform sampler2D uBloom;
uniform vec2 uRes;
uniform float uTime;
uniform float uExposure;
uniform float uBloomAmt;
uniform float uGrain;
uniform float uAberr;
out vec4 FragColor;
float phash(vec2 p){ return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
vec3 aces(vec3 x){
    // fitted ACES (Stephen Hill) — кинематографический тонмап
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14),
                 0.0, 1.0);
}
void main(){
    vec2 uv = gl_FragCoord.xy / uRes;
    // хроматическая аберрация — края сильнее, как у объектива
    vec2 d = uv - 0.5;
    vec2 off = d * (uAberr / uRes.x) * 8.0;
    vec3 col;
    col.r = texture(uTex, uv + off).r;
    col.g = texture(uTex, uv).g;
    col.b = texture(uTex, uv - off).b;
    // bloom: половинка размытия поднимается к полному
    col += texture(uBloom, uv).rgb * uBloomAmt;
    col *= uExposure;
    col = aces(col);
    // виньетка
    float v = smoothstep(0.95, 0.35, length(d) * 1.35);
    col *= mix(0.72, 1.0, v);
    // гамма + зерно плёнки
    col = pow(max(col, 0.0), vec3(1.0 / 2.2));
    col += (phash(uv * uRes + fract(uTime) * 317.0) - 0.5) * uGrain;
    FragColor = vec4(col, 1.0);
})";
    g_sh_comp = (glwin)gl_create_shader(vs, fs);
    return g_sh_comp;
}

// запрошенный скриншот: читаем scene-текстуру после resolve
// (glReadPixels на multisample-FBO не работает — чёрный кадр)
std::string g_shot_path;

// проход поста после кадра: resolve -> bright -> blur -> composite
void gl_post_run(int w, int h) {
    auto* f = glf(); if (!f) return;
    // resolve MSAA -> текстура сцены
    f->glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo_ms);
    f->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_pb_scene.fbo);
    f->glBlitFramebuffer(0, 0, w, h, 0, 0, w, h,
                         GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (!g_shot_path.empty()) {
        std::vector<unsigned char> px((size_t)w * h * 4);
        f->glBindFramebuffer(GL_READ_FRAMEBUFFER, g_pb_scene.fbo);
        f->glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        QImage img(px.data(), w, h, w * 4, QImage::Format_RGBA8888);
        img.mirrored().save(QString::fromStdString(g_shot_path));
        g_shot_path.clear();
    }
    if (!g_tx_vao) f->glGenVertexArrays(1, &g_tx_vao);
    f->glDisable(GL_DEPTH_TEST);
    f->glDepthMask(GL_FALSE);
    f->glBindVertexArray(g_tx_vao);
    // bright-pass в половинном размере
    if (!g_sh_bright) gl_bright_shader();
    auto* sb = (NGLShader*)g_sh_bright;
    f->glBindFramebuffer(GL_FRAMEBUFFER, g_pb_b0.fbo);
    f->glViewport(0, 0, g_pb_b0.w, g_pb_b0.h);
    f->glUseProgram(sb->prog);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, g_pb_scene.tex);
    f->glUniform1i(f->glGetUniformLocation(sb->prog, "uTex"), 0);
    f->glUniform1f(f->glGetUniformLocation(sb->prog, "uThresh"), 1.0f);
    f->glDrawArrays(GL_TRIANGLES, 0, 3);
    // blur ping-pong: B0 -> B1 (H), B1 -> B0 (V), 3 итерации
    if (!g_sh_blur) gl_blur_shader();
    auto* sl = (NGLShader*)g_sh_blur;
    f->glUseProgram(sl->prog);
    for (int i = 0; i < 3; i++) {
        f->glBindFramebuffer(GL_FRAMEBUFFER, g_pb_b1.fbo);
        f->glViewport(0, 0, g_pb_b1.w, g_pb_b1.h);
        f->glBindTexture(GL_TEXTURE_2D, g_pb_b0.tex);
        f->glUniform2f(f->glGetUniformLocation(sl->prog, "uDir"),
                       1.0f, 0.0f);
        f->glDrawArrays(GL_TRIANGLES, 0, 3);
        f->glBindFramebuffer(GL_FRAMEBUFFER, g_pb_b0.fbo);
        f->glViewport(0, 0, g_pb_b0.w, g_pb_b0.h);
        f->glBindTexture(GL_TEXTURE_2D, g_pb_b1.tex);
        f->glUniform2f(f->glGetUniformLocation(sl->prog, "uDir"),
                       0.0f, 1.0f);
        f->glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    // композит на экран (default fb)
    if (!g_sh_comp) gl_comp_shader();
    auto* sc = (NGLShader*)g_sh_comp;
    auto* win = cur_win();
    f->glBindFramebuffer(GL_FRAMEBUFFER,
                       win ? (GLuint)win->defaultFramebufferObject() : 0);
    f->glViewport(0, 0, w, h);
    f->glUseProgram(sc->prog);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, g_pb_scene.tex);
    f->glUniform1i(f->glGetUniformLocation(sc->prog, "uTex"), 0);
    f->glActiveTexture(GL_TEXTURE1);
    f->glBindTexture(GL_TEXTURE_2D, g_pb_b0.tex);
    f->glUniform1i(f->glGetUniformLocation(sc->prog, "uBloom"), 1);
    f->glActiveTexture(GL_TEXTURE0);
    f->glUniform2f(f->glGetUniformLocation(sc->prog, "uRes"),
                   (float)w, (float)h);
    f->glUniform1f(f->glGetUniformLocation(sc->prog, "uTime"),
                   (float)gl_time());
    f->glUniform1f(f->glGetUniformLocation(sc->prog, "uExposure"),
                   g_post_exposure);
    f->glUniform1f(f->glGetUniformLocation(sc->prog, "uBloomAmt"),
                   g_post_bloom);
    f->glUniform1f(f->glGetUniformLocation(sc->prog, "uGrain"), g_post_grain);
    f->glUniform1f(f->glGetUniformLocation(sc->prog, "uAberr"), g_post_aberr);
    f->glDrawArrays(GL_TRIANGLES, 0, 3);
    f->glBindVertexArray(0);
    f->glDepthMask(GL_TRUE);
    f->glEnable(GL_DEPTH_TEST);
}

// ---------- звезда ----------
// Диск с лимб-даркенингом (край темнее — физика фотосферы),
// грануляцией и горячим ободком. Яркость > 1 — уходит в bloom.
glwin g_star_shader = 0;
glwin g_star_mesh = 0;
QVector3D g_star_col(1.0f, 0.32f, 0.10f);

glwin gl_star_shader() {
    if (g_star_shader) return g_star_shader;
    std::string vs = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 u_viewProj;
uniform mat4 u_model;
out vec3 vN;
out vec3 vW;
out vec3 vLocal;
void main(){
    vec4 wp = u_model * vec4(aPos, 1.0);
    vW = wp.xyz;
    vN = normalize(mat3(u_model) * aPos);
    vLocal = normalize(aPos);
    gl_Position = u_viewProj * wp;
})";
    std::string fs = std::string(R"(#version 330 core
in vec3 vN;
in vec3 vW;
in vec3 vLocal;
uniform vec3 u_col;
uniform vec3 u_camPos;
uniform float uTime;
out vec4 FragColor;
)") + EPL_NOISE + R"(
void main(){
    vec3 n = normalize(vLocal);
    vec3 vdir = normalize(u_camPos - vW);
    float mu = clamp(dot(n, vdir), 0.0, 1.0);
    // лимб-даркенинг фотосферы: I = 1 - u(1-mu), u~0.75
    float I = 1.0 - 0.75 * (1.0 - mu);
    // грануляция: пятна конвекции, медленно движутся
    float gr = evn(n * 9.0 + vec3(uTime * 0.05));
    gr = 0.82 + 0.36 * gr + 0.14 * evn(n * 22.0 - vec3(uTime * 0.03));
    vec3 col = u_col * I * gr * 3.2;       // HDR — bloom подхватит
    // горячий ободок (хромосфера)
    float rim = pow(1.0 - abs(dot(n, vdir)), 3.0);
    col += u_col * rim * 1.6;
    FragColor = vec4(col, 1.0);
})";
    g_star_shader = (glwin)gl_create_shader(vs, fs);
    return g_star_shader;
}

// ---------- transmit overlay ----------
glwin g_tx_shader = 0;

glwin gl_tx_shader() {
    if (g_tx_shader) return g_tx_shader;
    std::string vs = R"(#version 330 core
void main(){
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";
    std::string fs = R"(#version 330 core
uniform vec2 uRes;
uniform float uTime;
uniform float uI;
out vec4 FragColor;
float thash(vec2 p){ return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
void main(){
    vec2 uv = gl_FragCoord.xy / uRes;
    float add = 0.0, dark = 0.0;
    // сканлайны
    dark += (0.5 + 0.5 * sin(gl_FragCoord.y * 1.9)) * 0.10 * uI;
    // статика
    add += thash(uv * uRes * 0.5 + fract(uTime) * 913.0) * 0.06 * uI;
    // бегущая полоса помехи
    float rb = smoothstep(0.10, 0.0, abs(fract(uv.y - uTime * 0.06) - 0.5));
    add += rb * 0.10 * uI;
    dark += rb * 0.10 * uI;
    // редкие глитчи построчно
    float row = floor(uv.y * 160.0);
    float g = step(0.9975, thash(vec2(row, floor(uTime * 24.0))));
    add += g * 0.20 * uI;
    // виньетка + краевая подсветка
    float d = length(uv - 0.5);
    dark += smoothstep(0.45, 0.85, d) * 0.35;
    add += smoothstep(0.55, 0.95, d) * 0.12 * uI;
    // итог: e>0 — засветка, e<0 — затемнение
    float e = clamp(add - dark, -1.0, 1.0);
    vec3 ink = e >= 0.0 ? vec3(0.75, 0.85, 1.0) : vec3(0.0, 0.01, 0.04);
    FragColor = vec4(ink, clamp(abs(e), 0.0, 1.0));
})";
    g_tx_shader = (glwin)gl_create_shader(vs, fs);
    return g_tx_shader;
}

// ---------- shared vars ----------
double g_vars[2048] = {0};
int g_frame_no = 0;

} // namespace

// ============================================================
// API
// ============================================================

long long nlgl_window(const std::string& title,
                      long long w, long long h) {
    gl_app();
    g_current_win = (glwin)new NGLWindow(title, (int)w, (int)h);
    g_current_shader = 0;
    return g_current_win;
}
void nlgl_show(long long win) { win_of(win)->show(); }
void nlgl_mouselock(long long win) { win_of(win)->lockMouse(); }
void nlgl_onFrame(long long win, std::function<void()> fn) {
    win_of(win)->setFrame(std::move(fn));
}
long long nlgl_key(long long vk) {
    auto* w = cur_win();
    return w && w->keyDown((int)vk) ? 1 : 0;
}
long long nlgl_width(long long win) { return win_of(win)->width(); }
long long nlgl_height(long long win) { return win_of(win)->height(); }

// ---------- camera ----------
void nlgl_campos(long long win, double x, double y, double z) {
    win_of(win)->camera().pos = QVector3D(x, y, z);
}
void nlgl_camrot(long long win, double yaw, double pitch) {
    auto& c = win_of(win)->camera();
    c.yaw = (float)yaw; c.pitch = (float)pitch;
}
void nlgl_camroll(long long win, double deg) {
    win_of(win)->camera().roll = (float)deg;
}
void nlgl_camspeed(long long win, double s) {
    win_of(win)->camera().speed = (float)s;
}
void nlgl_camfar(long long win, double d) {
    win_of(win)->camera().far_plane = (float)d;
}
void nlgl_camfly(long long win, long long fly) {
    win_of(win)->camera().fly = fly != 0;
}
double nlgl_camx(long long win) { return win_of(win)->camera().pos.x(); }
double nlgl_camy(long long win) { return win_of(win)->camera().pos.y(); }
double nlgl_camz(long long win) { return win_of(win)->camera().pos.z(); }

void nlgl_camupdate(long long win, double dt) {
    auto* w = win_of(win);
    NGLCamera& cam = w->camera();
    float dx = w->mouseDX() * cam.sens;
    float dy = w->mouseDY() * cam.sens;
    cam.last_mdx = dx; cam.last_mdy = dy;
    cam.yaw -= dx;
    cam.pitch -= dy;
    cam.pitch = qBound(-85.0f, cam.pitch, 85.0f);

    QVector3D move(0, 0, 0);
    QVector3D f2 = cam.fwd2d(), r2 = cam.right2d();
    if (w->keyDown(0x57) || w->keyDown(0x26)) move += f2;  // W / Up
    if (w->keyDown(0x53) || w->keyDown(0x28)) move -= f2;  // S / Down
    if (w->keyDown(0x44) || w->keyDown(0x27)) move += r2;  // D / Right
    if (w->keyDown(0x41) || w->keyDown(0x25)) move -= r2;  // A / Left
    if (move.lengthSquared() > 0) {
        move.normalize();
        float spd = w->keyDown(0x10) ? cam.sprint : cam.speed;  // Shift
        move *= spd;
    }
    cam.pos += move * (float)dt;
    if (cam.fly) {
        // полёт: Space/стр вверх, C — вниз
        float spd = w->keyDown(0x10) ? cam.sprint : cam.speed;
        if (w->keyDown(0x20)) cam.pos.setY(cam.pos.y() + spd * (float)dt);
        if (w->keyDown(0x43)) cam.pos.setY(cam.pos.y() - spd * (float)dt);
    }
}

// ---------- свет ----------
void nlgl_sun(double x, double y, double z) {
    g_sun_dir = QVector3D(x, y, z).normalized();
}
void nlgl_suncolor(double r, double g, double b) {
    g_sun_color = QVector3D(r, g, b);
}
void nlgl_ambient(double sr, double sg, double sb,
                  double gr, double gg, double gb) {
    g_hemi_sky = QVector3D(sr, sg, sb);
    g_hemi_ground = QVector3D(gr, gg, gb);
}
void nlgl_fog(double r, double g, double b, double s, double e) {
    g_fog_color = QVector3D(r, g, b);
    g_fog_start = (float)s; g_fog_end = (float)e;
}
void nlgl_point(double x, double y, double z,
                double r, double g, double b) {
    g_point_pos = QVector3D(x, y, z);
    g_point_color = QVector3D(r, g, b);
}
void nlgl_pointoff() { g_point_color = QVector3D(0, 0, 0); }

// ---------- меши ----------
long long nlgl_sphere(double rad, long long segs,
                      double r, double g, double b) {
    std::vector<float> v; std::vector<unsigned> idx;
    int rings = std::max(2, (int)segs / 2);
    for (int iy = 0; iy <= rings; iy++) {
        float phi = (float)iy / rings * M_PI;
        for (int ix = 0; ix <= segs; ix++) {
            float th = (float)ix / segs * 2.0f * M_PI;
            float nx = sin(phi) * cos(th), ny = cos(phi), nz = sin(phi) * sin(th);
            gl_push_vert(v, nx * rad, ny * rad, nz * rad, nx, ny, nz,
                         (float)r, (float)g, (float)b);
        }
    }
    for (int iy = 0; iy < rings; iy++) {
        for (int ix = 0; ix < segs; ix++) {
            unsigned a = iy * (segs + 1) + ix;
            idx.insert(idx.end(), {a, a + (unsigned)segs + 1, a + 1,
                                   a + 1, a + (unsigned)segs + 1,
                                   a + (unsigned)segs + 2});
        }
    }
    return (long long)gl_make_mesh(v, &idx);
}

long long nlgl_box(double w, double h, double d,
                   double r, double g, double b) {
    std::vector<float> v; std::vector<unsigned> idx;
    float hw = w / 2, hh = h / 2, hd = d / 2;
    auto face = [&](float ax,float ay,float az,float bx,float by,float bz,
                    float cx,float cy,float cz,float dx2,float dy2,float dz2,
                    float nx,float ny,float nz){
        unsigned base = (unsigned)(v.size() / 11);
        gl_push_vert(v, ax,ay,az, nx,ny,nz, (float)r,(float)g,(float)b);
        gl_push_vert(v, bx,by,bz, nx,ny,nz, (float)r,(float)g,(float)b);
        gl_push_vert(v, cx,cy,cz, nx,ny,nz, (float)r,(float)g,(float)b);
        gl_push_vert(v, dx2,dy2,dz2, nx,ny,nz, (float)r,(float)g,(float)b);
        idx.insert(idx.end(), {base, base+1, base+2, base, base+2, base+3});
    };
    face(-hw,-hh, hd,  hw,-hh, hd,  hw, hh, hd, -hw, hh, hd, 0,0,1);   // +Z
    face( hw,-hh,-hd, -hw,-hh,-hd, -hw, hh,-hd,  hw, hh,-hd, 0,0,-1);  // -Z
    face(-hw,-hh,-hd, -hw,-hh, hd, -hw, hh, hd, -hw, hh,-hd, -1,0,0);  // -X
    face( hw,-hh, hd,  hw,-hh,-hd,  hw, hh,-hd,  hw, hh, hd, 1,0,0);   // +X
    face(-hw, hh, hd,  hw, hh, hd,  hw, hh,-hd, -hw, hh,-hd, 0,1,0);   // +Y
    face(-hw,-hh,-hd,  hw,-hh,-hd,  hw,-hh, hd, -hw,-hh, hd, 0,-1,0);  // -Y
    return (long long)gl_make_mesh(v, &idx);
}

long long nlgl_cylinder(double rad, double h, long long segs,
                        double r, double g, double b) {
    std::vector<float> v; std::vector<unsigned> idx;
    float hh = h / 2;
    for (int i = 0; i <= segs; i++) {
        float a = (float)i / segs * 2.0f * M_PI;
        float c = cos(a), s = sin(a);
        gl_push_vert(v, c * rad, -hh, s * rad, c, 0, s,
                     (float)r, (float)g, (float)b);
        gl_push_vert(v, c * rad, hh, s * rad, c, 0, s,
                     (float)r, (float)g, (float)b);
    }
    for (int i = 0; i < segs; i++) {
        unsigned a = i * 2;
        idx.insert(idx.end(), {a, a + 1, a + 2, a + 1, a + 3, a + 2});
    }
    // крышки
    unsigned base = (unsigned)(v.size() / 11);
    gl_push_vert(v, 0, hh, 0, 0, 1, 0, (float)r, (float)g, (float)b);
    gl_push_vert(v, 0, -hh, 0, 0, -1, 0, (float)r, (float)g, (float)b);
    for (int i = 0; i < segs; i++) {
        idx.insert(idx.end(), {base, (unsigned)(i * 2 + 1),
                               (unsigned)(i * 2 + 3)});
        idx.insert(idx.end(), {base + 1, (unsigned)(i * 2 + 2),
                               (unsigned)(i * 2)});
    }
    return (long long)gl_make_mesh(v, &idx);
}

long long nlgl_cone(double rad, double h, long long segs,
                    double r, double g, double b) {
    std::vector<float> v; std::vector<unsigned> idx;
    float hh = h / 2;
    for (int i = 0; i < segs; i++) {
        float a0 = (float)i / segs * 2.0f * M_PI;
        float a1 = (float)(i + 1) / segs * 2.0f * M_PI;
        float c0 = cos(a0), s0 = sin(a0), c1 = cos(a1), s1 = sin(a1);
        QVector3D p1(c0 * rad, -hh, s0 * rad), p2(c1 * rad, -hh, s1 * rad),
                pk(0, hh, 0);
        QVector3D n = QVector3D::crossProduct(p2 - p1, pk - p1).normalized();
        unsigned base = (unsigned)(v.size() / 11);
        gl_push_vert(v, p1.x(), p1.y(), p1.z(), n.x(), n.y(), n.z(),
                     (float)r, (float)g, (float)b);
        gl_push_vert(v, p2.x(), p2.y(), p2.z(), n.x(), n.y(), n.z(),
                     (float)r, (float)g, (float)b);
        gl_push_vert(v, pk.x(), pk.y(), pk.z(), n.x(), n.y(), n.z(),
                     (float)r, (float)g, (float)b);
        idx.insert(idx.end(), {base, base + 1, base + 2});
    }
    return (long long)gl_make_mesh(v, &idx);
}

// OBJ: v/vt/vn/f, фан-триангуляция, нормализация по длинной стороне
long long nlgl_obj(const std::string& path, double size,
                   double r, double g, double b) {
    std::ifstream in(path);
    if (!in) {
        printf("[gl] obj not found: %s", path.c_str());
        fflush(stdout);
        return 0;
    }
    std::unordered_map<std::string, QVector3D> mtl;
    QVector3D cur_mtl((float)r, (float)g, (float)b);
    std::vector<QVector3D> vp, vn;
    std::vector<QVector2D> vt;
    std::vector<float> v; std::vector<unsigned> idx;
    auto parse_ref = [](const std::string& tok, int& vi, int& ti, int& ni) {
        vi = ti = ni = 0;
        int p1 = (int)tok.find('/');
        if (p1 < 0) { vi = atoi(tok.c_str()); return; }
        vi = atoi(tok.substr(0, p1).c_str());
        int p2 = (int)tok.find('/', p1 + 1);
        if (p2 < 0) { ti = atoi(tok.substr(p1 + 1).c_str()); return; }
        if (p2 > p1 + 1) ti = atoi(tok.substr(p1 + 1, p2 - p1 - 1).c_str());
        ni = atoi(tok.substr(p2 + 1).c_str());
    };
    struct Vt { int vi, ti, ni; };
    std::unordered_map<std::string, unsigned> remap;
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 2) continue;
        std::istringstream ss(line);
        std::string kw; ss >> kw;
        if (kw == "mtllib") {
            std::string mf; std::getline(ss, mf);
            while (!mf.empty() && (mf.front() == ' ' || mf.front() == '\t'))
                mf.erase(mf.begin());
            std::string dir = path.substr(0, path.find_last_of('/') + 1);
            std::ifstream mi(dir + mf);
            std::string ml, mn;
            while (std::getline(mi, ml)) {
                std::istringstream ms(ml);
                std::string mk; ms >> mk;
                if (mk == "newmtl") { ms >> mn; }
                else if (mk == "Kd" && !mn.empty()) {
                    double kr, kg, kb; ms >> kr >> kg >> kb;
                    mtl[mn] = QVector3D(kr, kg, kb);
                }
            }
        } else if (kw == "usemtl") {
            std::string mn; ss >> mn;
            auto it = mtl.find(mn);
            cur_mtl = (it != mtl.end()) ? it->second
                                        : QVector3D(r, g, b);
        } else if (kw == "v") {
            double x, y, z; ss >> x >> y >> z;
            vp.emplace_back(x, y, z);
        } else if (kw == "vt") {
            double u, vv; ss >> u >> vv;
            vt.emplace_back(u, vv);
        } else if (kw == "vn") {
            double x, y, z; ss >> x >> y >> z;
            vn.emplace_back((float)x, (float)y, (float)z);
        } else if (kw == "f") {
            std::vector<Vt> face;
            std::string tok;
            while (ss >> tok) {
                if (!tok.empty() && tok[0] == '#') break;
                Vt rr; parse_ref(tok, rr.vi, rr.ti, rr.ni);
                if (rr.vi < 0) rr.vi = (int)vp.size() + rr.vi + 1;
                if (rr.ti < 0) rr.ti = (int)vt.size() + rr.ti + 1;
                if (rr.ni < 0) rr.ni = (int)vn.size() + rr.ni + 1;
                face.push_back(rr);
            }
            if (face.size() < 3) continue;
            QVector3D n0;
            if (face[0].ni <= 0 && face.size() >= 3) {
                QVector3D a = vp[face[0].vi - 1], c = vp[face[1].vi - 1],
                          d = vp[face[2].vi - 1];
                n0 = QVector3D::crossProduct(c - a, d - a).normalized();
            }
            for (size_t i = 2; i < face.size(); i++) {
                Vt tri[3] = {face[0], face[i - 1], face[i]};
                unsigned base = (unsigned)(v.size() / 11);
                for (int k = 0; k < 3; k++) {
                    Vt& rr = tri[k];
                    QVector3D p = vp[rr.vi - 1];
                    QVector3D n = (rr.ni > 0 && rr.ni <= (int)vn.size())
                                  ? vn[rr.ni - 1] : n0;
                    float u = 0, vv = 0;
                    if (rr.ti > 0 && rr.ti <= (int)vt.size()) {
                        u = vt[rr.ti - 1].x(); vv = vt[rr.ti - 1].y();
                    }
                    gl_push_vert(v, p.x(), p.y(), p.z(), n.x(), n.y(), n.z(),
                                 cur_mtl.x(), cur_mtl.y(), cur_mtl.z(), u, vv);
                }
                idx.insert(idx.end(), {base, base + 1, base + 2});
            }
        }
    }
    if (v.empty()) {
        printf("[gl] obj empty: %s", path.c_str());
        fflush(stdout);
        return 0;
    }
    // нормализация: центр в 0, длинная сторона = size, на землю
    QVector3D mn(1e9f, 1e9f, 1e9f), mx(-1e9f, -1e9f, -1e9f);
    for (size_t i = 0; i < v.size(); i += 11) {
        mn.setX(std::min(mn.x(), v[i]));   mx.setX(std::max(mx.x(), v[i]));
        mn.setY(std::min(mn.y(), v[i+1])); mx.setY(std::max(mx.y(), v[i+1]));
        mn.setZ(std::min(mn.z(), v[i+2])); mx.setZ(std::max(mx.z(), v[i+2]));
    }
    QVector3D c0 = (mn + mx) * 0.5f;
    float sc = mx.distanceToPoint(mn);
    if (sc <= 0) sc = 1;
    sc = (float)size / sc;
    for (size_t i = 0; i < v.size(); i += 11) {
        v[i]   = (v[i]   - c0.x()) * sc;
        v[i+1] = (v[i+1] - mn.y()) * sc;
        v[i+2] = (v[i+2] - c0.z()) * sc;
    }
    auto* m = gl_make_mesh(v, &idx);
    if (!vt.empty()) m->use_uv = 1;
    printf("[gl] obj loaded: %s (%d tris)", path.c_str(),
           (int)(idx.size() / 3));
    fflush(stdout);
    return (long long)m;
}

// ---------- ручной меш ----------
static std::vector<float> g_mb_v;
static std::vector<unsigned> g_mb_i;
void nlgl_beginmesh() { g_mb_v.clear(); g_mb_i.clear(); }
long long nlgl_vertex3(double x, double y, double z,
                       double r, double g, double b) {
    gl_push_vert(g_mb_v, (float)x, (float)y, (float)z, 0, 0, 0,
                 (float)r, (float)g, (float)b);
    return (long long)(g_mb_v.size() / 11) - 1;
}
void nlgl_tri(long long a, long long b, long long c) {
    g_mb_i.insert(g_mb_i.end(), {(unsigned)a, (unsigned)b, (unsigned)c});
}
long long nlgl_endmesh() {
    // автонормали — суммируем грани и нормируем
    int n = (int)(g_mb_v.size() / 11);
    for (size_t t = 0; t + 2 < g_mb_i.size(); t += 3) {
        unsigned ia = g_mb_i[t], ib = g_mb_i[t + 1], ic = g_mb_i[t + 2];
        QVector3D a(g_mb_v[ia*11], g_mb_v[ia*11+1], g_mb_v[ia*11+2]);
        QVector3D bb(g_mb_v[ib*11], g_mb_v[ib*11+1], g_mb_v[ib*11+2]);
        QVector3D c(g_mb_v[ic*11], g_mb_v[ic*11+1], g_mb_v[ic*11+2]);
        QVector3D fn = QVector3D::crossProduct(bb - a, c - a);
        float* nv[3] = {&g_mb_v[ia*11+3], &g_mb_v[ib*11+3], &g_mb_v[ic*11+3]};
        for (int k = 0; k < 3; k++) {
            nv[k][0] += fn.x(); nv[k][1] += fn.y(); nv[k][2] += fn.z();
        }
    }
    for (int i = 0; i < n; i++) {
        QVector3D* nv = (QVector3D*)&g_mb_v[i*11+3];
        if (nv->length() > 1e-6) nv->normalize();
        else *nv = QVector3D(0, 1, 0);
    }
    return (long long)gl_make_mesh(g_mb_v, &g_mb_i);
}

void nlgl_meshtex(long long mesh, const std::string& path, double scale) {
    if (!mesh) return;
    auto* m = (NGLMesh*)mesh;
    m->tex_path = path;
    m->tex_scale = (float)scale;
    GLuint t = gl_load_tex(path);   // контекст есть — грузим сразу
    if (t) m->tex = t;              // иначе — лениво при отрисовке
}
void nlgl_meshalpha(long long mesh, double a) {
    if (mesh) ((NGLMesh*)mesh)->alpha = (float)a;
}
void nlgl_meshfree(long long mesh) {
    if (!mesh) return;
    auto* f = glf();
    auto* m = (NGLMesh*)mesh;
    if (f) {
        f->glDeleteVertexArrays(1, &m->vao);
        f->glDeleteBuffers(1, &m->vbo);
        f->glDeleteBuffers(1, &m->ebo);
    }
    delete m;
}

// ---------- отрисовка ----------
void nlgl_clear(double r, double g, double b) {
    auto* f = glf(); if (!f) return;
    f->glClearColor((float)r, (float)g, (float)b, 1.0);
    f->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    f->glEnable(GL_DEPTH_TEST);
}

void nlgl_draw(long long mesh, double x, double y, double z,
               double rx, double ry, double rz,
               double sx, double sy, double sz) {
    auto* f = glf(); if (!f || !mesh) return;
    auto* win = cur_win(); if (!win) return;
    auto* m = (NGLMesh*)mesh;
    auto* s = (NGLShader*)gl_default_shader(); if (!s) return;
    QMatrix4x4 model;
    model.translate(x, y, z);
    model.rotate(ry, 0, 1, 0);
    model.rotate(rx, 1, 0, 0);
    model.rotate(rz, 0, 0, 1);
    model.scale(sx, sy, sz);
    gl_draw_one(f, m, model, s, win->width(), win->height());
}

void nlgl_sceneadd(long long mesh, double x, double y, double z,
                   double rx, double ry, double rz,
                   double sx, double sy, double sz) {
    if (!mesh) return;
    QMatrix4x4 model;
    model.translate(x, y, z);
    model.rotate(rx, 1, 0, 0);
    model.rotate(ry, 0, 1, 0);
    model.rotate(rz, 0, 0, 1);
    model.scale(sx, sy, sz);
    g_scene.push_back({(NGLMesh*)mesh, model});
}
void nlgl_sceneclear() { g_scene.clear(); }
void nlgl_scenesave(long long key) { g_scene_cache[key] = g_scene; }
void nlgl_sceneload(long long key) {
    auto it = g_scene_cache.find(key);
    if (it != g_scene_cache.end()) g_scene = it->second;
}
void nlgl_sceneappend(long long key) {
    auto it = g_scene_cache.find(key);
    if (it != g_scene_cache.end())
        g_scene.insert(g_scene.end(), it->second.begin(), it->second.end());
}
void nlgl_scenedraw() {
    auto* f = glf(); if (!f) return;
    auto* s = (NGLShader*)gl_default_shader(); if (!s) return;
    auto* win = cur_win();
    f->glUseProgram(s->prog);
    for (auto& o : g_scene)
        gl_draw_one(f, o.m, o.model, s, win->width(), win->height());
}

// ---------- небо ----------
void nlgl_skyenable(long long on) { g_sky_enabled = on != 0; }
void nlgl_bhdir(double x, double y, double z) {
    g_bh_dir = QVector3D(x, y, z).normalized();
}
void nlgl_sky() {
    if (!g_sky_enabled) return;
    auto* win = cur_win();
    auto* f = glf();
    if (!win || !f) return;
    if (!g_sky_shader) gl_sky_shader();
    if (!g_sky_mesh) g_sky_mesh = nlgl_sphere(400, 32, 1, 1, 1);
    if (!g_sky_shader || !g_sky_mesh) return;
    NGLMesh* m = (NGLMesh*)g_sky_mesh;
    if (m->pending && !gl_upload_mesh(m)) return;
    NGLShader* s = (NGLShader*)g_sky_shader;

    f->glDepthMask(GL_FALSE);
    f->glUseProgram(s->prog);
    QMatrix4x4 view = win->camera().getView();
    QMatrix4x4 proj = win->camera().getProj(win->width(), win->height());
    QMatrix4x4 vp = proj * view;
    QMatrix4x4 model;
    QVector3D cp = win->camera().pos;
    model.translate(cp);
    f->glUniformMatrix4fv(f->glGetUniformLocation(s->prog, "u_viewProj"),
                        1, GL_FALSE, vp.constData());
    f->glUniformMatrix4fv(f->glGetUniformLocation(s->prog, "u_model"),
                        1, GL_FALSE, model.constData());
    f->glUniform3f(f->glGetUniformLocation(s->prog, "u_camPos"),
                   cp.x(), cp.y(), cp.z());
    f->glUniform1f(f->glGetUniformLocation(s->prog, "uTime"),
                   (float)gl_time());
    f->glUniform3f(f->glGetUniformLocation(s->prog, "uBH_Pos"),
                   g_bh_dir.x(), g_bh_dir.y(), g_bh_dir.z());
    f->glBindVertexArray(m->vao);
    f->glDrawElements(GL_TRIANGLES, m->icount, GL_UNSIGNED_INT, 0);
    f->glBindVertexArray(0);
    f->glDepthMask(GL_TRUE);
}

// ---------- энергопланета ----------
void nlgl_eband(long long i, double f, double a, double p, double s) {
    if (i < 0 || i >= 8) return;
    g_ebands[i][0] = (float)f; g_ebands[i][1] = (float)a;
    g_ebands[i][2] = (float)p; g_ebands[i][3] = (float)s;
}
double nlgl_ebget(long long i, long long c) {
    if (i < 0 || i >= 8 || c < 0 || c > 3) return 0;
    return g_ebands[i][c];
}
void nlgl_ebcol(long long i, double r, double g, double b) {
    if (i < 0 || i >= 8) return;
    g_ebcol[i][0] = (float)r; g_ebcol[i][1] = (float)g;
    g_ebcol[i][2] = (float)b;
}
double nlgl_ebcolget(long long i, long long c) {
    if (i < 0 || i >= 8 || c < 0 || c > 2) return 0;
    return g_ebcol[i][c];
}
void nlgl_ebase(double r, double g, double b) {
    g_ebase = QVector3D(r, g, b);
}
void nlgl_eatmo(double r, double g, double b) {
    g_eatmo = QVector3D(r, g, b);
}
void nlgl_eatmok(double k) { g_eatmo_k = (float)k; }

void nlgl_eplanet(double x, double y, double z, double rad) {
    auto* f = glf(); if (!f) return;
    if (!g_epl_shader) gl_epl_shader();
    if (!g_epl_mesh) g_epl_mesh = nlgl_sphere(1, 64, 1, 1, 1);
    if (!g_epl_shader || !g_epl_mesh) return;
    NGLMesh* m = (NGLMesh*)g_epl_mesh;
    if (m->pending && !gl_upload_mesh(m)) return;
    NGLShader* s = (NGLShader*)g_epl_shader;
    auto* win = cur_win(); if (!win) return;

    f->glUseProgram(s->prog);
    QMatrix4x4 view = win->camera().getView();
    QMatrix4x4 proj = win->camera().getProj(win->width(), win->height());
    QMatrix4x4 vp = proj * view;
    QMatrix4x4 model;
    model.translate(x, y, z);
    model.scale(rad);
    f->glUniformMatrix4fv(f->glGetUniformLocation(s->prog, "u_viewProj"),
                        1, GL_FALSE, vp.constData());
    f->glUniformMatrix4fv(f->glGetUniformLocation(s->prog, "u_model"),
                        1, GL_FALSE, model.constData());
    f->glUniform4fv(f->glGetUniformLocation(s->prog, "u_bands[0]"),
                    8, &g_ebands[0][0]);
    f->glUniform3fv(f->glGetUniformLocation(s->prog, "u_bcol[0]"),
                    8, &g_ebcol[0][0]);
    f->glUniform3f(f->glGetUniformLocation(s->prog, "u_base"),
                   g_ebase.x(), g_ebase.y(), g_ebase.z());
    f->glUniform3f(f->glGetUniformLocation(s->prog, "u_atmo"),
                   g_eatmo.x(), g_eatmo.y(), g_eatmo.z());
    f->glUniform1f(f->glGetUniformLocation(s->prog, "u_atmoK"), g_eatmo_k);
    f->glUniform3f(f->glGetUniformLocation(s->prog, "u_sunDir"),
                   g_sun_dir.x(), g_sun_dir.y(), g_sun_dir.z());
    f->glUniform3f(f->glGetUniformLocation(s->prog, "u_sunColor"),
                   g_sun_color.x(), g_sun_color.y(), g_sun_color.z());
    QVector3D cp = win->camera().pos;
    f->glUniform3f(f->glGetUniformLocation(s->prog, "u_camPos"),
                   cp.x(), cp.y(), cp.z());
    f->glUniform1f(f->glGetUniformLocation(s->prog, "uTime"),
                   (float)gl_time());
    f->glBindVertexArray(m->vao);
    f->glDrawElements(GL_TRIANGLES, m->icount, GL_UNSIGNED_INT, 0);
    f->glBindVertexArray(0);
}

// ---------- звезда ----------
void nlgl_starcol(double r, double g, double b) {
    g_star_col = QVector3D(r, g, b);
}
void nlgl_star(double x, double y, double z, double rad) {
    auto* f = glf(); if (!f) return;
    if (!g_star_shader) gl_star_shader();
    if (!g_star_mesh) g_star_mesh = nlgl_sphere(1, 48, 1, 1, 1);
    if (!g_star_shader || !g_star_mesh) return;
    NGLMesh* m = (NGLMesh*)g_star_mesh;
    if (m->pending && !gl_upload_mesh(m)) return;
    NGLShader* s = (NGLShader*)g_star_shader;
    auto* win = cur_win(); if (!win) return;

    f->glUseProgram(s->prog);
    QMatrix4x4 view = win->camera().getView();
    QMatrix4x4 proj = win->camera().getProj(win->width(), win->height());
    QMatrix4x4 vp = proj * view;
    QMatrix4x4 model;
    model.translate(x, y, z);
    model.scale(rad);
    f->glUniformMatrix4fv(f->glGetUniformLocation(s->prog, "u_viewProj"),
                        1, GL_FALSE, vp.constData());
    f->glUniformMatrix4fv(f->glGetUniformLocation(s->prog, "u_model"),
                        1, GL_FALSE, model.constData());
    f->glUniform3f(f->glGetUniformLocation(s->prog, "u_col"),
                   g_star_col.x(), g_star_col.y(), g_star_col.z());
    QVector3D cp = win->camera().pos;
    f->glUniform3f(f->glGetUniformLocation(s->prog, "u_camPos"),
                   cp.x(), cp.y(), cp.z());
    f->glUniform1f(f->glGetUniformLocation(s->prog, "uTime"),
                   (float)gl_time());
    f->glBindVertexArray(m->vao);
    f->glDrawElements(GL_TRIANGLES, m->icount, GL_UNSIGNED_INT, 0);
    f->glBindVertexArray(0);
}

// ---------- transmit overlay ----------
void nlgl_transmit(double intensity) {
    auto* f = glf(); if (!f) return;
    auto* win = cur_win(); if (!win) return;
    if (!g_tx_shader) gl_tx_shader();
    if (!g_tx_shader) return;
    NGLShader* s = (NGLShader*)g_tx_shader;
    if (!g_tx_vao) f->glGenVertexArrays(1, &g_tx_vao);
    f->glUseProgram(s->prog);
    f->glUniform2f(f->glGetUniformLocation(s->prog, "uRes"),
                   (float)win->width(), (float)win->height());
    f->glUniform1f(f->glGetUniformLocation(s->prog, "uTime"),
                   (float)gl_time());
    f->glUniform1f(f->glGetUniformLocation(s->prog, "uI"), (float)intensity);
    f->glDisable(GL_DEPTH_TEST);
    f->glDepthMask(GL_FALSE);
    f->glEnable(GL_BLEND);
    f->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    f->glBindVertexArray(g_tx_vao);
    f->glDrawArrays(GL_TRIANGLES, 0, 3);
    f->glBindVertexArray(0);
    f->glDisable(GL_BLEND);
    f->glDepthMask(GL_TRUE);
    f->glEnable(GL_DEPTH_TEST);
}

// ---------- 2D HUD ----------
void nlgl_begin2d() {
    auto* win = cur_win();
    if (g_painter) { g_painter->end(); delete g_painter; g_painter = nullptr; }
    if (g_dev) { delete g_dev; g_dev = nullptr; }
    g_dev = new QOpenGLPaintDevice(win ? win->width() : 640,
                                   win ? win->height() : 480);
    g_painter = new QPainter(g_dev);
    g_painter->setRenderHint(QPainter::Antialiasing);
}
void nlgl_end2d() {
    if (g_painter) { g_painter->end(); delete g_painter; g_painter = nullptr; }
    if (g_dev) { delete g_dev; g_dev = nullptr; }
}
void nlgl_text(double x, double y, const std::string& s,
               long long r, long long g, long long b,
               long long size) {
    if (!g_painter) return;
    g_painter->setPen(QColor((int)r, (int)g, (int)b));
    QFont fnt = g_painter->font();
    fnt.setPointSize((int)size);
    g_painter->setFont(fnt);
    g_painter->drawText(QPointF(x, y), QString::fromStdString(s));
}
void nlgl_rect(double x, double y, double w, double h,
               long long r, long long g, long long b) {
    if (!g_painter) return;
    g_painter->setPen(Qt::NoPen);
    g_painter->setBrush(QColor((int)r, (int)g, (int)b));
    g_painter->drawRect(QRectF(x, y, w, h));
}
void nlgl_recta(double x, double y, double w, double h,
                long long r, long long g, long long b, long long a) {
    if (!g_painter) return;
    g_painter->setPen(Qt::NoPen);
    g_painter->setBrush(QColor((int)r, (int)g, (int)b, (int)a));
    g_painter->drawRect(QRectF(x, y, w, h));
}
void nlgl_line(double x1, double y1, double x2, double y2,
               double w, long long r, long long g, long long b,
               long long a) {
    if (!g_painter) return;
    g_painter->setPen(QPen(QColor((int)r, (int)g, (int)b, (int)a), w));
    g_painter->setBrush(Qt::NoBrush);
    g_painter->drawLine(QPointF(x1, y1), QPointF(x2, y2));
}
void nlgl_disc(double x, double y, double rx, double ry,
               long long r, long long g, long long b, long long a) {
    if (!g_painter) return;
    g_painter->setPen(Qt::NoPen);
    g_painter->setBrush(QColor((int)r, (int)g, (int)b, (int)a));
    g_painter->drawEllipse(QPointF(x, y), rx, ry);
}
void nlgl_ring(double x, double y, double rx, double ry,
               double w, long long r, long long g, long long b,
               long long a) {
    if (!g_painter) return;
    g_painter->setPen(QPen(QColor((int)r, (int)g, (int)b, (int)a), w));
    g_painter->setBrush(Qt::NoBrush);
    g_painter->drawEllipse(QPointF(x, y), rx, ry);
}

// ---------- пост ----------
void nlgl_post(double exposure, double bloom, double grain) {
    g_post_exposure = (float)exposure;
    g_post_bloom = (float)bloom;
    g_post_grain = (float)grain;
}
void nlgl_postaberr(double px) { g_post_aberr = (float)px; }

// ---------- состояние ----------
double nlgl_var(long long i) {
    if (i < 0 || i >= 2048) return 0;
    return g_vars[i];
}
void nlgl_varset(long long i, double v) {
    if (i >= 0 && i < 2048) g_vars[i] = v;
}
long long nlgl_frametick() { return g_frame_no++; }
void nlgl_shot(const std::string& path) { g_shot_path = path; }

#endif // NLANG_HAS_QTGL
