# db_manager

Привязывает таблицы ORM к файлам баз данных и готовит эти файлы при старте: копирует справочник из поставки, создаёт пользовательскую БД, делает снимки, обновляет справочник и откатывает обновление.

ORM знает только структуру таблиц. Этот модуль — вторая половина: какие есть базы, какие таблицы в каждой и что с файлами делать.

```
db_manager/
├── db_manager.h       единственный внешний заголовок + описание баз приложения
└── lib/
    ├── manager.h      DBManager  — файлы одной базы
    ├── registry.h     DBRegistry — все базы, dbFor<Таблица>()
    ├── db_macro.h     ORM_DECLARE_DATABASE и помощники
    └── log.h          DB_ERROR_LOG / DB_DEBUG_LOG
```

```cpp
#include "db_manager/db_manager.h"   // больше ничего подключать не нужно
```

## Что нужно описывать руками

**`orm/orm.h`** — поля таблицы:

```cpp
#define ORM_FIELDS_RANWAY(X)         \
    X(QString, kod_icao, "KOD_ICAO") \
    X(QString, num_vpp,  "NUM_VPP" ) \
    X(int,     tora,     "TORA"    )

ORM_DECLARE_TABLE(Ranway, "RANWAY", ORM_FIELDS_RANWAY)
ORM_PRIMARY_KEY(Ranway, "KOD_ICAO", "NUM_VPP")
```

**`db_manager/db_manager.h`** — в каком файле таблица лежит:

```cpp
#define ORM_TABLES_AERODROM(X, DB) \
    X(Aerodrom, DB)                \
    X(Ranway,   DB)

ORM_DECLARE_DATABASE_NAMED(AerodromDB, "aerodrom", ORM_TABLES_AERODROM)
```

Получаются файлы `aerodrom.sqlite` (основной) и `aerodrom_user.sqlite` (пользовательский). Полная форма, если имена другие: `ORM_DECLARE_DATABASE(Имя, "main.sqlite", "user.sqlite", СПИСОК)`; пустое имя пользовательского файла (`""`) — база только на чтение.

| Задача | Действие |
|---|---|
| новое поле | строка `X(...)` в `ORM_FIELDS_XXX` |
| новая таблица в базе | описать в `orm/orm.h` + строка `X(Имя, DB)` в списке базы |
| новая база | описать таблицы + одна `ORM_DECLARE_DATABASE_NAMED(...)` |

Список баз нигде не дублируется: каждая регистрируется сама и попадает в `prepareAll()`.

## Инициализация — один раз при старте
# В качестве примера возьмем под каталогом поставки - /data в локальных файлах приложения, а под рабочим каталогом - системную директорию в локальных файлах

```cpp
const QString sourceDir = QFileInfo(                       // каталог поставки
        Aurora::Application::pathTo(QStringLiteral("data/")).toLocalFile()).absoluteFilePath();
const QString dataDir = QStandardPaths::writableLocation(  // каталог приложения
        QStandardPaths::AppLocalDataLocation);

if (!db::DBRegistry::instance().prepareAll(sourceDir, dataDir)) {
    // какая-то из баз не готова
}
```

`prepare()` - первичная иниализцаия вызывающаяся для каждоый БД в prepareAll:

1. **основная БД** — файла нет или он непригоден (нет объявленной таблицы/колонки, не открывается): берём из снимка `backup/<имя базы>/`, а если снимка нет — из поставки;
2. **пользовательская БД** — файла нет: берём из снимка, а если снимка нет — создаём пустую по структуре основной (`DataBase::cloneDataBase`). Существующую дополняем недостающими таблицами и колонками.

## Запросы

```cpp
db::DBManager& aerodromDb = db::dbFor<Ranway>();   // менеджер базы, где лежит RANWAY

// db::DBManager::open() возвращает std::unique_ptr
auto conn = aerodromDb.open();                     // connect + attach пользовательской
if (!conn) { return; }                             // база не готова

QList<Ranway> rows = conn->from<Ranway>().select()
        .where(RanwayFields::kod_icao, Comparison::Equal, "USPP").exec();

conn->update<Ranway>().set(RanwayFields::tora, 3400)   // правка уйдёт в пользовательскую БД
        .where(RanwayFields::kod_icao, Comparison::Equal, "USPP")
        .where(RanwayFields::num_vpp,  Comparison::Equal, "03").exec();
```

# Соединение закрывается, когда `conn` выходит из области видимости. В рабочем потоке создавать его нужно **внутри** задачи — `QSqlDatabase` нельзя передавать между потоками. Пример:

```cpp
QtConcurrent::run([field, value]() {
    auto conn = db::dbFor<Ranway>().open();
    if (!conn) { return QList<Ranway>(); }
    return conn->from<Ranway>().select().where(field, Comparison::Equal, value).exec();
});
```

## Обновление справочника и откат

```cpp
db::DBManager& db_ = db::dbFor<Aerodrom>();

if (db_.isUpdateAvailable()) {        // в поставке другая версия
    db_.updateFromSource();           // или updateFromSource(orm::MergePolicy::UserFirst)
}

db_.swapWithBackup();                 // откат; повторный вызов вернёт обновление обратно
db_.makeBackup(dataDir);              // снимок руками
db_.restoreFromBackup();              // вернуть обе БД из снимка копированием
```

Вызывать, когда ни одного `conn` не живёт: файлы подменяются.

`updateFromSource()`: снимок обеих БД → новая основная из поставки на место (не подошла программе — вернули прежние файлы) → новая пустая пользовательская по структуре новой основной → перенос правок → подмена пользовательской.

**Перенос правок** — один запрос на таблицу, от старой пользовательской БД:

```sql
INSERT INTO main."RANWAY" ("KOD_ICAO", "NUM_VPP", "TORA", ...)
SELECT COALESCE(b."KOD_ICAO", o."KOD_ICAO"), COALESCE(b."TORA", o."TORA"), ...
  FROM user_old."RANWAY" AS o
  LEFT JOIN base."RANWAY" AS b
    ON TRIM(b."KOD_ICAO") = TRIM(o."KOD_ICAO") AND TRIM(b."NUM_VPP") = TRIM(o."NUM_VPP")
```

| Запись | Результат |
|---|---|
| есть в новой поставке | значения поставки, из правки добираются только `NULL`-колонки |
| в поставке её нет | строка целиком из правки |

`orm::MergePolicy`: `SourceFirst` (по умолчанию) — `COALESCE(поставка, правка)`; `UserFirst` — наоборот, правка главнее. У таблицы без `ORM_PRIMARY_KEY` строки переносятся без сверки с поставкой.

## Файлы в каталоге приложения

```
aerodrom.sqlite            рабочая копия справочника
aerodrom.sqlite.version    версия поставки, из которой она взята
aerodrom_user.sqlite       правки пользователя
backup/AerodromDB/         один снимок: обе БД
```

## Правила

- `prepareAll()` — один раз в начале `main()`, из главного потока.
- `open()` — на каждую задачу; результат не хранить в поле и не передавать в другой поток.
- Проверять `open()` на `nullptr`, причина — в `lastError()`.
- Пути не собирать руками: `getTargetMain()` / `getTargetUser()`.
- Обновление и откат — только когда соединений нет.
- Снимок хранится один: новый затирает прежний (но не раньше, чем будет готов).
- Испорченная основная БД в снимок не попадает, иначе затёрла бы рабочий снимок.
- Не удалось сделать снимок — обновление отменяется, рабочие файлы не меняются.
