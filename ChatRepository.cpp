#include "ChatRepository.h"
#include "Utf16.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include "DbHelpers.h"

using json = nlohmann::json;
static bool is_valid_attachment_url(const std::string& url)
{
    static const std::string prefix = "/uploads/chat/";
    if (url.size() != prefix.size() + 16 + 4 && url.size() != prefix.size() + 16 + 5)
        return false;
    if (url.compare(0, prefix.size(), prefix) != 0)
        return false;

    std::string name = url.substr(prefix.size());
    for (size_t i = 0; i < 16; ++i)
        if (!std::isxdigit((unsigned char)name[i]) || std::isupper((unsigned char)name[i]))
            return false;

    std::string ext = name.substr(16);
    return ext == ".jpg" || ext == ".png" || ext == ".webp" || ext == ".pdf";
}

static void print_odbc_error(SQLSMALLINT handle_type, SQLHANDLE handle)
{
    SQLCHAR sqlstate[6];
    SQLCHAR message[SQL_MAX_MESSAGE_LENGTH];
    SQLINTEGER native_error;
    SQLSMALLINT message_len;
    SQLSMALLINT i = 1;

    while (SQLGetDiagRecA(handle_type, handle, i, sqlstate, &native_error,
        message, sizeof(message), &message_len) == SQL_SUCCESS)
    {
        std::cerr << "ODBC Error [" << sqlstate << "] " << message << std::endl;
        i++;
    }
}

std::optional<SavedMessage> ChatRepository::save(int sender_id, int recipient_id,
    const std::string& text, const std::string& attachment_url,long long reply_to, const std::string& attachment_name)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();

    SQLHSTMT stmt = nullptr;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

    const char* query =
        "INSERT INTO Messages (sender_id, recipient_id, body, attachment_url, reply_to, attachment_name) "
        "OUTPUT INSERTED.id, CONVERT(NVARCHAR(30), INSERTED.created_at, 126) "
        "VALUES (?, ?, ?, ?,?,?)";
    SQLPrepareA(stmt, (SQLCHAR*)query, SQL_NTS);

    SQLLEN i1 = 0, i2 = 0;
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &sender_id, 0, &i1);
    SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &recipient_id, 0, &i2);

    std::wstring wtext = utf8_to_wide(text);
    SQLLEN body_ind = SQL_NTS;
    SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR,
        (std::max)((SQLULEN)1, (SQLULEN)wtext.size()), 0, (SQLPOINTER)wtext.c_str(), 0, &body_ind);

    // пустой attachment_url -> NULL
    std::wstring wurl = utf8_to_wide(attachment_url);
    SQLLEN url_ind = attachment_url.empty() ? SQL_NULL_DATA : SQL_NTS;
    SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR,
        200, 0, (SQLPOINTER)wurl.c_str(), 0, &url_ind);
    SQLLEN reply_ind = reply_to > 0 ? 0 : SQL_NULL_DATA;
    SQLBindParameter(stmt, 5, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT, 0, 0, &reply_to, 0, &reply_ind);
    std::wstring wattname = utf8_to_wide(attachment_name);
    SQLLEN attname_ind = attachment_name.empty() ? SQL_NULL_DATA : SQL_NTS;
    SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR,
        200, 0, (SQLPOINTER)wattname.c_str(), 0, &attname_ind);

    if (!SQL_SUCCEEDED(SQLExecute(stmt)))
    {
        print_odbc_error(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK);
        return std::nullopt;
    }

    SQLBIGINT id = 0;
    SQLCHAR ts[32] = {};
    SQLLEN id_ind = 0, ts_ind = 0;
    SQLBindCol(stmt, 1, SQL_C_SBIGINT, &id, 0, &id_ind);
    SQLBindCol(stmt, 2, SQL_C_CHAR, ts, sizeof(ts), &ts_ind);

    std::optional<SavedMessage> out;
    if (SQL_SUCCEEDED(SQLFetch(stmt)))
        out = SavedMessage{ (long long)id, std::string((char*)ts) };

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    return out;
}

std::string ChatRepository::history(int me, int other, long long before_id, int limit)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();

    SQLHSTMT stmt = nullptr;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

    const char* query =
        "SELECT TOP (?) m.id, u.login, m.body, CONVERT(NVARCHAR(30), m.created_at, 126), "
        "m.attachment_url,m.attachment_name, "
        "CASE WHEN m.edited_at  IS NULL THEN 0 ELSE 1 END, "
        "CASE WHEN m.deleted_at IS NULL THEN 0 ELSE 1 END, "
        "m.reply_to "
        "FROM Messages m JOIN Users u ON u.id = m.sender_id "
        "WHERE ((m.sender_id = ? AND m.recipient_id = ?) OR (m.sender_id = ? AND m.recipient_id = ?)) "
        "AND (? = 0 OR m.id < ?) "
        "ORDER BY m.id DESC";
    SQLPrepareA(stmt, (SQLCHAR*)query, SQL_NTS);

    SQLLEN ind[7] = {};
    auto bind_int = [&](int idx, int& v) {
        SQLBindParameter(stmt, idx, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &v, 0, &ind[idx - 1]);
        };
    bind_int(1, limit);
    bind_int(2, me);  bind_int(3, other);
    bind_int(4, other); bind_int(5, me);
    SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT, 0, 0, &before_id, 0, &ind[5]);
    SQLBindParameter(stmt, 7, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT, 0, 0, &before_id, 0, &ind[6]);


    json items = json::array();

    if (SQL_SUCCEEDED(SQLExecute(stmt)))
    {
        SQLBIGINT id = 0;
        SQLCHAR login[64] = {}, ts[32] = {};
        SQLWCHAR body[2049] = {}, att[201] = {};
        SQLINTEGER edited = 0, deleted = 0;
        SQLBIGINT reply_to = 0;
        SQLLEN id_ind = 0, login_ind = 0, body_ind = 0, ts_ind = 0;
        SQLLEN att_ind = 0, edited_ind = 0, deleted_ind = 0, reply_ind = 0;

        SQLBindCol(stmt, 1, SQL_C_SBIGINT, &id, 0, &id_ind);
        SQLBindCol(stmt, 2, SQL_C_CHAR, login, sizeof(login), &login_ind);
        SQLBindCol(stmt, 3, SQL_C_WCHAR, body, sizeof(body), &body_ind);
        SQLBindCol(stmt, 4, SQL_C_CHAR, ts, sizeof(ts), &ts_ind);
        SQLBindCol(stmt, 5, SQL_C_WCHAR, att, sizeof(att), &att_ind);
        SQLBindCol(stmt, 6, SQL_C_LONG, &edited, 0, &edited_ind);
        SQLBindCol(stmt, 7, SQL_C_LONG, &deleted, 0, &deleted_ind);
        SQLBindCol(stmt, 8, SQL_C_SBIGINT, &reply_to, 0, &reply_ind);
        SQLWCHAR att_name[201] = {};
        SQLLEN att_name_i = 0;
        SQLBindCol(stmt, 9, SQL_C_WCHAR, att_name, sizeof(att_name), &att_name_i);
        while (SQL_SUCCEEDED(SQLFetch(stmt)))
        {
            json item = {
                {"id", (long long)id},
                {"from", std::string((char*)login)},
                {"text", wide_to_utf8((const wchar_t*)body, (int)(body_ind / sizeof(wchar_t)))},
                {"ts", std::string((char*)ts)}
            };

            if (att_ind != SQL_NULL_DATA && att_ind > 0)
                item["attachment"] = wide_to_utf8((const wchar_t*)att, (int)(att_ind / sizeof(wchar_t)));
            if (att_name_i > 0) item["attachment_name"] = wide_to_utf8((const wchar_t*)att_name, (int)(att_name_i / sizeof(wchar_t)));
            if (edited)  item["edited"] = true;
            if (deleted) item["deleted"] = true;
            if (reply_ind != SQL_NULL_DATA && reply_to > 0)
                item["reply_to"] = (long long)reply_to;

            items.push_back(std::move(item));
        }
    }
    else
    {
        print_odbc_error(SQL_HANDLE_STMT, stmt);
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);

    json out = { {"items", items} };
    return out.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string ChatRepository::сreate_group(int owner_id, std::string& group_name, std::vector<std::string>& members, const std::string& kind)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();

    // Единая точка выхода при ошибке: откат всей транзакции + JSON с ошибкой
    auto fail = [&](SQLHSTMT stmt, const char* msg) -> std::string
        {
            if (stmt)
            {
                print_odbc_error(SQL_HANDLE_STMT, stmt);
                SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            }
            SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK);
            json err = { {"error", msg} };
            return err.dump(-1, ' ', false, json::error_handler_t::replace);
        };

    // ---------- 1. Создаём группу ----------
    SQLHSTMT stmt = nullptr;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

    const char* q_group =
        "INSERT INTO ChatGroups (name, owner_id, kind) "
        "OUTPUT INSERTED.id "
        "VALUES (?, ?, ?)";
    SQLPrepareA(stmt, (SQLCHAR*)q_group, SQL_NTS);

    std::wstring wname = utf8_to_wide(group_name);
    std::wstring wkind = utf8_to_wide(kind);
    SQLLEN name_ind = SQL_NTS, owner_ind = 0, kind_ind = SQL_NTS;
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR,
        (std::max)((SQLULEN)1, (SQLULEN)wname.size()), 0, (SQLPOINTER)wname.c_str(), 0, &name_ind);
    SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &owner_id, 0, &owner_ind);
    SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR,
        (std::max)((SQLULEN)1, (SQLULEN)wkind.size()), 0, (SQLPOINTER)wkind.c_str(), 0, &kind_ind);

    if (!SQL_SUCCEEDED(SQLExecute(stmt)))
        return fail(stmt, "create group failed");

    SQLINTEGER group_id = 0;
    SQLLEN gid_ind = 0;
    SQLBindCol(stmt, 1, SQL_C_SLONG, &group_id, 0, &gid_ind);
    if (!SQL_SUCCEEDED(SQLFetch(stmt)))
        return fail(stmt, "no group id returned");

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    stmt = nullptr;

    // ---------- 2. Владелец -> GroupMembers с ролью 'owner' ----------
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

    const char* q_owner =
        "INSERT INTO GroupMembers (group_id, user_id, role, joined_at) "
        "VALUES (?, ?, 'owner', SYSUTCDATETIME())";
    SQLPrepareA(stmt, (SQLCHAR*)q_owner, SQL_NTS);

    SQLLEN o1 = 0, o2 = 0;
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &group_id, 0, &o1);
    SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &owner_id, 0, &o2);

    if (!SQL_SUCCEEDED(SQLExecute(stmt)))
        return fail(stmt, "add owner failed");

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    stmt = nullptr;

    // ---------- 3. Остальные участники по логину ----------
    json added = json::array();
    json skipped = json::array();

    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

    // Вставляем только если пользователь существует, это не владелец
    // и его ещё нет в группе (защита от дублей в списке)
    const char* q_member =
        "INSERT INTO GroupMembers (group_id, user_id, role, joined_at) "
        "SELECT ?, u.id, 'customer', SYSUTCDATETIME() "
        "FROM Users u "
        "WHERE u.login = ? AND u.id <> ? "
        "AND NOT EXISTS (SELECT 1 FROM GroupMembers gm WHERE gm.group_id = ? AND gm.user_id = u.id)";
    SQLPrepareA(stmt, (SQLCHAR*)q_member, SQL_NTS);

    for (const std::string& login : members)
    {
        std::wstring wlogin = utf8_to_wide(login);

        SQLLEN m_ind[4] = {};
        SQLLEN login_ind = SQL_NTS;
        SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &group_id, 0, &m_ind[0]);
        SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR,
            (std::max)((SQLULEN)1, (SQLULEN)wlogin.size()), 0, (SQLPOINTER)wlogin.c_str(), 0, &login_ind);
        SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &owner_id, 0, &m_ind[2]);
        SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &group_id, 0, &m_ind[3]);

        if (!SQL_SUCCEEDED(SQLExecute(stmt)))
            return fail(stmt, "add member failed");

        SQLLEN rows = 0;
        SQLRowCount(stmt, &rows);
        if (rows > 0) added.push_back(login);
        else          skipped.push_back(login);   // не найден / владелец / дубль

        SQLCloseCursor(stmt);
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);

    json out = {
        {"group_id", (int)group_id},
        {"name", group_name},
        {"kind", kind},
        {"added", added},
        {"skipped", skipped}
    };
    return out.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string ChatRepository::sync_direct(int me, long long after_id, const std::string& since, int limit)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();

    SQLHSTMT stmt = nullptr;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

    const char* query =
        "SELECT TOP (100) m.id, u.login, m.body, CONVERT(NVARCHAR(30), m.created_at, 126), "
        "m.attachment_url,m.attachment_name, "
        "CASE WHEN m.edited_at  IS NULL THEN 0 ELSE 1 END, "
        "CASE WHEN m.deleted_at IS NULL THEN 0 ELSE 1 END, "
        "m.reply_to "
        "FROM Messages m JOIN Users u ON u.id = m.sender_id "
        "WHERE m.recipient_id = ? AND (m.id > ? OR m.updated_at >= CONVERT(DATETIME2, ?, 126)) "
        "ORDER BY m.id ASC";
    std::wstring wsince = utf8_to_wide(since.empty() ? "9999-12-31T00:00:00" : since);
    SQLPrepareA(stmt, (SQLCHAR*)query, SQL_NTS);

    SQLLEN ind[4] = {};
    ind[3] = SQL_NTS;
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &limit, 0, &ind[0]);
    SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &me, 0, &ind[1]);
    SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT, 0, 0, &after_id, 0, &ind[2]);
    SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR,
        (std::max)((SQLULEN)1, (SQLULEN)wsince.size()), 0, (SQLPOINTER)wsince.c_str(), 0, &ind[3]);

    json items = json::array();

    if (SQL_SUCCEEDED(SQLExecute(stmt)))
    {
        SQLBIGINT id = 0;
        SQLCHAR login[64] = {}, ts[32] = {};
        SQLWCHAR body[2049] = {}, att[201] = {};
        SQLINTEGER edited = 0, deleted = 0;
        SQLBIGINT reply_to = 0;
        SQLLEN id_ind = 0, login_ind = 0, body_ind = 0, ts_ind = 0;
        SQLLEN att_ind = 0, edited_ind = 0, deleted_ind = 0, reply_ind = 0;

        SQLBindCol(stmt, 1, SQL_C_SBIGINT, &id, 0, &id_ind);
        SQLBindCol(stmt, 2, SQL_C_CHAR, login, sizeof(login), &login_ind);
        SQLBindCol(stmt, 3, SQL_C_WCHAR, body, sizeof(body), &body_ind);
        SQLBindCol(stmt, 4, SQL_C_CHAR, ts, sizeof(ts), &ts_ind);
        SQLBindCol(stmt, 5, SQL_C_WCHAR, att, sizeof(att), &att_ind);
        SQLBindCol(stmt, 6, SQL_C_LONG, &edited, 0, &edited_ind);
        SQLBindCol(stmt, 7, SQL_C_LONG, &deleted, 0, &deleted_ind);
        SQLBindCol(stmt, 8, SQL_C_SBIGINT, &reply_to, 0, &reply_ind);
        SQLWCHAR att_name[201] = {};
        SQLLEN att_name_i = 0;
        SQLBindCol(stmt, 9, SQL_C_WCHAR, att_name, sizeof(att_name), &att_name_i);

        while (SQL_SUCCEEDED(SQLFetch(stmt)))
        {
            json item = {
                {"id", (long long)id},
                {"from", std::string((char*)login)},
                {"text", wide_to_utf8((const wchar_t*)body, (int)(body_ind / sizeof(wchar_t)))},
                {"ts", std::string((char*)ts)}
            };

            if (att_ind != SQL_NULL_DATA && att_ind > 0)
                item["attachment"] = wide_to_utf8((const wchar_t*)att, (int)(att_ind / sizeof(wchar_t)));
            if (att_name_i > 0) item["attachment_name"] = wide_to_utf8((const wchar_t*)att_name, (int)(att_name_i / sizeof(wchar_t)));
            if (deleted) item["deleted"] = true;
            if (reply_ind != SQL_NULL_DATA && reply_to > 0)
                item["reply_to"] = (long long)reply_to;

            items.push_back(std::move(item));
        }
    }
    else
    {
        print_odbc_error(SQL_HANDLE_STMT, stmt);
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);

    return items.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::optional<bool> ChatRepository::attachment_in_use(const std::string& url)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();

    SQLHSTMT stmt = nullptr;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

    const char* query =
        "SELECT CASE WHEN EXISTS (SELECT 1 FROM Messages WHERE attachment_url = ?) "
        "OR EXISTS (SELECT 1 FROM GroupMessages WHERE attachment_url = ?) "
        "THEN 1 ELSE 0 END";
    SQLPrepareA(stmt, (SQLCHAR*)query, SQL_NTS);

    std::wstring wurl = utf8_to_wide(url);
    SQLLEN ind[2] = { SQL_NTS, SQL_NTS };
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR,
        (std::max)((SQLULEN)1, (SQLULEN)wurl.size()), 0, (SQLPOINTER)wurl.c_str(), 0, &ind[0]);
    SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR,
        (std::max)((SQLULEN)1, (SQLULEN)wurl.size()), 0, (SQLPOINTER)wurl.c_str(), 0, &ind[1]);

    std::optional<bool> result;   // при ошибке БД остаётся пустым

    if (SQL_SUCCEEDED(SQLExecute(stmt)))
    {
        SQLINTEGER used = 0;
        SQLLEN used_ind = 0;
        SQLBindCol(stmt, 1, SQL_C_LONG, &used, 0, &used_ind);
        if (SQL_SUCCEEDED(SQLFetch(stmt)))
            result = (used != 0);
    }
    else
    {
        print_odbc_error(SQL_HANDLE_STMT, stmt);
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    return result;
}

long ChatRepository::edit_direct(int sender_id, int recipient_id, long long id, const std::string& text)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();

    // править можно только своё и только неудалённое
    long rows = exec_update(dbc,
        "UPDATE Messages SET body = ?, edited_at = SYSUTCDATETIME(),updated_at = SYSUTCDATETIME() "
        "WHERE id = ? AND sender_id = ? AND recipient_id = ? AND deleted_at IS NULL",
        { DbParam::Text(text), DbParam::BigInt(id), DbParam::Int(sender_id), DbParam::Int(recipient_id) });

    SQLEndTran(SQL_HANDLE_DBC, dbc, rows < 0 ? SQL_ROLLBACK : SQL_COMMIT);
    return rows;
}

long ChatRepository::delete_direct(int sender_id, int recipient_id, long long id)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();

    long rows = exec_update(dbc,
        "UPDATE Messages SET body = N'', attachment_url = NULL, deleted_at = SYSUTCDATETIME(),updated_at = SYSUTCDATETIME() "
        "WHERE id = ? AND sender_id = ? AND recipient_id = ? AND deleted_at IS NULL",
        { DbParam::BigInt(id), DbParam::Int(sender_id), DbParam::Int(recipient_id) });

    SQLEndTran(SQL_HANDLE_DBC, dbc, rows < 0 ? SQL_ROLLBACK : SQL_COMMIT);
    return rows;
}

// сообщение с таким id есть в переписке этих двух пользователей
int ChatRepository::direct_message_exists(int a, int b, long long id)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();

    int r = db_exists(dbc,
        "SELECT 1 FROM Messages WHERE id = ? AND deleted_at IS NULL "
        "AND ((sender_id = ? AND recipient_id = ?) OR (sender_id = ? AND recipient_id = ?))",
        { DbParam::BigInt(id), DbParam::Int(a), DbParam::Int(b), DbParam::Int(b), DbParam::Int(a) });

    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    return r;
}

// ---------- личные диалоги: последнее сообщение с каждым собеседником ----------
std::string ChatRepository::dialogs_direct(int me, int limit)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();
    json items = json::array();

    db_select(dbc,
        "WITH d AS ( "
        "  SELECT m.id, m.body, m.attachment_url, m.deleted_at, m.created_at, m.sender_id, "
        "         CASE WHEN m.sender_id = ? THEN m.recipient_id ELSE m.sender_id END AS partner_id "
        "  FROM Messages m WHERE m.sender_id = ? OR m.recipient_id = ? "
        "), r AS ( "
        "  SELECT d.*, ROW_NUMBER() OVER (PARTITION BY partner_id ORDER BY id DESC) AS rn FROM d "
        ") "
        "SELECT TOP (?) u.login, r.id, LEFT(r.body, 100), r.attachment_url, "
        "CASE WHEN r.deleted_at IS NULL THEN 0 ELSE 1 END, "
        "CONVERT(NVARCHAR(30), r.created_at, 126), "
        "CASE WHEN r.sender_id = ? THEN 1 ELSE 0 END, "
        "(SELECT COUNT(*) FROM Messages x "
        " WHERE x.sender_id = r.partner_id AND x.recipient_id = ? AND x.deleted_at IS NULL "
        " AND x.id > ISNULL((SELECT dr.last_read_id FROM DirectReads dr "
        "                    WHERE dr.user_id = ? AND dr.partner_id = r.partner_id), 0)), "
        "ISNULL((SELECT dr2.last_read_id FROM DirectReads dr2 "
        "        WHERE dr2.user_id = r.partner_id AND dr2.partner_id = ?), 0) "
        "FROM r JOIN Users u ON u.id = r.partner_id "
        "WHERE r.rn = 1 "
        "ORDER BY r.id DESC",
        { DbParam::Int(me), DbParam::Int(me), DbParam::Int(me), DbParam::Int(limit),
          DbParam::Int(me), DbParam::Int(me), DbParam::Int(me), DbParam::Int(me) },
        [&](SQLHSTMT stmt)
        {
            SQLBIGINT last_id = 0;
            SQLINTEGER deleted = 0, mine = 0;
            SQLWCHAR login[64] = {}, text[101] = {}, att[201] = {};
            SQLCHAR ts[40] = {};
            SQLINTEGER unread = 0;
            SQLBIGINT peer_read = 0;
            SQLLEN unread_i = 0, peer_i = 0;
            SQLLEN login_i = 0, last_i = 0, text_i = 0, att_i = 0, del_i = 0, ts_i = 0, mine_i = 0;

            SQLBindCol(stmt, 1, SQL_C_WCHAR, login, sizeof(login), &login_i);
            SQLBindCol(stmt, 2, SQL_C_SBIGINT, &last_id, 0, &last_i);
            SQLBindCol(stmt, 3, SQL_C_WCHAR, text, sizeof(text), &text_i);
            SQLBindCol(stmt, 4, SQL_C_WCHAR, att, sizeof(att), &att_i);
            SQLBindCol(stmt, 5, SQL_C_LONG, &deleted, 0, &del_i);
            SQLBindCol(stmt, 6, SQL_C_CHAR, ts, sizeof(ts), &ts_i);
            SQLBindCol(stmt, 7, SQL_C_LONG, &mine, 0, &mine_i);
            SQLBindCol(stmt, 8, SQL_C_LONG, &unread, 0, &unread_i);
            SQLBindCol(stmt, 9, SQL_C_SBIGINT, &peer_read, 0, &peer_i);


            while (SQL_SUCCEEDED(SQLFetch(stmt)))
            {
                json item = {
                    {"type", "direct"},
                    {"with", wcol(login, sizeof(login), login_i)},
                    {"last_id", (long long)last_id},
                    {"text", wcol(text, sizeof(text), text_i)},
                    {"ts", std::string((char*)ts)},
                    {"mine", mine != 0},
                    {"unread", (int)unread },
                    {"peer_read_id", (long long)peer_read}
                };
                if (att_i > 0) item["has_attachment"] = true;
                if (deleted)   item["deleted"] = true;
                items.push_back(std::move(item));
            }
        });

    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);

    return items.dump(-1, ' ', false, json::error_handler_t::replace);
}

// ---------- поиск пользователей по началу логина ----------
std::string ChatRepository::search_users(int me, const std::string& q, int limit)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();
    json items = json::array();
    std::string my_role_out;

    // экранируем спецсимволы LIKE, иначе запрос "%" вернёт всех
    std::string pattern;
    for (char c : q)
    {
        if (c == '%' || c == '_' || c == '[' || c == '\\') pattern += '\\';
        pattern += c;
    }
    pattern += '%';

    // порядок параметров: limit, me (для подзапроса роли), pattern, me
    db_select(dbc,
        "SELECT TOP (?) login, role, (SELECT role FROM Users WHERE id = ?) "
        "FROM Users "
        "WHERE login LIKE ? ESCAPE '\\' AND id <> ? "
        "ORDER BY login",
        { DbParam::Int(limit), DbParam::Int(me), DbParam::Text(pattern), DbParam::Int(me) },
        [&](SQLHSTMT stmt)
        {
            SQLWCHAR login[64] = {};
            SQLCHAR role[40] = {}, my_role[40] = {};
            SQLLEN login_i = 0, role_i = 0, my_i = 0;

            SQLBindCol(stmt, 1, SQL_C_WCHAR, login, sizeof(login), &login_i);
            SQLBindCol(stmt, 2, SQL_C_CHAR, role, sizeof(role), &role_i);
            SQLBindCol(stmt, 3, SQL_C_CHAR, my_role, sizeof(my_role), &my_i);

            while (SQL_SUCCEEDED(SQLFetch(stmt)))
            {
                items.push_back({
                    {"login", wcol(login, sizeof(login), login_i)},
                    {"role", std::string((char*)role)}
                    });
                my_role_out = std::string((char*)my_role);
            }
        });

    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);

    json out = { {"my_role", my_role_out}, {"items", items} };
    return out.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::vector<Partner> ChatRepository::partners(int me)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();
    std::vector<Partner> result;

    db_select(dbc,
        "SELECT u.id, u.login, CONVERT(NVARCHAR(30), u.last_seen_at, 126) "
        "FROM Users u "
        "WHERE u.id IN (SELECT recipient_id FROM Messages WHERE sender_id = ? "
        "               UNION "
        "               SELECT sender_id FROM Messages WHERE recipient_id = ?)",
        { DbParam::Int(me), DbParam::Int(me) },
        [&](SQLHSTMT stmt)
        {
            SQLINTEGER id = 0;
            SQLWCHAR login[64] = {};
            SQLCHAR seen[40] = {};
            SQLLEN id_i = 0, login_i = 0, seen_i = 0;

            SQLBindCol(stmt, 1, SQL_C_LONG, &id, 0, &id_i);
            SQLBindCol(stmt, 2, SQL_C_WCHAR, login, sizeof(login), &login_i);
            SQLBindCol(stmt, 3, SQL_C_CHAR, seen, sizeof(seen), &seen_i);

            while (SQL_SUCCEEDED(SQLFetch(stmt)))
            {
                Partner p;
                p.id = (int)id;
                p.login = wcol(login, sizeof(login), login_i);
                if (seen_i > 0) p.last_seen = std::string((char*)seen);
                result.push_back(std::move(p));
            }
        });

    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    return result;
}

// записывает «был в сети» и возвращает время
std::string ChatRepository::touch_last_seen(int user_id)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();
    std::string ts;

    db_select(dbc,
        "UPDATE Users SET last_seen_at = SYSUTCDATETIME() "
        "OUTPUT CONVERT(NVARCHAR(30), INSERTED.last_seen_at, 126) "
        "WHERE id = ?",
        { DbParam::Int(user_id) },
        [&](SQLHSTMT stmt)
        {
            SQLCHAR buf[40] = {};
            SQLLEN ind = 0;
            SQLBindCol(stmt, 1, SQL_C_CHAR, buf, sizeof(buf), &ind);
            if (SQL_SUCCEEDED(SQLFetch(stmt)))
                ts = std::string((char*)buf);
        });

    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    return ts;
}

std::string ChatRepository::db_now()
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();
    std::string ts;

    db_select(dbc,
        "SELECT CONVERT(NVARCHAR(30), DATEADD(SECOND, -5, SYSUTCDATETIME()), 126)",
        {},
        [&](SQLHSTMT stmt)
        {
            SQLCHAR buf[40] = {};
            SQLLEN ind = 0;
            SQLBindCol(stmt, 1, SQL_C_CHAR, buf, sizeof(buf), &ind);
            if (SQL_SUCCEEDED(SQLFetch(stmt)))
                ts = std::string((char*)buf);
        });

    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    return ts;
}

long ChatRepository::mark_read_direct(int me, int partner_id, long long id)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();

    // позиция только растёт; CAST нужен, чтобы SQL Server знал типы параметров в SELECT
    long rows = exec_update(dbc,
        "MERGE DirectReads WITH (HOLDLOCK) AS t "
        "USING (SELECT CAST(? AS INT) AS user_id, CAST(? AS INT) AS partner_id, CAST(? AS BIGINT) AS last_id) AS s "
        "ON t.user_id = s.user_id AND t.partner_id = s.partner_id "
        "WHEN MATCHED AND t.last_read_id < s.last_id THEN UPDATE SET t.last_read_id = s.last_id "
        "WHEN NOT MATCHED THEN INSERT (user_id, partner_id, last_read_id) VALUES (s.user_id, s.partner_id, s.last_id);",
        { DbParam::Int(me), DbParam::Int(partner_id), DbParam::BigInt(id) });

    SQLEndTran(SQL_HANDLE_DBC, dbc, rows < 0 ? SQL_ROLLBACK : SQL_COMMIT);
    return rows;
}

bool ChatRepository::save_upload(const std::string& stored, const std::string& original,
    const std::string& mime, size_t size, int user_id)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();

    long rows = exec_update(dbc,
        "INSERT INTO Uploads (stored_name, original_name, mime, size_bytes, user_id) "
        "VALUES (?, ?, ?, ?, ?)",
        { DbParam::Text(stored), DbParam::Text(original), DbParam::Text(mime),
          DbParam::Int((int)size), DbParam::Int(user_id) });

    SQLEndTran(SQL_HANDLE_DBC, dbc, rows < 0 ? SQL_ROLLBACK : SQL_COMMIT);
    return rows > 0;
}

std::optional<UploadInfo> ChatRepository::find_upload(const std::string& stored)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();
    std::optional<UploadInfo> result;

    db_select(dbc,
        "SELECT original_name, user_id FROM Uploads WHERE stored_name = ?",
        { DbParam::Text(stored) },
        [&](SQLHSTMT stmt)
        {
            SQLWCHAR name[256] = {};
            SQLINTEGER uid = 0;
            SQLLEN name_i = 0, uid_i = 0;

            SQLBindCol(stmt, 1, SQL_C_WCHAR, name, sizeof(name), &name_i);
            SQLBindCol(stmt, 2, SQL_C_LONG, &uid, 0, &uid_i);

            if (SQL_SUCCEEDED(SQLFetch(stmt)))
            {
                UploadInfo info;
                info.original_name = wcol(name, sizeof(name), name_i);
                info.user_id = (int)uid;
                result = std::move(info);
            }
        });

    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    return result;
}

void ChatRepository::delete_upload(const std::string& stored)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();

    long rows = exec_update(dbc, "DELETE FROM Uploads WHERE stored_name = ?",
        { DbParam::Text(stored) });

    SQLEndTran(SQL_HANDLE_DBC, dbc, rows < 0 ? SQL_ROLLBACK : SQL_COMMIT);
}



