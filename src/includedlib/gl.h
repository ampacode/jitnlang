#pragma once
// Дескриптор модуля gl + eval-регистратор.
//
// eval-путь only: сгенерированный C++-код OpenGL/Qt не линкует
// (gl — интерфейс интерпретатора, как и модуль qt).
#include "../module.h"
#include "../nvm.h"
#include "../../modules/nlm_gl.h"
#include "str.h" // sarg

namespace includedlib {

// Вызов nlang-функции кадра из Qt-таймера: исключения не должны
// улетать внутрь Qt — ловим и показываем, иначе весь процесс
// «просто виснет» и не видно почему.
inline void gl_call(Nvm& vm, const NObject& fn) {
    try {
        NArgs args;
        fn.call(vm, args);
    } catch (const NLangError& e) {
        fprintf(stderr, "[gl] %s (line %d)\n", e.msg.c_str(), e.line);
    } catch (const std::exception& e) {
        fprintf(stderr, "[gl] %s\n", e.what());
    }
}

inline void register_gl(Nvm& vm, const std::string& alias) {
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
    auto flt = [](const NArgs& a, size_t i, double d = 0.0) {
        return a.pos.size() > i ? a.pos[i].asDouble() : d;
    };

    // --- окно ---
    add("window", [](Nvm&, const NArgs& a) {
        return NObject(nlgl_window(sarg(a, 0),
                                   a.pos.size() > 1 ? a.pos[1].asInt() : 640,
                                   a.pos.size() > 2 ? a.pos[2].asInt() : 480)); });
    add("show", [num](Nvm&, const NArgs& a) {
        nlgl_show(num(a, 0)); return NObject(); });
    add("mouselock", [num](Nvm&, const NArgs& a) {
        nlgl_mouselock(num(a, 0)); return NObject(); });
    add("onFrame", [num](Nvm& vm, const NArgs& a) {
        NObject fn = a.pos[1];
        nlgl_onFrame(num(a, 0), [&vm, fn]() mutable {
            gl_call(vm, fn); });
        return NObject(); });
    add("key", [num](Nvm&, const NArgs& a) {
        return NObject(nlgl_key(num(a, 0))); });
    add("width", [num](Nvm&, const NArgs& a) {
        return NObject(nlgl_width(num(a, 0))); });
    add("height", [num](Nvm&, const NArgs& a) {
        return NObject(nlgl_height(num(a, 0))); });

    // --- камера ---
    add("camPos", [num, flt](Nvm&, const NArgs& a) {
        nlgl_campos(num(a, 0), flt(a, 1), flt(a, 2), flt(a, 3));
        return NObject(); });
    add("camRot", [num, flt](Nvm&, const NArgs& a) {
        nlgl_camrot(num(a, 0), flt(a, 1), flt(a, 2));
        return NObject(); });
    add("camRoll", [num, flt](Nvm&, const NArgs& a) {
        nlgl_camroll(num(a, 0), flt(a, 1)); return NObject(); });
    add("camSpeed", [num, flt](Nvm&, const NArgs& a) {
        nlgl_camspeed(num(a, 0), flt(a, 1)); return NObject(); });
    add("camFar", [num, flt](Nvm&, const NArgs& a) {
        nlgl_camfar(num(a, 0), flt(a, 1)); return NObject(); });
    add("camFly", [num](Nvm&, const NArgs& a) {
        nlgl_camfly(num(a, 0), num(a, 1, 1)); return NObject(); });
    add("camX", [num](Nvm&, const NArgs& a) {
        return NObject(nlgl_camx(num(a, 0))); });
    add("camY", [num](Nvm&, const NArgs& a) {
        return NObject(nlgl_camy(num(a, 0))); });
    add("camZ", [num](Nvm&, const NArgs& a) {
        return NObject(nlgl_camz(num(a, 0))); });
    add("camUpdate", [num, flt](Nvm&, const NArgs& a) {
        nlgl_camupdate(num(a, 0), flt(a, 1, 0.016));
        return NObject(); });

    // --- свет ---
    add("sun", [flt](Nvm&, const NArgs& a) {
        nlgl_sun(flt(a, 0), flt(a, 1), flt(a, 2)); return NObject(); });
    add("sunColor", [flt](Nvm&, const NArgs& a) {
        nlgl_suncolor(flt(a, 0), flt(a, 1), flt(a, 2));
        return NObject(); });
    add("ambient", [flt](Nvm&, const NArgs& a) {
        nlgl_ambient(flt(a, 0), flt(a, 1), flt(a, 2),
                     flt(a, 3), flt(a, 4), flt(a, 5));
        return NObject(); });
    add("fog", [flt](Nvm&, const NArgs& a) {
        nlgl_fog(flt(a, 0), flt(a, 1), flt(a, 2),
                 flt(a, 3), flt(a, 4));
        return NObject(); });
    add("point", [flt](Nvm&, const NArgs& a) {
        nlgl_point(flt(a, 0), flt(a, 1), flt(a, 2),
                   flt(a, 3), flt(a, 4), flt(a, 5));
        return NObject(); });
    add("pointOff", [](Nvm&, const NArgs&) {
        nlgl_pointoff(); return NObject(); });

    // --- меши ---
    add("sphere", [num, flt](Nvm&, const NArgs& a) {
        return NObject(nlgl_sphere(flt(a, 0, 1.0), num(a, 1, 24),
                                   flt(a, 2, 1.0), flt(a, 3, 1.0),
                                   flt(a, 4, 1.0))); });
    add("box", [flt](Nvm&, const NArgs& a) {
        return NObject(nlgl_box(flt(a, 0), flt(a, 1), flt(a, 2),
                                flt(a, 3), flt(a, 4), flt(a, 5))); });
    add("cylinder", [num, flt](Nvm&, const NArgs& a) {
        return NObject(nlgl_cylinder(flt(a, 0), flt(a, 1),
                                     num(a, 2, 16),
                                     flt(a, 3), flt(a, 4), flt(a, 5))); });
    add("cone", [num, flt](Nvm&, const NArgs& a) {
        return NObject(nlgl_cone(flt(a, 0), flt(a, 1),
                                 num(a, 2, 16),
                                 flt(a, 3), flt(a, 4), flt(a, 5))); });
    add("obj", [flt](Nvm&, const NArgs& a) {
        return NObject(nlgl_obj(sarg(a, 0), flt(a, 1, 1.0),
                                flt(a, 2, 1.0), flt(a, 3, 1.0),
                                flt(a, 4, 1.0))); });
    add("beginMesh", [](Nvm&, const NArgs&) {
        nlgl_beginmesh(); return NObject(); });
    add("vertex3", [flt](Nvm&, const NArgs& a) {
        return NObject(nlgl_vertex3(flt(a, 0), flt(a, 1), flt(a, 2),
                                    flt(a, 3), flt(a, 4), flt(a, 5))); });
    add("tri", [num](Nvm&, const NArgs& a) {
        nlgl_tri(num(a, 0), num(a, 1), num(a, 2)); return NObject(); });
    add("endMesh", [](Nvm&, const NArgs&) {
        return NObject(nlgl_endmesh()); });
    add("meshTex", [num, flt](Nvm&, const NArgs& a) {
        nlgl_meshtex(num(a, 0), sarg(a, 1), flt(a, 2, 0.2));
        return NObject(); });
    add("meshAlpha", [num, flt](Nvm&, const NArgs& a) {
        nlgl_meshalpha(num(a, 0), flt(a, 1)); return NObject(); });
    add("meshFree", [num](Nvm&, const NArgs& a) {
        nlgl_meshfree(num(a, 0)); return NObject(); });

    // --- отрисовка ---
    add("clear", [flt](Nvm&, const NArgs& a) {
        nlgl_clear(flt(a, 0), flt(a, 1), flt(a, 2));
        return NObject(); });
    add("draw", [num, flt](Nvm&, const NArgs& a) {
        nlgl_draw(num(a, 0), flt(a, 1), flt(a, 2), flt(a, 3),
                  flt(a, 4), flt(a, 5), flt(a, 6),
                  flt(a, 7, 1.0), flt(a, 8, 1.0), flt(a, 9, 1.0));
        return NObject(); });
    add("sceneAdd", [num, flt](Nvm&, const NArgs& a) {
        nlgl_sceneadd(num(a, 0), flt(a, 1), flt(a, 2), flt(a, 3),
                      flt(a, 4), flt(a, 5), flt(a, 6),
                      flt(a, 7, 1.0), flt(a, 8, 1.0), flt(a, 9, 1.0));
        return NObject(); });
    add("sceneClear", [](Nvm&, const NArgs&) {
        nlgl_sceneclear(); return NObject(); });
    add("sceneDraw", [](Nvm&, const NArgs&) {
        nlgl_scenedraw(); return NObject(); });
    add("sceneSave", [num](Nvm&, const NArgs& a) {
        nlgl_scenesave(num(a, 0)); return NObject(); });
    add("sceneLoad", [num](Nvm&, const NArgs& a) {
        nlgl_sceneload(num(a, 0)); return NObject(); });
    add("sceneAppend", [num](Nvm&, const NArgs& a) {
        nlgl_sceneappend(num(a, 0)); return NObject(); });

    // --- небо ---
    add("sky", [](Nvm&, const NArgs&) {
        nlgl_sky(); return NObject(); });
    add("skyEnable", [num](Nvm&, const NArgs& a) {
        nlgl_skyenable(num(a, 0)); return NObject(); });
    add("bhDir", [flt](Nvm&, const NArgs& a) {
        nlgl_bhdir(flt(a, 0), flt(a, 1), flt(a, 2));
        return NObject(); });

    // --- энергопланета ---
    add("eband", [num, flt](Nvm&, const NArgs& a) {
        nlgl_eband(num(a, 0), flt(a, 1), flt(a, 2),
                   flt(a, 3), flt(a, 4));
        return NObject(); });
    add("ebget", [num](Nvm&, const NArgs& a) {
        return NObject(nlgl_ebget(num(a, 0), num(a, 1))); });
    add("ebcol", [num, flt](Nvm&, const NArgs& a) {
        nlgl_ebcol(num(a, 0), flt(a, 1), flt(a, 2), flt(a, 3));
        return NObject(); });
    add("ebcolget", [num](Nvm&, const NArgs& a) {
        return NObject(nlgl_ebcolget(num(a, 0), num(a, 1))); });
    add("ebase", [flt](Nvm&, const NArgs& a) {
        nlgl_ebase(flt(a, 0), flt(a, 1), flt(a, 2));
        return NObject(); });
    add("eatmo", [flt](Nvm&, const NArgs& a) {
        nlgl_eatmo(flt(a, 0), flt(a, 1), flt(a, 2));
        return NObject(); });
    add("eatmoK", [flt](Nvm&, const NArgs& a) {
        nlgl_eatmok(flt(a, 0)); return NObject(); });
    add("eplanet", [flt](Nvm&, const NArgs& a) {
        nlgl_eplanet(flt(a, 0), flt(a, 1), flt(a, 2), flt(a, 3, 8.0));
        return NObject(); });

    // --- звезда ---
    add("starCol", [flt](Nvm&, const NArgs& a) {
        nlgl_starcol(flt(a, 0), flt(a, 1), flt(a, 2));
        return NObject(); });
    add("star", [flt](Nvm&, const NArgs& a) {
        nlgl_star(flt(a, 0), flt(a, 1), flt(a, 2), flt(a, 3, 10.0));
        return NObject(); });

    // --- передача ---
    add("transmit", [flt](Nvm&, const NArgs& a) {
        nlgl_transmit(flt(a, 0, 1.0)); return NObject(); });

    // --- пост ---
    add("post", [flt](Nvm&, const NArgs& a) {
        nlgl_post(flt(a, 0, 1.15), flt(a, 1, 0.85), flt(a, 2, 0.045));
        return NObject(); });
    add("postAberr", [flt](Nvm&, const NArgs& a) {
        nlgl_postaberr(flt(a, 0, 0.6)); return NObject(); });

    // --- 2D ---
    add("begin2d", [](Nvm&, const NArgs&) {
        nlgl_begin2d(); return NObject(); });
    add("end2d", [](Nvm&, const NArgs&) {
        nlgl_end2d(); return NObject(); });
    add("text", [flt, num](Nvm&, const NArgs& a) {
        nlgl_text(flt(a, 0), flt(a, 1), sarg(a, 2),
                  num(a, 3, 255), num(a, 4, 255), num(a, 5, 255),
                  num(a, 6, 10));
        return NObject(); });
    add("rect", [flt, num](Nvm&, const NArgs& a) {
        nlgl_rect(flt(a, 0), flt(a, 1), flt(a, 2), flt(a, 3),
                  num(a, 4), num(a, 5), num(a, 6));
        return NObject(); });
    add("rectA", [flt, num](Nvm&, const NArgs& a) {
        nlgl_recta(flt(a, 0), flt(a, 1), flt(a, 2), flt(a, 3),
                   num(a, 4), num(a, 5), num(a, 6), num(a, 7));
        return NObject(); });
    add("line", [flt, num](Nvm&, const NArgs& a) {
        nlgl_line(flt(a, 0), flt(a, 1), flt(a, 2), flt(a, 3),
                  flt(a, 4, 1.0), num(a, 5), num(a, 6), num(a, 7),
                  num(a, 8, 255));
        return NObject(); });
    add("disc", [flt, num](Nvm&, const NArgs& a) {
        nlgl_disc(flt(a, 0), flt(a, 1), flt(a, 2), flt(a, 3),
                  num(a, 4), num(a, 5), num(a, 6), num(a, 7, 255));
        return NObject(); });
    add("ring", [flt, num](Nvm&, const NArgs& a) {
        nlgl_ring(flt(a, 0), flt(a, 1), flt(a, 2), flt(a, 3),
                  flt(a, 4, 1.0), num(a, 5), num(a, 6), num(a, 7),
                  num(a, 8, 255));
        return NObject(); });

    // --- состояние ---
    add("var", [num](Nvm&, const NArgs& a) {
        return NObject(nlgl_var(num(a, 0))); });
    add("varSet", [num, flt](Nvm&, const NArgs& a) {
        nlgl_varset(num(a, 0), flt(a, 1)); return NObject(); });
    add("frameTick", [](Nvm&, const NArgs&) {
        return NObject(nlgl_frametick()); });
    add("shot", [](Nvm&, const NArgs& a) {
        nlgl_shot(sarg(a, 0)); return NObject(); });

    vm.set(alias, NObject(d));
}

inline const ModuleDescriptor gl_desc = {
    "gl", "", {"nlmodules"}, &register_gl, "gl", nullptr
};

} // namespace includedlib
