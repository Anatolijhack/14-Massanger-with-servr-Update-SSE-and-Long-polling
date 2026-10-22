#include "ChatService.h"
#include "Session.h"
#include "Logger.h"

using namespace std::chrono_literals;
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

static std::string random_hex(size_t bytes)
{
    std::string raw(bytes, '\0');
    RAND_bytes(reinterpret_cast<unsigned char*>(&raw[0]), static_cast<int>(bytes));
    static const char* d = "0123456789abcdef";
    std::string out;
    for (unsigned char c : raw) { out += d[c >> 4]; out += d[c & 15]; }
    return out;
}

std::string ChatService::dump(const json& j)
{
    // replace: битый UTF-8 не бросит исключение, как раньше с 0xEF
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool ChatService::can_message(const std::string& a, const std::string& b)
{
    auto staff = [](const std::string& r) { return r == "admin" || r == "employee" || r == "seller"; };
    return staff(a) || staff(b);
}

bool ChatService::origin_allowed(const std::string& origin) const
{
    if (allowed_origins.empty()) return true;   // dev-режим
    return std::find(allowed_origins.begin(), allowed_origins.end(), origin) != allowed_origins.end();
}

void ChatService::reply_error(const std::shared_ptr<Session>& s, const char* code, const char* text)
{
    s->ws_send(dump({ {"type", "error"}, {"code", code}, {"message", text} }));
}

void ChatService::kick_user(int user_id)
{
    for (auto& s : hub.sessions_of(user_id)) s->ws_close(1008, "session ended");
}

void ChatService::on_binary(const std::shared_ptr<Session>& s)
{
    reply_error(s, "unsupported", "Binary frames are not supported");
}

void ChatService::on_text(const std::shared_ptr<Session>& s, const std::string& raw)
{
    json msg = json::parse(raw, nullptr, false);   // без исключений

    if (msg.is_discarded() || !msg.is_object() || !msg.contains("type") || !msg["type"].is_string())
        return reply_error(s, "bad_request", "Invalid message");

    const std::string type = msg["type"].get<std::string>();
    if (type == "ping") { s->ws_send(R"({"type":"pong"})"); return; }
    if (type == "auth") return handle_auth(s, msg);

    if (!s->chat_identity)
        return reply_error(s, "unauthorized", "Send auth first");

    if (type == "message") return handle_message(s, msg);
    if (type == "read")    return handle_read(s, msg);
    if (type == "typing")  return handle_typing(s, msg);
    if (type == "edit")    return handle_edit(s, msg);
    if (type == "delete")  return handle_delete(s, msg);

    reply_error(s, "unknown_type", "Unknown message type");
}

void ChatService::handle_message(const std::shared_ptr<Session>& s, const json& msg)
{
    if (!s->chat_identity)
        return reply_error(s, "unauthorized", "Authenticate first");

    if (!s->allow_chat_message())
        return reply_error(s, "rate_limited", "Too many messages");

    const bool has_to = msg.contains("to") && msg["to"].is_string();
    const bool has_group = msg.contains("group") && msg["group"].is_string();

    if (!has_to && !has_group)
        return reply_error(s, "bad_request", "to or group required");

    if (msg.contains("to") && msg.contains("group"))
        return reply_error(s, "bad_request", "Specify to or group");

    // ---------- текст (необязателен, если есть вложение) ----------
    std::string text;
    if (msg.contains("text"))
    {
        if (!msg["text"].is_string())
            return reply_error(s, "bad_request", "text must be a string");
        text = msg["text"].get<std::string>();
    }

    const char* ws = " \t\r\n";
    size_t b = text.find_first_not_of(ws);
    if (b == std::string::npos) text.clear();
    else text = text.substr(b, text.find_last_not_of(ws) - b + 1);

    // ---------- вложение ----------
    std::string attachment;
    if (msg.contains("attachment"))
    {
        if (!msg["attachment"].is_string())
            return reply_error(s, "bad_request", "attachment must be a string");

        attachment = msg["attachment"].get<std::string>();
        if (!is_valid_attachment_url(attachment))
            return reply_error(s, "bad_request", "Invalid attachment");
    }

    if (text.empty() && attachment.empty())
        return reply_error(s, "empty", "Message is empty");

    if (text.size() > 2000)
        return reply_error(s, "too_long", "Message is too long (max 2000 bytes)");

    // ---------- ответ на сообщение ----------
    long long reply_to = 0;
    if (msg.contains("reply_to"))
    {
        if (!msg["reply_to"].is_number_integer())
            return reply_error(s, "bad_request", "reply_to must be an integer");

        reply_to = msg["reply_to"].get<long long>();
        if (reply_to <= 0)
            return reply_error(s, "bad_request", "Invalid reply_to");
    }

    Session::ChatIdentity sender = *s->chat_identity;

    // ======================= ГРУППА / КАНАЛ =======================
    if (has_group)
    {
        std::string group_name = msg["group"].get<std::string>();

        bool queued = workers.try_push_task(
            [this, s, sender, group_name, text, attachment, reply_to]()
            {
                std::string attachment_name;
                if (!attachment.empty())
                {
                    auto n = attachment_name_for(attachment, sender.user_id);
                    if (!n)
                    {
                        s->ws_post([this, s]() { reply_error(s, "bad_request", "Invalid attachment"); });
                        return;
                    }
                    attachment_name = *n;
                }
                auto group = group_reposs.find_by_name(group_name);
                
                if (!group)
                {
                    s->ws_post([this, s]() {
                        reply_error(s, "group_not_found", "Group not found");
                        });
                    return;
                }

                // участник ли отправитель и какая у него роль
                auto role = group_reposs.get_member_role(group->id, sender.user_id);

                // не участник: отвечаем так же, как «группа не найдена»
                if (!role)
                {
                    s->ws_post([this, s]() {
                        reply_error(s, "group_not_found", "Group not found");
                        });
                    return;
                }

                // в канале пишут только владелец и админы
                if (group->kind == "channel" && *role != "owner" && *role != "admin")
                {
                    s->ws_post([this, s]() {
                        reply_error(s, "forbidden", "Only admins can post in a channel");
                        });
                    return;
                }

                // отвечать можно только на сообщение из этой же группы
                if (reply_to > 0 && group_reposs.group_message_exists(group->id, reply_to) != 1)
                {
                    s->ws_post([this, s]() {
                        reply_error(s, "reply_not_found", "Message to reply to not found");
                        });
                    return;
                }

                auto saved = group_reposs.save_messages(
                    group->id, sender.user_id, text, attachment, reply_to,attachment_name);

                if (!saved)
                {
                    s->ws_post([this, s]() {
                        reply_error(s, "server_error", "Could not save group message");
                        });
                    return;
                }

                json j = {
                    {"type", "group_message"},
                    {"id", saved->id},
                    {"group_id", group->id},
                    {"group", group->name},
                    {"kind", group->kind},
                    {"from", sender.login},
                    {"text", text},
                    {"ts", saved->created_at}
                };
                if (!attachment.empty())
                {
                    j["attachment"] = attachment;
                    j["attachment_name"] = attachment_name;
                };
                if (reply_to > 0)        j["reply_to"] = reply_to;
                std::string out = dump(j);

                auto members = group_reposs.get_member_ids(group->id);

                for (int user_id : members)
                {
                    for (auto& rs : hub.sessions_of(user_id))
                        rs->ws_send(out);
                }

                json ack = {
                    {"type", "group_sent"},
                    {"id", saved->id},
                    {"group_id", group->id},
                    {"group", group->name},
                    {"kind", group->kind},
                    {"ts", saved->created_at}
                };
                if (!attachment.empty())
                {
                    ack["attachment"] = attachment;
                    ack["attachment_name"] = attachment_name;
                };
                if (reply_to > 0)        ack["reply_to"] = reply_to;
                s->ws_send(dump(ack));
            }, 0);

        if (!queued)
            reply_error(s, "busy", "Server busy, try again");

        return;
    }

    // ===================== ЛИЧНОЕ СООБЩЕНИЕ =====================
    std::string to = msg["to"].get<std::string>();

    bool queued = workers.try_push_task([this, s, sender, to, text, attachment, reply_to]()
        {
            std::string attachment_name;
            if (!attachment.empty())
            {
                auto n = attachment_name_for(attachment, sender.user_id);
                if (!n)
                {
                    s->ws_post([this, s]() { reply_error(s, "bad_request", "Invalid attachment"); });
                    return;
                }
                attachment_name = *n;
            }

            auto recipient = users.find_by_username(to);

            // «нет такого» и «нельзя писать» отвечаем одинаково — логины не перебрать
            if (!recipient || recipient->id == sender.user_id || !can_message(sender.role, recipient->role))
            {
                s->ws_post([this, s]() { reply_error(s, "recipient_not_found", "Recipient not found"); });
                return;
            }

            // отвечать можно только на сообщение из этой же переписки
            if (reply_to > 0 && chats.direct_message_exists(sender.user_id, recipient->id, reply_to) != 1)
            {
                s->ws_post([this, s]() { reply_error(s, "reply_not_found", "Message to reply to not found"); });
                return;
            }

            auto saved = chats.save(sender.user_id, recipient->id, text, attachment, reply_to, attachment_name);

            if (!saved)
            {
                s->ws_post([this, s]() { reply_error(s, "server_error", "Could not save message"); });
                return;
            }

            json j = {
                {"type", "message"}, {"id", saved->id}, {"from", sender.login},
                {"text", text}, {"ts", saved->created_at}
            };
            if (!attachment.empty())
            {
                j["attachment"] = attachment;
                j["attachment_name"] = attachment_name;
            }
            if (reply_to > 0)        j["reply_to"] = reply_to;
            std::string out = dump(j);

            for (auto& rs : hub.sessions_of(recipient->id)) rs->ws_send(out);

            json ack = {
                {"type", "sent"}, {"id", saved->id},
                {"to", recipient->username}, {"ts", saved->created_at}
            };
            if (!attachment.empty())
            { 
                ack["attachment"] = attachment;
                ack["attachment_name"] = attachment_name;
            }
            if (reply_to > 0)        ack["reply_to"] = reply_to;
            s->ws_send(dump(ack));
        }, 0);

    if (!queued) reply_error(s, "busy", "Server busy, try again");
}

void ChatService::handle_auth(const std::shared_ptr<Session>& s, const json& msg)
{
    if (s->chat_identity) return reply_error(s, "already_authenticated", "Already authenticated");

    if (!msg.contains("token") || !msg["token"].is_string())
        return reply_error(s, "bad_request", "token required");

    std::string token = msg["token"].get<std::string>();

    bool queued = workers.try_push_task([this, s, token]()
        {
            auto session = sessions.find(token);          // ÁÄ — â ïóëå, íå â io-ïîòîêå

            s->ws_post([this, s, session]()               // îáðàòíî â ïîòîê ñîêåòà
                {
                    if (s->chat_identity) return;

                    if (!session)
                    {
                        reply_error(s, "unauthorized", "Invalid or expired token");
                        s->ws_close(1008, "unauthorized");
                        return;
                    }
                    bool first = false;
                    if (!hub.attach(session->user_id, s, &first))
                    {
                        reply_error(s, "too_many_connections", "Too many open connections");
                        s->ws_close(1008, "too many connections");
                        return;
                    }

                    s->chat_identity = Session::ChatIdentity{ session->user_id, session->username, session->role };
                    s->ws_send(dump({ {"type", "auth_ok"}, {"login", session->username} }));
                    announce_online(*s->chat_identity, s);
                });
        }, 0);

    if (!queued) reply_error(s, "busy", "Server busy, try again");
}

void ChatService::handle_edit(const std::shared_ptr<Session>& s, const json& msg)
{
    if (!s->chat_identity)
        return reply_error(s, "unauthorized", "Authenticate first");

    if (!s->allow_chat_message())
        return reply_error(s, "rate_limited", "Too many messages");

    if (!msg.contains("id") || !msg["id"].is_number_integer() || msg["id"].get<long long>() <= 0)
        return reply_error(s, "bad_request", "id required");
    const long long id = msg["id"].get<long long>();

    const bool has_to = msg.contains("to") && msg["to"].is_string();
    const bool has_group = msg.contains("group") && msg["group"].is_string();
    if (has_to == has_group)
        return reply_error(s, "bad_request", "Specify to or group");

    if (!msg.contains("text") || !msg["text"].is_string())
        return reply_error(s, "bad_request", "text required");

    std::string text = msg["text"].get<std::string>();
    const char* ws = " \t\r\n";
    size_t b = text.find_first_not_of(ws);
    if (b == std::string::npos) return reply_error(s, "empty", "Message is empty");
    text = text.substr(b, text.find_last_not_of(ws) - b + 1);
    if (text.size() > 2000) return reply_error(s, "too_long", "Message is too long (max 2000 bytes)");

    Session::ChatIdentity sender = *s->chat_identity;

    if (has_group)
    {
        std::string group_name = msg["group"].get<std::string>();

        bool queued = workers.try_push_task([this, s, sender, group_name, id, text]()
            {
                auto group = group_reposs.find_by_name(group_name);
                auto role = group ? group_reposs.get_member_role(group->id, sender.user_id) : std::nullopt;

                if (!group || !role)
                {
                    s->ws_post([this, s]() { reply_error(s, "group_not_found", "Group not found"); });
                    return;
                }

                long rows = group_reposs.edit_message(group->id, sender.user_id, id, text);

                if (rows < 0)
                {
                    s->ws_post([this, s]() { reply_error(s, "server_error", "Could not edit message"); });
                    return;
                }
                if (rows == 0)
                {
                    s->ws_post([this, s]() { reply_error(s, "message_not_found", "Message not found"); });
                    return;
                }

                std::string out = dump({
                    {"type", "message_edited"}, {"group_id", group->id}, {"group", group->name},
                    {"id", id}, {"text", text}, {"edited", true}
                    });

                for (int user_id : group_reposs.get_member_ids(group->id))
                    for (auto& rs : hub.sessions_of(user_id))
                        rs->ws_send(out);
            }, 0);

        if (!queued) reply_error(s, "busy", "Server busy, try again");
        return;
    }

    std::string to = msg["to"].get<std::string>();

    bool queued = workers.try_push_task([this, s, sender, to, id, text]()
        {
            auto recipient = users.find_by_username(to);
            if (!recipient)
            {
                s->ws_post([this, s]() { reply_error(s, "recipient_not_found", "Recipient not found"); });
                return;
            }

            long rows = chats.edit_direct(sender.user_id, recipient->id, id, text);

            if (rows < 0)
            {
                s->ws_post([this, s]() { reply_error(s, "server_error", "Could not edit message"); });
                return;
            }
            if (rows == 0)
            {
                s->ws_post([this, s]() { reply_error(s, "message_not_found", "Message not found"); });
                return;
            }

            std::string out = dump({
                {"type", "message_edited"}, {"id", id}, {"from", sender.login},
                {"to", recipient->username}, {"text", text}, {"edited", true}
                });

            // получателю и всем устройствам отправителя
            for (auto& rs : hub.sessions_of(recipient->id)) rs->ws_send(out);
            for (auto& rs : hub.sessions_of(sender.user_id)) rs->ws_send(out);
        }, 0);

    if (!queued) reply_error(s, "busy", "Server busy, try again");
}

void ChatService::handle_delete(const std::shared_ptr<Session>& s, const json& msg)
{
    if (!s->chat_identity)
        return reply_error(s, "unauthorized", "Authenticate first");

    if (!s->allow_chat_message())
        return reply_error(s, "rate_limited", "Too many messages");

    if (!msg.contains("id") || !msg["id"].is_number_integer() || msg["id"].get<long long>() <= 0)
        return reply_error(s, "bad_request", "id required");
    const long long id = msg["id"].get<long long>();

    const bool has_to = msg.contains("to") && msg["to"].is_string();
    const bool has_group = msg.contains("group") && msg["group"].is_string();
    if (has_to == has_group)
        return reply_error(s, "bad_request", "Specify to or group");

    Session::ChatIdentity sender = *s->chat_identity;

    if (has_group)
    {
        std::string group_name = msg["group"].get<std::string>();

        bool queued = workers.try_push_task([this, s, sender, group_name, id]()
            {
                auto group = group_reposs.find_by_name(group_name);
                auto role = group ? group_reposs.get_member_role(group->id, sender.user_id) : std::nullopt;

                if (!group || !role)
                {
                    s->ws_post([this, s]() { reply_error(s, "group_not_found", "Group not found"); });
                    return;
                }

                // владелец и админы удаляют любые сообщения, остальные только свои
                const bool moderator = (*role == "owner" || *role == "admin");
                long rows = group_reposs.delete_message(group->id, id, moderator ? 0 : sender.user_id);

                if (rows < 0)
                {
                    s->ws_post([this, s]() { reply_error(s, "server_error", "Could not delete message"); });
                    return;
                }
                if (rows == 0)
                {
                    s->ws_post([this, s]() { reply_error(s, "message_not_found", "Message not found"); });
                    return;
                }

                std::string out = dump({
                    {"type", "message_deleted"}, {"group_id", group->id},
                    {"group", group->name}, {"id", id}
                    });

                for (int user_id : group_reposs.get_member_ids(group->id))
                    for (auto& rs : hub.sessions_of(user_id))
                        rs->ws_send(out);
            }, 0);

        if (!queued) reply_error(s, "busy", "Server busy, try again");
        return;
    }

    std::string to = msg["to"].get<std::string>();

    bool queued = workers.try_push_task([this, s, sender, to, id]()
        {
            auto recipient = users.find_by_username(to);
            if (!recipient)
            {
                s->ws_post([this, s]() { reply_error(s, "recipient_not_found", "Recipient not found"); });
                return;
            }

            long rows = chats.delete_direct(sender.user_id, recipient->id, id);

            if (rows < 0)
            {
                s->ws_post([this, s]() { reply_error(s, "server_error", "Could not delete message"); });
                return;
            }
            if (rows == 0)
            {
                s->ws_post([this, s]() { reply_error(s, "message_not_found", "Message not found"); });
                return;
            }

            std::string out = dump({
                {"type", "message_deleted"}, {"id", id},
                {"from", sender.login}, {"to", recipient->username}
                });

            for (auto& rs : hub.sessions_of(recipient->id)) rs->ws_send(out);
            for (auto& rs : hub.sessions_of(sender.user_id)) rs->ws_send(out);
        }, 0);

    if (!queued) reply_error(s, "busy", "Server busy, try again");
}

// Вызывается сразу после auth_ok
void ChatService::announce_online(const Session::ChatIdentity& who, const std::shared_ptr<Session>& s)
{
    // это первое соединение пользователя? (новая сессия уже в хабе)
   

    workers.try_push_task([this, who, s]()
        {
            auto partners = chats.partners(who.user_id);

            // 1. снимок для самого пользователя: кто из собеседников онлайн
            json items = json::array();
            for (const auto& p : partners)
            {
                const bool online = !hub.sessions_of(p.id).empty();
                json it = { {"login", p.login}, {"online", online} };
                if (!online && !p.last_seen.empty()) it["last_seen"] = p.last_seen;
                items.push_back(std::move(it));
            }
            s->ws_send(dump({ {"type", "presence_snapshot"}, {"items", items} }));

            // 2. собеседникам сообщаем только про первое соединение
            

            std::string out = dump({ {"type", "presence"}, {"login", who.login}, {"online", true} });
            for (const auto& p : partners)
                for (auto& rs : hub.sessions_of(p.id))
                    rs->ws_send(out);
        }, 0);
}

// Вызывается при закрытии сокета, после hub.detach
void ChatService::announce_offline(const Session::ChatIdentity& who)
{
                             // остались другие устройства

    workers.try_push_task([this, who]()
        {
            std::string seen = chats.touch_last_seen(who.user_id);
            auto partners = chats.partners(who.user_id);

            json j = { {"type", "presence"}, {"login", who.login}, {"online", false} };
            if (!seen.empty()) j["last_seen"] = seen;
            std::string out = dump(j);

            for (const auto& p : partners)
                for (auto& rs : hub.sessions_of(p.id))
                    rs->ws_send(out);
        }, 0);
}

void ChatService::handle_typing(const std::shared_ptr<Session>& s, const json& msg)
{
    // ошибки не шлём: «печатает» не должно спамить клиента
    if (!s->chat_identity) return;
    Session::ChatIdentity sender = *s->chat_identity;

    // не чаще раза в 2 секунды на пользователя
    const bool has_to = msg.contains("to") && msg["to"].is_string();
    const bool has_group = msg.contains("group") && msg["group"].is_string();
    if (has_to == has_group) return;

    if (has_to)
    {
        std::string to = msg["to"].get<std::string>();

        workers.try_push_task([this, sender, to]()
            {
                auto recipient = users.find_by_username(to);
                if (!recipient || recipient->id == sender.user_id ||
                    !can_message(sender.role, recipient->role))
                    return;

                std::string out = dump({ {"type", "typing"}, {"from", sender.login} });
                for (auto& rs : hub.sessions_of(recipient->id))
                    rs->ws_send(out);
            }, 0);
        return;
    }

    std::string group_name = msg["group"].get<std::string>();

    workers.try_push_task([this, sender, group_name]()
        {
            auto group = group_reposs.find_by_name(group_name);
            if (!group) return;

            auto role = group_reposs.get_member_role(group->id, sender.user_id);
            if (!role) return;

            // в канале подписчики писать не могут, значит и «печатают» тоже
            if (group->kind == "channel" && *role != "owner" && *role != "admin")
                return;

            std::string out = dump({
                {"type", "typing"}, {"group_id", group->id},
                {"group", group->name}, {"from", sender.login}
                });

            for (int user_id : group_reposs.get_member_ids(group->id))
            {
                if (user_id == sender.user_id) continue;
                for (auto& rs : hub.sessions_of(user_id))
                    rs->ws_send(out);
            }
        }, 0);
}
void ChatService::on_close(const Session::ChatIdentity& who, const Session* s)
{
    // true только если это было последнее живое соединение пользователя
    if (hub.detach(who.user_id, s))
        announce_offline(who);
}

void ChatService::handle_read(const std::shared_ptr<Session>& s, const json& msg)
{
    if (!s->chat_identity) return;
    Session::ChatIdentity sender = *s->chat_identity;

    // отметка идемпотентна, при превышении просто пропускаем: клиент пришлёт позже


    if (!msg.contains("id") || !msg["id"].is_number_integer() || msg["id"].get<long long>() <= 0)
        return reply_error(s, "bad_request", "id required");
    const long long id = msg["id"].get<long long>();

    const bool has_to = msg.contains("to") && msg["to"].is_string();
    const bool has_group = msg.contains("group") && msg["group"].is_string();
    if (has_to == has_group)
        return reply_error(s, "bad_request", "Specify to or group");

    if (has_to)
    {
        std::string to = msg["to"].get<std::string>();

        workers.try_push_task([this, sender, to, id]()
            {
                auto partner = users.find_by_username(to);
                if (!partner || partner->id == sender.user_id) return;

                // 0: уже прочитано дальше, 1: сдвинулось
                if (chats.mark_read_direct(sender.user_id, partner->id, id) <= 0) return;

                // другим вкладкам читающего: сбросить счётчик
                std::string own = dump({ {"type", "read"}, {"with", partner->username}, {"id", id} });
                for (auto& rs : hub.sessions_of(sender.user_id)) rs->ws_send(own);

                // собеседнику: квитанция о прочтении
                std::string receipt = dump({ {"type", "read_receipt"}, {"by", sender.login}, {"id", id} });
                for (auto& rs : hub.sessions_of(partner->id)) rs->ws_send(receipt);
            }, 0);
        return;
    }

    std::string group_name = msg["group"].get<std::string>();

    workers.try_push_task([this, sender, group_name, id]()
        {
            auto group = group_reposs.find_by_name(group_name);
            if (!group) return;
            if (!group_reposs.get_member_role(group->id, sender.user_id)) return;

            if (group_reposs.mark_read(group->id, sender.user_id, id) <= 0) return;

            std::string own = dump({ {"type", "read"}, {"group", group->name},
                                     {"group_id", group->id}, {"id", id} });
            for (auto& rs : hub.sessions_of(sender.user_id)) rs->ws_send(own);
        }, 0);
}

std::optional<std::string> ChatService::attachment_name_for(const std::string& url, int user_id)
{
    static const std::string prefix = "/uploads/chat/";
    auto up = chats.find_upload(url.substr(prefix.size()));
    if (!up || up->user_id != user_id) return std::nullopt;
    return up->original_name;
}   

void ChatService::sse_open(const std::shared_ptr<Session>& s, const std::string& token)
{
    bool queued = workers.try_push_task([this, s, token]()
        {
            auto session = sessions.find(token);

            s->ws_post([this, s, session]()
                {
                    if (!session) { s->reject_http("401 Unauthorized"); return; }

                    bool first = false;
                    if (!hub.attach(session->user_id, s, &first))
                    {
                        s->reject_http("429 Too Many Requests");
                        return;
                    }

                    s->chat_identity = Session::ChatIdentity{ session->user_id, session->username, session->role };

                    const std::string id = random_hex(16);
                    s->stream_id = id;
                    {
                        std::lock_guard<std::mutex> lk(streams_m);
                        streams[id] = s;
                    }

                    s->sse_begin();
                    s->ws_send(dump({ {"type", "auth_ok"}, {"login", session->username}, {"stream", id} }));
                    announce_online(*s->chat_identity, s);
                });
        }, 0);

    if (!queued) s->reject_http("503 Service Unavailable");
}
std::shared_ptr<Session> ChatService::find_stream(const std::string& id)
{
    std::lock_guard<std::mutex> lk(streams_m);
    auto it = streams.find(id);
    if (it != streams.end()) return it->second.lock();
    auto d = devices.find(id);
    return d == devices.end() ? nullptr : d->second;
}

void ChatService::drop_stream(const std::string& id)
{
    std::shared_ptr<Session> keep;                 // освободим объект уже после снятия мьютекса
    {
        std::lock_guard<std::mutex> lk(streams_m);
        streams.erase(id);
        auto d = devices.find(id);
        if (d != devices.end()) { keep = std::move(d->second); devices.erase(d); }
    }
}

void ChatService::poll_open(const std::shared_ptr<Session>& http, const std::string& token)
{
    bool queued = workers.try_push_task([this, http, token]()
        {
            auto session = sessions.find(token);

            http->ws_post([this, http, session]()
                {
                    if (!session) { http->reject_http("401 Unauthorized"); return; }

                    auto dev = Session::make_device(*http);

                    bool first = false;
                    if (!hub.attach(session->user_id, dev, &first))
                    {
                        http->reject_http("429 Too Many Requests");
                        return;
                    }

                    dev->chat_identity = Session::ChatIdentity{ session->user_id, session->username, session->role };
                    dev->device_id = random_hex(16);
                    {
                        std::lock_guard<std::mutex> lk(streams_m);
                        devices[dev->device_id] = dev;
                    }

                    dev->arm_timer(60s);
                    dev->ws_send(dump({ {"type", "auth_ok"}, {"login", session->username},
                                        {"device", dev->device_id} }));
                    announce_online(*dev->chat_identity, dev);

                    dev->ws_post([dev, http]() { dev->poll_take(http, 0); });   // первый ответ с накопленным
                });
        }, 0);

    if (!queued) http->reject_http("503 Service Unavailable");
}
