cmake_minimum_required(VERSION 3.14)

project(test_orm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(ORM_WITH_CONCURRENT "Поддержка асинхронных запросов (Qt5::Concurrent)" OFF)

find_package(Qt5 REQUIRED COMPONENTS Core Sql)
if(ORM_WITH_CONCURRENT)
    find_package(Qt5 REQUIRED COMPONENTS Concurrent)
endif()

# ---------------------------------------------------------------------------
# ORM — header-only библиотека
# ---------------------------------------------------------------------------
add_library(orm INTERFACE)
add_library(orm::orm ALIAS orm)

# корень — для #include "orm/common.h", orm/lib — для #include "database.h" в common.h
target_include_directories(orm INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/orm/lib)
target_compile_features(orm INTERFACE cxx_std_17)
target_link_libraries(orm INTERFACE Qt5::Core Qt5::Sql)
if(ORM_WITH_CONCURRENT)
    target_link_libraries(orm INTERFACE Qt5::Concurrent)
endif()

# Заголовки видны в IDE (Qt Creator / CLion)
file(GLOB ORM_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/orm/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/orm/lib/*.h)
target_sources(orm INTERFACE ${ORM_HEADERS})

# ---------------------------------------------------------------------------
# Тестовое приложение
# ---------------------------------------------------------------------------
add_executable(test_orm main.cpp)
target_link_libraries(test_orm PRIVATE orm::orm)
target_compile_definitions(test_orm PRIVATE QT_DEPRECATED_WARNINGS)

if(MSVC)
    target_compile_options(test_orm PRIVATE /W4 /utf-8)
else()
    target_compile_options(test_orm PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Базы данных рядом с бинарником (main.cpp открывает их по "./...")
# ---------------------------------------------------------------------------
# Основная БД только читается — копируем заново при каждом её изменении.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/aerodrom.sqlite
               ${CMAKE_CURRENT_BINARY_DIR}/aerodrom.sqlite COPYONLY)

# Пользовательская БД: копия обновляется, когда меняется исходный файл
# в корне проекта (например, добавили таблицу). Правки, сделанные программой
# в build/, при этом перезаписываются.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/aerodrom_user.sqlite
               ${CMAKE_CURRENT_BINARY_DIR}/aerodrom_user.sqlite COPYONLY)
