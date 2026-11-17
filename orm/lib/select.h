#ifndef ORM_SELECT_H
#define ORM_SELECT_H

#include <functional>
#include <type_traits>

#include <QDebug>
#include <QList>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QString>
#include <QStringList>
#include <QVariant>

#include "field.h"
#include "table_macro.h"


template<typename Table>
class SqlSelectQuery {
    struct ColumnBinding {
        QString columnName;
        std::function<void(Table&, const QVariant&)> fill;
    };

public:
    SqlSelectQuery(QSqlDatabase database, Overlay overlay = Overlay())
        : m_db(std::move(database)), m_overlay(std::move(overlay)) {}

    // все поля, описанные в структуре
    SqlSelectQuery& select(void) { return *this; }

    // одна колонка по константе поля: select(AerodromFields::kod_icao)
    template<typename Type>
    SqlSelectQuery& select(const Field<Table, Type>& field)
    {
        m_columnList.append({
            field.columnName,
            [ptr = field.ptr](Table& table, const QVariant& value) {
                table.*ptr = value.value<Type>();
            }
        });
        return *this;
    }

    /*
     * Несколько колонок по константам полей.
     * Параметры ограничены типом Field<Table, ...>: без этого шаблон
     * перехватывал бы и строковые литералы — select("KOD_ICAO") уходил бы
     * в бесконечную рекурсию вместо select(const QString&).
     */
    template<typename... Types>
    SqlSelectQuery& select(const Field<Table, Types>&... fields)
    {
        (select(fields), ...);
        return *this;
    }

    SqlSelectQuery& select(const FieldMeta<Table>& field)
    {
        m_columnList.append({ field.columnName, field.set });
        return *this;
    }

    // одна колонка по имени — для вызовов из QML
    SqlSelectQuery& select(const QString& columnName)
    {
        for (const FieldMeta<Table>& field : Table::fields_type::meta()) {
            if (field.columnName.compare(columnName, Qt::CaseInsensitive) == 0 ||
                field.memberName.compare(columnName, Qt::CaseInsensitive) == 0)
            {
                return select(field);
            }
        }

        qWarning().nospace() << "ORM: в таблице " << Table::table_name
                             << " нет колонки " << columnName << "; доступны: "
                             << Table::fields_type::columns().join(QStringLiteral(", "));
        m_valid = false;
        return *this;
    }

    // несколько колонок по именам: select("KOD_ICAO", "NAME_AERO")
    template<typename... Args>
    SqlSelectQuery& select(const QString& first, const Args&... args)
    {
        select(first);
        (select(QString(args)), ...);
        return *this;
    }

    template<typename Type>
    SqlSelectQuery& where(const Field<Table, Type>& field,
                          const Comparison comparison, const QVariant& value)
    {
        m_conditionList.append({field.columnName, comparison, value,
                                std::is_same<Type, QString>::value});
        return *this;
    }

    SqlSelectQuery& where(const FieldMeta<Table>& field,
                          const Comparison comparison, const QVariant& value)
    {
        m_conditionList.append({field.columnName, comparison, value, field.isText});
        return *this;
    }

    SqlSelectQuery& where(const QString& columnName,
                          const Comparison comparison, const QVariant& value)
    {
        for (const FieldMeta<Table>& field : Table::fields_type::meta()) {
            if (field.columnName.compare(columnName, Qt::CaseInsensitive) == 0 ||
                field.memberName.compare(columnName, Qt::CaseInsensitive) == 0) {
                return where(field, comparison, value);
            }
        }

        qWarning().nospace() << "ORM: в таблице " << Table::table_name
                             << " нет колонки " << columnName << "; доступны: "
                             << Table::fields_type::columns().join(QStringLiteral(", "));
        m_valid = false;
        return *this;
    }

    template<typename Type>
    SqlSelectQuery& order(const Field<Table, Type>& field)
    {
        m_orderedList.append(field.columnName);
        return *this;
    }

    template<typename... Types>
    SqlSelectQuery& order(const Field<Table, Types>&... fields)
    {
        (order(fields), ...);
        return *this;
    }

    SqlSelectQuery& order(const FieldMeta<Table>& field)
    {
        m_orderedList.append(field.columnName);
        return *this;
    }

    SqlSelectQuery& order(const QString& columnName)
    {
        m_orderedList.append(columnName);
        return *this;
    }

    template<typename... Args>
    SqlSelectQuery& order(const QString& first, const Args&... args)
    {
        order(first);
        (order(QString(args)), ...);
        return *this;
    }

    SqlSelectQuery& limit(int count)  { m_limit = count;  return *this; }
    SqlSelectQuery& offset(int count) { m_offset = count; return *this; }

    // Текст последней ошибки; пустая строка — ошибки не было
    QString lastError(void) const { return m_lastError; }

    // SQL последнего выполненного запроса — удобно для отладки наложения
    QString lastSql(void) const { return m_lastSql; }

    QList<Table> exec(void)
    {
        m_lastError.clear();

        if (!m_valid) {
            m_lastError = QStringLiteral("запрос не собран: неизвестная колонка");
            return {};
        }

        QSqlQuery query(m_db);

        const bool prepared = m_overlay.enabled ? buildOverlaySql(query)
                                                : buildPlainSql(query);
        if (!prepared) {
            return {};
        }
        if (!query.exec()) {
            m_lastError = query.lastError().text();
            qWarning() << "ORM: ошибка выполнения SELECT:" << m_lastError
                       << "\n  SQL:" << m_lastSql;
            return {};
        }
        return readRows(query);
    }

    // Первая строка результата; second == false, если строк нет
    QPair<Table, bool> first(void)
    {
        const int saved = m_limit;
        m_limit = 1;
        const QList<Table> rows = exec();
        m_limit = saved;

        if (rows.isEmpty()) {
            return { Table{}, false };
        }
        return { rows.first(), true };
    }

private:
    QList<Table> readRows(QSqlQuery& query)
    {
        QList<Table> result;

        if (m_columnList.isEmpty()) {
            while (query.next()) {
                Table row;
                row.fill(query);
                result.append(row);
            }
            return result;
        }

        QList<int> columnIndexes;

        while (query.next()) {
            if (columnIndexes.isEmpty()) {
                const QSqlRecord record = query.record();
                for (const ColumnBinding& binding : m_columnList) {
                    const int index = record.indexOf(binding.columnName);
                    if (index < 0) {
                        qWarning().nospace() << "ORM: в результате запроса к "
                                             << Table::table_name
                                             << " нет колонки " << binding.columnName;
                    }
                    /*
                     * Добавляем индекс ВСЕГДА, даже -1. Если пропустить колонку
                     * через continue, columnIndexes станет короче m_columnList:
                     * колонки съедут на одну, а at(i) у последних выйдет за
                     * границы списка.
                     */
                    columnIndexes.append(index);
                }
            }

            Table row;
            for (int i = 0; i < m_columnList.size(); ++i) {
                const int index = columnIndexes.at(i);
                if (index >= 0) {
                    m_columnList.at(i).fill(row, query.value(index));
                }
            }
            result.append(row);
        }

        return result;
    }

    // список колонок: выбранные явно или все описанные в структуре
    QString selectedColumns(void) const
    {
        if (m_columnList.isEmpty()) {
            return Table::fields_type::columns().join(QStringLiteral(", "));
        }
        QStringList columns;
        for (const ColumnBinding& binding : m_columnList) {
            columns.append(binding.columnName);
        }
        return columns.join(QStringLiteral(", "));
    }

    /*
     * @details
     * формирует строку с условиями с проверкой типов:
     * если поле строковое — TRIM(LOWER(<поле>)) <оператор> TRIM(LOWER(:<prefix>N))
     * иначе               — <поле> <оператор> :<prefix>N  (без TRIM и LOWER)
     * оператор берётся из Comparison: =, >, <, >=, <=
     * @return строка без слова WHERE — его добавляет вызывающий
    */
    QString whereExpression(const QString& prefix) const
    {
        QStringList expressions;
        for (int i = 0; i < m_conditionList.size(); ++i) {
            expressions.append(orm::buildCondition(m_conditionList.at(i), i, prefix));
        }
        return expressions.join(QStringLiteral(" AND "));
    }

    QString limitOffset(void) const
    {
        if (m_limit <= 0) {
            return QString();
        }
        QString res = QStringLiteral(" LIMIT %1").arg(m_limit);
        if (m_offset > 0) {
            res += QStringLiteral(" OFFSET %1").arg(m_offset);
        }
        return res;
    }

    void bindConditions(QSqlQuery& query, const QString& prefix) const
    {
        for (int i = 0; i < m_conditionList.size(); ++i) {
            query.bindValue(QStringLiteral(":%1%2").arg(prefix).arg(i),
                            m_conditionList.at(i).value);
        }
    }

    bool prepare(QSqlQuery& query, const QString& sql)
    {
        m_lastSql = sql;
        if (!query.prepare(sql)) {
            m_lastError = query.lastError().text();
            qWarning() << "ORM: ошибка PREPARE:" << m_lastError << "\n  SQL:" << sql;
            return false;
        }
        return true;
    }

    static bool isTextColumn(const QString& column)
    {
        for (const FieldMeta<Table>& field : Table::fields_type::meta()) {
            if (field.columnName.compare(column, Qt::CaseInsensitive) == 0) {
                return field.isText;
            }
        }
        // если не нашли колонку - значит тектовое поле
        return true;
    }

    /* 
     * @details 
     * обычный запрос к БД если нет локальной 
     * 
     * SELECT <колонки> FROM AERODROM WHERE <условия> LIMIT n
    */
    bool buildPlainSql(QSqlQuery& query)
    {
        const QString prefix = QStringLiteral("p");

        QString sql = QStringLiteral("SELECT %1 FROM %2")
                .arg(selectedColumns(), QLatin1String(Table::table_name));

        const QString where = whereExpression(prefix);
        if (!where.isEmpty()) {
            sql += QStringLiteral(" WHERE ") + where;
        }

        if (!m_orderedList.isEmpty()) {
            sql += QLatin1String(" ORDER BY ") + m_orderedList.join(QLatin1String(", "));
        }

        sql += limitOffset();

        if (!prepare(query, sql)) {
            return false;
        }
        bindConditions(query, prefix);
        return true;
    }

    /*
     * @details
     * Запрос с наложением: сначала пользовательская БД, затем основная
     * умное объединение данных из двух источников без дублирования
     * в качестве примера запрос к RANWAY:
     *
     *    SELECT <колонки> FROM user.RANWAY
     *    WHERE <условия с :p0, :p1 ...>
     *    UNION ALL
     *    SELECT <колонки> FROM main.RANWAY AS b
     *    WHERE <условия с :q0, :q1 ...>
     *       AND NOT EXISTS (SELECT 1 FROM user.RANWAY AS u
     *                        WHERE TRIM(u.KOD_ICAO) = TRIM(b.KOD_ICAO)
     *                          AND TRIM(u.NUM_VPP) = TRIM(b.NUM_VPP))
     *    LIMIT n
     *
     *  Первая половина — всё, что есть у пользователя.
     *  Вторая — всё из основной, КРОМЕ строк, ключ которых есть у пользователя.
     *  Вместе: запись берётся из пользовательской БД, а если её там нет —
     *  из основной.
     */
    bool buildOverlaySql(QSqlQuery& query)
    {
        const QStringList key = orm::PrimaryKey<Table>::columns();
        if (key.isEmpty()) {
            m_lastError = QStringLiteral("для таблицы %1 не задан первичный ключ; "
                                         "добавьте ORM_PRIMARY_KEY в common.h")
                              .arg(QLatin1String(Table::table_name));
            qWarning() << "ORM:" << m_lastError;
            return false;
        }

        const QString table = QLatin1String(Table::table_name);
        const QString local = m_overlay.local + QLatin1Char('.') + table;
        const QString base = m_overlay.base  + QLatin1Char('.') + table;
        const QString columns = selectedColumns();

        /*
         * Условия стоят в запросе дважды — в каждой половине UNION.
         * Поэтому у половин разные имена параметров (:p0 и :q0), а значения
         * биндятся  удинаковые. 
         * Дело в том, что драйвер SQLite в Qt 5 не во всех версиях
         * может обрабатывать одни и те же имена повторенные в одном
         * запросе
         */
        const QString localPrefix = QStringLiteral("p");
        const QString basePrefix = QStringLiteral("q");

        QString sql = QStringLiteral("SELECT %1 FROM %2").arg(columns, local);
        const QString whereLocal = whereExpression(localPrefix);
        if (!whereLocal.isEmpty()) {
            sql += QStringLiteral(" WHERE ") + whereLocal;
        }

        sql += QStringLiteral(" UNION ALL SELECT %1 FROM %2 AS b WHERE ").arg(columns, base);
        const QString whereBase = whereExpression(basePrefix);
        if (!whereBase.isEmpty()) {
            sql += whereBase + QStringLiteral(" AND ");
        }

        // сопоставление строк по первичному ключу
        QStringList keyMatch;
        for (const QString& column : key) {
            keyMatch.append(isTextColumn(column)
                ? QStringLiteral("TRIM(u.%1) = TRIM(b.%1)").arg(column)
                : QStringLiteral("u.%1 = b.%1").arg(column));
        }

        sql += QStringLiteral("NOT EXISTS (SELECT 1 FROM %1 AS u WHERE %2)")
                   .arg(local, keyMatch.join(QStringLiteral(" AND ")));

        if (!m_orderedList.isEmpty()) {
            sql += QLatin1String(" ORDER BY ") + m_orderedList.join(QLatin1String(", "));
        }

        sql += limitOffset();

        if (!prepare(query, sql)) {
            return false;
        }
        bindConditions(query, localPrefix);
        bindConditions(query, basePrefix);
        return true;
    }

    QSqlDatabase m_db;
    QList<ColumnBinding> m_columnList;
    QList<Condition> m_conditionList;
    QList<QString> m_orderedList;
    Overlay m_overlay;

    int m_limit  = -1;
    int m_offset = 0;
    bool m_valid = true;
    QString m_lastError;
    QString m_lastSql;
};

#endif // ORM_SELECT_H
