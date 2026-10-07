#ifndef DB_MANAGER_LIB_REGISTRY_H
#define DB_MANAGER_LIB_REGISTRY_H

/*
 * @brief Реестр всех объявленных баз и поиск базы по таблице.
 *
 * @details
 * Внутренности модуля. Снаружи подключается только db_manager/db_manager.h.
 *
 * В реестр базы попадают сами: ORM_DECLARE_DATABASE создаёт переменную,
 * которая вызывает DBRegistry::add() до входа в main. Поэтому отдельного
 * списка баз в проекте нет, а prepareAll() видит их все.
 */

#include <QString>
#include <QStringList>

#include "manager.h"
#include "log.h"


namespace db {

/* 
 * @brief Реестр всех объявленных баз.
 */
class DBRegistry
{
public:
    static DBRegistry& instance(void)
    {
        static DBRegistry registry;
        return registry;
    }

    // вызывается макросом ORM_DECLARE_DATABASE при старте программы
    bool add(DBManager* manager)
    {
        if (manager != nullptr && !m_list.contains(manager)) {
            m_list.append(manager);
        }
        return true;
    }

    /*
     * Подготовить все базы. Возвращает false, если хоть одна не готова,
     * но при этом пытается подготовить остальные: одна испорченная база
     * не должна мешать работе с другими.
     */
    bool prepareAll(const QString& sourceDir, const QString& dataDir)
    {
        QString msg;
        bool ok = true;
        for (DBManager* manager : m_list) {
            bool prepare_result = manager->prepare(sourceDir, dataDir);
            if (!prepare_result) {
                msg = QStringLiteral("Ошибка при подготовке базы %1").arg(manager->getName());
                DB_ERROR_LOG(msg);
            } else {
                msg = QStringLiteral("БД %1 готова к работе").arg(manager->getName());
                DB_DEBUG_LOG(msg);
            }
            ok = prepare_result && ok;
        }
        return ok;
    }

    /*
     * @brief Обновить справочники всех баз из поставки.
     *
     * @details
     * Вызывается по явному действию пользователя, не при старте. Как и
     * prepareAll, не прекращает работу после первой неудачи: одна база,
     * которую не удалось обновить, не должна мешать остальным.
     */
    bool updateAllFromSource(orm::MergePolicy policy = orm::MergePolicy::SourceFirst)
    {
        bool ok = true;
        for (DBManager* manager : m_list) {
            ok = manager->updateFromSource(policy) && ok;
        }
        return ok;
    }

    // есть ли хоть одна база, для которой в поставке лежит другая версия
    bool isUpdateAvailable(void) const
    {
        for (DBManager* manager : m_list) {
            if (manager->isUpdateAvailable()) { return true; }
        }
        return false;
    }

    QList<DBManager*> all(void) const { return m_list; }

    DBManager* byName(const QString& name) const
    {
        for (DBManager* manager : m_list) {
            if (manager->getName().compare(name, Qt::CaseInsensitive) == 0) {
                return manager;
            }
        }
        return nullptr;
    }

private:
    DBRegistry(void) = default;
    QList<DBManager*> m_list;
};


// Менеджер базы, в которой лежит таблица: db::dbFor<Ranway>().open()
template<typename Table>
DBManager& dbFor(void)
{
    using Db = typename orm::DatabaseOf<Table>::type;
    return Db::manager();
}

} // namespace db

#endif // DB_MANAGER_LIB_REGISTRY_H
