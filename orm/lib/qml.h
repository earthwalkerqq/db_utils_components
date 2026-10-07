#ifndef ORM_QML_H
#define ORM_QML_H

/*
 * @brief Мост в QML
 * @details
 *
 *   1. CMake AUTOMOC решает, запускать ли moc на файле, ТЕКСТОВЫМ поиском
 *      строки "Q_GADGET"/"Q_OBJECT" в исходнике. Если Q_GADGET появляется
 *      только после раскрытия макроса ORM_DECLARE_TABLE, в тексте common.h
 *      его нет — moc не запустится, метаобъекта не будет, QML не увидит
 *      ничего. Обходится явным qt5_wrap_cpp, но это неочевидная ловушка.
 *
 *   2. Q_PROPERTY пришлось бы перечислять — то есть завести ещё одну копию
 *      списка полей, которую надо синхронизировать вручную. Ровно от этого
 *      мы и уходили.
 *
 * Вместо этого объект отдаётся как QVariantMap. Для QML это обычный
 * JS-объект: item.kod_icao, item.tora, list.length работают как с Q_GADGET,
 * но moc не нужен вовсе, и в QML попадают ВСЕ поля автоматически.
 *
 * Ключи QVariantMap — имена членов в C++ (kod_icao), а не имена колонок
 * (KOD_ICAO), потому что QML обращается к ним именно так.
 */

#include <QList>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include "field.h"


namespace orm {

// Объект -> QVariantMap { "kod_icao": "USPP", "lat_rad": 1.0108, ... }
template<typename Table>
QVariantMap toVariantMap(const Table& object)
{
    QVariantMap map;
    for (const FieldMeta<Table>& field : Table::fields_type::meta()) {
        map.insert(field.memberName, field.get(object));
    }
    return map;
}

// Список объектов -> QVariantList, готовый к передаче в QML
template<typename Table>
QVariantList toVariantList(const QList<Table>& rows)
{
    QVariantList list;
    list.reserve(rows.size());
    for (const Table& row : rows) {
        list.append(toVariantMap(row));
    }
    return list;
}

// QVariantMap -> объект (для данных, пришедших из QML обратно в C++)
template<typename Table>
Table fromVariantMap(const QVariantMap& map)
{
    Table object;
    for (const FieldMeta<Table>& field : Table::fields_type::meta()) {
        if (map.contains(field.memberName)) {
            field.set(object, map.value(field.memberName));
        }
    }
    return object;
}

/*
 * @details
 * Поиск описания поля по имени. Принимает и имя колонки ("KOD_ICAO"),
 * и имя члена ("kod_icao"), регистр не важен.
 * Возвращает nullptr, если поля нет — вызывающий решает, что делать.
 */
template<typename Table>
const FieldMeta<Table>* findField(const QString& name)
{
    for (const FieldMeta<Table>& field : Table::fields_type::meta()) {
        if (field.columnName.compare(name, Qt::CaseInsensitive) == 0 ||
            field.memberName.compare(name, Qt::CaseInsensitive) == 0) {
            return &field;
        }
    }
    return nullptr;
}

} // namespace orm

#endif // ORM_QML_H
