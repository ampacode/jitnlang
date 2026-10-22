cmake_minimum_required(VERSION 3.16)
project(nlang-cpp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

file(GLOB NLANG_SOURCES
    src/*.cpp
    src/parser/*.cpp
    src/expressions/*.cpp
    src/statements/*.cpp
    src/includedlib/*.cpp
)
# Точка входа компилятора — отдельный таргет
list(REMOVE_ITEM NLANG_SOURCES
    ${CMAKE_SOURCE_DIR}/src/main.cpp)

# libnlmodules.a — прекомпилированные модули стандартной библиотеки.
# Сгенерированные программы линкуются с ней: сборка один раз здесь.
file(GLOB NLM_SOURCES modules/*.cpp)

# qt-модуль — только если Qt6 Widgets есть в тулчейне
find_package(Qt6 QUIET COMPONENTS Widgets)
# gl-модуль — OpenGL окно: Qt6::OpenGLWidgets (QOpenGLWidget)
find_package(Qt6 QUIET COMPONENTS OpenGLWidgets)

if(NOT Qt6_FOUND)
    list(REMOVE_ITEM NLM_SOURCES
         ${CMAKE_SOURCE_DIR}/modules/nlm_qt.cpp)
endif()
if(NOT Qt6OpenGLWidgets_FOUND)
    list(REMOVE_ITEM NLM_SOURCES
         ${CMAKE_SOURCE_DIR}/modules/nlm_gl.cpp)
endif()

add_library(nlmodules STATIC ${NLM_SOURCES})
target_include_directories(nlmodules PUBLIC modules runtime)
if(WIN32)
    # internet-модуль: winsock + GetExtendedTcpTable
    # crypto-модуль: BCryptGenRandom
    target_link_libraries(nlmodules PUBLIC ws2_32 iphlpapi bcrypt)
endif()
if(Qt6_FOUND)
    target_compile_definitions(nlmodules PUBLIC NLANG_HAS_QT)
    target_link_libraries(nlmodules PUBLIC Qt6::Widgets)
endif()
if(Qt6OpenGLWidgets_FOUND)
    target_compile_definitions(nlmodules PUBLIC NLANG_HAS_QTGL)
    target_link_libraries(nlmodules PUBLIC Qt6::OpenGLWidgets)
endif()

# Ядро компилятора/интерпретатора
add_library(nlangcore STATIC ${NLANG_SOURCES})
target_include_directories(nlangcore PUBLIC src)
target_link_libraries(nlangcore PUBLIC nlmodules)

add_executable(nlang src/main.cpp)
target_link_libraries(nlang PRIVATE nlangcore)

if(WIN32)
    # ucrt64: libpq/sqlite3/curl — для eval-мостов модулей pg/sqlite/http,
    # линкуем только если они есть в окружении тулчейна
    find_library(LIBPQ pq)
    if(LIBPQ)
        target_link_libraries(nlang PRIVATE ${LIBPQ})
    endif()
    find_library(LIBSQLITE3 sqlite3)
    if(LIBSQLITE3)
        target_link_libraries(nlang PRIVATE ${LIBSQLITE3})
    endif()
    find_library(LIBCURL curl)
    if(LIBCURL)
        target_link_libraries(nlang PRIVATE ${LIBCURL})
    endif()
    # Статический рантайм (без полного -static — он ломает -lpq)
    target_link_options(nlang PRIVATE
        -static-libgcc -static-libstdc++
        -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
endif()

# runtime/ и modules/ + libnlmodules.a — рядом с бинарём:
# runtime — хедеры для сгенерированного кода,
# modules — заголовки модулей и статическая библиотека для линковки.
add_custom_command(TARGET nlang POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/runtime $<TARGET_FILE_DIR:nlang>/runtime
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/modules $<TARGET_FILE_DIR:nlang>/modules
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:nlmodules> $<TARGET_FILE_DIR:nlang>/modules/libnlmodules.a)

# nlang.exe -> <repo>/path/nlang.exe: команда `nlang` в PATH
if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/../path")
    add_custom_command(TARGET nlang POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:nlang>
            ${CMAKE_SOURCE_DIR}/../path/nlang.exe
            || ${CMAKE_COMMAND} -E echo "path/ locked - copy skipped")
endif()
