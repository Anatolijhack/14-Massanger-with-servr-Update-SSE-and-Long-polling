#include "GroupRepository.h"
#include <codecvt>
#include "Utf16.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;
std::optional<CreatedGroup> GroupRepository::find_by_name(const std::string& name)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();

    SQLHSTMT stmt = nullptr;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

    const char* query =
        "SELECT id, name, kind "
        "FROM ChatGroups "
        "WHERE name = ?";

    SQLPrepareA(stmt, (SQLCHAR*)query, SQL_NTS);

    std::wstring wname = utf8_to_wide(name);
    SQLLEN name_len = SQL_NTS;
    SQLBindParameter(
        stmt, 1, SQL_PARAM_INPUT,
        SQL_C_WCHAR, SQL_WVARCHAR,
        (std::max)((SQLULEN)1, (SQLULEN)wname.size()), 0,
        (SQLPOINTER)wname.c_str(), 0, &name_len
    );

    if (!SQL_SUCCEEDED(SQLExecute(stmt)))
    {
        print_odbc_error(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK);
        return std::nullopt;
    }

    SQLINTEGER out_id = 0;
    SQLWCHAR out_name[256] = {};
    SQLCHAR out_kind[16] = {};

    SQLLEN id_ind = 0, name_ind = 0, kind_ind = 0;

    SQLBindCol(stmt, 1, SQL_C_LONG, &out_id, 0, &id_ind);
    SQLBindCol(stmt, 2, SQL_C_WCHAR, out_name, sizeof(out_name), &name_ind);
    SQLBindCol(stmt, 3, SQL_C_CHAR, out_kind, sizeof(out_kind), &kind_ind);

    std::optional<CreatedGroup> result;

    if (SQL_SUCCEEDED(SQLFetch(stmt)))
    {
        CreatedGroup g;
        g.id = static_cast<int>(out_id);
        g.name = wide_to_utf8((const wchar_t*)out_name, (int)(name_ind / sizeof(wchar_t)));
        g.kind = std::string(reinterpret_cast<char*>(out_kind));
        result = std::move(g);
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    return result;
}


std::optional<GroupMessageRecord> GroupRepository::save_messages(
    int group_id,
    int sender_id,
    const std::string& body,
    const std::string& attachment_url, long long reply_to, const std::string& attachment_name)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();

    SQLHSTMT stmt = nullptr;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

    const char* query =
        "INSERT INTO GroupMessages (group_id, sender_id, body, attachment_url,reply_to,attachment_name) "
        "OUTPUT INSERTED.id, "
        "CONVERT(VARCHAR(33), INSERTED.created_at, 126) "
        "VALUES (?, ?, ?, ?,?,?)";

    SQLPrepareA(stmt, (SQLCHAR*)query, SQL_NTS);

    SQLINTEGER gid = group_id;
    SQLINTEGER uid = sender_id;

    SQLLEN gid_ind = 0;
    SQLLEN uid_ind = 0;

    SQLBindParameter(
        stmt, 1, SQL_PARAM_INPUT,
        SQL_C_LONG, SQL_INTEGER,
        0, 0, &gid, 0, &gid_ind
    );

    SQLBindParameter(
        stmt, 2, SQL_PARAM_INPUT,
        SQL_C_LONG, SQL_INTEGER,
        0, 0, &uid, 0, &uid_ind
    );

    // текст: UTF-8 -> UTF-16, как в ChatRepository::save
    std::wstring wbody = utf8_to_wide(body);
    SQLLEN body_ind = SQL_NTS;
    SQLBindParameter(
        stmt, 3, SQL_PARAM_INPUT,
        SQL_C_WCHAR, SQL_WVARCHAR,
        (std::max)((SQLULEN)1, (SQLULEN)wbody.size()), 0,
        (SQLPOINTER)wbody.c_str(), 0, &body_ind
    );

    // вложение: пустая строка -> NULL
    std::wstring wurl = utf8_to_wide(attachment_url);
    SQLLEN url_ind = attachment_url.empty() ? SQL_NULL_DATA : SQL_NTS;
    SQLBindParameter(
        stmt, 4, SQL_PARAM_INPUT,
        SQL_C_WCHAR, SQL_WVARCHAR,
        200, 0,
        (SQLPOINTER)wurl.c_str(), 0, &url_ind
    );

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

    SQLBIGINT out_id = 0;
    SQLCHAR out_created_at[40] = {};

    SQLLEN id_ind = 0;
    SQLLEN created_ind = 0;

    SQLBindCol(stmt, 1, SQL_C_SBIGINT, &out_id, 0, &id_ind);
    SQLBindCol(stmt, 2, SQL_C_CHAR, out_created_at, sizeof(out_created_at), &created_ind);

    std::optional<GroupMessageRecord> result;

    if (SQL_SUCCEEDED(SQLFetch(stmt)))
    {
        result = GroupMessageRecord{
            static_cast<long long>(out_id),
            std::string(reinterpret_cast<char*>(out_created_at))
        };
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    SQLEndTran(SQL_HANDLE_DBC, dbc, result ? SQL_COMMIT : SQL_ROLLBACK);
    return result;
}

    std::vector<int> GroupRepository::get_member_ids(int group_id)
    {
        auto leased = pool.acquire();
        SQLHDBC dbc = leased.handle();

        SQLHSTMT stmt = nullptr;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

        const char* query =
            "SELECT user_id "
            "FROM GroupMembers "
            "WHERE group_id = ?";

        SQLPrepareA(stmt, (SQLCHAR*)query, SQL_NTS);

        SQLINTEGER gid = group_id;
        SQLLEN gid_ind = 0;

        SQLBindParameter(
            stmt, 1, SQL_PARAM_INPUT,
            SQL_C_LONG, SQL_INTEGER,
            0, 0, &gid, 0, &gid_ind
        );

        SQLRETURN rc = SQLExecute(stmt);

        if (!SQL_SUCCEEDED(rc))
        {
            print_odbc_error(SQL_HANDLE_STMT, stmt);
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            return {};
        }

        SQLINTEGER user_id = 0;
        SQLLEN user_id_ind = 0;

        SQLBindCol(stmt, 1, SQL_C_LONG,
            &user_id, 0, &user_id_ind);

        std::vector<int> members;

        while (SQL_SUCCEEDED(SQLFetch(stmt)))
        {
            members.push_back(static_cast<int>(user_id));
        }

       
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
        return members;
    }

    std::optional<std::string> GroupRepository::get_member_role(int group_id, int user_id)
    {
        auto leased = pool.acquire();
        SQLHDBC dbc = leased.handle();

        SQLHSTMT stmt = nullptr;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

        const char* query =
            "SELECT role "
            "FROM GroupMembers "
            "WHERE group_id = ? AND user_id = ?";

        SQLPrepareA(stmt, (SQLCHAR*)query, SQL_NTS);

        SQLINTEGER gid = group_id;
        SQLINTEGER uid = user_id;
        SQLLEN gid_ind = 0;
        SQLLEN uid_ind = 0;

        SQLBindParameter(
            stmt, 1, SQL_PARAM_INPUT,
            SQL_C_LONG, SQL_INTEGER,
            0, 0, &gid, 0, &gid_ind
        );

        SQLBindParameter(
            stmt, 2, SQL_PARAM_INPUT,
            SQL_C_LONG, SQL_INTEGER,
            0, 0, &uid, 0, &uid_ind
        );

        if (!SQL_SUCCEEDED(SQLExecute(stmt)))
        {
            print_odbc_error(SQL_HANDLE_STMT, stmt);
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK);
            return std::nullopt;
        }

        SQLCHAR role[32] = {};
        SQLLEN role_ind = 0;

        SQLBindCol(stmt, 1, SQL_C_CHAR, role, sizeof(role), &role_ind);

        std::optional<std::string> result;

        if (SQL_SUCCEEDED(SQLFetch(stmt)))
            result = std::string(reinterpret_cast<char*>(role));

        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
        return result;
    }
   
    long GroupRepository::add_member(int group_id, const std::string& login)
    {
        auto leased = pool.acquire();
        SQLHDBC dbc = leased.handle();

        long rows = exec_update(dbc,
            "INSERT INTO GroupMembers (group_id, user_id, role, joined_at) "
            "SELECT ?, u.id, 'customer', SYSUTCDATETIME() "
            "FROM Users u "
            "WHERE u.login = ? "
            "AND NOT EXISTS (SELECT 1 FROM GroupMembers gm WHERE gm.group_id = ? AND gm.user_id = u.id)",
            { DbParam::Int(group_id), DbParam::Text(login), DbParam::Int(group_id) });

        SQLEndTran(SQL_HANDLE_DBC, dbc, rows < 0 ? SQL_ROLLBACK : SQL_COMMIT);
        return rows;
    }

    long GroupRepository::remove_member_by_login(int group_id, const std::string& login, bool only_customers)
    {
        auto leased = pool.acquire();
        SQLHDBC dbc = leased.handle();

        // владельца не удаляет никто; админ удаляет только обычных участников
        std::string sql =
            "DELETE gm FROM GroupMembers gm "
            "JOIN Users u ON u.id = gm.user_id "
            "WHERE gm.group_id = ? AND u.login = ? AND gm.role <> 'owner'";
        if (only_customers)
            sql += " AND gm.role = 'customer'";

        long rows = exec_update(dbc, sql, { DbParam::Int(group_id), DbParam::Text(login) });

        SQLEndTran(SQL_HANDLE_DBC, dbc, rows < 0 ? SQL_ROLLBACK : SQL_COMMIT);
        return rows;
    }

    long GroupRepository::remove_member_by_id(int group_id, int user_id)
    {
        auto leased = pool.acquire();
        SQLHDBC dbc = leased.handle();

        long rows = exec_update(dbc,
            "DELETE FROM GroupMembers WHERE group_id = ? AND user_id = ? AND role <> 'owner'",
            { DbParam::Int(group_id), DbParam::Int(user_id) });

        SQLEndTran(SQL_HANDLE_DBC, dbc, rows < 0 ? SQL_ROLLBACK : SQL_COMMIT);
        return rows;
    }

    long GroupRepository::rename_group(int group_id, const std::string& name)
    {
        auto leased = pool.acquire();
        SQLHDBC dbc = leased.handle();

        long rows = exec_update(dbc,
            "UPDATE ChatGroups SET name = ? WHERE id = ?",
            { DbParam::Text(name), DbParam::Int(group_id) });

        SQLEndTran(SQL_HANDLE_DBC, dbc, rows < 0 ? SQL_ROLLBACK : SQL_COMMIT);
        return rows;
    }

    long GroupRepository::set_role(int group_id, const std::string& login, const std::string& role)
    {
        auto leased = pool.acquire();
        SQLHDBC dbc = leased.handle();

        // роль владельца не меняется
        long rows = exec_update(dbc,
            "UPDATE gm SET gm.role = ? "
            "FROM GroupMembers gm JOIN Users u ON u.id = gm.user_id "
            "WHERE gm.group_id = ? AND u.login = ? AND gm.role <> 'owner'",
            { DbParam::Text(role), DbParam::Int(group_id), DbParam::Text(login) });

        SQLEndTran(SQL_HANDLE_DBC, dbc, rows < 0 ? SQL_ROLLBACK : SQL_COMMIT);
        return rows;
    }

    bool GroupRepository::delete_group(int group_id)
    {
        auto leased = pool.acquire();
        SQLHDBC dbc = leased.handle();

        // внешние ключи без CASCADE: сначала сообщения и участники, потом сама группа
        long r1 = exec_update(dbc, "DELETE FROM GroupMessages WHERE group_id = ?", { DbParam::Int(group_id) });
        long r2 = (r1 < 0) ? -1 : exec_update(dbc, "DELETE FROM GroupMembers WHERE group_id = ?", { DbParam::Int(group_id) });
        long r3 = (r2 < 0) ? -1 : exec_update(dbc, "DELETE FROM ChatGroups WHERE id = ?", { DbParam::Int(group_id) });

        if (r3 < 0)
        {
            SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK);
            return false;
        }

        SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
        return r3 > 0;
    }

    std::string GroupRepository::sync_groups(int me, long long after_id, const std::string& since, int limit)
    {
        auto leased = pool.acquire();
        SQLHDBC dbc = leased.handle();

        SQLHSTMT stmt = nullptr;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

        const char* query =
            "SELECT TOP (?) gm.id, g.id, g.name, g.kind, u.login, gm.body, "
            "CONVERT(NVARCHAR(30), gm.created_at, 126), gm.attachment_url,gm.attachment_name, "
            "CASE WHEN gm.edited_at  IS NULL THEN 0 ELSE 1 END, "
            "CASE WHEN gm.deleted_at IS NULL THEN 0 ELSE 1 END, "
            "gm.reply_to "
            "FROM GroupMessages gm "
            "JOIN GroupMembers mem ON mem.group_id = gm.group_id AND mem.user_id = ? "
            "JOIN ChatGroups g ON g.id = gm.group_id "
            "JOIN Users u ON u.id = gm.sender_id "
            "WHERE (gm.id > ? OR gm.updated_at >= CONVERT(DATETIME2, ?, 126)) "
            "ORDER BY gm.id ASC";
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
            SQLBIGINT msg_id = 0;
            SQLINTEGER group_id = 0;
            SQLWCHAR group_name[256] = {};
            SQLCHAR kind[16] = {};
            SQLCHAR login[64] = {};
            SQLWCHAR body[2049] = {};
            SQLCHAR ts[40] = {};
            SQLWCHAR att[201] = {};
            SQLINTEGER edited = 0, deleted = 0;
            SQLBIGINT reply_to = 0;

            SQLLEN msg_id_ind = 0, group_id_ind = 0, gname_ind = 0, kind_ind = 0;
            SQLLEN login_ind = 0, body_ind = 0, ts_ind = 0, att_ind = 0;
            SQLLEN edited_ind = 0, deleted_ind = 0, reply_ind = 0;

            SQLBindCol(stmt, 1, SQL_C_SBIGINT, &msg_id, 0, &msg_id_ind);
            SQLBindCol(stmt, 2, SQL_C_LONG, &group_id, 0, &group_id_ind);
            SQLBindCol(stmt, 3, SQL_C_WCHAR, group_name, sizeof(group_name), &gname_ind);
            SQLBindCol(stmt, 4, SQL_C_CHAR, kind, sizeof(kind), &kind_ind);
            SQLBindCol(stmt, 5, SQL_C_CHAR, login, sizeof(login), &login_ind);
            SQLBindCol(stmt, 6, SQL_C_WCHAR, body, sizeof(body), &body_ind);
            SQLBindCol(stmt, 7, SQL_C_CHAR, ts, sizeof(ts), &ts_ind);
            SQLBindCol(stmt, 8, SQL_C_WCHAR, att, sizeof(att), &att_ind);
            SQLBindCol(stmt, 9, SQL_C_LONG, &edited, 0, &edited_ind);
            SQLBindCol(stmt, 10, SQL_C_LONG, &deleted, 0, &deleted_ind);
            SQLBindCol(stmt, 11, SQL_C_SBIGINT, &reply_to, 0, &reply_ind);
            SQLWCHAR att_name[201] = {};
            SQLLEN att_name_i = 0;
            SQLBindCol(stmt, 12, SQL_C_WCHAR, att_name, sizeof(att_name), &att_name_i);

            while (SQL_SUCCEEDED(SQLFetch(stmt)))
            {
                json item = {
                    {"id", (long long)msg_id},
                    {"group_id", (int)group_id},
                    {"group", wide_to_utf8((const wchar_t*)group_name, (int)(gname_ind / sizeof(wchar_t)))},
                    {"kind", std::string((char*)kind)},
                    {"from", std::string((char*)login)},
                    {"text", wide_to_utf8((const wchar_t*)body, (int)(body_ind / sizeof(wchar_t)))},
                    {"ts", std::string((char*)ts)}
                };

                if (att_ind != SQL_NULL_DATA && att_ind > 0)
                    item["attachment"] = wide_to_utf8((const wchar_t*)att, (int)(att_ind / sizeof(wchar_t)));
                if (att_name_i > 0) item["attachment_name"] = wcol(att_name, sizeof(att_name), att_name_i);
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

        return items.dump(-1, ' ', false, json::error_handler_t::replace);
    }

    long GroupRepository::edit_message(int group_id, int sender_id, long long id, const std::string& text)
    {
        auto leased = pool.acquire();
        SQLHDBC dbc = leased.handle();

        long rows = exec_update(dbc,
            "UPDATE GroupMessages SET body = ?, edited_at = SYSUTCDATETIME(), updated_at = SYSUTCDATETIME() "
            "WHERE id = ? AND group_id = ? AND sender_id = ? AND deleted_at IS NULL",
            { DbParam::Text(text), DbParam::BigInt(id), DbParam::Int(group_id), DbParam::Int(sender_id) });

        SQLEndTran(SQL_HANDLE_DBC, dbc, rows < 0 ? SQL_ROLLBACK : SQL_COMMIT);
        return rows;
    }

    long GroupRepository::delete_message(int group_id, long long id, int only_sender_id)
    {
        auto leased = pool.acquire();
        SQLHDBC dbc = leased.handle();

        std::string sql =
            "UPDATE GroupMessages SET body = N'', attachment_url = NULL, deleted_at = SYSUTCDATETIME(),updated_at = SYSUTCDATETIME()  "
            "WHERE id = ? AND group_id = ? AND deleted_at IS NULL";
        std::vector<DbParam> ps = { DbParam::BigInt(id), DbParam::Int(group_id) };

        if (only_sender_id > 0)
        {
            sql += " AND sender_id = ?";
            ps.push_back(DbParam::Int(only_sender_id));
        }

        long rows = exec_update(dbc, sql, std::move(ps));

        SQLEndTran(SQL_HANDLE_DBC, dbc, rows < 0 ? SQL_ROLLBACK : SQL_COMMIT);
        return rows;
    }

    int GroupRepository::group_message_exists(int group_id, long long id)
    {
        auto leased = pool.acquire();
        SQLHDBC dbc = leased.handle();

        int r = db_exists(dbc,
            "SELECT 1 FROM GroupMessages WHERE id = ? AND group_id = ? AND deleted_at IS NULL",
            { DbParam::BigInt(id), DbParam::Int(group_id) });

        SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
        return r;
    }

    // ---------- история группы (от новых к старым, как history личных сообщений) ----------
    std::string GroupRepository::history(int group_id, long long before_id, int limit)
    {
        auto leased = pool.acquire();
        SQLHDBC dbc = leased.handle();
        json items = json::array();

        db_select(dbc,
            "SELECT TOP (?) gm.id, u.login, gm.body, CONVERT(NVARCHAR(30), gm.created_at, 126), "
            "gm.attachment_url,gm.attachment_name, "
            "CASE WHEN gm.edited_at  IS NULL THEN 0 ELSE 1 END, "
            "CASE WHEN gm.deleted_at IS NULL THEN 0 ELSE 1 END, "
            "gm.reply_to "
            "FROM GroupMessages gm JOIN Users u ON u.id = gm.sender_id "
            "WHERE gm.group_id = ? AND (? = 0 OR gm.id < ?) "
            "ORDER BY gm.id DESC",
            { DbParam::Int(limit), DbParam::Int(group_id), DbParam::BigInt(before_id), DbParam::BigInt(before_id) },
            [&](SQLHSTMT stmt)
            {
                SQLBIGINT id = 0, reply_to = 0;
                SQLINTEGER edited = 0, deleted = 0;
                SQLCHAR ts[40] = {};
                SQLWCHAR login[64] = {}, body[2049] = {}, att[201] = {};
                SQLLEN id_i = 0, login_i = 0, body_i = 0, ts_i = 0, att_i = 0, ed_i = 0, del_i = 0, rep_i = 0;

                SQLBindCol(stmt, 1, SQL_C_SBIGINT, &id, 0, &id_i);
                SQLBindCol(stmt, 2, SQL_C_WCHAR, login, sizeof(login), &login_i);
                SQLBindCol(stmt, 3, SQL_C_WCHAR, body, sizeof(body), &body_i);
                SQLBindCol(stmt, 4, SQL_C_CHAR, ts, sizeof(ts), &ts_i);
                SQLBindCol(stmt, 5, SQL_C_WCHAR, att, sizeof(att), &att_i);
                SQLBindCol(stmt, 6, SQL_C_LONG, &edited, 0, &ed_i);
                SQLBindCol(stmt, 7, SQL_C_LONG, &deleted, 0, &del_i);
                SQLBindCol(stmt, 8, SQL_C_SBIGINT, &reply_to, 0, &rep_i);
                SQLWCHAR att_name[201] = {};
                SQLLEN att_name_i = 0;
                SQLBindCol(stmt, 9, SQL_C_WCHAR, att_name, sizeof(att_name), &att_name_i);

                while (SQL_SUCCEEDED(SQLFetch(stmt)))
                {
                    json item = {
                        {"id", (long long)id},
                        {"from", wcol(login, sizeof(login), login_i)},
                        {"text", wcol(body, sizeof(body), body_i)},
                        {"ts", std::string((char*)ts)}
                    };
                    if (att_i > 0)   item["attachment"] = wcol(att, sizeof(att), att_i);
                    if (att_name_i > 0) item["attachment_name"] = wcol(att_name, sizeof(att_name), att_name_i);
                    if (edited)      item["edited"] = true;
                    if (deleted)     item["deleted"] = true;
                    if (rep_i != SQL_NULL_DATA && reply_to > 0) item["reply_to"] = (long long)reply_to;
                    items.push_back(std::move(item));
                }
            });

        SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);

        json out = { {"items", items} };
        return out.dump(-1, ' ', false, json::error_handler_t::replace);
    }

    // ---------- мои группы ----------
    std::string GroupRepository::list_for_user(int me)
    {
        auto leased = pool.acquire();
        SQLHDBC dbc = leased.handle();
        json items = json::array();

        db_select(dbc,
            "SELECT g.id, g.name, g.kind, mem.role, "
            "(SELECT COUNT(*) FROM GroupMembers x WHERE x.group_id = g.id) "
            "FROM GroupMembers mem JOIN ChatGroups g ON g.id = mem.group_id "
            "WHERE mem.user_id = ? "
            "ORDER BY g.name",
            { DbParam::Int(me) },
            [&](SQLHSTMT stmt)
            {
                SQLINTEGER id = 0, count = 0;
                SQLWCHAR name[256] = {};
                SQLCHAR kind[16] = {}, role[32] = {};
                SQLLEN id_i = 0, name_i = 0, kind_i = 0, role_i = 0, cnt_i = 0;

                SQLBindCol(stmt, 1, SQL_C_LONG, &id, 0, &id_i);
                SQLBindCol(stmt, 2, SQL_C_WCHAR, name, sizeof(name), &name_i);
                SQLBindCol(stmt, 3, SQL_C_CHAR, kind, sizeof(kind), &kind_i);
                SQLBindCol(stmt, 4, SQL_C_CHAR, role, sizeof(role), &role_i);
                SQLBindCol(stmt, 5, SQL_C_LONG, &count, 0, &cnt_i);

                while (SQL_SUCCEEDED(SQLFetch(stmt)))
                {
                    items.push_back({
                        {"id", (int)id},
                        {"name", wcol(name, sizeof(name), name_i)},
                        {"kind", std::string((char*)kind)},
                        {"role", std::string((char*)role)},
                        {"members", (int)count}
                        });
                }
            });

        SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);

        json out = { {"items", items} };
        return out.dump(-1, ' ', false, json::error_handler_t::replace);
    }

    // ---------- состав группы: владелец, админы, затем остальные по алфавиту ----------
    std::string GroupRepository::members(int group_id)
    {
        auto leased = pool.acquire();
        SQLHDBC dbc = leased.handle();
        json items = json::array();

        db_select(dbc,
            "SELECT u.login, gm.role, CONVERT(NVARCHAR(30), gm.joined_at, 126) "
            "FROM GroupMembers gm JOIN Users u ON u.id = gm.user_id "
            "WHERE gm.group_id = ? "
            "ORDER BY CASE gm.role WHEN 'owner' THEN 0 WHEN 'admin' THEN 1 ELSE 2 END, u.login",
            { DbParam::Int(group_id) },
            [&](SQLHSTMT stmt)
            {
                SQLWCHAR login[64] = {};
                SQLCHAR role[32] = {}, joined[40] = {};
                SQLLEN login_i = 0, role_i = 0, joined_i = 0;

                SQLBindCol(stmt, 1, SQL_C_WCHAR, login, sizeof(login), &login_i);
                SQLBindCol(stmt, 2, SQL_C_CHAR, role, sizeof(role), &role_i);
                SQLBindCol(stmt, 3, SQL_C_CHAR, joined, sizeof(joined), &joined_i);

                while (SQL_SUCCEEDED(SQLFetch(stmt)))
                {
                    items.push_back({
                        {"login", wcol(login, sizeof(login), login_i)},
                        {"role", std::string((char*)role)},
                        {"joined_at", std::string((char*)joined)}
                        });
                }
            });

        SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);

        json out = { {"items", items} };
        return out.dump(-1, ' ', false, json::error_handler_t::replace);
    }

    // ---------- группы для списка диалогов: последнее сообщение каждой ----------
    // Возвращает JSON-массив. Группа без сообщений получает время создания группы.
    std::string GroupRepository::dialogs_groups(int me, int limit)
    {
        auto leased = pool.acquire();
        SQLHDBC dbc = leased.handle();
        json items = json::array();

        db_select(dbc,
            "SELECT TOP (?) g.id, g.name, g.kind, lm.id, LEFT(lm.body, 100), lm.attachment_url, "
            "CASE WHEN lm.deleted_at IS NULL THEN 0 ELSE 1 END, "
            "CONVERT(NVARCHAR(30), COALESCE(lm.created_at, g.created_at), 126), u.login, "
            "(SELECT COUNT(*) FROM GroupMessages y "
            " WHERE y.group_id = g.id AND y.id > mem.last_read_id AND y.sender_id <> ? "
            " AND y.deleted_at IS NULL AND y.created_at >= mem.joined_at) "
            "FROM GroupMembers mem "
            "JOIN ChatGroups g ON g.id = mem.group_id "
            "OUTER APPLY (SELECT TOP 1 x.id, x.body, x.attachment_url, x.deleted_at, x.created_at, x.sender_id "
            "             FROM GroupMessages x WHERE x.group_id = g.id ORDER BY x.id DESC) lm "
            "LEFT JOIN Users u ON u.id = lm.sender_id "
            "WHERE mem.user_id = ? "
            "ORDER BY COALESCE(lm.created_at, g.created_at) DESC",
            { DbParam::Int(limit), DbParam::Int(me), DbParam::Int(me) },
            [&](SQLHSTMT stmt)
            {
                SQLINTEGER gid = 0, deleted = 0;
                SQLBIGINT last_id = 0;
                SQLWCHAR gname[256] = {}, text[101] = {}, att[201] = {}, from[64] = {};
                SQLCHAR kind[16] = {}, ts[40] = {};
                SQLINTEGER unread = 0;
                SQLLEN unread_i = 0;
                SQLLEN gid_i = 0, gname_i = 0, kind_i = 0, last_i = 0, text_i = 0, att_i = 0, del_i = 0, ts_i = 0, from_i = 0;

                SQLBindCol(stmt, 1, SQL_C_LONG, &gid, 0, &gid_i);
                SQLBindCol(stmt, 2, SQL_C_WCHAR, gname, sizeof(gname), &gname_i);
                SQLBindCol(stmt, 3, SQL_C_CHAR, kind, sizeof(kind), &kind_i);
                SQLBindCol(stmt, 4, SQL_C_SBIGINT, &last_id, 0, &last_i);
                SQLBindCol(stmt, 5, SQL_C_WCHAR, text, sizeof(text), &text_i);
                SQLBindCol(stmt, 6, SQL_C_WCHAR, att, sizeof(att), &att_i);
                SQLBindCol(stmt, 7, SQL_C_LONG, &deleted, 0, &del_i);
                SQLBindCol(stmt, 8, SQL_C_CHAR, ts, sizeof(ts), &ts_i);
                SQLBindCol(stmt, 9, SQL_C_WCHAR, from, sizeof(from), &from_i);
                SQLBindCol(stmt, 10, SQL_C_LONG, &unread, 0, &unread_i);

                while (SQL_SUCCEEDED(SQLFetch(stmt)))
                {
                    json item = {
                        {"type", "group"},
                        {"group_id", (int)gid},
                        {"group", wcol(gname, sizeof(gname), gname_i)},
                        {"kind", std::string((char*)kind)},
                        {"text", wcol(text, sizeof(text), text_i)},
                        {"ts", std::string((char*)ts)},
                        {"unread", (int)unread }
                    };
                    if (last_i != SQL_NULL_DATA) item["last_id"] = (long long)last_id;
                    if (from_i > 0)              item["from"] = wcol(from, sizeof(from), from_i);
                    if (att_i > 0)               item["has_attachment"] = true;
                    if (deleted)                 item["deleted"] = true;
                    items.push_back(std::move(item));
                }
            });

        SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);

        return items.dump(-1, ' ', false, json::error_handler_t::replace);
    }

    int GroupRepository::transfer_owner(int group_id, int old_owner_id, const std::string& new_login)
    {
        auto leased = pool.acquire();
        SQLHDBC dbc = leased.handle();

        auto rollback = [&](int code) { SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK); return code; };

        // 1. новый владелец должен уже состоять в группе
        long r1 = exec_update(dbc,
            "UPDATE gm SET gm.role = 'owner' "
            "FROM GroupMembers gm JOIN Users u ON u.id = gm.user_id "
            "WHERE gm.group_id = ? AND u.login = ? AND gm.role <> 'owner'",
            { DbParam::Int(group_id), DbParam::Text(new_login) });
        if (r1 < 0) return rollback(-1);
        if (r1 == 0) return rollback(0);

        // 2. прежний владелец становится админом
        long r2 = exec_update(dbc,
            "UPDATE GroupMembers SET role = 'admin' "
            "WHERE group_id = ? AND user_id = ? AND role = 'owner'",
            { DbParam::Int(group_id), DbParam::Int(old_owner_id) });
        if (r2 <= 0) return rollback(-1);

        // 3. owner_id в самой группе
        long r3 = exec_update(dbc,
            "UPDATE ChatGroups SET owner_id = (SELECT id FROM Users WHERE login = ?) WHERE id = ?",
            { DbParam::Text(new_login), DbParam::Int(group_id) });
        if (r3 <= 0) return rollback(-1);

        SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
        return 1;
    }

    long GroupRepository::mark_read(int group_id, int me, long long id)
    {
        auto leased = pool.acquire();
        SQLHDBC dbc = leased.handle();

        long rows = exec_update(dbc,
            "UPDATE GroupMembers SET last_read_id = ? "
            "WHERE group_id = ? AND user_id = ? AND last_read_id < ?",
            { DbParam::BigInt(id), DbParam::Int(group_id), DbParam::Int(me), DbParam::BigInt(id) });

        SQLEndTran(SQL_HANDLE_DBC, dbc, rows < 0 ? SQL_ROLLBACK : SQL_COMMIT);
        return rows;
    }