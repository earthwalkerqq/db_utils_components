# ORM для SQLite

Header-only ORM. Эта библиотека не проверяет схему: из БД читаются только колонки, описанные в структуре. Если в базе есть лишние колонки, это не мешает.

```
orm/
├── orm.h    ← описание таблиц (ЕДИНСТВЕННЫЙ файл, который правится)
├── README.md
└── lib/        ← реализация
```

Подключение одно, и в нём есть всё: структуры таблиц, `DataBase`, запросы и `orm::toVariantList`.

```cpp
#include "orm/orm.h"
```

---

## 1. Описание таблиц (`common.h`)

```cpp
#define ORM_FIELDS_AERODROM(X)                 \
    X(QString, kod_icao,  "KOD_ICAO" )         \
    X(double,  lat_rad,   "LAT_RAD"  )         \
    X(QString, name_aero, "NAME_AERO")

ORM_DECLARE_TABLE(Aerodrom, "AERODROM", ORM_FIELDS_AERODROM)
ORM_PRIMARY_KEY(Aerodrom, "KOD_ICAO")
```

Строка `X(тип, имя_в_C++, "КОЛОНКА_В_БД")` описывает одно поле.

| Задача                   | Что сделать                                                               |
|--------------------------|---------------------------------------------------------------------------|
| Добавить поле            | добавить строку `X(...)` в нужный `ORM_FIELDS_...`                        |
| Переименовать поле в C++ | поменять второй аргумент (колонка в БД не меняется)                       |
| Новая таблица            | новый `#define ORM_FIELDS_XXX(X)`, `ORM_DECLARE_TABLE`, `ORM_PRIMARY_KEY` |

Правила:

- Типы полей: `QString`, `int`, `double`, `bool`. Подходит любой тип, который `QVariant` умеет приводить. NULL из БД превращается в значение по умолчанию (`""`, `0`).
- Если в БД хранится дробное число, а в структуре указан `int`, дробная часть отбрасывается.
- `ORM_PRIMARY_KEY` обязателен для `update()` и для работы с пользовательской БД.

Из одного описания макрос генерирует:

```cpp
Aerodrom a;                       // структура: a.kod_icao, a.lat_rad, ...
Aerodrom::table_name              // "AERODROM"
AerodromFields::kod_icao          // константа поля для запросов
AerodromFields::columns()         // {"KOD_ICAO", "LAT_RAD", "NAME_AERO"}
```

---

## 2. Соединение

```cpp
DataBase db(pathToMainDb);
if (!db.connect()) { qWarning() << db.lastError(); return; }

// необязательно: пользовательская БД поверх основной (см. раздел 6)
db.attach(pathToUserDb);
```

`DataBase` привязан к потоку, в котором его создали. В `QtConcurrent::run` создавайте его внутри задачи. Соединение закрывается в деструкторе. Копировать объект нельзя.

---

## 3. SELECT

```cpp
// все поля, все строки
QList<Aerodrom> all = db.from<Aerodrom>().select().exec();

// условие; константа поля проверяется при компиляции
QList<Ranway> rws = db.from<Ranway>()
        .select()
        .where(RanwayFields::kod_icao, Comparison::Equal, "USPP")
        .where(RanwayFields::tora, Comparison::Greater, 2500)   // условия объединяются через AND
        .limit(10).offset(0)
        .exec();

// только нужные колонки (остальные поля останутся пустыми)
db.from<Aerodrom>().select(AerodromFields::kod_icao, AerodromFields::name_aero).exec();

// одна запись
QPair<Aerodrom, bool> r = db.from<Aerodrom>().select()
        .where(AerodromFields::kod_icao, Comparison::Equal, "USPP").first();
if (r.second) { /* r.first — найденная запись */ }
```

- `Comparison` принимает значения `Equal`, `Greater`, `Less`, `GreaterOrEqual`, `LessOrEqual`.
- Строки сравниваются без учёта регистра и пробелов по краям: `"uspp "` находит `USPP`. Числа сравниваются как числа.
- Колонку можно передать строкой: `"KOD_ICAO"` или `"kod_icao"`. Так удобно, когда имя приходит из QML: `.select("KOD_ICAO", "NAME_AERO")`, `.where("KOD_ICAO", Comparison::Equal, v)`. Неизвестное имя даёт предупреждение в лог и пустой результат.
- `limit(0)` означает «без ограничения».
- При ошибке `exec()` возвращает пустой список. Причину можно узнать через `lastError()`, а собранный SQL через `lastSql()`.

---

## 4. UPDATE

```cpp
auto q = db.update<Ranway>()
        .set(RanwayFields::tora, 3400)
        .set(RanwayFields::pokr_vpp, "ASPH")
        .where(RanwayFields::kod_icao, Comparison::Equal, "USPP")
        .where(RanwayFields::num_vpp,  Comparison::Equal, "03");
if (!q.exec()) qWarning() << q.lastError();

// все поля объекта сразу (колонки PRIMARY KEY пропускаются автоматически)
db.update<Ranway>().setAll(rw)
        .where(RanwayFields::kod_icao, Comparison::Equal, rw.kod_icao)
        .where(RanwayFields::num_vpp,  Comparison::Equal, rw.num_vpp)
        .exec();

// по имени колонки
db.update<Ranway>().set("TORA", 3400).whereColumn("KOD_ICAO", Comparison::Equal, "USPP").exec();
```

Защитные ограничения:

- `WHERE` обязателен.
- Колонки первичного ключа менять нельзя.
- Нужен хотя бы один `set`.

Если что-то из этого нарушено, `exec()` вернёт `false`, а причину покажет `lastError()`.

---

## 5. INSERT

```cpp
Aerodrom a;
a.kod_icao = "XXXX";
a.name_aero = "TEST";

db.into<Aerodrom>().insertObject(a);                 // все поля объекта
db.into<Aerodrom>().insertObject(a, {"LEV_KTA"});    // кроме перечисленных

db.into<Aerodrom>()                                   // вручную
        .insert(AerodromFields::kod_icao, AerodromFields::name_aero)
        .values("XXXX", "TEST")
        .exec();
```

---

## 6. Пользовательская БД поверх основной

Основная БД приходит извне и не меняется. Правки пользователя хранятся в отдельной БД с теми же таблицами. После `db.attach(userDb)` все запросы автоматически работают с «наложением»:

| Запрос | Без `attach` | С `attach` |
|---|---|---|
| `select` | из основной | запись из пользовательской, иначе из основной (сравнение по `ORM_PRIMARY_KEY`) |
| `update` | в основной | если запись есть в пользовательской, она обновляется; если нет, запись копируется из основной с изменениями; всё в одной транзакции |
| `insert` | в основную | в пользовательскую |

Код запросов от этого не меняется. Условия `where` применяются к итоговым данным, то есть к записи, которую видит пользователь. `attach` действует только в пределах соединения, поэтому его нужно вызывать в каждом новом `DataBase`.

> Без `attach` запрос `update` пишет прямо в основную БД.

---

## 7. QML

QML получает `QVariantMap`, для JS это обычный объект: `item.kod_icao`, `item.tora`.

```cpp
QVariantList list = orm::toVariantList(rows);         // QList<T>  -> QVariantList
QVariantMap  map  = orm::toVariantMap(row);           // T         -> QVariantMap
Ranway       rw   = orm::fromVariantMap<Ranway>(map); // из QML обратно в C++
auto* f = orm::findField<Ranway>("KOD_ICAO");         // nullptr, если такого поля нет
```

Имена ключей в QML совпадают с именами полей в C++ (второй аргумент `X(...)`).

Асинхронный запрос для QML (так устроен `src/dbtable.h`):

```cpp
QtConcurrent::run([target, field, value]() {
    DataBase db(target);                              // соединение создаётся в рабочем потоке
    if (!db.connect()) return QList<Ranway>();
    return db.from<Ranway>().select().where(field, Comparison::Equal, value).exec();
});
// результат забирается через QFutureWatcher в потоке GUI -> orm::toVariantList -> сигнал в QML
```

---

## 8. Обобщённый код по полям

```cpp
for (const FieldMeta<Ranway>& f : RanwayFields::meta()) {
    f.columnName;        // "TORA"
    f.memberName;        // "tora"
    f.get(rw);           // QVariant со значением поля
    f.set(rw, 3400);     // записать значение
}
```

На `meta()` построены `setAll`, `insertObject` и `toVariantMap`. Поэтому новое поле, добавленное в `common.h`, само попадает во все эти функции и в QML, без дополнительного кода.
