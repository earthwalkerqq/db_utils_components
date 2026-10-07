#ifndef ORM_FIELD_H
#define ORM_FIELD_H

/*
 * @brief Описание поля таблицы в двух видах и всё, что нужно для условий WHERE.
 * @details
 * Field<Object, Type> — тип поля сохранён в типе. Используется там, где нужна
 *                       проверка на этапе компиляции: where(), select().
 *
 * FieldMeta<Table> — тип поля спрятан внутрь лямбд, поэтому все поля таблицы
 *                    имеют ОДИН тип и складываются в обычный QList.
 *                    Используется там, где полей надо перебрать сколько есть:
 *                    преобразование в QVariantMap для QML, INSERT объекта,
 *                    UPDATE всех полей, поиск поля по имени колонки.
 */

#include <functional>

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>


template<typename Obj, typename T>
struct Field {
    QString columnName;
    T Obj::* ptr;
};


template<typename Table>
struct FieldMeta {
    QString columnName;   // имя колонки в БД:      "KOD_ICAO"
    QString memberName;   // имя члена в C++ и QML: "kod_icao"
    /* 
     * так как условие в запросах используем TRIM(LOWER()), то нужно
     * знать тип данных поля, чтобы не использовать эту конструкцию с числами
    */
    bool isText;          // поле строковое

    std::function<void(Table&, const QVariant&)> set;   // значение из БД -> поле объекта
    std::function<QVariant(const Table&)>        get;   // поле объекта   -> QVariant
};


enum class Comparison {
    Equal,
    Greater,
    Less,
    GreaterOrEqual,
    LessOrEqual
};


struct Condition {
    QString columnName;
    Comparison comparison = Comparison::Equal;
    QVariant value;

    /*
     * true  — сравнение строковое, применяется TRIM(LOWER(...)) с обеих сторон;
     * false — обычное сравнение.
     */
    bool textCompare = false;
};


namespace orm {

/*
 * Значения по умолчанию для Overlay. Объявлены ДО struct Overlay: он
 * инициализирует свои поля этими константами, а использовать тип до его
 * объявления нельзя.
 */
struct OverlayDefault {
    static constexpr bool enabled = false;
    static inline const QString local = QStringLiteral("user");
    static inline const QString base = QStringLiteral("main");
};

} // namespace orm


/*
 * Наложение пользовательской БД на основную.
 *
 * Основная БД — главная схема соединения ("main"), приходит извне и не
 * меняется. Пользовательская подключается через DataBase::attach() под
 * именем local. Пока attach не выполнен, enabled == false и запросы
 * строятся к одной основной БД, как раньше.
 *
 * Лежит здесь, а не в database.h: select.h должен знать этот тип,
 * а database.h включает select.h, а не наоборот.
 */
struct Overlay {
    bool enabled = orm::OverlayDefault::enabled; // true только после успешного ATTACH
    QString local = orm::OverlayDefault::local;  // схема пользовательской БД
    QString base = orm::OverlayDefault::base;    // схема основной БД
};


namespace orm {

/*
 * Первичный ключ таблицы — нужен наложению, чтобы понять, какую строку
 * основной БД перекрывает строка пользовательской.
 *
 * По умолчанию ключ не задан. Задаётся в common.h макросом ORM_PRIMARY_KEY
 * рядом с описанием таблицы.
 */
/*
 * @brief Описание таблицы в виде данных: имя, объявленные колонки, ключ.
 *
 * @details
 * То же, что знают о таблице шаблоны ORM, но без типов — обычная структура,
 * которую можно сложить в QList и обойти циклом. Нужна там, где код не
 * шаблонный и работает со «сколько есть» таблицами: создание пользовательской
 * БД по структуре основной, перенос правок при обновлении, проверка схемы
 * файла. Заполняется макросами db_manager из метаданных ORM.
 */
struct TableSchema {
    QString     name;      // имя таблицы в БД:          "RANWAY"
    QStringList columns;   // объявленные колонки:        RanwayFields::columns()
    QStringList key;       // первичный ключ:             ORM_PRIMARY_KEY
};


/*
 * @brief Чья версия значения главнее при переносе правок в новую БД.
 *
 * SourceFirst — значение из новой поставки, а из прежней пользовательской
 *               берётся только то, что в поставке NULL;
 * UserFirst   — наоборот: правка пользователя главнее, из поставки берётся
 *               только то, чего в правке нет (NULL).
 */
enum class MergePolicy {
    SourceFirst,
    UserFirst
};


template<typename Table>
struct PrimaryKey {
    static QStringList columns(void) { return QStringList(); }
};

inline QString comparisonToSql(Comparison comparison)
{
    switch (comparison) {
        case Comparison::Equal:          return QStringLiteral("=");
        case Comparison::Less:           return QStringLiteral("<");
        case Comparison::Greater:        return QStringLiteral(">");
        case Comparison::LessOrEqual:    return QStringLiteral("<=");
        case Comparison::GreaterOrEqual: return QStringLiteral(">=");
    }
    return QString();
}

/*
 * Одно выражение WHERE с именованным параметром :<prefix><index>.
 *
 * prefix нужен запросу с наложением: одни и те же условия стоят в нём
 * дважды — для пользовательской и для основной БД. Одно и то же имя
 * параметра, использованное в запросе дважды, Qt с SQLite обрабатывает
 * не во всех версиях, поэтому половины получают разные имена (:p0 и :q0),
 * а значения биндятся дважды.
 */
inline QString buildCondition(const Condition& condition, int index,
                              const QString& prefix = QStringLiteral("p"))
{
    const QString op = comparisonToSql(condition.comparison);
    const QString placeholder = QStringLiteral(":%1%2").arg(prefix).arg(index);

    if (condition.textCompare) {
        return QStringLiteral("TRIM(LOWER(%1)) %2 TRIM(LOWER(%3))")
                .arg(condition.columnName, op, placeholder);
    }
    return QStringLiteral("%1 %2 %3").arg(condition.columnName, op, placeholder);
}

} // namespace orm

#endif // ORM_FIELD_H
