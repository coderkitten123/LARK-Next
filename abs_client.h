#ifndef ABS_CLIENT_H
#define ABS_CLIENT_H

#include <string>
#include <vector>

// =================================================================================
// AbsClient - Audiobookshelf REST API client (Stage 1)
//
// Talks plain HTTP over POSIX sockets (no TLS needed: LAN-only usage pattern).
// JSON handled by vendored cJSON (third_party/cJSON).
//
// Auth: Audiobookshelf API key ("Bearer <key>" header). Keys are created in the
// web UI and are long-lived (unlike login JWTs which expire in ~1h on 2.31+).
// The key is persisted via DatabaseManager settings ("abs_server", "abs_token").
// =================================================================================

struct AbsLibrary {
    std::string id;
    std::string name;
};

struct AbsItem {
    std::string id;
    std::string title;
    std::string author;
    int         num_tracks;
    double      duration;   // seconds
    double      progress;   // seconds already listened (currentTime)
    bool        finished;
};

struct AbsChapter {
    double      start;      // seconds
    double      end;        // seconds
    std::string title;
};

struct AbsTrack {
    int         index;      // 1-based
    double      start_offset; // seconds from book start
    double      duration;   // seconds
    std::string content_url;  // relative, e.g. /api/items/<id>/file/<ino>
    std::string mime_type;
};

class AbsClient {
public:
    AbsClient();
    ~AbsClient();

    // --- configuration (persisted) ---
    void setServer(const std::string& base_url, const std::string& api_key);
    bool isConfigured() const;
    const std::string& server() const { return m_server; }

    // --- connectivity ---
    // GET /status -> { serverVersion: "x.y.z" }
    bool checkServer(std::string& version_out);

    // --- libraries & items ---
    bool getLibraries(std::vector<AbsLibrary>& out);
    // Paginated library listing. search empty = plain list.
    bool getItems(const std::string& library_id, int page, int limit,
                  const std::string& search, std::vector<AbsItem>& out);

    // Full detail: chapters, tracks, progress for one item.
    bool getItemDetail(const std::string& item_id,
                       std::vector<AbsChapter>& chapters,
                       std::vector<AbsTrack>& tracks,
                       double& current_time, double& user_duration);

    // --- media ---
    // Downloads the cover endpoint JPEG to out_path. Server-side resize.
    bool getCover(const std::string& item_id, int width, const std::string& out_path);

    // Starts a playback session and fills audio tracks. Always asks for MP3.
    bool startPlayback(const std::string& item_id, std::string& session_id,
                       std::vector<AbsTrack>& tracks);

    // --- progress sync ---
    // Streaming-session sync (throttled by caller). Returns false on error.
    bool syncSession(const std::string& session_id, double current_time,
                     double time_listened, double duration);
    bool closeSession(const std::string& session_id);
    // Simple progress write (no session): PATCH /api/me/progress/<itemId>
    bool setItemProgress(const std::string& item_id, double current_time, double duration);

    // --- downloads (Stage 5 groundwork) ---
    // Fetches any server path (with token auth) to a local file.
    bool downloadFile(const std::string& content_url, const std::string& out_path);

    // --- helpers ---
    // Builds a fully qualified stream URL: server + content_url + "?token=..."
    // Suitable for the existing StreamVFS/wget playback path.
    std::string streamUrl(const std::string& content_url);

    // Absolute URL for a relative content_url (without token), for logs.
    std::string absoluteUrl(const std::string& content_url);

private:
    // Low-level HTTP. Returns true on 2xx; body always filled on 2xx.
    // out_status receives the numeric status code (or 0 on transport error).
    bool http(const std::string& method, const std::string& path,
              const std::string& body, std::string& response_body,
              int& out_status);
    bool httpDownload(const std::string& path, const std::string& out_path,
                      int& out_status);

    bool parseHost(const std::string& url, std::string& host, int& port);

    std::string m_server;   // e.g. http://192.168.68.104:13378
    std::string m_host;
    int         m_port;
    std::string m_token;    // API key
};

#endif // ABS_CLIENT_H
