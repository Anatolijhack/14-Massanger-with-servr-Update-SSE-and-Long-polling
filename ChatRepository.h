#pragma once
#include "ConnectionPool.h"
#include <optional>
#include <string>

struct SavedMessage { long long id; std::string created_at; };
struct Partner
{
    int id = 0;
    std::string login;
    std::string last_seen;   // пусто, если неизвестно
};

struct UploadInfo
{
    std::string original_name;
    int user_id = 0;
};
class ChatRepository
{
public:
    explicit ChatRepository(ConnectionPool& pool) : pool(pool) {}
    std::optional<SavedMessage> save(int sender_id, int recipient_id, const std::string& text,
        const std::string& attachment_url = "", long long reply_to = 0, const std::string& attachment_name = "");
    std::string history(int me, int other, long long before_id, int limit);  // {"items":[...]}, новые первыми
    std::string сreate_group(int owner_id, std::string& group_name, std::vector<std::string>& members, const std::string& kind = "group");
    std::string sync_direct(int me, long long after_id, const std::string& since, int limit);
    std::optional<bool> attachment_in_use(const std::string& url);
    long edit_direct(int sender_id, int recipient_id, long long id, const std::string& text);
    long delete_direct(int sender_id, int recipient_id, long long id);
    int  direct_message_exists(int a, int b, long long id);
    std::string dialogs_direct(int me, int limit);
    std::string search_users(int me, const std::string& q, int limit);
    std::vector<Partner> partners(int me);
    std::string touch_last_seen(int user_id);
    std::string db_now();
    long mark_read_direct(int me, int partner_id, long long id);
    bool save_upload(const std::string& stored, const std::string& original,
        const std::string& mime, size_t size, int user_id);
    std::optional<UploadInfo> find_upload(const std::string& stored);
    void delete_upload(const std::string& stored);
private:
    ConnectionPool& pool;
};