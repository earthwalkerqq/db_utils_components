#ifndef DB_MANAGER_LIB_DB_MACRO_H
#define DB_MANAGER_LIB_DB_MACRO_H

#include <QList>
#include <QString>

#include "registry.h"


/*
 * @details
 * Структура файлов БД одна и та же, у каждой базы есть 
 * 1. основной файл (readonly) - <имя>.sqlite
 * 2. файл с правками пользователя - <имя>_user.sqlite.
 */
#define EXP ".sqlite"
#define MAIN_NAME(name) name EXP
#define USER_NAME(name) name "_user" EXP
#define FILL_NAMES(name) MAIN_NAME(name), USER_NAME(name)

/*
 * @brief Создает структуру информации о таблице (имя/колонки/ключи).
 * Используется для создания списка таблиц, входящих в бд
*/
#define ORM_DETAIL_DB_TABLE(Struct, DB)                                        \
    db::DBManager::TableInfo{ QString::fromLatin1(::Struct::table_name),       \
                              ::Struct::fields_type::columns(),                \
                              orm::PrimaryKey<::Struct>::columns() },

/* 
 * @brief Связь таблица - бд.
 * Делает возможным обращаться к бд через таблицу.
 * orm::DatabaseOf<объект_таблицы>::type
*/
#define ORM_DETAIL_DB_BIND(Struct, DB)                                         \
    namespace orm {                                                            \
    template<> struct DatabaseOf<::Struct> { using type = ::DB; };             \
    }

#define ORM_DECLARE_DATABASE_NAMED(Name, base, TABLE_LIST)                     \
    ORM_DETAIL_EXPAND(Name, FILL_NAMES(base), TABLE_LIST)

#define ORM_DETAIL_EXPAND(...) ORM_DECLARE_DATABASE(__VA_ARGS__)

/*
 * ORM_DECLARE_DATABASE(<Имя>, "<основной_файл>", "<пользовательский_файл>", <СПИСОК_ТАБЛИЦ>)
 *
 * Порождает структуру содержающую по имени объекта бд следующее:
 *  1. db_name - имя объекта бд
 *  2. main_file - название файла с основной бд (readonly)
 *  3. user_file - название файла с пользовательской бд
 *  4. tables(void) - метод геттер для получения всех таблиц бд
 *  5. manager(void) - метод геттер для получения объекта менеджера управления бд
 *   
 * Пользовательский файл можно оставить пустым (""), тогда база работает
 * только на чтение, без наложения правок.
 *
 * Писать на уровне файла (не внутри функции или класса): макрос объявляет
 * структуру, переменную и специализацию шаблона в namespace orm.
 *
 * Также менеджер управления бд заносится в общий реестр db::DBRegistry
 */
#define ORM_DECLARE_DATABASE(Name, MainFile, UserFile, TABLE_LIST)             \
    struct Name {                                                              \
        static constexpr const char* db_name   = #Name;                        \
        static constexpr const char* main_file = MainFile;                     \
        static constexpr const char* user_file = UserFile;                     \
                                                                               \
        static QList<db::DBManager::TableInfo> tables(void) {                  \
            return QList<db::DBManager::TableInfo>{                            \
                TABLE_LIST(ORM_DETAIL_DB_TABLE, Name) };                       \
        }                                                                      \
                                                                               \
        static db::DBManager& manager(void) {                                  \
            static db::DBManager manager(QString::fromLatin1(db_name),         \
                                         QString::fromLatin1(main_file),       \
                                         QString::fromLatin1(user_file),       \
                                         tables());                            \
            return manager;                                                    \
        }                                                                      \
    };                                                                         \
    inline const bool Name##_registered_in_DBRegistry =                        \
        db::DBRegistry::instance().add(&Name::manager());                      \
    TABLE_LIST(ORM_DETAIL_DB_BIND, Name)


#endif // DB_MANAGER_LIB_DB_MACRO_H
