#!/usr/bin/env python3
"""Fake Audiobookshelf server for the LARKPlayer host preview.

usage: fake_abs_server.py [PORT=8099] [TOKEN_TTL_SECONDS=3600]
env:   FAKE_ABS_TOKEN_TTL overrides the TTL default (arg wins).
       FAKE_ABS_SORT=1 honours ?sort=media.metadata.title (default: fixed order, Harry Potter first).
Auth:  static API key 'key123', or a token from POST /login (user liam / secret).
       /login tokens expire after TTL seconds -> HTTP 401 afterwards.
"""
import io, json, os, re, sys, threading, time, hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8099
TTL = float(sys.argv[2]) if len(sys.argv) > 2 else float(os.environ.get("FAKE_ABS_TOKEN_TTL", "3600"))
API_KEY = "key123"
USER, PASSWORD = "liam", "secret"
LIB_ID = "lib_audiobooks"

_lock = threading.Lock()
_tokens = {}          # token -> expiry (epoch)
_token_n = [0]
_sessions = {}        # session id -> item id
_sess_n = [0]
_progress = {}        # item id -> {currentTime, duration, isFinished}
_cover_cache = {}

H = 3600
BOOKS = [
    # (title, author, hours, chapters, tracks, portrait)
    ("Harry Potter and the Chamber Of Secrets (full-cast edition)", "J.K. Rowling", 46805 / 3600.0, 19, 1, False),
    ("Treasure Island", "Robert Louis Stevenson", 7.2, 34, 1, False),
    ("The Hobbit", "J.R.R. Tolkien", 11.0, 19, 2, True),
    ("Pride and Prejudice", "Jane Austen", 11.6, 61, 1, False),
    ("A Tale of Two Cities", "Charles Dickens", 13.4, 45, 3, False),
    ("Dune", "Frank Herbert", 21.2, 48, 1, False),
    ("The Extraordinarily Long and Winding Title of a Book About Everything That Ever Happened "
     "Anywhere, Including Several Subtitles: A Complete and Unabridged Reading in Many Parts",
     "Prof. Bartholomew Featherstonehaugh-Wetherby III", 17.5, 12, 1, False),
    ("Moby Dick; or, The Whale", "Herman Melville", 24.6, 135, 1, True),
    ("The Complete Sherlock Holmes", "Arthur Conan Doyle", 30.0, 56, 4, False),
    ("Frankenstein", "Mary Shelley", 8.4, 24, 1, False),
    ("Dracula", "Bram Stoker", 15.3, 27, 1, False),
    ("Alice's Adventures in Wonderland", "Lewis Carroll", 3.0, 12, 1, False),
    ("Nineteen Eighty-Four", "George Orwell", 11.4, 3, 1, True),
    ("War and Peace", "Leo Tolstoy", 30.0, 365, 6, False),
]
PROG_SPEC = {  # index -> (fraction, finished)
    0: (0.25, False), 2: (0.5, False), 4: (1.0, True), 7: (0.07, False), 11: (0.9, False),
}


def make_items():
    items = []
    for i, (title, author, hours, nch, ntr, portrait) in enumerate(BOOKS):
        dur = round(hours * H) if i else 46805
        items.append(dict(id="li_%03d" % (i + 1), title=title, author=author, duration=float(dur),
                          chapters=nch, tracks=ntr, portrait=portrait, idx=i))
        if i in PROG_SPEC:
            frac, fin = PROG_SPEC[i]
            _progress[items[-1]["id"]] = dict(currentTime=float(round(dur * frac, 2)),
                                              duration=float(dur), isFinished=fin)
    return items


ITEMS = make_items()
BY_ID = {it["id"]: it for it in ITEMS}


def prog_json(item):
    p = _progress.get(item["id"])
    if not p:
        return None
    return dict(id="mp_" + item["id"], libraryItemId=item["id"], currentTime=p["currentTime"],
                duration=p["duration"], progress=(p["currentTime"] / p["duration"] if p["duration"] else 0),
                isFinished=p["isFinished"])


def item_json(item, expanded=False):
    d = {
        "id": item["id"], "libraryId": LIB_ID, "mediaType": "book",
        "media": {
            "metadata": {"title": item["title"], "authorName": item["author"]},
            "duration": item["duration"], "numTracks": item["tracks"], "numChapters": item["chapters"],
        },
    }
    p = prog_json(item)
    if p:
        d["userMediaProgress"] = p
    if expanded:
        d["media"]["chapters"] = chapters_of(item)
        d["media"]["tracks"] = tracks_of(item)
    return d


def chapters_of(item):
    n, dur = item["chapters"], item["duration"]
    step = dur / n
    return [dict(id=k, start=round(k * step, 3), end=round((k + 1) * step, 3),
                 title="Chapter %d" % (k + 1) if k else "Opening Credits")
            for k in range(n)]


def tracks_of(item):
    n, dur = item["tracks"], item["duration"]
    step = dur / n
    return [dict(index=k + 1, startOffset=round(k * step, 3), duration=round(step, 3),
                 contentUrl="/api/items/%s/file/%d" % (item["id"], k + 1), mimeType="audio/mpeg")
            for k in range(n)]


def cover_bytes(item, width):
    key = (item["id"], width)
    if key in _cover_cache:
        return _cover_cache[key]
    from PIL import Image, ImageDraw, ImageFont
    h = hashlib.md5(item["id"].encode()).digest()
    bg = (60 + h[0] % 150, 60 + h[1] % 150, 60 + h[2] % 150)
    W, Hh = 400, (600 if item["portrait"] else 400)
    im = Image.new("RGB", (W, Hh), bg)
    dr = ImageDraw.Draw(im)
    font = None
    for f in ("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
              "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"):
        try:
            font = ImageFont.truetype(f, 38)
            break
        except Exception:
            pass
    if font is None:
        font = ImageFont.load_default()
    words, lines, cur = item["title"].split(), [], ""
    for w in words:
        t = (cur + " " + w).strip()
        if dr.textlength(t, font=font) <= W - 40:
            cur = t
        else:
            lines.append(cur); cur = w
    lines.append(cur)
    lines = lines[:8]
    lh = 46
    y = (Hh - lh * len(lines)) // 2
    for ln in lines:
        x = (W - dr.textlength(ln, font=font)) // 2
        dr.text((x, y), ln, fill=(255, 255, 255), font=font)
        y += lh
    if width and width != W:
        im = im.resize((width, int(Hh * width / W)))
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=85)
    _cover_cache[key] = buf.getvalue()
    return _cover_cache[key]


class QuietServer(ThreadingHTTPServer):
    def handle_error(self, request, client_address):
        if isinstance(sys.exc_info()[1], (BrokenPipeError, ConnectionResetError)):
            return
        super().handle_error(request, client_address)


class Handler(BaseHTTPRequestHandler):
    server_version = "FakeABS/0.1"

    def log_message(self, fmt, *args):
        pass

    def _log(self, status, note=""):
        print("%s %s -> %s %s" % (self.command, self.path, status, note), flush=True)

    def _send(self, status, body=b"", ctype="application/json", note=""):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)
        self._log(status, note)

    def _body_json(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        try:
            return json.loads(raw.decode() or "{}")
        except Exception:
            return {}

    def _auth_ok(self, qs):
        tok = ""
        a = self.headers.get("Authorization", "")
        if a.lower().startswith("bearer "):
            tok = a[7:].strip()
        elif "token" in qs:
            tok = qs["token"][0]
        if tok == API_KEY:
            return True
        with _lock:
            exp = _tokens.get(tok)
        return exp is not None and time.time() < exp

    def _route(self):
        u = urlparse(self.path)
        path, qs = u.path, parse_qs(u.query)
        m = self.command

        if m == "GET" and path == "/status":
            return self._send(200, {"isInit": True, "language": "en-us", "serverVersion": "2.99.0-fake"})
        if m == "POST" and path == "/login":
            b = self._body_json()
            if b.get("username") == USER and b.get("password") == PASSWORD:
                with _lock:
                    _token_n[0] += 1
                    tok = "tok-%d" % _token_n[0]
                    _tokens[tok] = time.time() + TTL
                return self._send(200, {"user": {"id": "usr_1", "username": USER, "type": "admin",
                                                 "token": tok, "accessToken": tok}},
                                  note="(issued %s, ttl %ds)" % (tok, TTL))
            return self._send(401, "Invalid username or password", "text/plain")

        if not self._auth_ok(qs):
            return self._send(401, "Unauthorized", "text/plain")

        if m == "GET" and path in ("/api/me", "/api/authorize"):
            return self._send(200, {"user": {"id": "usr_1", "username": USER, "type": "admin"}} if path == "/api/authorize"
                              else {"id": "usr_1", "username": USER})
        if m == "GET" and path == "/api/libraries":
            return self._send(200, {"libraries": [{"id": LIB_ID, "name": "Audiobooks", "mediaType": "book"}]})

        mm = re.match(r"^/api/libraries/([^/]+)/items$", path)
        if m == "GET" and mm:
            limit = int(qs.get("limit", ["0"])[0] or 0)
            page = int(qs.get("page", ["0"])[0] or 0)
            sort = qs.get("sort", [""])[0]
            items = list(ITEMS)
            if sort == "media.metadata.title" and os.environ.get("FAKE_ABS_SORT"):
                items.sort(key=lambda it: re.sub(r"^(the|a|an) ", "", it["title"].lower()))
            total = len(items)
            if limit > 0:
                items = items[page * limit:(page + 1) * limit]
            return self._send(200, {"results": [item_json(i) for i in items], "total": total,
                                    "limit": limit, "page": page, "sortBy": sort, "mediaType": "book"})

        mm = re.match(r"^/api/libraries/([^/]+)/search$", path)
        if m == "GET" and mm:
            q = qs.get("q", [""])[0].lower()
            hits = [i for i in ITEMS if q in i["title"].lower() or q in i["author"].lower()]
            return self._send(200, {"book": [{"libraryItem": item_json(i), "matchKey": "title",
                                              "matchText": i["title"]} for i in hits],
                                    "narrators": [], "series": [], "tags": [], "authors": []})

        mm = re.match(r"^/api/items/([^/]+)$", path)
        if m == "GET" and mm:
            it = BY_ID.get(mm.group(1))
            if not it:
                return self._send(404, "Not found", "text/plain")
            return self._send(200, item_json(it, expanded=True))

        mm = re.match(r"^/api/items/([^/]+)/cover$", path)
        if m == "GET" and mm:
            it = BY_ID.get(mm.group(1))
            if not it:
                return self._send(404, "Not found", "text/plain")
            w = int(qs.get("width", ["0"])[0] or 0)
            return self._send(200, cover_bytes(it, w), "image/jpeg")

        mm = re.match(r"^/api/items/([^/]+)/play$", path)
        if m == "POST" and mm:
            it = BY_ID.get(mm.group(1))
            if not it:
                return self._send(404, "Not found", "text/plain")
            self._body_json()
            with _lock:
                _sess_n[0] += 1
                sid = "sess-%d" % _sess_n[0]
                _sessions[sid] = it["id"]
            p = _progress.get(it["id"], {})
            return self._send(200, {"id": sid, "libraryItemId": it["id"], "mediaType": "book",
                                    "duration": it["duration"], "currentTime": p.get("currentTime", 0),
                                    "audioTracks": tracks_of(it), "chapters": chapters_of(it)})

        mm = re.match(r"^/api/items/([^/]+)/file/(\d+)$", path)
        if m == "GET" and mm:
            if mm.group(1) not in BY_ID:
                return self._send(404, "Not found", "text/plain")
            return self._send(200, b"\0" * 65536, "audio/mpeg")

        mm = re.match(r"^/api/session/([^/]+)/(sync|close)$", path)
        if m == "POST" and mm:
            sid = mm.group(1)
            b = self._body_json()
            iid = _sessions.get(sid)
            if iid is None:
                return self._send(404, "Session not found", "text/plain")
            ct = b.get("currentTime")
            if ct is not None and (mm.group(2) == "sync" or float(ct) > 0):
                it = BY_ID[iid]
                _progress[iid] = dict(currentTime=float(ct), duration=it["duration"],
                                      isFinished=float(ct) >= it["duration"] - 1)
            if mm.group(2) == "close":
                _sessions.pop(sid, None)
            return self._send(200, {"success": True} if mm.group(2) == "sync" else "OK",
                              "application/json" if mm.group(2) == "sync" else "text/plain")

        mm = re.match(r"^/api/me/progress/([^/]+)$", path)
        if m in ("PATCH", "POST") and mm:
            it = BY_ID.get(mm.group(1))
            if not it:
                return self._send(404, "Not found", "text/plain")
            b = self._body_json()
            ct = float(b.get("currentTime", 0))
            dur = float(b.get("duration", it["duration"]) or it["duration"])
            _progress[it["id"]] = dict(currentTime=ct, duration=dur,
                                       isFinished=bool(b.get("isFinished", dur > 0 and ct >= dur - 1)))
            return self._send(200, "OK", "text/plain")

        return self._send(404, "Not found: " + path, "text/plain")

    def do_GET(self): self._route()
    def do_POST(self): self._route()
    def do_PATCH(self): self._route()
    def do_HEAD(self): self._route()


if __name__ == "__main__":
    srv = QuietServer(("127.0.0.1", PORT), Handler)
    srv.daemon_threads = True
    print("fake ABS on 127.0.0.1:%d, token ttl %ds, api key %s, login %s/%s" % (PORT, TTL, API_KEY, USER, PASSWORD),
          flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
