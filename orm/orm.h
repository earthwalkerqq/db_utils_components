#ifndef ORM_COMMON_H
#define ORM_COMMON_H

/*
 * Внутренности ORM. Снаружи подключается только этот файл
 *
 * Состав:
 *   field.h        Field, FieldMeta, Comparison, Condition
 *   table_macro.h  макросы для создания структур БД в compile time 
 *   select.h       class SqlSelectQuery
 *   insert.h       class SqlInsertQuery
 *   update.h       class SqlUpdateQuery
 *   database.h     class DataBase
 *   qml.h          toVariantMap / toVariantList / findField
 */

#include "connection.h"
#include "database.h"
#include "field.h"
#include "insert.h"
#include "qml.h"
#include "select.h"
#include "table_macro.h"
#include "update.h"

/*
 * @brief Описание таблиц базы данных.
 * @details Для уменьшения возможных ошибок при инициализации
 * структур таблиц БД, эта самая инициализация вынесена
 * в compile time. Определен контекст данных в виде 
 * (<тип_данных>, <имя_элемента_стурктуры>, "<ИМЯ_КОЛОНКИ_В_БД>")
 * и через набор X_MACRO определяется структура таблицы
 *
 * Из этой этого набора макрос ORM_DECLARE_TABLE порождает член структуры,
 * чтение колонки в fill(), константу поля (например AerodromFields::kod_icao)
 * и элемент кортежа all() для обобщённого обхода полей.
 *
 * Для добавления полей в существующую структуру, необходимо
 * добавить в ORM_FIELDS_XXX записи, как уже было описано
 * X(<тип_данных>, <имя_элемента_стурктуры>, "<ИМЯ_КОЛОНКИ_В_БД>")
 *
 * Для добавления новой таблицы необходимо описать ее поля в
 * ORM_FIELDS_XXX и передать это в макрос 
 * ORM_DECLARE_TABLE(<имя_объекта_структуры>, "<ИМЯ_ТАБЛИЦЫ_В_БД>", ORM_FIELDS_XXX)
 *
 * Первичный ключ задаётся строкой ниже — он нужен запросам с наложением
 * пользовательской БД на основную:
 * ORM_PRIMARY_KEY(<имя_объекта_структуры>, "<КОЛОНКА_1>", "<КОЛОНКА_2>", ...).
 */

/* AERODROM */
#define ORM_FIELDS_AERODROM(X)                                                 \
    X(QString, kod_icao,    "KOD_ICAO"   )                                     \
    X(QString, ident_icao,  "IDENT_ICAO" )                                     \
    X(double,  long_rad,    "LONG_RAD"   )                                     \
    X(double,  lat_rad,     "LAT_RAD"    )                                     \
    X(QString, name_aero,   "NAME_AERO"  )                                     \
    X(double,  lev_kta,     "LEV_KTA"    )                                     \
    X(QString, aero_nazn,   "AERO_NAZN"  )                                     \
    X(QString, code_iata,   "CODE_IATA"  )                                     \
    X(double,  mag_var_rad, "MAG_VAR_RAD")

ORM_DECLARE_TABLE(Aerodrom, "AERODROM", ORM_FIELDS_AERODROM)
ORM_PRIMARY_KEY(Aerodrom, "KOD_ICAO")

/* RANWAY */
#define ORM_FIELDS_RANWAY(X)                                                   \
    X(QString, kod_icao,    "KOD_ICAO"   )                                     \
    X(QString, num_vpp,     "NUM_VPP"    )                                     \
    X(double,  rw_true_rad, "RW_TRUE_RAD")                                     \
    X(double,  rwy_grad,    "RWY_GRAD"   )                                     \
    X(int,     resa,        "RESA"       )                                     \
    X(int,     tora,        "TORA"       )                                     \
    X(int,     toda,        "TODA"       )                                     \
    X(int,     asda,        "ASDA"       )                                     \
    X(int,     lda,         "LDA"        )                                     \
    X(int,     dsplcd_thr,  "DSPLCD_THR" )                                     \
    X(int,     rw_length,   "RW_LENGTH"  )                                     \
    X(int,     rw_width,    "RW_WIDTH"   )                                     \
    X(QString, pokr_vpp,    "POKR_VPP"   )                                     \
    X(int,     gruz_vpp_t,  "GRUZ_VPP_T" )

ORM_DECLARE_TABLE(Ranway, "RANWAY", ORM_FIELDS_RANWAY)
ORM_PRIMARY_KEY(Ranway, "KOD_ICAO", "NUM_VPP")

#define ORM_FIELDS_PROC_LEG(X)                          \
    X(QString, kod_icao,     "KOD_ICAO"    )            \
    X(QString, route_ident,  "ROUTE_IDENT" )            \
    X(QString, rt_type,      "RT_TYPE"     )            \
    X(QString, trans_ident,  "TRANS_IDENT" )            \
    X(int,     seq_nr,       "SEQ_NR"      )            \
    X(QString, path_term,    "PATH_TERM"   )            \
    X(QString, fix_ident,    "FIX_IDENT"   )            \
    X(QString, type_point,   "TYPE_POINT"  )            \
    X(QString, desc_code,    "DESC_CODE"   )            \
    X(QString, desc_40,      "DESC_CODE_40")            \
    X(QString, desc_41,      "DESC_CODE_41")            \
    X(QString, desc_42,      "DESC_CODE_42")            \
    X(QString, desc_43,      "DESC_CODE_43")            \
    X(double,  lat_rad_pm,   "LAT_RAD_PM"  )            \
    X(double,  long_rad_pm,  "LONG_RAD_PM" )            \
    X(double,  ob_mag_crs,   "OB_MAG_CRS"  )            \
    X(QString, turn_dir,     "TURN_DIR"    )            \
    X(double,  lat_rad_ckt,  "LAT_RAD_CKT" )            \
    X(double,  long_rad_ckt, "LONG_RAD_CKT")            \
    X(double,  arc_rad,      "ARC_RAD"     )            \
    X(QString, alt_desc,     "ALT_DESC"    )            \
    X(int,     h1,           "H1"          )            \
    X(int,     h2,           "H2"          )            \
    X(int,     speed_limit,  "SPEED_LIMIT" )            \
    X(QString, recd_nav,     "RECD_NAV"    )            \
    X(double,  rho,          "RHO"         )            \
    X(QString, dist_or_time, "DIST_OR_TIME")            \
    X(double,  lat_rad_nav,  "LAT_RAD_RECD_NAV" )       \
    X(double,  long_rad_nav, "LONG_RAD_RECD_NAV")       \
    X(double,  theta,        "THETA"       )            \
    X(double,  rte_dist,     "RTE_DIST_FROM")           \
    X(int,     end_trass,    "END_TRASS"   )

ORM_DECLARE_TABLE(PD_VIHOD,  "PD_VIHOD", ORM_FIELDS_PROC_LEG)
ORM_DECLARE_TABLE(PE_VHOD, "PE_VHOD",  ORM_FIELDS_PROC_LEG)
ORM_DECLARE_TABLE(PF_POSADKA, "PF_POSADKA", ORM_FIELDS_PROC_LEG)

#define ORM_FIELDS_KURS_POS(X)                          \
    X(QString, kod_icao,     "KOD_ICAO"    )            \
    X(QString, num_vpp,      "NUM_VPP"     )            \
    X(double,  lat_rad,      "LAT_RAD"     )            \
    X(double,  long_rad,     "LONG_RAD"    )            \
    X(double,  rw_mag,       "RW_MAG"      )            \
    X(double,  thres_elev,   "THRES_ELEV"  )

ORM_DECLARE_TABLE(KursPos, "KURS_POS", ORM_FIELDS_KURS_POS)

// Для работы с mbtiles
using Blob = QByteArray;

#define ORM_FIELDS_METADATA(X)                                                 \
    X(QString, name,        "name"       )                                     \
    X(QString, value,       "value"      )                                     \

ORM_DECLARE_TABLE(Metadata, "metadata", ORM_FIELDS_METADATA)
ORM_PRIMARY_KEY(Metadata, "name")

#define ORM_FIELDS_TILES(X)                                                    \
    X(int,     zoom_level,  "zoom_level" )                                     \
    X(int,     tile_column, "tile_column")                                     \
    X(int,     tile_row,    "tile_row"   )                                     \
    X(Blob,    tile_data,   "tile_data"  )                                     \

ORM_DECLARE_TABLE(Tiles, "tiles", ORM_FIELDS_TILES)
ORM_PRIMARY_KEY(Tiles, "zoom_level", "tile_column", "tile_row")

#endif // ORM_COMMON_H
