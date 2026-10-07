#ifndef DB_MANAGER_H
#define DB_MANAGER_H

/*
 * @brief Связь «таблица - файл БД» и подготовка файлов баз при старте.
 *
 * @details
 * Единственный заголовок модуля для внешнего использования, как orm/orm.h
 * у ORM. Внутренности лежат в lib/ и подключаются сами:
 *
 *   lib/manager.h    class DBManager   — файлы одной базы
 *   lib/registry.h   class DBRegistry  — все базы, dbFor<Таблица>()
 *   lib/db_macro.h   ORM_DECLARE_DATABASE и помощники
 *
 * ORM описывает только структуру таблиц (orm/orm.h) и не знает, в каком файле
 * эта таблица лежит. Здесь описывается вторая половина: какие есть базы и
 * какие таблицы в каждой.
 *
 * Как пользоваться:
 *   db::DBRegistry::instance().prepareAll(sourceDir, dataDir)
 *        — подготовить ВСЕ базы при старте приложения. Обновления здесь НЕ
 *          происходит: если файла нет или он непригоден, он берётся из снимка,
 *          а если снимка нет - из поставки; пользовательская БД при отсутствии
 *          создаётся пустой по структуре основной;
 *   db::dbFor<Aerodrom>().open()
 *        — готовое соединение (connect + attach пользовательской);
 *   db::dbFor<Aerodrom>().getTargetMain() / .getTargetUser()
 *        — пути к рабочим файлам;
 *   db::dbFor<Aerodrom>().isUpdateAvailable()
 *        — в поставке лежит версия, отличная от установленной;
 *   db::dbFor<Aerodrom>().updateFromSource()
 *        — обновить справочник из поставки, перенеся правки пользователя
 *          в новую пользовательскую БД (отдельное действие, не при старте);
 *   db::DBRegistry::instance().updateAllFromSource()
 *        — то же для всех баз сразу;
 *   db::dbFor<Aerodrom>().makeBackup(dataDir)
 *        — снимок обеих БД руками;
 *   db::dbFor<Aerodrom>().swapWithBackup()
 *        — откат: снимок и рабочие файлы меняются местами (можно повторить);
 *   db::dbFor<Aerodrom>().restoreFromBackup()
 *        — вернуть обе БД из снимка копированием (снимок остаётся).
 *
 * Добавить таблицу в существующую базу:
 *   1. описать её в orm/orm.h (ORM_DECLARE_TABLE + ORM_PRIMARY_KEY);
 *   2. дописать строку X(Имя, DB) в список таблиц этой базы.
 *
 * Добавить новую базу:
 *   1. описать её таблицы в orm/orm.h;
 *   2. завести список ORM_TABLES_XXX(X, DB);
 *   3. написать ORM_DECLARE_DATABASE_NAMED(...). Больше нигде ничего
 *      дописывать не нужно: база сама попадёт в DBRegistry и в prepareAll().
 *
 * Параметр DB в списке таблиц — заготовка под имя базы, его подставляет сам
 * макрос; писать вместо него что-то другое не нужно.
 *
 * Если имена файлов не по шаблону <имя>.sqlite / <имя>_user.sqlite, есть
 * полная форма:
 *   ORM_DECLARE_DATABASE(Имя, "основной.sqlite", "пользовательский.sqlite", СПИСОК)
 * Пользовательский файл можно оставить пустым (""), тогда база работает
 * только на чтение, без наложения правок.
 */

#include "lib/manager.h"
#include "lib/registry.h"
#include "lib/db_macro.h"


/* aerodrom.sqlite */
#define ORM_TABLES_AERODROM(X, DB)                                             \
    X(Aerodrom, DB)                                                            \
    X(Ranway, DB)

/*
 * @brief Порождает связь между файлом бд (локальным/глобальным) и
 * таблицами из orm
*/
ORM_DECLARE_DATABASE_NAMED(AerodromDB, "aerodrom", ORM_TABLES_AERODROM)

/*
 * Пример второй базы
 *
 * #define ORM_TABLES_LATYPES(X, DB)                                           \
 *     X(Sabolet, DB)                                                          \
 *     X(Ser,     DB)
 *
 * ORM_DECLARE_DATABASE_NAMED(LatypesDB, "latypes", ORM_TABLES_LATYPES)
 */

#endif // DB_MANAGER_H
