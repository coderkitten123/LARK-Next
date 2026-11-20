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
#include <climits>

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

static bool getContentLength(const std::string& headers, size_t& length, bool& present) {
	present = false;
	std::string normalized = lower(headers);
	size_t pos = normalized.find("\r\ncontent-length:");
	if (pos == std::string::npos) return true;
	pos += 17;
	size_t end = normalized.find("\r\n", pos);
	std::string value = trim(normalized.substr(pos, end == std::string::npos ? end : end - pos));
	if (value.empty()) return false;
	char* endptr = NULL;
	errno = 0;
	unsigned long long parsed = strtoull(value.c_str(), &endptr, 10);
	if (errno == ERANGE || endptr == value.c_str() || *endptr != '\0' || parsed > (unsigned long long)(size_t)-1)
		return false;
	length = (size_t)parsed;
	present = true;
	return true;
}

// Minimal URL encoding for query parameters (space, '&', '=', '+', '%', '#')

static bool sendAll(int fd, const char* data, size_t length) {
	size_t sent = 0;
	while (sent < length) {
		ssize_t n = send(fd, data + sent, length - sent, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR) continue;
			return false;
		}
		if (n == 0) return false;
		sent += (size_t)n;
	}
	return true;
}
static std::string urlEncode(const std::string& s) {
	static const char* hex = "0123456789ABCDEF";
	std::string r;
	r.reserve(s.size());
	for (size_t i = 0; i < s.size(); i++) {
		unsigned char c = (unsigned char)s[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
			r += (char)c;
		} else {
			r += '%';
			r += hex[c >> 4];
			r += hex[c & 15];
		}
	}
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
		std::string port_text = u.substr(colon + 1);
		char* endptr = NULL;
		errno = 0;
		long parsed_port = strtol(port_text.c_str(), &endptr, 10);
		if (errno == ERANGE || endptr == port_text.c_str() || *endptr != '\0' ||
			parsed_port <= 0 || parsed_port >= 65536) return false;
		port = (int)parsed_port;
	} else {
		host = u;
		port = 80;
	}
	return !host.empty();
}

void AbsClient::setServer(const std::string& base_url, const std::string& api_key) {
	m_server = base_url;
	if (m_server.size() && m_server[m_server.size() - 1] == '/')
		m_server.erase(m_server.size() - 1);
	m_api_key = api_key;
	m_token = api_key;
	m_host.clear();
	m_port = 80;
	if (!parseHost(m_server, m_host, m_port)) {
		m_host.clear();
		m_port = 80;
	}
}

void AbsClient::setCredentials(const std::string& user, const std::string& pass) {
	m_user = user;
	m_pass = pass;
	if (m_api_key.empty()) m_token.clear();   // force a fresh login
}

bool AbsClient::isConfigured() const {
	return !m_server.empty() && !m_host.empty() && (!m_token.empty() || canLogin());
}

bool AbsClient::login() {
	m_last_error.clear();
	if (m_user.empty()) { m_last_error = "No username set"; return false; }
	m_token.clear();
	cJSON* req = cJSON_CreateObject();
	cJSON_AddStringToObject(req, "username", m_user.c_str());
	cJSON_AddStringToObject(req, "password", m_pass.c_str());
	char* body = cJSON_PrintUnformatted(req);
	cJSON_Delete(req);
	std::string body_s = body ? body : "";
	free(body);

	std::string resp;
	int status;
	// "x-return-tokens" asks 2.26+ servers for the JWT in the body; older
	// servers ignore it and return the legacy user.token.
	bool ok = httpRaw("POST", "/login", body_s, resp, status, "x-return-tokens: true\r\n");
	if (!ok) {
		if (status == 401 || status == 400) m_last_error = "Wrong username or password";
		else if (status == 0) m_last_error = "Server unreachable";
		else m_last_error = "Login failed (HTTP " + std::to_string(status) + ")";
		return false;
	}
	cJSON* root = cJSON_Parse(resp.c_str());
	cJSON* user = root ? cJSON_GetObjectItemCaseSensitive(root, "user") : NULL;
	std::string tok = jsonGetString(user, "accessToken");
	if (tok.empty()) tok = jsonGetString(user, "token");
	if (root) cJSON_Delete(root);
	if (tok.empty()) { m_last_error = "Server sent no login token"; return false; }
	m_token = tok;
	g_print("AbsClient: logged in as %s\n", m_user.c_str());
	return true;
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

bool AbsClient::httpRaw(const std::string& method, const std::string& path,
						const std::string& body, std::string& response_body,
						int& out_status, const char* extra_headers) {
	response_body.clear();
	out_status = 0;
	if (m_host.empty()) return false;

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return false;

	struct timeval tv;
	tv.tv_sec = m_timeout_sec; tv.tv_usec = 0;
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
	req += extra_headers;
	if (!body.empty()) {
		req += "Content-Type: application/json\r\n";
		req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
	}
	req += "Connection: close\r\n\r\n";
	if (!body.empty()) req += body;

	if (!sendAll(fd, req.data(), req.size())) {
		close(fd);
		return false;
	}

	// Read whole response (Connection: close)
	std::string raw;
	char buf[8192];
	ssize_t n;
	int recv_error = 0;
	for (;;) {
		n = recv(fd, buf, sizeof(buf), 0);
		if (n > 0) raw.append(buf, (size_t)n);
		else if (n == 0) break;
		else if (errno != EINTR) { recv_error = errno; break; }
	}
	close(fd);
	if (recv_error != 0) {
		g_print("AbsClient: %s %s - recv failed (%s)\n", method.c_str(), path.c_str(), strerror(recv_error));
		return false;
	}

	if (raw.empty()) {
		g_print("AbsClient: %s %s - no response (recv timeout/errno %d: %s)\n",
				method.c_str(), path.c_str(), errno, strerror(errno));
		return false;
	}

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
	bool chunked = headers.find("transfer-encoding: chunked") != std::string::npos;
	size_t expected_length = 0;
	bool has_content_length = false;
	if (!chunked && !getContentLength(raw.substr(0, body_start), expected_length, has_content_length))
		return false;
	body_start += 4;
	std::string body_out = raw.substr(body_start);

	// De-chunk if needed
	if (chunked) {
		std::string decoded;
		size_t pos = 0;
		bool complete = false;
		while (pos < body_out.size()) {
			size_t crlf = body_out.find("\r\n", pos);
			if (crlf == std::string::npos) break;
			std::string size_line = body_out.substr(pos, crlf - pos);
			size_t ext = size_line.find(";");
			if (ext != std::string::npos) size_line.resize(ext);
			char* endptr = NULL;
			errno = 0;
			unsigned long chunk_len = strtoul(size_line.c_str(), &endptr, 16);
			if (errno == ERANGE || endptr == size_line.c_str() || *endptr != 0) break;
			pos = crlf + 2;
			if (chunk_len == 0) {
				while (pos <= body_out.size()) {
					size_t trailer_end = body_out.find("\r\n", pos);
					if (trailer_end == std::string::npos) break;
					if (trailer_end == pos) complete = true;
					pos = trailer_end + 2;
					if (complete) break;
				}
				break;
			}
			if (pos > body_out.size() || body_out.size() - pos < 2 ||
				chunk_len > body_out.size() - pos - 2) break;
			decoded.append(body_out, pos, (size_t)chunk_len);
			pos += (size_t)chunk_len;
			if (body_out.compare(pos, 2, "\r\n") != 0) break;
			pos += 2;
		}
		if (!complete) return false;
		response_body = decoded;
	} else {
		if (has_content_length && body_out.size() != expected_length) return false;
		response_body = body_out;
	}

	if (out_status < 200 || out_status >= 300) {
		g_print("AbsClient: %s %s -> HTTP %d\n", method.c_str(), path.c_str(), out_status);
		if (out_status == 401)
			g_print("AbsClient: unauthorized (token rejected)\n");
		return false;
	}
	return true;
}

// Auth wrapper: log in lazily, and once more if the server rejects the token
// (login JWTs are short-lived on newer servers).
bool AbsClient::http(const std::string& method, const std::string& path,
					 const std::string& body, std::string& response_body,
					 int& out_status) {
	if (m_token.empty() && canLogin() && path != "/status") login();   // /status needs no auth
	bool ok = httpRaw(method, path, body, response_body, out_status);
	if (!ok && out_status == 401 && canLogin()) {
		g_print("AbsClient: token rejected, logging in again\n");
		if (login()) ok = httpRaw(method, path, body, response_body, out_status);
	}
	return ok;
}

bool AbsClient::httpDownload(const std::string& path, const std::string& out_path,
							 int& out_status) {
	if (m_token.empty() && canLogin()) login();
	bool ok = httpDownloadRaw(path, out_path, out_status);
	if (!ok && out_status == 401 && canLogin()) {
		if (login()) ok = httpDownloadRaw(path, out_path, out_status);
	}
	return ok;
}

bool AbsClient::httpDownloadRaw(const std::string& path, const std::string& out_path,
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
	if (!sendAll(fd, req.data(), req.size())) {
		g_print("AbsClient: download send failed (%s)\n", strerror(errno));
		close(fd); return false;
	}

	FILE* f = fopen(out_path.c_str(), "wb");
	if (!f) {
		g_print("AbsClient: download fopen %s failed (%s)\n", out_path.c_str(), strerror(errno));
		close(fd); return false;
	}

	// Read header incrementally until \r\n\r\n
	std::string header;
	char buf[16384];
	ssize_t n;
	size_t hb = std::string::npos;
	for (;;) {
		n = recv(fd, buf, sizeof(buf), 0);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) break;
		header.append(buf, n);
		hb = header.find("\r\n\r\n");
		if (hb != std::string::npos) break;
	}
	if (n <= 0 && hb == std::string::npos) {
		// No complete header received
		fclose(f); remove(out_path.c_str()); close(fd);
		g_print("AbsClient: download %s - incomplete response header\n", path.c_str());
		return false;
	}

	size_t sp1 = header.find(' ');
	size_t sp2 = header.find(' ', sp1 + 1);
	if (sp1 != std::string::npos && sp2 != std::string::npos)
		out_status = atoi(header.substr(sp1 + 1, sp2 - sp1 - 1).c_str());
	std::string hdr = lower(header.substr(0, hb));
	bool chunked = hdr.find("transfer-encoding: chunked") != std::string::npos;
	size_t expected_length = 0;
	bool has_content_length = false;
	if (!chunked && !getContentLength(header.substr(0, hb), expected_length, has_content_length)) {
		fclose(f); remove(out_path.c_str()); close(fd);
		g_print("AbsClient: download %s - invalid Content-Length\n", path.c_str());
		return false;
	}
	int recv_error = 0;

	// Collect the remaining body in memory (covers are small, <= a few MB)
	std::string body;
	body.append(header, hb + 4, header.size() - hb - 4);
	for (;;) {
		n = recv(fd, buf, sizeof(buf), 0);
		if (n > 0) body.append(buf, (size_t)n);
		else if (n == 0) break;
		else if (errno != EINTR) { recv_error = errno; break; }
	}
	close(fd);
	if (recv_error != 0) {
		fclose(f); remove(out_path.c_str());
		g_print("AbsClient: download %s - recv failed (%s)\n", path.c_str(), strerror(recv_error));
		return false;
	}

	if (chunked) {
		// Decode chunked framing: <hex-size>\r\n<data>\r\n ... 0\r\n[\r\n]
		std::string decoded;
		size_t pos = 0;
		bool complete = false;
		while (pos < body.size()) {
			size_t eol = body.find("\r\n", pos);
			if (eol == std::string::npos) break;
			std::string size_line = body.substr(pos, eol - pos);
			size_t ext = size_line.find(';');
			if (ext != std::string::npos) size_line.resize(ext);
			char* endptr = NULL;
			errno = 0;
			unsigned long sz = strtoul(size_line.c_str(), &endptr, 16);
			if (errno == ERANGE || endptr == size_line.c_str() || *endptr != '\0') break;
			pos = eol + 2;
			if (sz == 0) {
				while (pos <= body.size()) {
					size_t trailer_end = body.find("\r\n", pos);
					if (trailer_end == std::string::npos) break;
					if (trailer_end == pos) complete = true;
					pos = trailer_end + 2;
					if (complete) break;
				}
				break;
			}
			if (pos > body.size() || body.size() - pos < 2 || sz > body.size() - pos - 2) break;
			decoded.append(body, pos, (size_t)sz);
			pos += (size_t)sz;
			if (body.compare(pos, 2, "\r\n") != 0) break;
			pos += 2;
		}
		if (!complete) {
			fclose(f); remove(out_path.c_str());
			g_print("AbsClient: download %s - malformed chunked body\n", path.c_str());
			return false;
		}
		body.swap(decoded);
	} else if (has_content_length && body.size() != expected_length) {
		fclose(f); remove(out_path.c_str());
		g_print("AbsClient: download %s - incomplete body (%zu/%zu bytes)\n",
				path.c_str(), body.size(), expected_length);
		return false;
	}

	size_t wrote = fwrite(body.data(), 1, body.size(), f);
	fclose(f);
	if (wrote != body.size()) {
		g_print("AbsClient: download short write %zu/%zu (%s)\n",
				wrote, body.size(), strerror(errno));
		remove(out_path.c_str());
		return false;
	}

	if (out_status < 200 || out_status >= 300) {
		remove(out_path.c_str());
		g_print("AbsClient: download %s -> HTTP %d\n", path.c_str(), out_status);
		return false;
	}
	return true;
}

bool AbsClient::downloadToFd(const std::string& content_url, int fd, long long max_bytes,
							 long long& bytes_written) {
	if (m_token.empty() && canLogin()) login();
	int status = 0;
	bytes_written = 0;
	bool ok = httpDownloadToFdRaw(content_url, fd, max_bytes, bytes_written, status);
	if (!ok && status == 401 && canLogin()) {
		if (login()) {
			if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0) {
				g_print("AbsClient: cannot reset download fd for authentication retry (%s)\n", strerror(errno));
				return false;
			}
			bytes_written = 0;
			ok = httpDownloadToFdRaw(content_url, fd, max_bytes, bytes_written, status);
		}
	}
	return ok;
}

// Streams a GET response directly into `fd` as bytes arrive, instead of
// buffering the whole body in memory first (the pattern httpDownloadRaw
// above uses - fine for small covers, wasteful for an hours-long audio
// track). Used for the RAM-backed playback buffers in ram_cache.h: `fd` is
// normally a memfd, so these writes never touch the eMMC either way.
bool AbsClient::httpDownloadToFdRaw(const std::string& path, int fd, long long max_bytes,
									long long& bytes_written, int& out_status) {
	bytes_written = 0;
	out_status = 0;
	if (m_host.empty()) return false;

	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) return false;
	struct timeval tv;
	tv.tv_sec = 60; tv.tv_usec = 0;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	struct hostent* he = gethostbyname(m_host.c_str());
	if (!he) { close(sock); return false; }
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(m_port);
	memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));
	if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(sock); return false; }

	std::string req = "GET " + path + " HTTP/1.1\r\n";
	req += "Host: " + m_host + (m_port == 80 ? "" : (":" + std::to_string(m_port))) + "\r\n";
	if (!m_token.empty()) req += "Authorization: Bearer " + m_token + "\r\n";
	req += "Accept-Encoding: identity\r\n";
	req += "Connection: close\r\n\r\n";
	if (!sendAll(sock, req.data(), req.size())) { close(sock); return false; }

	// Buffer only the header (small); the body is written out as it arrives.
	std::string buf;
	char tmp[16384];
	ssize_t n;
	size_t header_end = std::string::npos;
	while (header_end == std::string::npos) {
		n = recv(sock, tmp, sizeof(tmp), 0);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) { close(sock); return false; }
		buf.append(tmp, n);
		header_end = buf.find("\r\n\r\n");
		if (header_end == std::string::npos && buf.size() > 65536) { close(sock); return false; }
	}
	std::string header = buf.substr(0, header_end);
	std::string pending = buf.substr(header_end + 4);   // body bytes already read

	size_t sp1 = header.find(' ');
	size_t sp2 = header.find(' ', sp1 + 1);
	if (sp1 != std::string::npos && sp2 != std::string::npos)
		out_status = atoi(header.substr(sp1 + 1, sp2 - sp1 - 1).c_str());
	bool chunked = lower(header).find("transfer-encoding: chunked") != std::string::npos;
	size_t expected_length = 0;
	bool has_content_length = false;
	if (!chunked && !getContentLength(header, expected_length, has_content_length)) {
		close(sock);
		return false;
	}

	bool ok = true;
	auto write_all = [&](const char* data, size_t len) -> bool {
		if (len > (size_t)LLONG_MAX ||
			(max_bytes >= 0 && ((long long)len > max_bytes - bytes_written)) ||
			(!chunked && has_content_length &&
			 (bytes_written > (long long)expected_length ||
			  (long long)len > (long long)expected_length - bytes_written))) return false;
		size_t off = 0;
		while (off < len) {
			ssize_t w = write(fd, data + off, len - off);
			if (w < 0 && errno == EINTR) continue;
			if (w <= 0) return false;
			off += (size_t)w;
		}
		bytes_written += (long long)len;
		return true;
	};

	if (!chunked) {
		if (!pending.empty() && !write_all(pending.data(), pending.size())) ok = false;
		while (ok) {
			n = recv(sock, tmp, sizeof(tmp), 0);
			if (n < 0 && errno == EINTR) continue;
			if (n < 0) { ok = false; break; }
			if (n == 0) break;   // EOF: done
			if (!write_all(tmp, (size_t)n)) { ok = false; break; }
		}
		if (ok && has_content_length &&
			(unsigned long long)bytes_written != (unsigned long long)expected_length) ok = false;
	} else {
		// Chunked framing: "<hex-size>\r\n<data>\r\n" ... "0\r\n[trailer]\r\n"
		bool complete = false;
		for (;;) {
			size_t eol = pending.find("\r\n");
			while (eol == std::string::npos) {
				n = recv(sock, tmp, sizeof(tmp), 0);
				if (n < 0 && errno == EINTR) continue;
				if (n <= 0 || pending.size() > 65536) { ok = false; break; }
				pending.append(tmp, n);
				eol = pending.find("\r\n");
			}
			if (!ok) break;
			std::string size_line = pending.substr(0, eol);
			size_t ext = size_line.find(';');
			if (ext != std::string::npos) size_line.resize(ext);
			char* endptr = NULL;
			errno = 0;
			unsigned long sz = strtoul(size_line.c_str(), &endptr, 16);
			if (errno == ERANGE || endptr == size_line.c_str() || *endptr != '\0') { ok = false; break; }
			if (max_bytes >= 0 &&
				((unsigned long long)sz > (unsigned long long)(max_bytes - bytes_written))) {
				ok = false;
				break;
			}
			pending.erase(0, eol + 2);
			if (sz == 0) {
				for (;;) {
					eol = pending.find("\r\n");
					while (eol == std::string::npos) {
						n = recv(sock, tmp, sizeof(tmp), 0);
						if (n < 0 && errno == EINTR) continue;
						if (n <= 0 || pending.size() > 65536) { ok = false; break; }
						pending.append(tmp, n);
						eol = pending.find("\r\n");
					}
					if (!ok) break;
					if (eol == 0) { complete = true; break; }
					pending.erase(0, eol + 2);
				}
				break;
			}
			if (sz > pending.max_size() - 2) { ok = false; break; }
			while (pending.size() < (size_t)sz + 2) {
				n = recv(sock, tmp, sizeof(tmp), 0);
				if (n < 0 && errno == EINTR) continue;
				if (n <= 0) { ok = false; break; }
				pending.append(tmp, n);
			}
			if (!ok) break;
			if (pending.compare((size_t)sz, 2, "\r\n") != 0 ||
				!write_all(pending.data(), (size_t)sz)) { ok = false; break; }
			pending.erase(0, (size_t)sz + 2);   // drop payload + trailing CRLF
		}
		if (!complete) ok = false;
	}

	close(sock);
	if (out_status < 200 || out_status >= 300) ok = false;
	return ok;
}

// ---------------------------------------------------------------------------------
// API endpoints
// ---------------------------------------------------------------------------------

bool AbsClient::checkServer(std::string& version_out) {
	std::string resp;
	int status;
	if (!http("GET", "/status", "", resp, status)) {
		g_print("AbsClient: checkServer http failed (status %d)\n", status);
		return false;
	}
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
						 const std::string& search, std::vector<AbsItem>& out,
						 int& total_out) {
	out.clear();
	total_out = -1;
	std::string resp;
	int status;
	bool ok;
	if (!search.empty()) {
		// Server-side search; results come in a different shape
		std::string path = "/api/libraries/" + library_id + "/search?q=" + urlEncode(search);
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
				it.duration = 0; it.progress = -1; it.num_tracks = 0; it.num_chapters = 0;
				it.finished = false;
				if (media) {
					it.duration = jsonGetNumber(media, "duration");
					it.num_tracks = (int)jsonGetNumber(media, "numTracks");
					it.num_chapters = (int)jsonGetNumber(media, "numChapters");
				}
				if (!it.id.empty()) out.push_back(it);
			}
		}
		cJSON_Delete(root);
		total_out = (int)out.size();
		return true;
	}

	std::string path = "/api/libraries/" + library_id +
	"/items?limit=" + std::to_string(limit) +
	"&page=" + std::to_string(page) + "&sort=media.metadata.title";
	ok = http("GET", path, "", resp, status);
	if (!ok) return false;
	cJSON* root = cJSON_Parse(resp.c_str());
	if (!root) return false;
	total_out = (int)jsonGetNumber(root, "total", -1);
	cJSON* results = cJSON_GetObjectItemCaseSensitive(root, "results");
	cJSON* r;
	cJSON_ArrayForEach(r, results) {
		AbsItem it;
		it.duration = 0; it.progress = 0; it.num_tracks = 0; it.num_chapters = 0;
		it.finished = false;
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
		// Books never listened to have no progress record: fall back to the
		// media metadata duration so the detail card can show the total length
		if (it.duration <= 0 && media)
			it.duration = jsonGetNumber(media, "duration");
		if (media) {
			it.num_tracks = (int)jsonGetNumber(media, "numTracks");
			it.num_chapters = (int)jsonGetNumber(media, "numChapters");
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
	if (session_id.empty() || tracks.empty()) {
		if (!session_id.empty()) closeSession(session_id);
		session_id.clear();
		tracks.clear();
		return false;
	}
	return true;
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
	int status = 0;
	// Closing without a body leaves the last synced position intact. Sending
	// zeros here is interpreted as a final sync and can reset progress to 0.
	// Treat an already-closed session as success so startup recovery is repeatable.
	bool ok = http("POST", "/api/session/" + session_id + "/close", "", resp, status);
	return ok || status == 404;
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
	g_print("ABS: downloading %s\n", content_url.substr(0, 90).c_str());
	return httpDownload(content_url, out_path, status);
}
