#ifndef ORM_UPDATE_H
#define ORM_UPDATE_H

/*
 * @details
 * Два способа:
 *   .set(поле, значение)   — точечное обновлене по полю
 *   .setAll(объект, ключи) — все поля объекта разом, через meta()
 *
 * WHERE обязателен: запрос без него отклоняется, чтобы случайно
 * не переписать всю таблицу.
 */

#include <type_traits>

#include <QDebug>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

#include "field.h"
#include "table_macro.h"


template<typename Table>
class SqlUpdateQuery {
    struct Assignment {
        QString columnName;
        QVariant value;
    };

public:
    explicit SqlUpdateQuery(QSqlDatabase database, Overlay overlay = Overlay()) :
             m_db(std::move(database)), m_overlay(overlay) {}

    template<typename Type>
    SqlUpdateQuery& set(const Field<Table, Type>& field, const QVariant& value)
    {
        // перезаписываем, если поле уже есть в выборке
        for (Assignment& assignment : m_setList) {
            if (assignment.columnName == field.columnName) {
                assignment.value = value;
                return *this;
            }
        }
        m_setList.append({field.columnName, value});
        return *this;
    }

    template<typename Type, typename... Args>
    SqlUpdateQuery& set(const Field<Table, Type>& field, const QVariant& value, Args&&... args)
    {
        set(field, value); 
        return set(std::forward<Args>(args)...);
    }

    SqlUpdateQuery& set(const QString& columnName, const QVariant& value)
    {
        for (const FieldMeta<Table>& field : Table::fields_type::meta()) {
            if (field.columnName.compare(columnName, Qt::CaseInsensitive) == 0 ||
                field.memberName.compare(columnName, Qt::CaseInsensitive) == 0)
            {
                set_(field.columnName, value);
                return *this;
            }
        }

        qWarning().nospace() << "ORM: в таблице " << Table::table_name
                             << " нет колонки " << columnName << "; доступны: "
                             << Table::fields_type::columns().join(QStringLiteral(", "));
        m_valid = false;
        return *this;
    }

    template<typename... Args>
    SqlUpdateQuery& set(const QString& columnName, const QVariant& value, Args&&... args) {
        set(columnName, value); 
        return set(std::forward<Args>(args)...);
    }

    /*
     * Все поля объекта разом.
     * Колонки первичного ключа (ORM_PRIMARY_KEY) пропускаются автоматически:
     * менять ключ запрещено в validateQuery(), и без пропуска setAll(объект)
     * всегда завершался бы ошибкой. skipColumns — дополнительные колонки,
     * которые обновлять не нужно.
     */
    SqlUpdateQuery& setAll(const Table& object,
                           const QStringList& skipColumns = QStringList())
    {
        const QStringList key = orm::PrimaryKey<Table>::columns();
        for (const FieldMeta<Table>& field : Table::fields_type::meta()) {
            if (key.contains(field.columnName) ||
                skipColumns.contains(field.columnName, Qt::CaseInsensitive)) {
                continue;
            }
            set_(field.columnName, field.get(object));
        }
        return *this;
    }

    template<typename Type>
    SqlUpdateQuery& where(const Field<Table, Type>& field,
                          const Comparison comparison, const QVariant& value)
    {
        m_conditionList.append({field.columnName, comparison, value,
                                std::is_same<Type, QString>::value});
        return *this;
    }

    SqlUpdateQuery& where(const FieldMeta<Table>& field,
                          const Comparison comparison, const QVariant& value)
    {
        m_conditionList.append({field.columnName, comparison, value, field.isText});
        return *this;
    }

    // WHERE по имени колонки — для вызовов из QML; см. пояснение в select.h
    SqlUpdateQuery& whereColumn(const QString& columnName,
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

    QString lastError(void) const { return m_lastError; }

    bool exec(void)
    {
        m_lastError.clear();

        if (!validateQuery()) { return false; }

        /*
         * С наложением UPDATE — это два запроса в одной транзакции.
         * Они выполняются целиком внутри execOverlaySql(), поэтому здесь
         * повторно вызывать query.exec() нельзя: он выполнил бы последний
         * запрос ещё раз, уже вне транзакции.
         */
        if (m_overlay.enabled) {
            return execOverlaySql();
        }

        QSqlQuery query(m_db);
        if (!buildPlainSql(query)) {
            return false;
        }
        if (!query.exec()) {
            m_lastError = query.lastError().text();
            qWarning() << "ORM: ошибка выполнения UPDATE:" << m_lastError;
            return false;
        }

        return true;
    }

private:
    void set_(const QString& columnName, const QVariant& value)
    {
        for (Assignment& assignment : m_setList) {
            if (assignment.columnName == columnName) {
                assignment.value = value;
                return;
            }
        }
        m_setList.append({columnName, value});
    }

    bool validateQuery(void)
    {
        if (!m_valid) {
            m_lastError = QStringLiteral("запрос не собран: неизвестная колонка");
            return false;
        }
        if (m_setList.isEmpty()) {
            m_lastError = QStringLiteral("нет полей для обновления");
            qWarning() << "ORM: UPDATE" << Table::table_name << "—" << m_lastError;
            return false;
        }
        if (isSetPrimaryKey()) {
            m_lastError = QStringLiteral("нельзя обновлять поля с PRIMARY KEY");
            qWarning() << "ORM: UPDATE" << Table::table_name << "—" << m_lastError;
            return false;
        }
        if (m_conditionList.isEmpty()) {
            m_lastError = QStringLiteral("UPDATE без WHERE запрещён");
            qWarning() << "ORM: UPDATE" << Table::table_name << "—" << m_lastError;
            return false;
        }
        return true;
    }

    bool isSetPrimaryKey(void)
    {
        QStringList key = orm::PrimaryKey<Table>::columns();
        for (Assignment& assignment : m_setList) {
            for (const QString& column : key) {
                if (assignment.columnName == column) {
                    return true;
                }
            }
        }
        return false;
    }

    /*
     * Условия WHERE без самого слова WHERE — через orm::buildCondition,
     * как в select.h: для строк TRIM(LOWER(...)), для чисел — обычное
     * сравнение, и учитывается оператор (=, >, <, >=, <=).
     *
     * Имена колонок без префикса схемы: в обоих запросах (UPDATE и
     * INSERT ... SELECT) во FROM ровно одна таблица, и SQLite однозначно
     * относит колонки к ней. Подзапрос NOT EXISTS — отдельная область
     * видимости, условия туда не попадают.
     */
    QString whereExpression(void) const
    {
        QStringList expressions;
        for (int i = 0; i < m_conditionList.size(); ++i) {
            expressions.append(orm::buildCondition(m_conditionList.at(i), i));
        }
        return expressions.join(QStringLiteral(" AND "));
    }

    QString wherePrimaryKeyExpression(const QString& base, const QString& local)
    {
        QStringList expressions;
        QStringList key = orm::PrimaryKey<Table>::columns();
        for (const QString& column : key) {
            if (isTextColumn(column)) expressions.append(QStringLiteral("TRIM(%1.%2) = TRIM(%3.%2)")
                                                  .arg(local, column, base));
            else expressions.append(QStringLiteral("%1.%2 = %3.%2").arg(local, column, base));
        }

        return expressions.join(QStringLiteral(" AND "));
    }

    bool buildPlainSql(QSqlQuery& query)
    {
        /*
         * Порядок присваиваний сохраняется таким, каким его задал вызывающий:
         * m_setList — список, а не QMap
         */
        QStringList assignments;
        for (int i = 0; i < m_setList.size(); ++i) {
            assignments.append(QStringLiteral("%1 = :v%2")
                                   .arg(m_setList.at(i).columnName).arg(i));
        }

        QStringList expressions;
        for (int i = 0; i < m_conditionList.size(); ++i) {
            expressions.append(orm::buildCondition(m_conditionList.at(i), i));
        }

        const QString queryStr = QStringLiteral("UPDATE %1 SET %2 WHERE %3")
                .arg(QLatin1String(Table::table_name),
                     assignments.join(QStringLiteral(", ")),
                     expressions.join(QStringLiteral(" AND ")));

        if (!query.prepare(queryStr)) {
            m_lastError = query.lastError().text();
            qWarning() << "ORM: ошибка PREPARE:" << m_lastError << "\n  SQL:" << queryStr;
            return false;
        }

        for (int i = 0; i < m_setList.size(); ++i) {
            query.bindValue(QStringLiteral(":v%1").arg(i), m_setList.at(i).value);
        }
        for (int i = 0; i < m_conditionList.size(); ++i) {
            query.bindValue(QStringLiteral(":p%1").arg(i), m_conditionList.at(i).value);
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
     * Список SELECT для шага 2: для каждой колонки структуры —
     *   изменяемая колонка -> :v<N> (новое значение);
     *   колонка ключа      -> TRIM(main.T.колонка) (в основной ключи с пробелом);
     *   остальные          -> main.T.колонка как есть.
     */
    QString allMainFieldWithUpdate(const QString& base, const QString& prefix) const
    {
        const QStringList key = orm::PrimaryKey<Table>::columns();
        QStringList fieldsList;

        bool match;
        for (const QString& column : Table::fields_type::columns()) {
            match = false;
            for (int i = 0; i < m_setList.size(); ++i) {
                if (column == m_setList.at(i).columnName) {
                    fieldsList.append(QStringLiteral(":%1%2").arg(prefix).arg(i));
                    match = true;
                    break;
                }
            }
            if (match) continue;

            if (key.contains(column) && isTextColumn(column))
                fieldsList.append(QStringLiteral("TRIM(%1.%2)").arg(base, column));
            else
                fieldsList.append(QStringLiteral("%1.%2").arg(base, column));
        }

        return fieldsList.join(QStringLiteral(", "));
    }

    /*
     * Один шаг: подготовить, привязать значения, выполнить.
     * rows — сколько строк затронул шаг.
     *
     * Каждый шаг — отдельный QSqlQuery и отдельный prepare(): SQLite
     * компилирует только первый оператор строки, всё после ';' молча
     * отбрасывается. Откат транзакции делает вызывающий — в одном месте,
     * при любой ошибке, в том числе в prepare().
     */
    bool execStep(const QString& sql, const char* step)
    {
        QSqlQuery query(m_db);
        if (!query.prepare(sql)) {
            m_lastError = query.lastError().text();
            qWarning() << "ORM: ошибка PREPARE," << step << ":" << m_lastError << "\n  SQL:" << sql;
            return false;
        }

        for (int i = 0; i < m_setList.size(); ++i) {
            query.bindValue(QStringLiteral(":v%1").arg(i), m_setList.at(i).value);
        }
        for (int i = 0; i < m_conditionList.size(); ++i) {
            query.bindValue(QStringLiteral(":p%1").arg(i), m_conditionList.at(i).value);
        }

        if (!query.exec()) {
            m_lastError = query.lastError().text();
            qWarning() << "ORM: ошибка выполнения," << step << ":" << m_lastError << "\n  SQL:" << sql;
            return false;
        }

        return true;
    }

    /*
     * @details
     * 1. если в локальной бд есть запись, то просто обновляем ее поля 
     * 2. если в локальной бд нет записи, но запись есть в основной бд,
     *    то копируем ее в локальную и изменяем ее
     *
     * Вся логика обернута в единую транзакцию для атомарности операции
     *
     *  BEGIN;

     *  ЗАПРОС 1: всё, что уже есть у пользователя, — просто обновить

        UPDATE user.RANWAY
        SET RW_LENGTH = :v0
        WHERE TRIM(LOWER(KOD_ICAO)) = TRIM(LOWER(:p0))
        AND TRIM(LOWER(NUM_VPP))  = TRIM(LOWER(:p1));

     *  ЗАПРОС 2: всё, чего у пользователя нет, взять из основной
                  и вставить уже с изменениями

        INSERT INTO user.RANWAY (KOD_ICAO, NUM_VPP, RW_TRUE_RAD, RWY_GRAD, RESA, TORA, TODA,
                                ASDA, LDA, DSPLCD_THR, RW_LENGTH, RW_WIDTH, POKR_VPP, GRUZ_VPP_T)
        SELECT TRIM(b.KOD_ICAO), TRIM(b.NUM_VPP),               -- ключ: нормализованный
            b.RW_TRUE_RAD, b.RWY_GRAD, b.RESA, b.TORA, b.TODA,
            b.ASDA, b.LDA, b.DSPLCD_THR,
            :v0,                                            -- RW_LENGTH: НОВОЕ значение
            b.RW_WIDTH, b.POKR_VPP, b.GRUZ_VPP_T            -- остальное: как в основной
        FROM main.RANWAY AS b
        WHERE TRIM(LOWER(b.KOD_ICAO)) = TRIM(LOWER(:p0))
        AND TRIM(LOWER(b.NUM_VPP))  = TRIM(LOWER(:p1))
        AND NOT EXISTS (SELECT 1 FROM user.RANWAY AS l
                            WHERE TRIM(l.KOD_ICAO) = TRIM(b.KOD_ICAO)
                            AND TRIM(l.NUM_VPP)  = TRIM(b.NUM_VPP));

        COMMIT;
    */  
    bool execOverlaySql(void)
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
        const QString base = m_overlay.base + QLatin1Char('.') + table;
        const QString prefixValue = QStringLiteral("v");

        const QString whereExp = whereExpression();

        // 1. всё, что уже есть у пользователя, — просто обновить
        QStringList assignments;
        for (int i = 0; i < m_setList.size(); ++i) {
            assignments.append(QStringLiteral("%1 = :%2%3")
                               .arg(m_setList.at(i).columnName, prefixValue).arg(i));
        }

        const QString updateSql = QStringLiteral("UPDATE %1 SET %2 WHERE %3")
                .arg(local, assignments.join(QStringLiteral(", ")), whereExp);

        // 2. всё, чего у пользователя нет, взять из основной и вставить с изменениями
        QString insertSql = QStringLiteral("INSERT INTO %1 (%2) SELECT %3 FROM %4")
                .arg(local, Table::fields_type::columns().join(QStringLiteral(", ")))
                .arg(allMainFieldWithUpdate(base, prefixValue), base);
        insertSql += QStringLiteral(" WHERE ") + whereExp;
        insertSql += QStringLiteral(" AND NOT EXISTS (SELECT 1 FROM %1 WHERE %2)")
                .arg(local, wherePrimaryKeyExpression(base, local));

        if (!m_db.transaction()) {
            m_lastError = m_db.lastError().text();
            qWarning() << "ORM: не удалось начать транзакцию:" << m_lastError;
            return false;
        }

        if (!execStep(updateSql, "шаг 1 (UPDATE)") ||
            !execStep(insertSql, "шаг 2 (INSERT)")) {
            m_db.rollback();
            return false;
        }

        if (!m_db.commit()) {
            m_lastError = m_db.lastError().text();
            qWarning() << "ORM: не удалось зафиксировать транзакцию:" << m_lastError;
            m_db.rollback();
            return false;
        }

        return true;
    }

    QSqlDatabase m_db;
    QList<Assignment> m_setList;
    QList<Condition> m_conditionList;
    Overlay m_overlay;

    bool m_valid = true;
    QString m_lastError;
};

#endif // ORM_UPDATE_H
