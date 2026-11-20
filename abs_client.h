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
// Auth, two modes:
//   * API key ("Bearer <key>" header): created in the web UI, long-lived.
//     Persisted via DatabaseManager settings ("abs_server", "abs_token").
//   * Username + password: POST /login yields a token (JWTs expire in ~1h on
//     2.31+), so the client logs in lazily and transparently re-logs in once
//     whenever the server answers 401. Credentials: "abs_user", "abs_pass".
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
	int         num_chapters;
	double      duration;   // seconds
	double      progress;   // seconds already listened (currentTime); <0 = unknown
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
	// Username/password mode: used when no API key is set. Logs in on demand.
	void setCredentials(const std::string& user, const std::string& pass);
	bool isConfigured() const;
	// POST /login. On success the session token is stored for later requests.
	// On failure lastError() holds a short human-readable reason.
	bool login();
	const std::string& lastError() const { return m_last_error; }
	const std::string& server() const { return m_server; }

	// --- connectivity ---
	// GET /status -> { serverVersion: "x.y.z" }
	bool checkServer(std::string& version_out);

	// --- libraries & items ---
	bool getLibraries(std::vector<AbsLibrary>& out);
	// Paginated library listing. search empty = plain list.
	// total_out receives the server-side total item count (-1 when unknown,
	// e.g. for searches). Pass the same variable to subsequent calls.
	bool getItems(const std::string& library_id, int page, int limit,
				  const std::string& search, std::vector<AbsItem>& out,
				  int& total_out);

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

	// Streams a GET response straight into an open fd (progressive writes, no
	// whole-body buffering in a std::string first), for RAM-backed playback
	// buffers (see ram_cache.h). Aborts early once max_bytes is written
	// (max_bytes < 0 = unlimited) so an oversized track can't blow the RAM
	// budget; bytes_written is set even then, so the caller knows how far it
	// got before falling back to a real file.
	bool downloadToFd(const std::string& content_url, int fd, long long max_bytes,
					  long long& bytes_written);

	// --- helpers ---
	// Builds a fully qualified stream URL: server + content_url + "?token=..."
	// Suitable for the existing StreamVFS/wget playback path.
	std::string streamUrl(const std::string& content_url);

	// Absolute URL for a relative content_url (without token), for logs.
	std::string absoluteUrl(const std::string& content_url);

	// Socket timeout (seconds) for API calls. Lower it around operations that
	// must stay quick even when the server is unreachable (e.g. app exit).
	void setHttpTimeout(int seconds) { m_timeout_sec = seconds; }

private:
	// Low-level HTTP. Returns true on 2xx; body always filled on 2xx.
	// out_status receives the numeric status code (or 0 on transport error).
	// http()/httpDownload() add auth handling (lazy login, one retry on 401)
	// around the *Raw variants.
	bool http(const std::string& method, const std::string& path,
			  const std::string& body, std::string& response_body,
			  int& out_status);
	bool httpRaw(const std::string& method, const std::string& path,
				 const std::string& body, std::string& response_body,
				 int& out_status, const char* extra_headers = "");
	bool httpDownload(const std::string& path, const std::string& out_path,
					  int& out_status);
	bool httpDownloadRaw(const std::string& path, const std::string& out_path,
						 int& out_status);
	bool httpDownloadToFdRaw(const std::string& path, int fd, long long max_bytes,
							 long long& bytes_written, int& out_status);
	bool canLogin() const { return m_api_key.empty() && !m_user.empty(); }

	bool parseHost(const std::string& url, std::string& host, int& port);

	std::string m_server;   // e.g. http://192.168.68.104:13378
	std::string m_host;
	int         m_port;
	std::string m_token;    // bearer token in use (API key or login token)
	std::string m_api_key;  // non-empty = API-key mode
	std::string m_user;
	std::string m_pass;
	std::string m_last_error;
	int         m_timeout_sec = 20;   // socket read/write timeout for API calls
};

#endif // ABS_CLIENT_H
