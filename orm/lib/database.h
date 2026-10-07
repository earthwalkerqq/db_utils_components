#ifndef ORM_DATABASE_H
#define ORM_DATABASE_H

/*
 * @brief Соединение с БД и точка входа в построители запросов.
 * @details
 * QSqlDatabase нельзя использовать из потока, отличного
 * от того, где соединение было создано. Поэтому в многопоточном приложении
 * объект DataBase создаётся внутри рабочего потока (например, в лямбде
 * QtConcurrent::run), живёт на время задачи и там же уничтожается.
 * Имя соединения уникально (UUID + <id_потока>)
 */

#include <QDebug>
#include <QFile>
#include <QList>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QUuid>

#include "connection.h"
#include "field.h"
#include "log.h"
#include "table_macro.h"
#include "insert.h"
#include "select.h"
#include "update.h"

class DataBase
{
public:
    /*
     * target_main — основная БД. Она всегда открывается как главная схема
     * соединения, а главная схема в SQLite всегда называется "main" —
     * переименовать её нельзя, поэтому отдельного параметра для имени нет.
     */
    explicit DataBase(QString target_main) :
             m_target_main(std::move(target_main)) {}

    // соединение принадлежит объекту: копировать нельзя
    DataBase(const DataBase&) = delete;
    DataBase& operator=(const DataBase&) = delete;

    ~DataBase(void)
    {
        close();
    }

    static QString connectionName(void)
    {
        const quintptr threadId = reinterpret_cast<quintptr>(QThread::currentThreadId());
        const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        return QStringLiteral("orm_%1_%2").arg(threadId).arg(uuid);
    }

    bool connect(void)
    {
        if (m_db.isOpen()) {
            return true;
        }

        m_connectionName = connectionName();

        m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
        m_db.setDatabaseName(m_target_main);

        if (!m_db.open()) {
            m_lastError = m_db.lastError().text();
            qWarning().nospace() << "ORM: не удалось открыть БД " << m_target_main
                                 << ": " << m_lastError;
            m_db = QSqlDatabase();
            QSqlDatabase::removeDatabase(m_connectionName);
            m_connectionName.clear();
            return false;
        }
        return true;
    }

    /*
     * @details
     * Подключает пользовательскую БД к уже открытому соединению с основной.
     * После успешного вызова все SELECT строятся с наложением: запись берётся
     * из пользовательской БД, а если её там нет — из основной.
     *
     * Вызывать тольк после connect(). ATTACH живёт в рамках соединения, поэтому
     * в каждой задаче QtConcurrent, где создаётся свой DataBase, нужна пара
     * connect() + attach().
     */
    bool attach(const QString& target_user,
                const QString& alias = QStringLiteral("user"))
    {
        if (!m_db.isOpen()) {
            m_lastError = QStringLiteral("соединение не открыто: вызовите connect() перед attach()");
            ORM_ERROR_LOG(m_lastError);
            return false;
        }

        QSqlQuery query(m_db);
        query.prepare(QStringLiteral("ATTACH DATABASE :target AS :alias"));
        query.bindValue(QStringLiteral(":target"), target_user);
        query.bindValue(QStringLiteral(":alias"), alias);

        if (!query.exec()) {
            m_lastError = query.lastError().text();
            ORM_ERROR_LOG(m_lastError);
            return false;
        }

        m_target_user = target_user;
        m_overlay.local = alias;
        m_overlay.enabled = true;
        return true;
    }

    void close(void)
    {
        if (m_connectionName.isEmpty()) {
            return;
        }
        if (m_db.isOpen()) {
            m_db.close();
        }
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
        m_connectionName.clear();
        m_overlay.enabled = false;
    }

    bool isOpen(void) const { return m_db.isOpen(); }
    QString lastError(void) const { return m_lastError; }
    QSqlDatabase& getDB(void) { return m_db; }
    bool isOverlayEnabled(void) const { return m_overlay.enabled; }

    template<typename Table>
    SqlSelectQuery<Table> from(void) { return SqlSelectQuery<Table>(m_db, m_overlay); }

    template<typename Table>
    SqlInsertQuery<Table> into(void) { return SqlInsertQuery<Table>(m_db, m_overlay); }

    template<typename Table>
    SqlUpdateQuery<Table> update(void) { return SqlUpdateQuery<Table>(m_db, m_overlay); }

    QStringList tables(void) const { return m_db.tables(QSql::Tables); }

    /*
     * @brief Создать пустую БД со структурой другой БД.
     *
     * @param
     *   mainPath — БД-образец (обычно основная, поставляемая);
     *   userPath — файл, который нужно создать;
     *   tables   — какие таблицы перенести (остальные не нужны).
     *
     * @details
     * Текст CREATE TABLE берётся из sqlite_master образца, поэтому структура
     * переносится полностью: типы, NOT NULL, DEFAULT, составной PRIMARY KEY,
     * CHECK, COLLATE.
     *
     * @return true — файл создан и содержит все перечисленные таблицы.
     *         При ошибке незаконченный файл удаляется, текст в lastError().
     */
    bool cloneDataBase(const QString& mainPath, const QString& userPath,
                       const QStringList& tables)
    {
        m_lastError.clear();

        if (QFile::exists(userPath)) {
            m_lastError = QStringLiteral("файл %1 уже существует").arg(userPath);
            ORM_ERROR_LOG(m_lastError);
            return false;
        }

        bool ok = true;
        {
            orm::Connection connection(userPath);     // пустой файл создаст SQLite
            if (!connection.isOpen()) {
                m_lastError = QStringLiteral("не удалось создать %1").arg(userPath);
                return false;
            }
            QSqlDatabase db = connection.db();

            QSqlQuery attach(db);
            attach.prepare(QStringLiteral("ATTACH DATABASE :target AS base"));
            attach.bindValue(QStringLiteral(":target"), mainPath);
            if (!attach.exec()) {
                m_lastError = QStringLiteral("не подключить образец %1: %2")
                                  .arg(mainPath, attach.lastError().text());
                ORM_ERROR_LOG(m_lastError);
                ok = false;
            }

            if (ok && !db.transaction()) {
                m_lastError = QStringLiteral("не начать транзакцию: %1")
                                  .arg(db.lastError().text());
                ORM_ERROR_LOG(m_lastError);
                ok = false;
            }

            for (const QString& table : tables) {
                if (!ok) { break; }

                // 1. текст CREATE TABLE
                QSqlQuery select(db);
                select.prepare(QStringLiteral(
                    "SELECT sql FROM base.sqlite_master "
                    "WHERE type = 'table' AND name = :name"));
                select.bindValue(QStringLiteral(":name"), table);

                if (!select.exec() || !select.next()) {
                    m_lastError = QStringLiteral("в образце нет таблицы %1").arg(table);
                    ORM_ERROR_LOG(m_lastError);
                    ok = false;
                    break;
                }

                QSqlQuery create(db);
                if (!create.exec(select.value(0).toString())) {
                    m_lastError = QStringLiteral("не создать %1: %2")
                                      .arg(table, create.lastError().text());
                    ORM_ERROR_LOG(m_lastError);
                    ok = false;
                    break;
                }

                // 2. индексы таблицы; 
                // у автоиндексов PRIMARY KEY/UNIQUE sql = NULL
                QSqlQuery indexes(db);
                indexes.prepare(QStringLiteral(
                    "SELECT sql FROM base.sqlite_master "
                    "WHERE type = 'index' AND tbl_name = :name AND sql IS NOT NULL"));
                indexes.bindValue(QStringLiteral(":name"), table);

                if (indexes.exec()) {
                    while (indexes.next()) {
                        QSqlQuery createIndex(db);
                        if (!createIndex.exec(indexes.value(0).toString())) {
                            // индекс - это скорость, а не структура: не критично
                            const QString msg = QStringLiteral("индекс не создан: %1")
                                                    .arg(createIndex.lastError().text());
                            ORM_DEBUG_LOG(msg);
                        }
                    }
                }
            }

            if (ok) {
                ok = db.commit();
            } else {
                db.rollback();
            }

            QSqlQuery detach(db);
            detach.exec(QStringLiteral("DETACH DATABASE base"));
        }

        if (!ok) {
            QFile::remove(userPath);
        }
        return ok;
    }

    /*
     * @brief Перенести правки пользователя в новую пользовательскую БД.
     *
     * @details метод позволяет определять приоритет записей, при помощи
     * orm::MergePolicy, переданного в качестве параметра, можно брать за
     * приоритет раннии изменения пользователя или новые записи из поставки. 
     * В ходе обсуждений пришли к тому, что мы берем записи из поставки,
     * как приоритетные (если, опять же, не указано обратного через policy)
     * @param
     *   newMainPath — новая основная БД (из поставки), уже на рабочем месте;
     *   oldUserPath — прежняя пользовательская БД с правками;
     *   newUserPath — новая пользовательская БД, уже созданная cloneDataBase()
     *                 по структуре новой основной (пустая);
     *   tables      — таблицы с их колонками и первичным ключом;
     *   policy      — чья версия значения главнее
     *
     * все происходит внутри транзакции
     * @return true — все таблицы перенесены 
     */
    bool mergeUserDataBase(const QString& newMainPath, const QString& oldUserPath,
                           const QString& newUserPath,
                           const QList<orm::TableSchema>& tables,
                           orm::MergePolicy policy = orm::MergePolicy::SourceFirst)
    {
        m_lastError.clear();

        orm::Connection connection(newUserPath);      // новая пользовательская - главная схема
        if (!connection.isOpen()) {
            m_lastError = QStringLiteral("не открыть %1").arg(newUserPath);
            return false;
        }
        QSqlDatabase db = connection.db();

        if (!attachAs(db, newMainPath, QStringLiteral("base")) ||
            !attachAs(db, oldUserPath, QStringLiteral("user_old"))) {
            return false;
        }

        bool ok = db.transaction();
        if (!ok) {
            m_lastError = QStringLiteral("не начать транзакцию: %1")
                              .arg(db.lastError().text());
            ORM_ERROR_LOG(m_lastError);
        }

        for (const orm::TableSchema& table : tables) {
            if (!ok) { break; }

            QStringList columns;
            QStringList values;
            for (const QString& column : table.columns) {
                columns.append(QStringLiteral("\"%1\"").arg(quoted(column)));
                values.append(mergedValue(column, policy));
            }

            QStringList match;
            for (const QString& keyColumn : table.key) {
                match.append(QStringLiteral("TRIM(b.\"%1\") = TRIM(o.\"%1\")")
                                 .arg(quoted(keyColumn)));
            }

            QString sql;
            if (match.isEmpty()) {
                // ключа нет - сопоставлять нечем, переносим строки как есть
                const QString msg = QStringLiteral("У таблицы %1 нет ORM_PRIMARY_KEY - "
                                                   "правки перенесены без сверки с поставкой")
                                        .arg(table.name);
                ORM_ERROR_LOG(msg);
                sql = QStringLiteral("INSERT INTO main.\"%1\" (%2) SELECT %2 FROM user_old.\"%1\"")
                          .arg(quoted(table.name), columns.join(QStringLiteral(", ")));
            } else {
                sql = QStringLiteral("INSERT INTO main.\"%1\" (%2) SELECT %3 "
                                     "FROM user_old.\"%1\" AS o "
                                     "LEFT JOIN base.\"%1\" AS b ON %4")
                          .arg(quoted(table.name))
                          .arg(columns.join(QStringLiteral(", ")))
                          .arg(values.join(QStringLiteral(", ")))
                          .arg(match.join(QStringLiteral(" AND ")));
            }

            QSqlQuery insert(db);
            if (!insert.exec(sql)) {
                m_lastError = QStringLiteral("не перенести правки таблицы %1: %2")
                                  .arg(table.name, insert.lastError().text());
                ORM_ERROR_LOG(m_lastError);
                ok = false;
                break;
            }
        }

        if (ok) {
            ok = db.commit();
        } else {
            db.rollback();
        }

        QSqlQuery detach(db);
        detach.exec(QStringLiteral("DETACH DATABASE user_old"));
        detach.exec(QStringLiteral("DETACH DATABASE base"));
        return ok;
    }

private:
    bool attachAs(QSqlDatabase& db, const QString& target, const QString& alias)
    {
        QSqlQuery attach(db);
        attach.prepare(QStringLiteral("ATTACH DATABASE :target AS %1").arg(alias));
        attach.bindValue(QStringLiteral(":target"), target);
        if (!attach.exec()) {
            m_lastError = QStringLiteral("не подключить %1 как %2: %3")
                              .arg(target, alias, attach.lastError().text());
            ORM_ERROR_LOG(m_lastError);
            return false;
        }
        return true;
    }

    // COALESCE в порядке, который задаёт политика слияния
    static QString mergedValue(const QString& column, orm::MergePolicy policy)
    {
        const QString first = policy == orm::MergePolicy::SourceFirst
                                   ? QStringLiteral("b") : QStringLiteral("o");
        const QString second = policy == orm::MergePolicy::SourceFirst
                                   ? QStringLiteral("o") : QStringLiteral("b");
        return QStringLiteral("COALESCE(%1.\"%3\", %2.\"%3\")")
                   .arg(first, second, quoted(column));
    }

    // кавычки внутри имени удваиваются: "NAME""X"
    static QString quoted(const QString& name)
    {
        QString escaped = name;
        escaped.replace(QLatin1String("\""), QLatin1String("\"\""));
        return escaped;
    }

    QString m_connectionName;
    QString m_target_main;
    QString m_target_user;
    QString m_lastError;
    QSqlDatabase m_db;

    Overlay m_overlay;
};

#endif // ORM_DATABASE_H
