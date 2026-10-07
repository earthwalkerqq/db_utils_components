#ifndef ORM_CONNECTION_H
#define ORM_CONNECTION_H

/*
 * @brief Короткоживущее соединение с файлом БД.
 * @details
 * Открывается на одну операцию и закрывается в деструкторе: пока файл открыт,
 * его нельзя подменить или удалить, а снимок с него может оказаться неполным.
 * Имя соединения уникально — два соединения с одинаковым именем Qt хранить
 * не умеет, второе молча закрыло бы первое.
 *
 * В деструкторе важен порядок: close(), затем обнулить объект QSqlDatabase и
 * только потом removeDatabase(), иначе Qt пишет "connection is still in use".
 */

#include <QSqlDatabase>
#include <QSqlError>
#include <QString>
#include <QUuid>

#include "log.h"

namespace orm {

class Connection
{
public:
    explicit Connection(const QString& target)
    {
        /*
         * Имя строится здесь, а не берётся у DataBase: connection.h
         * включается раньше database.h, и класса DataBase ещё не видно.
         */
        m_name = QStringLiteral("orm_connection_%1")
                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
        m_db.setDatabaseName(target);
        if (!(m_open = m_db.open())) {
            const QString msg = QStringLiteral("Не удалось открыть БД %1: %2")
                                    .arg(target, m_db.lastError().text());
            ORM_ERROR_LOG(msg);
            m_db = QSqlDatabase();
            QSqlDatabase::removeDatabase(m_name);
            m_name.clear();
        }
    }

    ~Connection(void)
    {
        if (m_name.isEmpty()) {
            return;                 // соединение не открылось, удалять нечего
        }
        if (m_db.isOpen()) {
            m_db.close();
        }
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_name);
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    bool isOpen(void) const { return m_open; }
    QSqlDatabase& db(void) { return m_db; }

private:
    QString      m_name;
    QSqlDatabase m_db;
    bool         m_open = false;
};

} // namespace orm

#endif // ORM_CONNECTION_H
