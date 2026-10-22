#pragma once
// Модуль gl — окно OpenGL (Qt6) и 3D-пайплайн для nlang.
//
// Как устроено простым языком:
//  - одно окно = один QOpenGLWidget с таймером 16 мс; кадр
//    рисует nlang-функция, поданная через gl.onFrame;
//  - меши, окна и сцены адресуются целочисленными хендлами —
//    nlang-сторона держит их как обычные числа;
//  - «энергопланета» (pseudoworld): облик сферы задаётся
//    спектром из 8 полос — частота/амплитуда/фаза/скорость
//    каждой полосы и её цвет настраиваются отдельно;
//  - gl.transmit — фильтр «передачи через гиперпространство»:
//    сканлайны, статика, бегущая полоса помех, виньетка.
//
// Сигналы принимают std::function — eval-слой
// (includedlib/gl.h) заворачивает в них NObject-функцию.
#include <string>
#include <functional>

// ---------- окно ----------
long long nlgl_window(const std::string& title,
                      long long w, long long h);
void      nlgl_show(long long win);
void      nlgl_mouselock(long long win);
void      nlgl_onFrame(long long win, std::function<void()> fn);
long long nlgl_key(long long vk);
long long nlgl_width(long long win);
long long nlgl_height(long long win);

// ---------- камера ----------
void   nlgl_campos(long long win, double x, double y, double z);
void   nlgl_camrot(long long win, double yaw, double pitch);
void   nlgl_camroll(long long win, double deg);
void   nlgl_camspeed(long long win, double s);
void   nlgl_camfar(long long win, double d);
void   nlgl_camfly(long long win, long long fly);
double nlgl_camx(long long win);
double nlgl_camy(long long win);
double nlgl_camz(long long win);
void   nlgl_camupdate(long long win, double dt);

// ---------- свет / атмосфера ----------
void nlgl_sun(double x, double y, double z);
void nlgl_suncolor(double r, double g, double b);
void nlgl_ambient(double sr, double sg, double sb,
                  double gr, double gg, double gb);
void nlgl_fog(double r, double g, double b, double s, double e);
void nlgl_point(double x, double y, double z,
                double r, double g, double b);
void nlgl_pointoff();

// ---------- меши ----------
long long nlgl_sphere(double rad, long long segs,
                      double r, double g, double b);
long long nlgl_box(double w, double h, double d,
                   double r, double g, double b);
long long nlgl_cylinder(double rad, double h, long long segs,
                        double r, double g, double b);
long long nlgl_cone(double rad, double h, long long segs,
                    double r, double g, double b);
long long nlgl_obj(const std::string& path, double size,
                   double r, double g, double b);
// ручной меш: begin -> vertex3* -> tri* -> end
void      nlgl_beginmesh();
long long nlgl_vertex3(double x, double y, double z,
                       double r, double g, double b);
void      nlgl_tri(long long a, long long b, long long c);
long long nlgl_endmesh();
void      nlgl_meshtex(long long mesh, const std::string& path,
                       double scale);
void      nlgl_meshalpha(long long mesh, double a);
void      nlgl_meshfree(long long mesh);

// ---------- отрисовка ----------
void nlgl_clear(double r, double g, double b);
void nlgl_draw(long long mesh, double x, double y, double z,
               double rx, double ry, double rz,
               double sx, double sy, double sz);
void nlgl_sceneadd(long long mesh, double x, double y, double z,
                   double rx, double ry, double rz,
                   double sx, double sy, double sz);
void nlgl_sceneclear();
void nlgl_scenedraw();
void nlgl_scenesave(long long key);
void nlgl_sceneload(long long key);
void nlgl_sceneappend(long long key);

// ---------- небо: звёзды + чёрная дыра ----------
void nlgl_sky();
void nlgl_skyenable(long long on);
void nlgl_bhdir(double x, double y, double z);

// ---------- энергопланета (pseudoworld) ----------
// 8 полос: частота, амплитуда, фаза, скорость дрейфа
void   nlgl_eband(long long i, double f, double a,
                  double p, double s);
double nlgl_ebget(long long i, long long c);
void   nlgl_ebcol(long long i, double r, double g, double b);
double nlgl_ebcolget(long long i, long long c);
void   nlgl_ebase(double r, double g, double b);
void   nlgl_eatmo(double r, double g, double b);
void   nlgl_eatmok(double k);
void   nlgl_eplanet(double x, double y, double z, double rad);

// ---------- звезда ----------
void nlgl_starcol(double r, double g, double b);
void nlgl_star(double x, double y, double z, double rad);

// ---------- передача через гиперпространство ----------
void nlgl_transmit(double intensity);

// ---------- пост (HDR -> bloom -> ACES) ----------
void nlgl_post(double exposure, double bloom, double grain);
void nlgl_postaberr(double px);

// ---------- 2D HUD ----------
void nlgl_begin2d();
void nlgl_end2d();
void nlgl_text(double x, double y, const std::string& s,
               long long r, long long g, long long b,
               long long size);
void nlgl_rect(double x, double y, double w, double h,
               long long r, long long g, long long b);
void nlgl_recta(double x, double y, double w, double h,
                long long r, long long g, long long b,
                long long a);
void nlgl_line(double x1, double y1, double x2, double y2,
               double w, long long r, long long g, long long b,
               long long a);
void nlgl_disc(double x, double y, double rx, double ry,
               long long r, long long g, long long b,
               long long a);
void nlgl_ring(double x, double y, double rx, double ry,
               double w, long long r, long long g, long long b,
               long long a);

// ---------- состояние / отладка ----------
double    nlgl_var(long long i);
void      nlgl_varset(long long i, double v);
long long nlgl_frametick();
void      nlgl_shot(const std::string& path);
