#ifndef DB_MANAGER_LIB_MANAGER_H
#define DB_MANAGER_LIB_MANAGER_H

/*
 * @brief Одна база данных: пара файлов и её таблицы.
 *
 * @details
 * Внутренности модуля. Снаружи подключается только db_manager/db_manager.h.
 *
 * DBManager отвечает за файлы базы: копирование из поставки, проверку схемы,
 * создание и дополнение пользовательской БД, резервную копию и возврат из неё.
 */

#include <memory>
#include <utility>

#include <QDebug>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <QVariant>

#include "orm/orm.h"

#include "log.h"


namespace orm {

/*
 * Таблица -> база. Специализируется макросом ORM_DECLARE_DATABASE.
 * Если таблицу забыли приписать к базе, случится ошибка на compile time
 */
template<typename Table>
struct DatabaseOf;

} // namespace orm


namespace db {

class DBManager
{
public:
    /*
     * Описание таблицы берётся из ORM: имя, объявленные колонки, первичный
     * ключ. Тип один и тот же у ORM и у менеджера, поэтому список таблиц
     * можно передавать в DataBase::mergeUserDataBase() без преобразований.
     */
    using TableInfo = orm::TableSchema;

    DBManager(QString name, QString mainFile, QString userFile, QList<TableInfo> tables) :
        m_name(std::move(name)),
        m_main_file(std::move(mainFile)),
        m_user_file(std::move(userFile)),
        m_tables(std::move(tables)) {}

    // без копирования - один объект класса на базу
    DBManager(const DBManager&) = delete;
    DBManager& operator=(const DBManager&) = delete;

    // getters
    QString getName(void) const       { return m_name; }
    QString getTargetMain(void) const { return m_target_main; }
    QString getTargetUser(void) const { return m_target_user; }
    QString getBackupDir(void) const  { return m_backup_dir; }
    bool isReady(void) const          { return m_ready; }
    QString lastError(void) const     { return m_last_error; }

    QStringList tableNames(void) const
    {
        QStringList names;
        for (const TableInfo& table : m_tables) {
            names.append(table.name);
        }
        return names;
    }

    /*
     * @brief Каталог со снимками этой базы: <dataDir>/backup/<имя базы>/
     *
     * @param
     * dataDir - рабочая директория приложения
     * dbName - имя базы
     *
     * @return Путь к каталогу снимков - в случае успеха,
     * пустой QString - в случае неудачи
     */
    static QString backupRoot(const QString& dataDir, const QString& dbName)
    {
        const QString root = QDir(QDir(dataDir).filePath(QStringLiteral("backup")))
                                 .filePath(dbName);
        if (!QDir(root).exists()) {
            if (!QDir().mkpath(root)) {
                const QString msg = QStringLiteral("Не удалось создать каталог %1").arg(root);
                DB_ERROR_LOG(msg);
                return QString();
            }
        }
        return root;
    }

    /*
     * @brief Подготовка файлов базы. Вызывается ОДИН раз при старте приложения.
     *
     * @param
     *   sourceDir — каталог с поставляемой БД (у неё то же имя файла, что у
     *               рабочей копии); на Авроре это data/ внутри пакета;
     *   dataDir   — каталог приложения, куда можно писать.
     *
     * @details
     * Обновления здесь НЕ происходит: если в поставке лежит другая версия
     * справочника, рабочая копия остаётся прежней. Обновление - отдельное
     * действие, updateFromSource(), его вызывает приложение, когда решит.
     *
     * Порядок:
     *   1. основная БД. Если файла нет или он не подходит (нет объявленной
     *      таблицы или колонки, файл не открывается):
     *        1.1. берём из снимка backup/<имя базы>/;
     *        1.2. если снимка нет - берём из поставки.
     *   2. пользовательская БД. Если файла нет:
     *        2.1. берём из снимка (там правки пользователя, их стоит вернуть);
     *        2.2. если снимка нет - создаём пустую по структуре основной
     *             (DataBase::cloneDataBase).
     *      Существующую дополняем недостающими таблицами и колонками.
     */
    bool prepare(const QString& sourceDir, const QString& dataDir)
    {
        m_ready = false;
        m_last_error.clear();
        m_source_dir = sourceDir;
        m_data_dir = dataDir;

        // в проде за рабочую директорию будут выступать локальные файлы приложения
        if (!QDir(dataDir).exists() && !QDir().mkpath(dataDir)) {
            return fail(QStringLiteral("не удалось создать каталог %1").arg(dataDir));
        }

        // поставку не создаём: её кладёт установщик пакета
        if (!QDir(sourceDir).exists()) {
            return fail(QStringLiteral("директории источника %1 не существует").arg(sourceDir));
        }

        m_target_main = QDir(dataDir).filePath(m_main_file);
        m_target_user = m_user_file.isEmpty() ? QString()
                                              : QDir(dataDir).filePath(m_user_file);
        const QString source = QDir(sourceDir).filePath(m_main_file);

        /* ---------------------------------------------------- 1. основная БД */
        if (!isUsable(m_target_main)) {
            if (QFile::exists(m_target_main)) {
                /*
                 * Непригодный файл удаляем сразу: пустой файл, который SQLite
                 * создаёт при неудачном открытии, иначе навсегда заблокирует
                 * копирование - QFile::copy не перезаписывает существующий.
                 */
                const QString msg = QStringLiteral("Рабочая копия %1 непригодна, "
                                                   "восстанавливаем").arg(m_target_main);
                DB_ERROR_LOG(msg);
                if (!removeDbFiles(m_target_main)) {
                    return fail(QStringLiteral("не удалось удалить %1").arg(m_target_main));
                }
            }

            if (takeFromBackup(m_main_file, m_target_main, true)) {
                const QString msg = QStringLiteral("Основная БД восстановлена из снимка");
                DB_DEBUG_LOG(msg);
            } else {
                if (!QFile::exists(source)) {
                    return fail(QStringLiteral("исходный файл БД не найден: %1").arg(source));
                }
                if (!copyFile(m_target_main, source)) {
                    return fail(QStringLiteral("не удалось скопировать %1 в %2")
                                    .arg(source, m_target_main));
                }
                writeInstalledVersion(dataDir, sourceVersion(source));
                const QString msg = QStringLiteral("Основная БД взята из поставки");
                DB_DEBUG_LOG(msg);
            }
        }

        QFile::setPermissions(m_target_main, QFile::ReadOwner | QFile::WriteOwner);

        QString what;
        if (!hasDeclaredTables(m_target_main, &what)) {
            return fail(QStringLiteral("БД %1 не подходит: %2").arg(m_target_main, what));
        }

        /*
         * Маркера может не быть: рабочая копия осталась от прежней версии
         * программы или снимок вернули без него. Считаем, что в копии лежит
         * текущая версия поставки, иначе isUpdateAvailable() будет вечно
         * обещать обновление.
         */
        if (installedVersion(dataDir).isEmpty()) {
            writeInstalledVersion(dataDir, sourceVersion(source));
        }

        /* --------------------------------------------- 2. пользовательская БД */
        if (m_target_user.isEmpty()) {      // база без пользовательских правок
            m_ready = true;
            return true;
        }

        if (!QFile::exists(m_target_user)) {
            if (takeFromBackup(m_user_file, m_target_user, false)) {
                const QString msg = QStringLiteral("Пользовательская БД восстановлена из снимка");
                DB_DEBUG_LOG(msg);
            } else if (!createEmptyUser()) {
                return false;
            }
        }

        // дополняем недостающими таблицами и колонками (новое поле в ORM)
        if (!syncUserTables()) {
            return false;
        }
        QFile::setPermissions(m_target_user, QFile::ReadOwner | QFile::WriteOwner);

        m_ready = true;
        return true;
    }

    /*
     * @brief Обновить справочник из поставки, сохранив правки пользователя.
     *
     * @param policy — чья версия значения главнее, см. orm::MergePolicy.
     *                 По умолчанию SourceFirst: значение из новой поставки,
     *                 а из прежней правки берётся только то, что в поставке
     *                 NULL.
     *
     * @details
     * Вызывается приложением по явному действию, не при старте.
     *
     * Порядок:
     *   1. снимок текущих файлов (основной и пользовательской);
     *   2. новая основная из поставки встаёт на рабочее место;
     *      если она не подходит программе - возвращаем прежние файлы из снимка;
     *   3. рядом создаётся новая пустая пользовательская БД по структуре новой
     *      основной (DataBase::cloneDataBase);
     *   4. в неё переносятся правки из прежней пользовательской
     *      (DataBase::mergeUserDataBase);
     *   5. новая пользовательская подменяет прежнюю.
     *
     * Вызывать, когда с базой никто не работает: файлы подменяются, а открытое
     * соединение продолжило бы читать подменённый файл.
     */
    bool updateFromSource(orm::MergePolicy policy = orm::MergePolicy::SourceFirst)
    {
        if (m_data_dir.isEmpty() || m_source_dir.isEmpty()) {
            return fail(QStringLiteral("updateFromSource() до prepare()"));
        }

        const QString source = QDir(m_source_dir).filePath(m_main_file);
        if (!QFile::exists(source)) {
            return fail(QStringLiteral("исходный файл БД не найден: %1").arg(source));
        }

        /* --------------------------------------------------- 1. снимок */
        if (!makeBackup(m_data_dir)) {
            return fail(QStringLiteral("обновление отменено: не удалось сделать "
                                       "резервную копию"));
        }

        m_ready = false;

        /* ------------------------------------- 2. новая основная на место */
        if (!copyFile(m_target_main, source)) {
            restoreFromBackup();            // вернём прежние файлы
            m_ready = hasDeclaredTables(m_target_main);
            return fail(QStringLiteral("не удалось скопировать %1 в %2")
                            .arg(source, m_target_main));
        }
        QFile::setPermissions(m_target_main, QFile::ReadOwner | QFile::WriteOwner);

        QString what;
        if (!hasDeclaredTables(m_target_main, &what)) {
            /*
             * Поставка не подходит этой версии программы: в ней нет таблицы
             * или колонки, которую программа уже использует. Откатываемся на
             * прежние файлы - они в снимке.
             */
            const QString reason = what;
            restoreFromBackup();
            m_ready = hasDeclaredTables(m_target_main);
            return fail(QStringLiteral("новая БД не подходит (%1) - вернули прежнюю")
                            .arg(reason));
        }
        writeInstalledVersion(m_data_dir, sourceVersion(source));

        /* ------------------------------- 3-5. пользовательская БД заново */
        if (!m_target_user.isEmpty()) {
            const QString newUser = m_target_user + QStringLiteral(".new");
            removeDbFiles(newUser);

            DataBase db(m_target_main);     // своё соединение открывают сами методы
            if (!db.cloneDataBase(m_target_main, newUser, tableNames())) {
                return fail(db.lastError());
            }

            // прежняя пользовательская ещё на месте - из неё и переносим
            if (QFile::exists(m_target_user) &&
                !db.mergeUserDataBase(m_target_main, m_target_user, newUser, m_tables, policy)) {
                removeDbFiles(newUser);
                return fail(db.lastError());
            }

            if (!removeDbFiles(m_target_user)) {
                removeDbFiles(newUser);
                return fail(QStringLiteral("не удалось убрать %1").arg(m_target_user));
            }
            if (!QFile::rename(newUser, m_target_user)) {
                return fail(QStringLiteral("не удалось поставить на место %1")
                                .arg(m_target_user));
            }
            QFile::setPermissions(m_target_user, QFile::ReadOwner | QFile::WriteOwner);
        }

        m_ready = hasDeclaredTables(m_target_main);
        if (m_ready) {
            const QString msg = QStringLiteral("БД обновлена из поставки, прежние файлы - в %1")
                                    .arg(m_backup_dir);
            DB_DEBUG_LOG(msg);
        }
        return m_ready;
    }

    /*
     * @brief В поставке лежит версия, отличная от установленной?
     *
     * @details
     * Сравнивается не дата рабочей копии, а запомненная версия поставки:
     * после возврата из снимка рабочая копия всегда старше поставки.
     */
    bool isUpdateAvailable(void) const
    {
        if (m_data_dir.isEmpty() || m_source_dir.isEmpty()) {
            return false;
        }
        const QString source = QDir(m_source_dir).filePath(m_main_file);
        return QFile::exists(source) && isNewSource(source, m_data_dir);
    }

    /*
     * @brief Снимок обеих БД: основной и пользовательской.
     *
     * @details
     * Хранится один снимок: новый затирает предыдущий, но не раньше, чем
     * новый будет готов. Вызывается перед обновлением, можно вызвать и руками
     * (например, перед массовой правкой данных).
     */
    bool makeBackup(const QString& dataDir)
    {
        // нечего сохранять
        if ((m_target_main.isEmpty() || !QFile::exists(m_target_main)) &&
            (m_target_user.isEmpty() || !QFile::exists(m_target_user))) {
            return true;
        }

        const QString root = backupRoot(dataDir, m_name);
        if (root.isEmpty()) { return false; }

        /*
         * Испорченная основная БД в снимок не идёт: иначе она затёрла бы
         * прежний снимок, и возвращать стало бы нечего. Пользовательская
         * сохраняется всё равно - в ней правки.
         */
        if (!m_target_main.isEmpty() && QFile::exists(m_target_main)) {
            if (hasDeclaredTables(m_target_main)) {
                if (!snapshotFile(m_target_main, QDir(root).filePath(m_main_file))) {
                    return false;
                }
            } else {
                const QString msg = QStringLiteral("Основная БД испорчена - в снимок не идёт, "
                                                   "прежний снимок сохранён");
                DB_ERROR_LOG(msg);
            }
        }

        if (!m_target_user.isEmpty() && QFile::exists(m_target_user)
            && !snapshotFile(m_target_user, QDir(root).filePath(m_user_file))) {
            return false;
        }

        m_backup_dir = root;
        const QString msg = QStringLiteral("Резервная копия - %1").arg(root);
        DB_DEBUG_LOG(msg);
        return true;
    }

    /*
     * @brief Вернуть обе БД из снимка КОПИРОВАНИЕМ: снимок остаётся на месте.
     *
     * @details
     * Нужен для восстановления: рабочие файлы потерялись или испорчены.
     * Текущие файлы при этом затираются, поэтому для отката обновления
     * с возможностью вернуться обратно есть swapWithBackup().
     */
    bool restoreFromBackup(void)
    {
        if (m_data_dir.isEmpty()) {
            return fail(QStringLiteral("restoreFromBackup() до prepare()"));
        }

        const QString root = backupRoot(m_data_dir, m_name);
        if (root.isEmpty()) { return false; }

        const QString snapshotMain = QDir(root).filePath(m_main_file);
        if (!QFile::exists(snapshotMain)) {
            return fail(QStringLiteral("снимка нет: %1").arg(snapshotMain));
        }

        QString what;
        if (!hasDeclaredTables(snapshotMain, &what)) {
            return fail(QStringLiteral("снимок не подходит этой версии программы (%1) - "
                                       "восстановление отменено").arg(what));
        }

        m_ready = false;

        if (!copyFile(m_target_main, snapshotMain)) {
            return fail(QStringLiteral("не удалось вернуть %1").arg(m_main_file));
        }

        if (!m_target_user.isEmpty()) {
            const QString snapshotUser = QDir(root).filePath(m_user_file);
            if (QFile::exists(snapshotUser) && !copyFile(m_target_user, snapshotUser)) {
                return fail(QStringLiteral("не удалось вернуть %1").arg(m_user_file));
            }
            if (!syncUserTables()) {
                return false;
            }
        }

        m_ready = hasDeclaredTables(m_target_main);
        return m_ready;
    }

    /*
     * @brief Поменять рабочие файлы и снимок местами.
     *
     * @details
     * Откат обновления: прежние файлы из снимка встают на рабочее место, а
     * текущие уезжают в снимок. Повторный вызов возвращает всё как было,
     * поэтому откат можно откатить.
     *
     * Восстанавливаются обе базы сразу: справочник и правки из снимка -
     * согласованная пара, смешивать её с текущими файлами нельзя.
     *
     * Вызывать, когда с базой никто не работает.
     */
    bool swapWithBackup(void)
    {
        if (m_data_dir.isEmpty()) {
            return fail(QStringLiteral("swapWithBackup() до prepare()"));
        }

        const QString root = backupRoot(m_data_dir, m_name);
        if (root.isEmpty()) { return false; }

        const QString snapshotMain = QDir(root).filePath(m_main_file);
        if (!QFile::exists(snapshotMain)) {
            return fail(QStringLiteral("снимка нет: %1").arg(snapshotMain));
        }

        QString what;
        if (!hasDeclaredTables(snapshotMain, &what)) {
            /*
             * Снимок старше текущей версии программы: в нём нет таблицы или
             * колонки, которую программа уже использует. Вернуть его нельзя -
             * запросы перестанут работать.
             */
            return fail(QStringLiteral("снимок не подходит этой версии программы (%1) - "
                                       "откат отменён").arg(what));
        }

        m_ready = false;

        if (!swapFiles(m_target_main, snapshotMain)) {
            return fail(QStringLiteral("не удалось вернуть %1").arg(m_main_file));
        }

        if (!m_target_user.isEmpty()) {
            const QString snapshotUser = QDir(root).filePath(m_user_file);
            if (QFile::exists(snapshotUser) && !swapFiles(m_target_user, snapshotUser)) {
                return fail(QStringLiteral("не удалось вернуть %1").arg(m_user_file));
            }
        }

        /*
         * Рабочая копия теперь прежняя, а в поставке лежит новая. В маркер
         * пишем версию поставки: обновление уже «обработано» для этого
         * каталога, и isUpdateAvailable() не будет предлагать его снова,
         * пока не придёт следующая поставка.
         */
        const QString source = QDir(m_source_dir).filePath(m_main_file);
        writeInstalledVersion(m_data_dir, sourceVersion(source));

        if (!m_target_user.isEmpty() && !syncUserTables()) {
            return false;
        }

        m_ready = hasDeclaredTables(m_target_main);
        if (m_ready) {
            const QString msg = QStringLiteral("Снимок восстановлен, текущие файлы - в %1")
                                    .arg(root);
            DB_DEBUG_LOG(msg);
        }
        return m_ready;
    }

    /*
     * @brief Готовое соединение: основная БД открыта, пользовательская
     * подключена через attach.
     *
     * @details
     * QSqlDatabase нельзя передавать между потоками, поэтому объект
     * возвращается наружу и создаётся в том потоке, где будет работать
     * (например, внутри лямбды QtConcurrent::run).
     */
    std::unique_ptr<DataBase> open(void) const
    {
        if (!m_ready) {
            const QString msg = QStringLiteral("База %1 не подготовлена, вызовите prepare()")
                                    .arg(m_name);
            DB_ERROR_LOG(msg);
            return nullptr;
        }

        return connect();
    }

private:
    // файл есть и подходит объявленной в ORM схеме
    bool isUsable(const QString& target)
    {
        return QFile::exists(target) && hasDeclaredTables(target);
    }

    /*
     * @brief Взять файл из снимка на рабочее место.
     *
     * @param
     *   fileName  — имя файла внутри каталога снимков;
     *   target    — куда положить;
     *   checkSchema — проверять схему снимка (для основной БД обязательно;
     *                 у пользовательской недостающие колонки добавит
     *                 syncUserTables).
     */
    bool takeFromBackup(const QString& fileName, const QString& target, bool checkSchema)
    {
        if (m_data_dir.isEmpty()) { return false; }

        const QString root = backupRoot(m_data_dir, m_name);
        if (root.isEmpty()) { return false; }

        const QString snapshot = QDir(root).filePath(fileName);
        if (!QFile::exists(snapshot)) { return false; }

        if (checkSchema) {
            QString what;
            if (!hasDeclaredTables(snapshot, &what)) {
                const QString msg = QStringLiteral("Снимок %1 не подходит (%2) - пропускаем")
                                        .arg(snapshot, what);
                DB_ERROR_LOG(msg);
                return false;
            }
        }

        m_backup_dir = root;
        return copyFile(target, snapshot);
    }

    /*
     * @brief Создать пустую пользовательскую БД по структуре основной.
     *
     * @details
     * Структуру переносит ORM: DataBase::cloneDataBase() берёт готовый
     * CREATE TABLE из sqlite_master основной БД. Соединение объекту DataBase
     * здесь не нужно - метод открывает своё.
     */
    bool createEmptyUser(void)
    {
        DataBase db(m_target_main);
        if (!db.cloneDataBase(m_target_main, m_target_user, tableNames())) {
            return fail(db.lastError());
        }
        const QString msg = QStringLiteral("Создана пустая пользовательская БД %1")
                                .arg(m_target_user);
        DB_DEBUG_LOG(msg);
        return true;
    }

    std::unique_ptr<DataBase> connect(void) const
    {
        std::unique_ptr<DataBase> db(new DataBase(m_target_main));
        if (!db->connect()) {
            return nullptr;
        }
        if (!m_target_user.isEmpty() && !db->attach(m_target_user)) {
            return nullptr;
        }
        return db;
    }
    /*
     * @brief Консистентная копия одного файла БД.
     *
     * @details
     * VACUUM INTO переносит и незавершённый журнал (WAL), поэтому снимок
     * получается целым даже после аварийного завершения приложения.
     * Если SQLite старый и команду не знает или возникла другая ошибка,
     * просто копируем файл.
     *
     * Предыдущий снимок сначала уезжает в подкаталог tmp и возвращается
     * оттуда, если новый снимок сделать не удалось: иначе неудачная попытка
     * оставила бы пользователя вообще без резервной копии.
     */
    static bool snapshotFile(const QString& source, const QString& target)
    {
        QString msg;
        const bool targetExists = QFile::exists(target);

        if (targetExists && !moveTargetToTmp(target)) {
            return false;
        }

        {
            orm::Connection connection(source);
            if (connection.isOpen()) {
                QSqlQuery vacuum(connection.db());
                vacuum.prepare(QStringLiteral("VACUUM INTO :target"));
                vacuum.bindValue(QStringLiteral(":target"), target);
                if (vacuum.exec()) {
                    removeTmp(target);
                    return true;
                }
                msg = QStringLiteral("VACUUM INTO не сработал (%1). Этап простого копирования")
                          .arg(vacuum.lastError().text());
                DB_DEBUG_LOG(msg);
            }
        }

        if (!QFile::copy(source, target)) {
            msg = QStringLiteral("Не удалось скопировать %1 в %2").arg(source, target);
            DB_ERROR_LOG(msg);
            if (targetExists) { backTargetFromTmp(target); }
            return false;
        }

        // журналы исходника переносим вместе с файлом
        for (const QString& suffix : { QStringLiteral("-wal"), QStringLiteral("-shm") }) {
            if (QFile::exists(source + suffix)) {
                QFile::remove(target + suffix);
                QFile::copy(source + suffix, target + suffix);
            }
        }

        removeTmp(target);
        return true;
    }

    // <каталог файла>/tmp — временное место для предыдущей версии файла
    static QString tmpDirOf(const QString& target)
    {
        return QFileInfo(target).absoluteDir().filePath(QStringLiteral("tmp"));
    }

    static bool moveTargetToTmp(const QString& target)
    {
        const QString tmpDir = tmpDirOf(target);
        QString msg;

        if (!QDir(tmpDir).exists() && !QDir().mkpath(tmpDir)) {
            msg = QStringLiteral("Не удалось создать папку tmp в %1").arg(tmpDir);
            DB_ERROR_LOG(msg);
            return false;
        }

        const QString currentTarget = QDir(tmpDir).filePath(QFileInfo(target).fileName());
        if (QFile::exists(currentTarget)) {
            QFile::remove(currentTarget);
        }

        if (!QFile::rename(target, currentTarget)) {
            msg = QStringLiteral("Ошибка перемещения файла в локальный tmp: %1")
                      .arg(currentTarget);
            DB_ERROR_LOG(msg);
            return false;
        }
        return true;
    }

    static bool backTargetFromTmp(const QString& target)
    {
        const QString tmpDir = tmpDirOf(target);
        if (!QDir(tmpDir).exists()) { return true; }

        const QString currentTarget = QDir(tmpDir).filePath(QFileInfo(target).fileName());
        if (!QFile::exists(currentTarget)) { return true; }

        QFile::remove(target);
        if (!QFile::rename(currentTarget, target)) {
            const QString msg = QStringLiteral("Ошибка перемещения файла из tmp: %1")
                                    .arg(currentTarget);
            DB_ERROR_LOG(msg);
            return false;
        }
        return true;
    }

    static void removeTmp(const QString& target)
    {
        const QString tmpDir = tmpDirOf(target);
        if (QDir(tmpDir).exists()) {
            QDir(tmpDir).removeRecursively();
        }
    }

    /*
     * @brief Обмен двумя файлами БД через tmp внутри каталога снимка.
     *
     * @details
     * Три переименования: рабочий файл -> tmp, файл снимка -> рабочее место,
     * tmp -> каталог снимка. Переименование в пределах одного раздела
     * мгновенно, поэтому окно, когда файла нет на месте, минимально.
     * Журналы рабочего файла удаляются: SQLite применил бы их к подменённому
     * файлу и испортил его.
     */
    static bool swapFiles(const QString& working, const QString& snapshot)
    {
        const QString tmpDir = tmpDirOf(snapshot);
        if (!QDir(tmpDir).exists() && !QDir().mkpath(tmpDir)) {
            const QString msg = QStringLiteral("Не удалось создать папку tmp в %1").arg(tmpDir);
            DB_ERROR_LOG(msg);
            return false;
        }

        const QString parked = QDir(tmpDir).filePath(QFileInfo(snapshot).fileName());
        QFile::remove(parked);

        const bool workingExists = QFile::exists(working);
        if (workingExists) {
            removeJournals(working);
            if (!QFile::rename(working, parked)) {
                const QString msg = QStringLiteral("Не удалось убрать %1").arg(working);
                DB_ERROR_LOG(msg);
                return false;
            }
        }

        if (!QFile::rename(snapshot, working)) {
            const QString msg = QStringLiteral("Не удалось поставить снимок на место %1")
                                    .arg(working);
            DB_ERROR_LOG(msg);
            if (workingExists) { QFile::rename(parked, working); }   // откат
            return false;
        }

        if (workingExists && !QFile::rename(parked, snapshot)) {
            /*
             * Рабочий файл уже заменён, вернуть прежний в снимок не удалось.
             * Данные целы, но следующий обмен будет не с чем делать —
             * говорим об этом явно.
             */
            const QString msg = QStringLiteral("Снимок восстановлен, но прежний файл остался "
                                               "в %1").arg(parked);
            DB_ERROR_LOG(msg);
            return true;
        }

        removeTmp(snapshot);
        return true;
    }

    /*
     * @param
     * dest - куда копировать
     * source - источник (что копировать)
    */
    static bool copyFile(const QString& dest, const QString& source)
    {
        QString msg;
        if (QFile::exists(dest)) {
            QFile::remove(dest);
            removeJournals(dest);
        }

        if (!QFile::copy(source, dest)) {
            msg = QStringLiteral("БД %1 не удалось скопировать в %2")
                  .arg(source).arg(dest);
            DB_ERROR_LOG(msg);
            return false;
        }

        return true;
    }

    /*
    * @brief удаление временных служебных файлов SQLite
    */
    static void removeJournals(const QString& target)
    {
        for (const QString& suffix : { QStringLiteral("-wal"), QStringLiteral("-shm") }) {
            if (QFile::exists(target + suffix)) {
                QFile::remove(target + suffix);
            }
        }
    }

    /*
     * @brief удалить файл БД вместе с журналами
    */
    static bool removeDbFiles(const QString& target)
    {
        removeJournals(target);
        return !QFile::exists(target) || QFile::remove(target);
    }

    /*
     * @brief проверяет лежит ли в поставке версия, отличная от установленной
     *
     * @details
     * Сравнивается не дата рабочей копии, а запомненная версия поставки:
     * после restoreBackup() рабочая копия всегда старше поставки, и сравнение
     * по датам накатывало бы обновление при каждом запуске, отменяя
     * восстановление.
     */
    bool isNewSource(const QString& source, const QString& dataDir) const
    {
        const QString installed = installedVersion(dataDir);
        return !installed.isEmpty() && installed != sourceVersion(source);
    }

    QString versionMarkerPath(const QString& dataDir) const
    {
        return QDir(dataDir).filePath(m_main_file + QStringLiteral(".version"));
    }

    static QString sourceVersion(const QString& source)
    {
        const QDateTime changed = QFileInfo(source).lastModified();
        return changed.isValid() ? QString::number(changed.toMSecsSinceEpoch()) : QString();
    }

    QString installedVersion(const QString& dataDir) const
    {
        QFile marker(versionMarkerPath(dataDir));
        if (!marker.open(QFile::ReadOnly)) {
            return QString();
        }
        const QString value = QString::fromUtf8(marker.readAll()).trimmed();
        marker.close();
        return value;
    }

    void writeInstalledVersion(const QString& dataDir, const QString& value) const
    {
        QFile marker(versionMarkerPath(dataDir));
        if (!marker.open(QFile::WriteOnly | QFile::Truncate)) {
            const QString msg = QStringLiteral("Не удалось записать версию БД в %1")
                                    .arg(versionMarkerPath(dataDir));
            DB_ERROR_LOG(msg);
            return;
        }
        marker.write(value.toUtf8());
        marker.close();
    }

    bool fail(const QString& text)
    {
        m_last_error = text;
        DB_ERROR_LOG(text);
        return false;
    }

    /*
     * @brief
     * Проверка, что в БД есть все описанные в ORM таблицы и колонки.
     *
     * @details
     * Лишние колонки в базе не мешают - будут читаться только объявленные. А вот
     * отсутствие объявленной колонки ломает любой запрос к этой таблице.
     * Пустой или испорченный файл
     * проверку тоже не проходит: он не открывается или в нём нет таблиц.
     *
     * @param what — если не nullptr, туда пишется сообщение ошибки.
     */
    bool hasDeclaredTables(const QString& target, QString* what = nullptr)
    {
        orm::Connection connection(target);
        QString msg;
        if (!connection.isOpen()) {
            msg = QStringLiteral("файл не открывается: %1")
                  .arg(connection.db().lastError().text());
            fill_err(what, msg);
            return false;
        }

        const QStringList existing = connection.db().tables(QSql::Tables);
        // проверка, чтобы все таблицы по запросу из бд были у нас в scope
        for (const TableInfo& table : m_tables) {
            if (!existing.contains(table.name, Qt::CaseInsensitive)) {
                msg = QStringLiteral("нет таблицы %1").arg(table.name);
                fill_err(what, msg);
                return false;
            }

            // проверка, чтобы все колонки по запросу из бд были у нас в scope
            const QStringList columns = columnsOf(connection.db(), orm::OverlayDefault::base, table.name);
            for (const QString& column : table.columns) {
                if (!columns.contains(column, Qt::CaseInsensitive)) {
                    msg = QStringLiteral("в таблице %1 нет колонки %2")
                          .arg(table.name, column);
                    fill_err(what, msg);
                    return false;
                }
            }
        }
        return true;
    }

    static inline void fill_err(QString* what, const QString& msg)
    {
        if (!what) return;
        *what = msg;
    }

    /*
     * @details
     * Пользовательская БД открывается как основная схема соединения, а
     * поставляемая подключается к нему как base. Тогда CREATE TABLE,
     * взятый из base, создаёт таблицу именно в пользовательском файле.
     */
    bool syncUserTables(void)
    {
        orm::Connection connection(m_target_user);
        if (!connection.isOpen()) {
            return fail(QStringLiteral("не удалось создать пользовательскую БД %1: %2")
                            .arg(m_target_user, connection.db().lastError().text()));
        }

        QSqlQuery attach(connection.db());
        attach.prepare(QStringLiteral("ATTACH DATABASE :target AS base"));
        attach.bindValue(QStringLiteral(":target"), m_target_main);
        if (!attach.exec()) {
            return fail(QStringLiteral("Не удалось подключить основную БД - %1")
                            .arg(attach.lastError().text()));
        }

        const QStringList existing = connection.db().tables(QSql::Tables);
        bool ok = true;
        for (const TableInfo& table : m_tables) {
            const bool present = existing.contains(table.name, Qt::CaseInsensitive);
            if (!present) {
                ok = createFromBase(connection.db(), table.name);
            } else {
                ok = addMissingColumns(connection.db(), table);
            }
            if (!ok) { break; }
        }

        // DETACH до закрытия соединения: иначе Qt закрывает его с предупреждением
        QSqlQuery detach(connection.db());
        detach.exec(QStringLiteral("DETACH DATABASE base"));
        return ok;
    }

    bool createFromBase(QSqlDatabase& db, const QString& table)
    {
        QSqlQuery select(db);
        select.prepare(QStringLiteral(
            "SELECT sql FROM base.sqlite_master WHERE type = 'table' AND name = :name"));
        select.bindValue(QStringLiteral(":name"), table);

        if (!select.exec() || !select.next()) {
            return fail(QStringLiteral("в основной БД нет описания таблицы %1").arg(table));
        }

        const QString sql = select.value(0).toString();
        if (sql.isEmpty()) {
            return fail(QStringLiteral("пустое описание таблицы %1").arg(table));
        }

        QSqlQuery create(db);
        if (!create.exec(sql)) {
            return fail(QStringLiteral("не удалось создать таблицу %1: %2")
                            .arg(table, create.lastError().text()));
        }
        const QString msg = QStringLiteral("В пользовательской БД создана таблица %1").arg(table);
        DB_DEBUG_LOG(msg);
        return true;
    }

    /*
     * @details
     * Пришла новая версия основной БД с новыми колонками — добавляем их
     * в уже существующую пользовательскую таблицу. Иначе выборка из неё
     * упала бы на колонке, которой там нет.
     */
    bool addMissingColumns(QSqlDatabase& db, const TableInfo& table)
    {
        const QStringList userColumns = columnsOf(db, QStringLiteral("main"), table.name);
        if (userColumns.isEmpty()) {
            return fail(QStringLiteral("не удалось прочитать колонки таблицы %1").arg(table.name));
        }

        for (const QString& column : table.columns) {
            if (userColumns.contains(column, Qt::CaseInsensitive)) {
                continue;
            }

            const QString type = columnType(db, QStringLiteral("base"), table.name, column);
            if (type.isNull()) {
                const QString msg = QStringLiteral("Колонки %1.%2 нет и в основной БД - пропускаем")
                                        .arg(table.name, column);
                DB_ERROR_LOG(msg);
                continue;
            }

            QSqlQuery alter(db);
            const QString sql = QStringLiteral("ALTER TABLE \"%1\" ADD COLUMN \"%2\" %3")
                                    .arg(quoted(table.name), quoted(column), type);
            if (!alter.exec(sql)) {
                return fail(QStringLiteral("не удалось добавить колонку %1.%2: %3")
                                .arg(table.name, column, alter.lastError().text()));
            }
            const QString msg = QStringLiteral("В пользовательскую таблицу %1 добавлена колонка %2")
                                    .arg(table.name, column);
            DB_DEBUG_LOG(msg);

            if (!fillFromBase(db, table, column)) {
                return false;
            }
        }
        return true;
    }

    /*
     * Новая колонка у уже изменённых пользователем записей пустая: их копия
     * сделана тогда, когда колонки ещё не было. Заполняем её значением из
     * основной БД — пользователь эту колонку не менял, значит верное
     * значение там. Записи сопоставляются по первичному ключу, как и в
     * запросах ORM (TRIM — в справочнике встречаются хвостовые пробелы).
     */
    bool fillFromBase(QSqlDatabase& db, const TableInfo& table, const QString& column)
    {
        if (table.key.isEmpty()) {
            const QString msg = QStringLiteral("У таблицы %1 нет ORM_PRIMARY_KEY - колонка %2 "
                                               "останется пустой").arg(table.name, column);
            DB_ERROR_LOG(msg);
            return true;
        }

        QStringList match;
        for (const QString& keyColumn : table.key) {
            match.append(QStringLiteral("TRIM(base_row.\"%1\") = TRIM(\"%2\".\"%1\")")
                             .arg(quoted(keyColumn), quoted(table.name)));
        }

        const QString sql =
            QStringLiteral("UPDATE \"%1\" SET \"%2\" = ("
                           "SELECT base_row.\"%2\" FROM base.\"%1\" AS base_row WHERE %3)")
                .arg(quoted(table.name), quoted(column), match.join(QStringLiteral(" AND ")));

        QSqlQuery update(db);
        if (!update.exec(sql)) {
            return fail(QStringLiteral("не удалось заполнить колонку %1.%2: %3")
                            .arg(table.name, column, update.lastError().text()));
        }
        return true;
    }

    /*
     * @return Возвращает список названий всех таблиц из бд, в случае успеха
     * и пустой список, в случае ошибки запроса
    */
    static QStringList columnsOf(QSqlDatabase& db, const QString& schema, const QString& table)
    {
        QStringList columns;
        QSqlQuery info(db);
        // PRAGMA не принимает параметры, поэтому имя подставляется в текст;
        // имена таблиц приходят из макросов, а кавычки удваиваются.
        if (!info.exec(QStringLiteral("PRAGMA %1.table_info(\"%2\")")
                           .arg(schema, quoted(table)))) {
            QString msg = QStringLiteral("Ошибка при получении списка таблиц БД");
            DB_ERROR_LOG(msg);
            return columns;
        }
        while (info.next()) {
            // index 1 - name - имя колонки в бд
            columns.append(info.value(1).toString());
        }
        return columns;
    }

    // Тип колонки в поставляемой БД; QString() — такой колонки там нет.
    static QString columnType(QSqlDatabase& db, const QString& schema,
                              const QString& table, const QString& column)
    {
        QSqlQuery info(db);
        if (!info.exec(QStringLiteral("PRAGMA %1.table_info(\"%2\")")
                           .arg(schema, quoted(table)))) {
            return QString();
        }
        while (info.next()) {
            if (info.value(1).toString().compare(column, Qt::CaseInsensitive) == 0) {
                const QString type = info.value(2).toString();
                return type.isEmpty() ? QStringLiteral("TEXT") : type;
            }
        }
        return QString();
    }
    
    /* замена одинарной кавычки на двойную */
    static QString quoted(const QString& name)
    {
        QString escaped = name;
        escaped.replace(QLatin1String("\""), QLatin1String("\"\""));
        return escaped;
    }

    QString m_name;             // название бд
    QString m_main_file;        // название основного файла (readonly)
    QString m_user_file;        // название пользовательского файла
    QList<TableInfo> m_tables;  // список таблиц бд

    QString m_source_dir;       // директория с поставляемой бд
    QString m_data_dir;         // директория куда пишем
    QString m_target_main;      // m_data_dir + m_main_file
    QString m_target_user;      // m_data_dir + m_user_file
    QString m_last_error;
    QString m_backup_dir;       // Каталог со снимками этой базы: <dataDir>/backup/<dbName>/
    bool m_ready = false;
};

} // namespace db

#endif // DB_MANAGER_LIB_MANAGER_H
