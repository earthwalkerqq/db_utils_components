#ifndef ORM_INSERT_H
#define ORM_INSERT_H

/*
 * Два способа:
 *   .insert(поля...).values(значения...)  — значения присваиваются позиционно
 *   .insertObject(объект)                 — все поля объекта разом, через meta().
 */

#include <QDebug>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QVariant>

#include "field.h"
#include "table_macro.h"


template<typename Table>
class SqlInsertQuery {
public:
    explicit SqlInsertQuery(QSqlDatabase database, Overlay overlay = Overlay()) : 
             m_db(std::move(database)), m_overlay(overlay) {}

    SqlInsertQuery& insert(void) { return *this; }

    template<typename Type>
    SqlInsertQuery& insert(const Field<Table, Type>& field)
    {
        if (!m_columnList.contains(field.columnName)) {
            m_columnList.append(field.columnName);
        }
        return *this;
    }

    /*
     * Параметры ограничены типом Field<Table, ...>: без этого шаблон
     * перехватывал бы и строки, и FieldMeta — insert("KOD_ICAO") и
     * insert(field) из цикла ниже уходили бы сюда же и зацикливались.
     */
    template<typename... Types>
    SqlInsertQuery& insert(const Field<Table, Types>&... fields)
    {
        (insert(fields), ...);
        return *this;
    }

    SqlInsertQuery& insert(const FieldMeta<Table>& field)
    {
        if (!m_columnList.contains(field.columnName)) {
            m_columnList.append(field.columnName);
        }
        return *this;
    }

    SqlInsertQuery& insert(const QString& columnName)
    {
        for (const FieldMeta<Table>& field : Table::fields_type::meta()) {
            if (field.columnName.compare(columnName, Qt::CaseInsensitive) == 0 ||
                field.memberName.compare(columnName, Qt::CaseInsensitive) == 0)
            {
                return insert(field);
            }
        }

        qWarning().nospace() << "ORM: в таблице " << Table::table_name
                             << " нет колонки " << columnName << "; доступны: "
                             << Table::fields_type::columns().join(QStringLiteral(", "));
        m_valid = false;
        return *this;
    }

    template<typename... Args>
    SqlInsertQuery& insert(const QString& first, const Args&... args) {
        insert(first);
        (insert(QString(args)), ...);
        return *this;
    }

    SqlInsertQuery& values(const QVariant& value)
    {
        m_valuesList.append(value);
        return *this;
    }

    template<typename First, typename... Args>
    SqlInsertQuery& values(const First& first, const Args&... args)
    {
        values(QVariant(first));
        (values(QVariant(args)), ...);
        return *this;
    }

    /*
     * Вставка объекта целиком: колонки и значения берутся из meta(),
     * поэтому перепутать порядок невозможно.
     *
     * skipColumns — колонки, которые не надо вставлять (например, если
     * значение проставляет сама БД).
     */
    bool insertObject(const Table& object, const QStringList& skipColumns = QStringList())
    {
        m_columnList.clear();
        m_valuesList.clear();

        for (const FieldMeta<Table>& field : Table::fields_type::meta()) {
            if (skipColumns.contains(field.columnName, Qt::CaseInsensitive)) {
                continue;
            }
            m_columnList.append(field.columnName);
            m_valuesList.append(field.get(object));
        }
        return exec();
    }

    QString lastError(void) const { return m_lastError; }
    int numRowsAffected(void) const { return m_rowsAffected; }

    bool exec(void)
    {
        m_lastError.clear();
        m_rowsAffected = 0;

        if (!m_valid) {
            m_lastError = QStringLiteral("запрос не собран: неизвестная колонка");
            return false;
        }

        if (m_columnList.isEmpty()) {
            int field_size = Table::fields_type::meta().size();
            if (field_size != m_valuesList.size()) {
                m_lastError = QStringLiteral("колонок %1, значений %2")
                                .arg(field_size).arg(m_valuesList.size());
                qWarning() << "ORM: INSERT без колонок в" << Table::table_name;
                return false;
            } else {
                for (const FieldMeta<Table>& field : Table::fields_type::meta()) {
                    m_columnList.append(field.columnName);
                }
            }
        } else if (m_columnList.size() != m_valuesList.size()) {
            m_lastError = QStringLiteral("колонок %1, значений %2")
                            .arg(m_columnList.size()).arg(m_valuesList.size());
            qWarning() << "ORM: INSERT в" << Table::table_name
                    << "— количество полей не сходится с количеством значений:"
                    << m_lastError;
            return false;
        }

        QSqlQuery query(m_db);
        if (!bindQuery(query)) {
            return false;
        }
        if (!query.exec()) {
            m_lastError = query.lastError().text();
            qWarning() << "ORM: ошибка выполнения INSERT:" << m_lastError;
            return false;
        }

        m_rowsAffected = query.numRowsAffected();
        return true;
    }

private:
    bool bindQuery(QSqlQuery& query)
    {
        QStringList placeholders;
        for (int i = 0; i < m_valuesList.size(); ++i) {
            placeholders.append(QStringLiteral(":v%1").arg(i));
        }

        const QString table_name = (m_overlay.enabled) 
                ? m_overlay.local + QLatin1Char('.') + QLatin1String(Table::table_name)
                : QLatin1String(Table::table_name);

        const QString queryStr = QStringLiteral("INSERT INTO %1 (%2) VALUES (%3)")
                .arg(table_name,
                     m_columnList.join(QStringLiteral(", ")),
                     placeholders.join(QStringLiteral(", ")));

        if (!query.prepare(queryStr)) {
            m_lastError = query.lastError().text();
            qWarning() << "ORM: ошибка PREPARE:" << m_lastError << "\n  SQL:" << queryStr;
            return false;
        }

        for (int i = 0; i < m_valuesList.size(); ++i) {
            query.bindValue(QStringLiteral(":v%1").arg(i), m_valuesList.at(i));
        }
        return true;
    }

    QSqlDatabase m_db;
    QStringList m_columnList;
    QVariantList m_valuesList;
    Overlay m_overlay;

    int m_rowsAffected = 0;
    bool m_valid = true;
    QString m_lastError;
};

#endif // ORM_INSERT_H
