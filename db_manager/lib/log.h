#ifndef DB_MANAGER_LOG_H
#define DB_MANAGER_LOG_H

/*
 * @brief Макросы журнала DBManager.
 * @details
 * Имена с префиксом DB_, чтобы не конфликтовать с ORM_ERROR_LOG из
 * orm/lib/log.h: одинаковые имена давали бы переопределение макроса, и какой
 * из них сработает, зависело бы от порядка включения файлов.
 */

#include <QDebug>

#define DB_ERROR_LOG(msg)                                                      \
    qWarning().nospace() << "[DB_ERROR:" << __FILE__ << ":" << __LINE__        \
                         << "]: " << msg

#define DB_DEBUG_LOG(msg)                                                      \
    qDebug().nospace() << "[DB_INFO]: " << msg

#endif // DB_MANAGER_LOG_H
