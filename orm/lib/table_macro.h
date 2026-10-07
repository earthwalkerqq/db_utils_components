#ifndef ORM_TABLE_MACRO_H
#define ORM_TABLE_MACRO_H

/*
 * Пример описания поля таблицы
 *     X(QString, kod_icao, "KOD_ICAO")
 * а макрос ORM_DECLARE_TABLE порождает из неё:
 *   - член структуры                  QString kod_icao{};
 *   - строку в fill()                 чтение колонки KOD_ICAO
 *   - константу поля                  AerodromFields::kod_icao
 *   - имя колонки в columns()         "KOD_ICAO"
 *   - элемент meta() с set/get        для QML, INSERT и UPDATE объекта
 *   - элемент кортежа all()           когда нужен тип поля без накладных расходов
 *
 * Имя колонки, имя члена и его тип указаны ровно один раз и разойтись
 * между собой не могут.
 *
 * ВАЖНО для QML: макрос НЕ порождает Q_GADGET и Q_PROPERTY намеренно.
 * CMake AUTOMOC решает, запускать ли moc, текстовым поиском строки
 * "Q_GADGET" в файле — после макроса её там нет, метаобъект не собрался бы.
 * Вместо этого объект отдаётся в QML как QVariantMap
 * (более подробно описано в ./qml.h):
 * для QML это обычный JS-объект. В таком случае moc не нужен
 */

#include <tuple>

#include <QDebug>
#include <QList>
#include <QSet>
#include <QSqlRecord>
#include <QString>
#include <QStringList>
#include <QVariant>

#include "field.h"
#include "log.h"


namespace orm {
namespace detail {

/*
 * @brief
 * Чтение одной колонки из текущей строки результата.
 */
template<typename T, typename Q>
T readColumn(const Q& q, const QSqlRecord& record, const QString& column,
                const char* tableName, const char* memberName)
{
    QString msg;
    const int index = record.indexOf(column);
    if (index < 0) {
        // предупреждаем один раз на колонку, иначе лог зальёт на каждой строке
        thread_local QSet<QString> reported;
        const QString key = QString::fromLatin1(tableName) + QLatin1Char('.') + column;
        if (!reported.contains(key)) {
            reported.insert(key);
            msg = QStringLiteral("В результате запроса нет колонки %1.%2; поле %3 останется пустым")
                        .arg(tableName)
                        .arg(column)
                        .arg(memberName);
            ORM_ERROR_LOG(msg);
        }
        return T{};
    }

    const QVariant value = q.value(index);
    if (!value.isValid() || value.isNull()) {
        return T{};
    }
    if (!value.canConvert<T>()) {
        msg = QStringLiteral("Значение %1.%2 не приводится к типу поля %3; получено: %4")
                        .arg(tableName)
                        .arg(column)
                        .arg(memberName)
                        .arg(value.toString());
        ORM_ERROR_LOG(msg);
        return T{};
    }
    return value.value<T>();
}

} // namespace detail
} // namespace orm


/* ------------------------------------------------------------------------- *
 *  Обработчики списка полей: каждый получает (T, name, column)
 *  и выдаёт свой кусок кода.
 * ------------------------------------------------------------------------- */

// член структуры; {} — гарантированная инициализация (иначе double/int — мусор)
#define ORM_DETAIL_MEMBER(T, name, column)                                     \
    T name{};

// строка тела fill()
#define ORM_DETAIL_FILL(T, name, column)                                       \
    name = orm::detail::readColumn<T>(q, record, QStringLiteral(column),       \
                                      table_name, #name);

/* именованная константа поля: <имя_объекта_структуры>Fields::<имя_колонки>
 * AerodromFields::kod_icao (как пример)
 */
#define ORM_DETAIL_FIELD(T, name, column)                                      \
    inline static const Field<Self, T> name{ QStringLiteral(column),           \
                                            &Self::name };

// имя колонки для columns()
#define ORM_DETAIL_COLUMN(T, name, column)                                     \
    result.append(QStringLiteral(column));

// элемент рантайм-списка meta(): T запечатан внутри лямбд
#define ORM_DETAIL_META(T, name, column)                                       \
    result.append(FieldMeta<Self>{                                             \
        QStringLiteral(column),                                                \
        QStringLiteral(#name),                                                 \
        std::is_same<T, QString>::value,                                       \
        [](Self& object, const QVariant& value) {                              \
            object.name = value.value<T>();                                    \
        },                                                                     \
        [](const Self& object) {                                               \
            return QVariant::fromValue(object.name);                           \
        } });

/*
 * @details
 * Элемент кортежа all(). Запятая стоит ВПЕРЕДИ: препроцессор не умеет
 * пропускать её у последнего элемента, поэтому список склеивается
 * с пустым кортежем через std::tuple_cat.
 */
#define ORM_DETAIL_TUPLE(Type, name, column)                                   \
    , std::make_tuple(Field<Self, Type>{ QStringLiteral(column), &Self::name })


/* @details
 *
 * Пример использования:
 *    ORM_DECLARE_TABLE(Aerodrom, "AERODROM", ORM_FIELDS_AERODROM)
 *
 *  Struct##Fields — склейка токенов, получается имя AerodromFields.
 *  using Self = Struct — обработчики получают только (T, name, column),
 *  имени структуры среди аргументов нет, поэтому берём его через псевдоним.
 */
#define ORM_DECLARE_TABLE(Struct, TableName, FIELD_LIST)                       \
    struct Struct##Fields;                                                     \
                                                                               \
    struct Struct {                                                            \
        using fields_type = Struct##Fields;                                    \
        static constexpr const char* table_name = TableName;                   \
                                                                               \
        FIELD_LIST(ORM_DETAIL_MEMBER)                                          \
                                                                               \
        template<typename Query>                                               \
        void fill(const Query& q) {                                            \
            const QSqlRecord record = q.record();                              \
            FIELD_LIST(ORM_DETAIL_FILL)                                        \
        }                                                                      \
    };                                                                         \
                                                                               \
    struct Struct##Fields {                                                    \
        using Self = Struct;                                                   \
                                                                               \
        /* именованные константы: AerodromFields::kod_icao */                  \
        FIELD_LIST(ORM_DETAIL_FIELD)                                           \
                                                                               \
        /* только имена колонок */                                             \
        static QStringList columns() {                                         \
            QStringList result;                                                \
            FIELD_LIST(ORM_DETAIL_COLUMN)                                      \
            return result;                                                     \
        }                                                                      \
                                                                               \
        /* рантайм-список с set/get: обычный QList, обычный for */             \
        static const QList<FieldMeta<Self>>& meta() {                          \
            static const QList<FieldMeta<Self>> cached = [] {                  \
                QList<FieldMeta<Self>> result;                                 \
                FIELD_LIST(ORM_DETAIL_META)                                    \
                return result;                                                 \
            }();                                                               \
            return cached;                                                     \
        }                                                                      \
                                                                               \
        /* кортеж с сохранением типов: код без накладных расходов */           \
        static auto all() {                                                    \
            return std::tuple_cat(std::tuple<>{}                               \
                                  FIELD_LIST(ORM_DETAIL_TUPLE));               \
        }                                                                      \
    };

/*
 * @brief Первичный ключ таблицы.
 * @details
 * Пример использования:
 *    ORM_PRIMARY_KEY(Ranway, "KOD_ICAO", "NUM_VPP")
 *
 *  Специализирует orm::PrimaryKey<Ranway>. Нужен запросам с наложением
 *  пользовательской БД на основную. Таблицы без ключа работают как раньше,
 *  но выборка из них при подключённой пользовательской БД вернёт ошибку.
 * ------------------------------------------------------------------------- */
#define ORM_PRIMARY_KEY(Struct, ...)                                           \
    namespace orm {                                                            \
    template<> struct PrimaryKey<::Struct> {                                   \
        static QStringList columns(void) { return QStringList{ __VA_ARGS__ }; }\
    };                                                                         \
    }

#endif // ORM_TABLE_MACRO_H
