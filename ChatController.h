#pragma once
#include "ChatRepository.h"
#include "UserRepository.h"
#include "SessionRepository.h"
#include "AuthMiddleware.h"
#include "Router.h"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <random>
#include "MultipartParser.h"

static std::optional<std::string> read_upload(const std::string& stored_name)
{
    std::ifstream in("uploads/chat/" + stored_name, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
// Оставляет только безопасное имя. Расширение всегда ставится по реальному типу файла,
// чтобы нельзя было выдать zip за "invoice.exe".
static std::string sanitize_filename(std::string raw, const std::string& ext)
{
    // только имя, без путей (Windows и Unix)
    auto slash = raw.find_last_of("/\\");
    if (slash != std::string::npos) raw = raw.substr(slash + 1);

    // убираем управляющие и запрещённые в именах символы
    std::string clean;
    for (unsigned char c : raw)
    {
        if (c < 0x20 || c == 0x7F) continue;
        if (std::strchr("<>:\"|?*", c)) continue;
        clean += (char)c;
    }

    // старое расширение отбрасываем
    auto dot = clean.find_last_of('.');
    if (dot != std::string::npos) clean.erase(dot);

    // пробелы и точки по краям
    auto b = clean.find_first_not_of(" .");
    auto e = clean.find_last_not_of(" .");
    clean = (b == std::string::npos) ? std::string() : clean.substr(b, e - b + 1);

    if (clean.empty()) clean = "file";

    // ограничиваем длину, не разрезая многобайтовый символ UTF-8
    const size_t max_stem = 100;
    if (clean.size() > max_stem)
    {
        size_t cut = max_stem;
        while (cut > 0 && ((unsigned char)clean[cut] & 0xC0) == 0x80) --cut;
        clean.erase(cut);
    }

    return clean + ext;
}
static bool can_message(const std::string& a, const std::string& b)
{
    auto staff = [](const std::string& r) { return r == "admin" || r == "employee" || r == "seller"; };
    return staff(a) || staff(b);
};
using json = nlohmann::json;
static std::string generate_filename(const std::string& ext)
{
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;

    std::ostringstream oss;
    oss << std::hex << dist(gen) << ext;
    return oss.str();
}
struct FileType
{
    const char* mime;
    const char* ext;
    size_t      max_size;
    bool        is_image;
};
static const FileType kFileTypes[] = {
    { "image/jpeg",      ".jpg",  10u * 1024 * 1024, true  },
    { "image/png",       ".png",  10u * 1024 * 1024, true  },
    { "image/webp",      ".webp", 10u * 1024 * 1024, true  },
    { "application/pdf", ".pdf",  25u * 1024 * 1024, false },
    { "text/plain",      ".txt",   5u * 1024 * 1024, false },
    { "application/zip", ".zip",  50u * 1024 * 1024, false },
    { "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
                         ".docx", 25u * 1024 * 1024, false },
    { "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
                         ".xlsx", 25u * 1024 * 1024, false },
};

static const FileType* find_type_by_mime(const std::string& mime)
{
    for (const auto& t : kFileTypes)
        if (mime == t.mime) return &t;
    return nullptr;
}

static const FileType* find_type_by_ext(const std::string& ext)
{
    for (const auto& t : kFileTypes)
        if (ext == t.ext) return &t;
    return nullptr;
}

static bool ends_with(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() &&
        s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
static bool is_valid_attachment_url(const std::string& url)
{
    static const std::string prefix = "/uploads/chat/";
    if (url.compare(0, prefix.size(), prefix) != 0)
        return false;

    std::string name = url.substr(prefix.size());
    if (name.size() <= 16) return false;

    for (size_t i = 0; i < 16; ++i)
    {
        char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }

    return find_type_by_ext(name.substr(16)) != nullptr;
}
static bool is_allowed_image_type(const std::string& content_type)
{
    return content_type == "image/jpeg" || content_type == "image/png" || content_type == "image/webp";
}
static std::string normalize_content_type(std::string ct)
{
    auto semi = ct.find(';');
    if (semi != std::string::npos) ct.erase(semi);

    auto b = ct.find_first_not_of(" \t");
    auto e = ct.find_last_not_of(" \t");
    ct = (b == std::string::npos) ? std::string() : ct.substr(b, e - b + 1);

    std::transform(ct.begin(), ct.end(), ct.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return ct;
}

// Расширение только для разрешённых типов, иначе nullopt
static std::optional<std::string> extension_for(const std::string& content_type)
{
    if (content_type == "image/jpeg") return ".jpg";
    if (content_type == "image/png")  return ".png";
    if (content_type == "image/webp") return ".webp";
    return std::nullopt;
}

// Content-Type присылает клиент, поэтому проверяем и реальную сигнатуру файла
static bool matches_signature(const std::string& mime, const char* d, size_t n)
{
    if (mime == "image/jpeg")
        return n >= 3 && (unsigned char)d[0] == 0xFF && (unsigned char)d[1] == 0xD8 && (unsigned char)d[2] == 0xFF;

    if (mime == "image/png")
        return n >= 8 && std::memcmp(d, "\x89PNG\r\n\x1A\n", 8) == 0;

    if (mime == "image/webp")
        return n >= 12 && std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d + 8, "WEBP", 4) == 0;

    if (mime == "application/pdf")
        return n >= 5 && std::memcmp(d, "%PDF-", 5) == 0;

    // zip, docx и xlsx — это zip-архивы
    if (mime == "application/zip" ||
        mime.find("openxmlformats") != std::string::npos)
        return n >= 4 && std::memcmp(d, "PK\x03\x04", 4) == 0;

    // у txt сигнатуры нет, поэтому проверяем отсутствие нулевых байтов
    if (mime == "text/plain")
        return std::memchr(d, 0, n) == nullptr;

    return false;
}

static Response ok(const json& j, const char* status = "200 OK")
{
    return Response{ j.dump(-1, ' ', false, json::error_handler_t::replace), "application/json", status };
}

static Response err(const char* msg, const char* status)
{
    return ok(json{ {"error", msg} }, status);
}

// строковое поле из тела или "" если нет
static std::string str_field(const json& b, const char* key)
{
    if (!b.is_object() || !b.contains(key) || !b[key].is_string()) return {};
    return b[key].get<std::string>();
}

static std::string trim(const std::string& s)
{
    auto b = s.find_first_not_of(" \t\r\n");
    auto e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}
class ChatController
{
public:
    ChatController(ConnectionPool& pool, SessionRepository& sessions)
        : chats(pool), users(pool), groups(pool), sessions(sessions) {}

    void register_routes(Router& router)
    {
        router.add("GET", "/chat/history", [this](const Request& req) { return history(req); });
        router.add("POST", "/chat/groups", [this](const Request& req) { return create_group(req); });
        router.add("POST", "/chat/upload", [this](const Request& req) { return upload_attachment(req); });
        router.add("GET", "/uploads/chat/:name", [this](const Request& req) { return get_attachment(req); });
        router.add("GET", "/uploads/chat/:name/:filename", [this](const Request& req) { return get_attachment_named(req); });
        router.add("POST", "/chat/groups/members/add", [this](const Request& req) { return group_add_member(req); });
        router.add("POST", "/chat/groups/members/remove", [this](const Request& req) { return group_remove_member(req); });
        router.add("POST", "/chat/groups/leave", [this](const Request& req) { return group_leave(req); });
        router.add("POST", "/chat/groups/rename", [this](const Request& req) { return group_rename(req); });
        router.add("POST", "/chat/groups/role", [this](const Request& req) { return group_set_role(req); });
        router.add("POST", "/chat/groups/delete", [this](const Request& req) { return group_delete(req); });
        router.add("GET", "/chat/sync", [this](const Request& req) { return sync(req); });
        router.add("GET", "/chat/groups", [this](const Request& req) { return my_groups(req); });
        router.add("GET", "/chat/groups/:name/members", [this](const Request& req) { return group_members(req); });
        router.add("GET", "/chat/groups/:name/history", [this](const Request& req) { return group_history(req); });
        router.add("GET", "/chat/dialogs", [this](const Request& req) { return dialogs(req); });
        router.add("GET", "/users/search", [this](const Request& req) { return search_users(req); });
        router.add("POST", "/chat/groups/transfer", [this](const Request& req) { return group_transfer(req); });
        
    }

private:

    Response group_transfer(const Request& req)
    {
        auto c = group_ctx(req);
        if (c.error) return *c.error;

        if (c.role != "owner")
            return err("Only owner can transfer ownership", "403 Forbidden");

        std::string login = trim(str_field(c.body, "login"));
        if (login.empty()) return err("login required", "400 Bad Request");

        int r = groups.transfer_owner(c.group_id, c.user_id, login);
        if (r < 0)  return err("Server error", "500 Internal Server Error");
        if (r == 0) return err("Member not found", "404 Not Found");

        return ok({ {"status", "ownership_transferred"}, {"group", c.group_name}, {"owner", login} });
    }

    Response my_groups(const Request& req)
    {
        auto auth = require_auth(req, sessions);
        if (!auth.ok) return auth.error;

        return Response{ groups.list_for_user(auth.user_id), "application/json" };
    }

    Response search_users(const Request& req)
    {
        auto auth = require_auth(req, sessions);
        if (!auth.ok) return auth.error;

        auto it = req.params.find("q");
        std::string q = (it == req.params.end()) ? std::string() : trim(it->second);

        // слишком короткий запрос не ищем: иначе можно перебрать всех пользователей
        if (q.size() < 2)
            return Response{ R"({"items":[]})", "application/json" };
        if (q.size() > 50)
            return err("q too long", "400 Bad Request");

        json r = json::parse(chats.search_users(auth.user_id, q, 50), nullptr, false);

        json items = json::array();
        if (!r.is_discarded() && r.contains("items"))
        {
            const std::string my_role = r.value("my_role", std::string());

            for (auto& it : r["items"])
            {
                // оставляем только тех, кому пользователь может написать
                if (!can_message(my_role, it.value("role", std::string())))
                    continue;

                items.push_back({ {"login", it["login"]} });   // роль наружу не отдаём
                if (items.size() >= 20) break;
            }
        }

        json out = { {"items", items} };
        return Response{ out.dump(-1, ' ', false, json::error_handler_t::replace), "application/json" };
    }

    //Response dialogs(const Request& req)
    //{
    //
    //    auto auth = require_auth(req, sessions);
    //    if (!auth.ok) return auth.error;

    //    const int limit = 100;

    //    std::vector<json> all;
    //    for (const std::string& part : { chats.dialogs_direct(auth.user_id, limit),
    //                                     groups.dialogs_groups(auth.user_id, limit) })
    //    {
    //        json arr = json::parse(part, nullptr, false);
    //        if (arr.is_discarded() || !arr.is_array()) continue;
    //        for (auto& x : arr) all.push_back(std::move(x));
    //    }

    //    // формат ts одинаковый, поэтому строки сортируются как время
    //    std::stable_sort(all.begin(), all.end(), [](const json& a, const json& b) {
    //        return a.value("ts", std::string()) > b.value("ts", std::string());
    //        });

    //    if (all.size() > (size_t)limit) all.resize(limit);

    //    json out = { {"items", all} };
    //    return Response{ out.dump(-1, ' ', false, json::error_handler_t::replace), "application/json" };
    //}
    Response history(const Request& req)
    {
        auto auth = require_auth(req, sessions);
        if (!auth.ok) return auth.error;

        auto param = [&](const char* k) {
            auto it = req.params.find(k);
            return it == req.params.end() ? std::string() : it->second;
            };

        std::string with = param("with");
        if (with.empty())
            return Response{ R"({"error":"with required"})", "application/json", "400 Bad Request" };

        long long before = 0;
        int limit = 50;
        try
        {
            if (!param("before").empty()) before = std::stoll(param("before"));
            if (!param("limit").empty())  limit = std::clamp(std::stoi(param("limit")), 1, 100);
        }
        catch (...)
        {
            return Response{ R"({"error":"Bad before/limit"})", "application/json", "400 Bad Request" };
        }

        auto other = users.find_by_username(with);
        if (!other) return Response{ R"({"items":[]})", "application/json" };   // как «нет сообщений»

        // выдаются только строки, где ты — одна из сторон, чужую переписку не получить
        return Response{ chats.history(auth.user_id, other->id, before, limit), "application/json" };
    }

    Response create_group(const Request& req)
    {
        auto auth = require_auth(req, sessions);
        if (!auth.ok) return auth.error;

        auto bad = [](const char* msg) {
            json err = { {"error", msg} };
            return Response{ err.dump(), "application/json", "400 Bad Request" };
            };

        // ---------- разбор тела ----------
        json body = json::parse(req.body, nullptr, /*allow_exceptions*/ false);
        if (body.is_discarded() || !body.is_object())
            return bad("Invalid JSON");

        if (!body.contains("name") || !body["name"].is_string())
            return bad("name required");

        std::string name = body["name"].get<std::string>();

        // обрезаем пробелы по краям
        auto b = name.find_first_not_of(" \t\r\n");
        auto e = name.find_last_not_of(" \t\r\n");
        name = (b == std::string::npos) ? std::string() : name.substr(b, e - b + 1);

        if (name.empty())
            return bad("name must not be empty");
        if (name.size() > 100)                       // подгоните под длину колонки ChatGroups.name
            return bad("name too long");

        // ---------- участники ----------
        std::vector<std::string> members;
        if (body.contains("members"))
        {
            if (!body["members"].is_array())
                return bad("members must be an array");

            if (body["members"].size() > 200)
                return bad("too many members");

            for (const auto& m : body["members"])
            {
                if (!m.is_string())
                    return bad("members must be strings");
                std::string login = m.get<std::string>();
                if (!login.empty())
                    members.push_back(std::move(login));
            }
        }
        std::string kind = "group";
        if (body.contains("kind"))
        {
            if (!body["kind"].is_string())
                return bad("kind must be a string");

            kind = body["kind"].get<std::string>();
            if (kind != "group" && kind != "channel")
                return bad("kind must be group or channel");
        }

        // ---------- создание ----------
        // владелец берётся только из сессии, а не из тела запроса
        std::string result = chats.сreate_group(auth.user_id, name, members, kind);

        // репозиторий при сбое возвращает {"error": "..."}
        json parsed = json::parse(result, nullptr, false);
        if (parsed.is_discarded() || parsed.contains("error"))
            return Response{ result, "application/json", "500 Internal Server Error" };

        return Response{ result, "application/json", "201 Created" };
    }

    
    Response upload_attachment(const Request& req)
    {
        auto auth = require_roles(req, sessions, { "admin", "employee", "customer" });
        if (!auth.ok)
            return auth.error;

        auto error = [](const char* msg, const char* status) {
            json err = { {"error", msg} };
            return Response{ err.dump(), "application/json", status };
            };

        auto content_type_it = req.headers.find("content-type");
        if (content_type_it == req.headers.end() ||
            content_type_it->second.find("multipart/form-data") == std::string::npos)
        {
            return error("Expected multipart/form-data", "400 Bad Request");
        }

        constexpr size_t max_size = 10 * 1024 * 1024;

        // Отсекаем огромные тела до разбора multipart (запас на заголовки частей)
        if (req.body.size() > max_size + 64 * 1024)
            return error("File too large", "413 Content Too Large");

        auto boundary = extract_boundary(content_type_it->second);
        if (!boundary)
            return error("Missing boundary", "400 Bad Request");

        auto files = parse_multipart(req.body, *boundary);
        if (files.empty())
            return error("No file uploaded", "400 Bad Request");

        const UploadedFile& file = files[0];

        const std::string content_type = normalize_content_type(file.content_type);

        const FileType* type = find_type_by_mime(content_type);
        if (!type)
            return error("Unsupported file type", "400 Bad Request");

        if (file.data.empty())
            return error("Empty file", "400 Bad Request");

        if (file.data.size() > type->max_size)
            return error("File too large", "413 Content Too Large");

        if (!matches_signature(content_type, file.data.data(), file.data.size()))
            return error("File content does not match its type", "400 Bad Request");


        std::error_code ec;
        std::filesystem::create_directories("uploads/chat", ec);
        if (ec)
            return error("Failed to save file", "500 Internal Server Error");

        // Имя генерируется на сервере, расширение берётся из белого списка,
        // поэтому имя из запроса не может привести к path traversal
        std::string saved_name = generate_filename(type->ext);
        std::string saved_path = "uploads/chat/" + saved_name;

        std::ofstream out(saved_path, std::ios::binary);
        if (!out.is_open())
            return error("Failed to save file", "500 Internal Server Error");

        out.write(file.data.data(), static_cast<std::streamsize>(file.data.size()));
        out.close();

        if (!out)
        {
            std::filesystem::remove(saved_path, ec);
            return error("Failed to write file", "500 Internal Server Error");
        }
        std::string original_name = sanitize_filename(file.filename, saved_name);

        if (!chats.save_upload(saved_name, original_name, content_type, file.data.size(), auth.user_id))
        {
            std::error_code rm_ec;
            std::filesystem::remove(saved_path, rm_ec);       // не оставляем файл без записи в БД
            return error("Failed to save file", "500 Internal Server Error");
        }

        json response = {
            {"status", "uploaded"},
            {"file_url", "/uploads/chat/" + saved_name},
            {"content_type", content_type},
            {"file_size", file.data.size()}
        };

        return Response{ response.dump(), "application/json" };
    }

    Response get_attachment(const Request& req)
    {
        auto it = req.params.find("name");
        if (it == req.params.end())
            return Response{ "Not Found", "text/plain", "404 Not Found" };

        const std::string url = "/uploads/chat/" + it->second;
        if (!is_valid_attachment_url(url))
            return Response{ "Not Found", "text/plain", "404 Not Found" };

        std::ifstream in("uploads/chat/" + it->second, std::ios::binary);
        if (!in)
            return Response{ "Not Found", "text/plain", "404 Not Found" };

        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        const std::string& name = it->second;
        const FileType* type = find_type_by_ext(name.substr(16));   // имя уже проверено is_valid_attachment_url

        const char* ct = (type && type->is_image) ? type->mime : "application/octet-stream";

        return Response{ std::move(data), ct };
    }

    struct GroupCtx
    {
        std::optional<Response> error;
        int user_id = 0;
        int group_id = 0;
        std::string group_name;
        std::string role;
        json body;
    };

    GroupCtx group_ctx(const Request& req)
    {
        GroupCtx c;

        auto auth = require_auth(req, sessions);
        if (!auth.ok) { c.error = auth.error; return c; }
        c.user_id = auth.user_id;

        c.body = json::parse(req.body, nullptr, false);
        if (c.body.is_discarded() || !c.body.is_object())
        {
            c.error = err("Invalid JSON", "400 Bad Request");
            return c;
        }

        c.group_name = trim(str_field(c.body, "group"));
        if (c.group_name.empty())
        {
            c.error = err("group required", "400 Bad Request");
            return c;
        }

        // «нет такой группы» и «вы не участник» отвечаем одинаково
        auto group = groups.find_by_name(c.group_name);
        if (!group)
        {
            c.error = err("Group not found", "404 Not Found");
            return c;
        }

        auto role = groups.get_member_role(group->id, c.user_id);
        if (!role)
        {
            c.error = err("Group not found", "404 Not Found");
            return c;
        }

        c.group_id = group->id;
        c.role = *role;
        return c;
    }

    Response group_add_member(const Request& req)
    {
        auto c = group_ctx(req);
        if (c.error) return *c.error;

        if (c.role != "owner" && c.role != "admin")
            return err("Forbidden", "403 Forbidden");

        std::string login = trim(str_field(c.body, "login"));
        if (login.empty()) return err("login required", "400 Bad Request");

        long rows = groups.add_member(c.group_id, login);
        if (rows < 0) return err("Server error", "500 Internal Server Error");
        if (rows == 0) return err("User not found or already in group", "404 Not Found");

        return ok({ {"status", "added"}, {"group", c.group_name}, {"login", login} });
    }

    Response group_remove_member(const Request& req)
    {
        auto c = group_ctx(req);
        if (c.error) return *c.error;

        if (c.role != "owner" && c.role != "admin")
            return err("Forbidden", "403 Forbidden");

        std::string login = trim(str_field(c.body, "login"));
        if (login.empty()) return err("login required", "400 Bad Request");

        // админ удаляет только обычных участников, владелец и админов тоже
        long rows = groups.remove_member_by_login(c.group_id, login, c.role == "admin");
        if (rows < 0) return err("Server error", "500 Internal Server Error");
        if (rows == 0) return err("Member not found or cannot be removed", "404 Not Found");

        return ok({ {"status", "removed"}, {"group", c.group_name}, {"login", login} });
    }

    Response group_leave(const Request& req)
    {
        auto c = group_ctx(req);
        if (c.error) return *c.error;

        if (c.role == "owner")
            return err("Owner cannot leave. Delete the group instead", "400 Bad Request");

        long rows = groups.remove_member_by_id(c.group_id, c.user_id);
        if (rows < 0) return err("Server error", "500 Internal Server Error");

        return ok({ {"status", "left"}, {"group", c.group_name} });
    }

    Response group_rename(const Request& req)
    {
        auto c = group_ctx(req);
        if (c.error) return *c.error;

        if (c.role != "owner" && c.role != "admin")
            return err("Forbidden", "403 Forbidden");

        std::string name = trim(str_field(c.body, "name"));
        if (name.empty())      return err("name required", "400 Bad Request");
        if (name.size() > 100) return err("name too long", "400 Bad Request");

        auto existing = groups.find_by_name(name);
        if (existing && existing->id != c.group_id)
            return err("Name already taken", "409 Conflict");

        long rows = groups.rename_group(c.group_id, name);
        if (rows < 0) return err("Server error", "500 Internal Server Error");

        return ok({ {"status", "renamed"}, {"old_name", c.group_name}, {"name", name} });
    }

    Response group_set_role(const Request& req)
    {
        auto c = group_ctx(req);
        if (c.error) return *c.error;

        if (c.role != "owner")
            return err("Only owner can change roles", "403 Forbidden");

        std::string login = trim(str_field(c.body, "login"));
        std::string role = str_field(c.body, "role");

        if (login.empty()) return err("login required", "400 Bad Request");
        if (role != "admin" && role != "customer")
            return err("role must be admin or customer", "400 Bad Request");

        long rows = groups.set_role(c.group_id, login, role);
        if (rows < 0) return err("Server error", "500 Internal Server Error");
        if (rows == 0) return err("Member not found", "404 Not Found");

        return ok({ {"status", "role_changed"}, {"group", c.group_name}, {"login", login}, {"role", role} });
    }

    Response group_delete(const Request& req)
    {
        auto c = group_ctx(req);
        if (c.error) return *c.error;

        if (c.role != "owner")
            return err("Only owner can delete the group", "403 Forbidden");

        if (!groups.delete_group(c.group_id))
            return err("Server error", "500 Internal Server Error");

        return ok({ {"status", "deleted"}, {"group", c.group_name} });
    }

    Response sync(const Request& req)
    {
        auto auth = require_auth(req, sessions);
        if (!auth.ok) return auth.error;

        auto param = [&](const char* k) {
            auto it = req.params.find(k);
            return it == req.params.end() ? std::string() : it->second;
            };

        long long dm_after = 0, group_after = 0;
        try
        {
            if (!param("dm_after").empty())    dm_after = std::stoll(param("dm_after"));
            if (!param("group_after").empty()) group_after = std::stoll(param("group_after"));
        }
        catch (...)
        {
            return Response{ R"({"error":"Bad cursor"})", "application/json", "400 Bad Request" };
        }

        // время, с которого искать правки и удаления (пусто = не искать)
        std::string since = param("since");
        if (since.size() > 40 ||
            since.find_first_not_of("0123456789-:.T") != std::string::npos)
            return Response{ R"({"error":"Bad cursor"})", "application/json", "400 Bad Request" };

        // время читаем ДО выборки, чтобы изменения во время запроса попали в следующий раз
        std::string server_time = chats.db_now();

        const int limit = 200;

        json out = {
            {"messages",       json::parse(chats.sync_direct(auth.user_id, dm_after, since, limit))},
            {"group_messages", json::parse(groups.sync_groups(auth.user_id, group_after, since, limit))},
            {"server_time",    server_time}
        };

        return Response{ out.dump(-1, ' ', false, json::error_handler_t::replace), "application/json" };
    }

    struct GroupAccess
    {
        std::optional<Response> error;
        int group_id = 0;
    };

    GroupAccess group_access(const Request& req, int user_id)
    {
        GroupAccess a;

        auto it = req.params.find("name");
        if (it == req.params.end() || it->second.empty())
        {
            a.error = err("group name required", "400 Bad Request");
            return a;
        }

        // «нет такой группы» и «вы не участник» отвечаем одинаково
        auto group = groups.find_by_name(it->second);
        if (!group || !groups.get_member_role(group->id, user_id))
        {
            a.error = err("Group not found", "404 Not Found");
            return a;
        }

        a.group_id = group->id;
        return a;
    }

    Response group_members(const Request& req)
    {
        auto auth = require_auth(req, sessions);
        if (!auth.ok) return auth.error;

        auto acc = group_access(req, auth.user_id);
        if (acc.error) return *acc.error;

        return Response{ groups.members(acc.group_id), "application/json" };
    }

    Response group_history(const Request& req)
    {
        auto auth = require_auth(req, sessions);
        if (!auth.ok) return auth.error;

        auto acc = group_access(req, auth.user_id);
        if (acc.error) return *acc.error;

        auto param = [&](const char* k) {
            auto it = req.params.find(k);
            return it == req.params.end() ? std::string() : it->second;
            };

        long long before = 0;
        int limit = 50;
        try
        {
            if (!param("before").empty()) before = std::stoll(param("before"));
            if (!param("limit").empty())  limit = std::clamp(std::stoi(param("limit")), 1, 100);
        }
        catch (...)
        {
            return err("Bad before/limit", "400 Bad Request");
        }

        return Response{ groups.history(acc.group_id, before, limit), "application/json" };
    }

    Response dialogs(const Request& req)
    {
        auto auth = require_auth(req, sessions);
        if (!auth.ok) return auth.error;

        const int limit = 100;

        std::vector<json> all;
        for (const std::string& part : { chats.dialogs_direct(auth.user_id, limit),
                                         groups.dialogs_groups(auth.user_id, limit) })
        {
            json arr = json::parse(part, nullptr, false);
            if (arr.is_discarded() || !arr.is_array()) continue;
            for (auto& x : arr) all.push_back(std::move(x));
        }

        // формат ts одинаковый, поэтому строки сортируются как время
        std::stable_sort(all.begin(), all.end(), [](const json& a, const json& b) {
            return a.value("ts", std::string()) > b.value("ts", std::string());
            });

        // сумма считается до обрезки, чтобы значок на иконке учитывал все чаты
        long long unread_total = 0;
        for (const auto& x : all)
            unread_total += x.value("unread", 0);

        if (all.size() > (size_t)limit) all.resize(limit);

        json out = { {"items", all}, {"unread_total", unread_total} };
        return Response{ out.dump(-1, ' ', false, json::error_handler_t::replace), "application/json" };
    };

    Response get_attachment_named(const Request& req)
    {
        auto n = req.params.find("name");
        auto f = req.params.find("filename");
        if (n == req.params.end() || f == req.params.end())
            return Response{ "Not Found", "text/plain", "404 Not Found" };

        if (!is_valid_attachment_url("/uploads/chat/" + n->second))
            return Response{ "Not Found", "text/plain", "404 Not Found" };

        // имя в ссылке должно совпасть с сохранённым, иначе по чужой ссылке
        // можно было бы скачать файл под любым именем, например «invoice.exe»
        auto up = chats.find_upload(n->second);
        if (!up || up->original_name != f->second)
            return Response{ "Not Found", "text/plain", "404 Not Found" };

        auto data = read_upload(n->second);
        if (!data)
            return Response{ "Not Found", "text/plain", "404 Not Found" };

        const std::string& name = n->second;
        const char* ct = "application/octet-stream";          // всё, кроме картинок, скачивается
        if (ends_with(name, ".jpg"))       ct = "image/jpeg";
        else if (ends_with(name, ".png"))  ct = "image/png";
        else if (ends_with(name, ".webp")) ct = "image/webp";

        return Response{ std::move(*data), ct };
    };

   
    ChatRepository chats;
    UserRepository users;
    SessionRepository& sessions;
    GroupRepository groups;
};