// =================================================================================
// AbsClient implementation (Stage 1)
// Minimal HTTP/1.1 client over POSIX sockets + cJSON-based JSON parsing.
// =================================================================================

#include "abs_client.h"
#include <glib.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

extern "C" {
#include "third_party/cJSON/cJSON.h"
}

// ---------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string lower(const std::string& s) {
    std::string r = s;
    for (size_t i = 0; i < r.size(); i++) r[i] = (char)tolower((unsigned char)r[i]);
    return r;
}

static std::string jsonGetString(const cJSON* obj, const char* key, const char* fallback = "") {
    if (!obj) return fallback;
    cJSON* n = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(n) && n->valuestring) return n->valuestring;
    return fallback;
}

static double jsonGetNumber(const cJSON* obj, const char* key, double fallback = 0.0) {
    if (!obj) return fallback;
    cJSON* n = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(n)) return n->valuedouble;
    return fallback;
}

static bool jsonGetBool(const cJSON* obj, const char* key, bool fallback = false) {
    if (!obj) return fallback;
    cJSON* n = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(n)) return cJSON_IsTrue(n) != 0;
    return fallback;
}

// ---------------------------------------------------------------------------------
// Construction / configuration
// ---------------------------------------------------------------------------------

AbsClient::AbsClient() : m_port(80) {}

AbsClient::~AbsClient() {}

bool AbsClient::parseHost(const std::string& url, std::string& host, int& port) {
    // accept http://host:port[/...] (https:// not supported on LAN HTTP path)
    std::string u = url;
    if (u.compare(0, 7, "http://") == 0) u = u.substr(7);
    else if (u.compare(0, 8, "https://") == 0) {
        u = u.substr(8);
        g_print("AbsClient: WARNING https:// configured but only plain http is supported\n");
        return false;
    } else {
        g_print("AbsClient: server URL must start with http://\n");
        return false;
    }
    size_t slash = u.find('/');
    if (slash != std::string::npos) u = u.substr(0, slash);
    size_t colon = u.rfind(':');
    if (colon != std::string::npos) {
        host = u.substr(0, colon);
        port = atoi(u.substr(colon + 1).c_str());
    } else {
        host = u;
        port = 80;
    }
    return !host.empty() && port > 0 && port < 65536;
}

void AbsClient::setServer(const std::string& base_url, const std::string& api_key) {
    m_server = base_url;
    if (m_server.size() && m_server[m_server.size() - 1] == '/')
        m_server.erase(m_server.size() - 1);
    m_token = api_key;
    parseHost(m_server, m_host, m_port);
}

bool AbsClient::isConfigured() const {
    return !m_server.empty() && !m_token.empty();
}

std::string AbsClient::absoluteUrl(const std::string& content_url) {
    if (content_url.compare(0, 7, "http://") == 0) return content_url;
    return m_server + content_url;
}

std::string AbsClient::streamUrl(const std::string& content_url) {
    std::string url = absoluteUrl(content_url);
    url += (url.find('?') == std::string::npos ? "?" : "&");
    url += "token=" + m_token;
    return url;
}

// ---------------------------------------------------------------------------------
// HTTP core
// ---------------------------------------------------------------------------------

bool AbsClient::http(const std::string& method, const std::string& path,
                     const std::string& body, std::string& response_body,
                     int& out_status) {
    response_body.clear();
    out_status = 0;
    if (m_host.empty()) return false;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct timeval tv;
    tv.tv_sec = 20; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct hostent* he = gethostbyname(m_host.c_str());
    if (!he) { close(fd); return false; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        g_print("AbsClient: connect to %s:%d failed (%s)\n", m_host.c_str(), m_port, strerror(errno));
        close(fd);
        return false;
    }

    std::string req = method + " " + path + " HTTP/1.1\r\n";
    req += "Host: " + m_host + (m_port == 80 ? "" : (":" + std::to_string(m_port))) + "\r\n";
    if (!m_token.empty()) req += "Authorization: Bearer " + m_token + "\r\n";
    req += "Accept-Encoding: identity\r\n";
    req += "User-Agent: LARKPlayer-ABS/0.1\r\n";
    if (!body.empty()) {
        req += "Content-Type: application/json\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "Connection: close\r\n\r\n";
    if (!body.empty()) req += body;

    if (send(fd, req.data(), req.size(), 0) < 0) {
        close(fd);
        return false;
    }

    // Read whole response (Connection: close)
    std::string raw;
    char buf[8192];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) raw.append(buf, n);
    close(fd);

    // Parse status line
    size_t line_end = raw.find("\r\n");
    if (line_end == std::string::npos) return false;
    // "HTTP/1.1 200 OK"
    {
        size_t sp1 = raw.find(' ');
        size_t sp2 = raw.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
        out_status = atoi(raw.substr(sp1 + 1, sp2 - sp1 - 1).c_str());
    }

    // Split headers/body
    size_t body_start = raw.find("\r\n\r\n");
    if (body_start == std::string::npos) return false;
    std::string headers = lower(raw.substr(0, body_start));
    body_start += 4;
    std::string body_out = raw.substr(body_start);

    // De-chunk if needed
    if (headers.find("transfer-encoding: chunked") != std::string::npos) {
        std::string decoded;
        size_t pos = 0;
        while (pos < body_out.size()) {
            size_t crlf = body_out.find("\r\n", pos);
            if (crlf == std::string::npos) break;
            int chunk_len = (int)strtol(body_out.substr(pos, crlf - pos).c_str(), NULL, 16);
            if (chunk_len <= 0) break;
            size_t data_start = crlf + 2;
            if (data_start + chunk_len > body_out.size()) break;
            decoded.append(body_out, data_start, chunk_len);
            pos = data_start + chunk_len + 2;
        }
        response_body = decoded;
    } else {
        response_body = body_out;
    }

    if (out_status < 200 || out_status >= 300) {
        g_print("AbsClient: %s %s -> HTTP %d\n", method.c_str(), path.c_str(), out_status);
        if (out_status == 401)
            g_print("AbsClient: unauthorized - check the API token\n");
        return false;
    }
    return true;
}

bool AbsClient::httpDownload(const std::string& path, const std::string& out_path,
                             int& out_status) {
    out_status = 0;
    if (m_host.empty()) return false;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct timeval tv;
    tv.tv_sec = 60; tv.tv_usec = 0;   // covers may be big / NAS-backed
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct hostent* he = gethostbyname(m_host.c_str());
    if (!he) { close(fd); return false; }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return false; }

    std::string req = "GET " + path + " HTTP/1.1\r\n";
    req += "Host: " + m_host + (m_port == 80 ? "" : (":" + std::to_string(m_port))) + "\r\n";
    if (!m_token.empty()) req += "Authorization: Bearer " + m_token + "\r\n";
    req += "Accept-Encoding: identity\r\n";
    req += "Connection: close\r\n\r\n";
    if (send(fd, req.data(), req.size(), 0) < 0) { close(fd); return false; }

    FILE* f = fopen(out_path.c_str(), "wb");
    if (!f) { close(fd); return false; }

    // Read header incrementally until \r\n\r\n, writing remaining bytes to file
    std::string header;
    bool header_done = false;
    char buf[16384];
    ssize_t n;
    while (!header_done && (n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        header.append(buf, n);
        size_t hb = header.find("\r\n\r\n");
        if (hb != std::string::npos) {
            header_done = true;
            size_t sp1 = header.find(' ');
            size_t sp2 = header.find(' ', sp1 + 1);
            if (sp1 != std::string::npos && sp2 != std::string::npos)
                out_status = atoi(header.substr(sp1 + 1, sp2 - sp1 - 1).c_str());
            std::string hdr = lower(header.substr(0, hb));
            if (hdr.find("transfer-encoding: chunked") == std::string::npos) {
                fwrite(header.data() + hb + 4, 1, header.size() - hb - 4, f);
            }
        }
    }
    // Continue payload (non-chunked assumed for media endpoints)
    while (n > 0 && (n = recv(fd, buf, sizeof(buf), 0)) > 0)
        fwrite(buf, 1, n, f);

    fclose(f);
    close(fd);

    if (out_status < 200 || out_status >= 300) {
        remove(out_path.c_str());
        g_print("AbsClient: download %s -> HTTP %d\n", path.c_str(), out_status);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------
// API endpoints
// ---------------------------------------------------------------------------------

bool AbsClient::checkServer(std::string& version_out) {
    std::string resp;
    int status;
    if (!http("GET", "/status", "", resp, status)) return false;
    cJSON* root = cJSON_Parse(resp.c_str());
    if (!root) return false;
    version_out = jsonGetString(root, "serverVersion");
    cJSON_Delete(root);
    return !version_out.empty();
}

bool AbsClient::getLibraries(std::vector<AbsLibrary>& out) {
    out.clear();
    std::string resp;
    int status;
    if (!http("GET", "/api/libraries", "", resp, status)) return false;
    cJSON* root = cJSON_Parse(resp.c_str());
    if (!root) return false;
    cJSON* libs = cJSON_GetObjectItemCaseSensitive(root, "libraries");
    cJSON* lib;
    cJSON_ArrayForEach(lib, libs) {
        AbsLibrary l;
        l.id = jsonGetString(lib, "id");
        l.name = jsonGetString(lib, "name");
        if (!l.id.empty()) out.push_back(l);
    }
    cJSON_Delete(root);
    return true;
}

bool AbsClient::getItems(const std::string& library_id, int page, int limit,
                         const std::string& search, std::vector<AbsItem>& out) {
    out.clear();
    std::string resp;
    int status;
    bool ok;
    if (!search.empty()) {
        // Server-side search; results come in a different shape
        std::string path = "/api/libraries/" + library_id + "/search?q=" + search;
        ok = http("GET", path, "", resp, status);
        if (!ok) return false;
        cJSON* root = cJSON_Parse(resp.c_str());
        if (!root) return false;
        cJSON* books = cJSON_GetObjectItemCaseSensitive(root, "book");
        if (cJSON_IsArray(books)) {
            cJSON* b;
            cJSON_ArrayForEach(b, books) {
                cJSON* lib_item = cJSON_GetObjectItemCaseSensitive(b, "libraryItem");
                if (!lib_item) continue;
                AbsItem it;
                it.id = jsonGetString(lib_item, "id");
                cJSON* media = cJSON_GetObjectItemCaseSensitive(lib_item, "media");
                cJSON* meta = media ? cJSON_GetObjectItemCaseSensitive(media, "metadata") : NULL;
                it.title = meta ? jsonGetString(meta, "title") : "";
                it.author = meta ? jsonGetString(meta, "authorName") : "";
                it.duration = 0; it.progress = 0; it.num_tracks = 0; it.finished = false;
                if (!it.id.empty()) out.push_back(it);
            }
        }
        cJSON_Delete(root);
        return true;
    }

    std::string path = "/api/libraries/" + library_id +
                       "/items?limit=" + std::to_string(limit) +
                       "&page=" + std::to_string(page) + "&sort=media.metadata.title";
    ok = http("GET", path, "", resp, status);
    if (!ok) return false;
    cJSON* root = cJSON_Parse(resp.c_str());
    if (!root) return false;
    cJSON* results = cJSON_GetObjectItemCaseSensitive(root, "results");
    cJSON* r;
    cJSON_ArrayForEach(r, results) {
        AbsItem it;
        it.id = jsonGetString(r, "id");
        cJSON* media = cJSON_GetObjectItemCaseSensitive(r, "media");
        cJSON* meta = media ? cJSON_GetObjectItemCaseSensitive(media, "metadata") : NULL;
        it.title = meta ? jsonGetString(meta, "title") : "";
        it.author = meta ? jsonGetString(meta, "authorName") : "";
        cJSON* prog = cJSON_GetObjectItemCaseSensitive(r, "userMediaProgress");
        if (!prog) prog = cJSON_GetObjectItemCaseSensitive(r, "mediaProgress");
        if (prog) {
            it.progress = jsonGetNumber(prog, "currentTime");
            it.duration = jsonGetNumber(prog, "duration");
            it.finished = jsonGetBool(prog, "isFinished");
        }
        if (!it.id.empty()) out.push_back(it);
    }
    cJSON_Delete(root);
    return true;
}

bool AbsClient::getItemDetail(const std::string& item_id,
                              std::vector<AbsChapter>& chapters,
                              std::vector<AbsTrack>& tracks,
                              double& current_time, double& user_duration) {
    chapters.clear();
    tracks.clear();
    current_time = 0;
    user_duration = 0;
    std::string resp;
    int status;
    std::string path = "/api/items/" + item_id + "?expanded=1&include=progress";
    if (!http("GET", path, "", resp, status)) return false;
    cJSON* root = cJSON_Parse(resp.c_str());
    if (!root) return false;

    cJSON* media = cJSON_GetObjectItemCaseSensitive(root, "media");
    cJSON* ch;
    if (media) {
        cJSON* chs = cJSON_GetObjectItemCaseSensitive(media, "chapters");
        cJSON_ArrayForEach(ch, chs) {
            AbsChapter c;
            c.start = jsonGetNumber(ch, "start");
            c.end = jsonGetNumber(ch, "end");
            c.title = jsonGetString(ch, "title");
            chapters.push_back(c);
        }
        cJSON* ts = cJSON_GetObjectItemCaseSensitive(media, "tracks");
        cJSON* t;
        cJSON_ArrayForEach(t, ts) {
            AbsTrack tr;
            tr.index = (int)jsonGetNumber(t, "index");
            tr.start_offset = jsonGetNumber(t, "startOffset");
            tr.duration = jsonGetNumber(t, "duration");
            tr.content_url = jsonGetString(t, "contentUrl");
            tr.mime_type = jsonGetString(t, "mimeType");
            if (!tr.content_url.empty()) tracks.push_back(tr);
        }
    }
    cJSON* prog = cJSON_GetObjectItemCaseSensitive(root, "userMediaProgress");
    if (prog) {
        current_time = jsonGetNumber(prog, "currentTime");
        user_duration = jsonGetNumber(prog, "duration");
    }
    cJSON_Delete(root);
    return true;
}

bool AbsClient::getCover(const std::string& item_id, int width, const std::string& out_path) {
    int status;
    std::string path = "/api/items/" + item_id + "/cover?width=" +
                       std::to_string(width) + "&format=jpeg";
    return httpDownload(path, out_path, status);
}

bool AbsClient::startPlayback(const std::string& item_id, std::string& session_id,
                              std::vector<AbsTrack>& tracks) {
    tracks.clear();
    session_id.clear();
    std::string resp;
    int status;
    std::string body = "{\"deviceInfo\":{\"clientName\":\"LARKPlayer\",\"clientVersion\":\"0.1\"},"
                       "\"supportedMimeTypes\":[\"audio/mpeg\"]}";
    if (!http("POST", "/api/items/" + item_id + "/play", body, resp, status)) return false;
    cJSON* root = cJSON_Parse(resp.c_str());
    if (!root) return false;
    session_id = jsonGetString(root, "id");
    cJSON* ts = cJSON_GetObjectItemCaseSensitive(root, "audioTracks");
    cJSON* t;
    cJSON_ArrayForEach(t, ts) {
        AbsTrack tr;
        tr.index = (int)jsonGetNumber(t, "index");
        tr.start_offset = jsonGetNumber(t, "startOffset");
        tr.duration = jsonGetNumber(t, "duration");
        tr.content_url = jsonGetString(t, "contentUrl");
        tr.mime_type = jsonGetString(t, "mimeType");
        if (!tr.content_url.empty()) tracks.push_back(tr);
    }
    cJSON_Delete(root);
    return !session_id.empty();
}

bool AbsClient::syncSession(const std::string& session_id, double current_time,
                            double time_listened, double duration) {
    if (session_id.empty()) return false;
    std::string resp;
    int status;
    char body[256];
    snprintf(body, sizeof(body), "{\"currentTime\":%.2f,\"timeListened\":%.2f,\"duration\":%.2f}",
             current_time, time_listened, duration);
    return http("POST", "/api/session/" + session_id + "/sync", body, resp, status);
}

bool AbsClient::closeSession(const std::string& session_id) {
    if (session_id.empty()) return true;
    std::string resp;
    int status;
    return http("POST", "/api/session/" + session_id + "/close",
                "{\"currentTime\":0,\"timeListened\":0,\"duration\":0}", resp, status);
}

bool AbsClient::setItemProgress(const std::string& item_id, double current_time, double duration) {
    std::string resp;
    int status;
    char body[256];
    snprintf(body, sizeof(body), "{\"currentTime\":%.2f,\"duration\":%.2f}",
             current_time, duration);
    return http("PATCH", "/api/me/progress/" + item_id, body, resp, status);
}

bool AbsClient::downloadFile(const std::string& content_url, const std::string& out_path) {
    int status;
    return httpDownload(content_url, out_path, status);
}
