// Host preview: in-memory DatabaseManager. Settings persist to $LARK_PREVIEW_SETTINGS (key=value lines).
#include "database_manager.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <algorithm>

namespace {

struct Store {
    std::map<std::string, std::string> settings;
    std::vector<Book> books;
    std::vector<Bookmark> bookmarks;
    int next_book_id = 1;
    int next_bm_id = 1;
};
Store g;

std::string settings_path() {
    const char* p = getenv("LARK_PREVIEW_SETTINGS");
    return (p && *p) ? p : "/tmp/lark-next-preview/settings.txt";
}

std::string esc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\n') o += "\\n";
        else if (s[i] == '\\') o += "\\\\";
        else o += s[i];
    }
    return o;
}
std::string unesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) { i++; o += (s[i] == 'n') ? '\n' : s[i]; }
        else o += s[i];
    }
    return o;
}

void save_settings() {
    std::string path = settings_path();
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        std::string cmd = "mkdir -p '" + path.substr(0, slash) + "'";
        if (system(cmd.c_str()) != 0) {}
    }
    std::ofstream out(path.c_str());
    for (std::map<std::string, std::string>::const_iterator it = g.settings.begin(); it != g.settings.end(); ++it)
        out << it->first << "=" << esc(it->second) << "\n";
}

void load_settings() {
    g.settings.clear();
    std::ifstream in(settings_path().c_str());
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        g.settings[line.substr(0, eq)] = unesc(line.substr(eq + 1));
    }
}

Book* find_book(const std::string& fp) {
    for (size_t i = 0; i < g.books.size(); i++) if (g.books[i].filepath == fp) return &g.books[i];
    return NULL;
}

void add_book(const char* fp, int pos, int dur, bool fin, int ago) {
    Book b;
    b.id = g.next_book_id++;
    b.filepath = fp; b.last_position = pos; b.duration = dur; b.is_finished = fin;
    b.last_played_at = (int)time(NULL) - ago;
    g.books.push_back(b);
}

bool newer_first(const Book& a, const Book& b) { return a.last_played_at > b.last_played_at; }

} // namespace

DatabaseManager::DatabaseManager() : db(nullptr) {}
DatabaseManager::~DatabaseManager() {}

bool DatabaseManager::init(const std::string&) {
    load_settings();
    g.books.clear(); g.bookmarks.clear();
    g.next_book_id = 1; g.next_bm_id = 1;
    add_book("/mnt/us/audiobooks/Treasure Island.m4b", 1260, 3600, false, 3600);
    add_book("/mnt/us/audiobooks/Moby Dick - Unabridged.m4b", 2400, 3600, false, 86400);
    add_book("/mnt/us/audiobooks/The Hobbit.m4b", 3600, 3600, true, 7 * 86400);
    return true;
}

bool DatabaseManager::executeQuery(const std::string&) { return true; }
int DatabaseManager::getBookId(const std::string& fp) { Book* b = find_book(fp); return b ? b->id : 0; }

bool DatabaseManager::setSetting(const std::string& key, const std::string& value) {
    g.settings[key] = value;
    save_settings();
    return true;
}

std::string DatabaseManager::getSetting(const std::string& key, const std::string& def) {
    std::map<std::string, std::string>::const_iterator it = g.settings.find(key);
    return it == g.settings.end() ? def : it->second;
}

bool DatabaseManager::updateBookProgress(const std::string& fp, int position, int duration, bool fin) {
    Book* b = find_book(fp);
    if (!b) { add_book(fp.c_str(), position, duration, fin, 0); return true; }
    b->last_position = position; b->duration = duration; b->is_finished = fin;
    b->last_played_at = (int)time(NULL);
    return true;
}

bool DatabaseManager::getBookProgress(const std::string& fp, Book& out) {
    Book* b = find_book(fp);
    if (!b) return false;
    out = *b;
    return true;
}

std::string DatabaseManager::getLastPlayedFile() { return getSetting("last_opened_file", ""); }

bool DatabaseManager::addBookmark(const std::string& fp, int position, const std::string& name) {
    int id = getBookId(fp);
    if (id == 0) { updateBookProgress(fp, 0, 0, false); id = getBookId(fp); }
    Bookmark m;
    m.id = g.next_bm_id++; m.book_id = id; m.position = position; m.name = name;
    m.created_at = (int)time(NULL);
    g.bookmarks.push_back(m);
    return true;
}

static bool bm_less(const Bookmark& a, const Bookmark& b) { return a.position < b.position; }

std::vector<Bookmark> DatabaseManager::getBookmarks(const std::string& fp) {
    std::vector<Bookmark> out;
    int id = getBookId(fp);
    if (id == 0) return out;
    for (size_t i = 0; i < g.bookmarks.size(); i++) if (g.bookmarks[i].book_id == id) out.push_back(g.bookmarks[i]);
    std::stable_sort(out.begin(), out.end(), bm_less);
    return out;
}

bool DatabaseManager::deleteBookmark(int id) {
    for (size_t i = 0; i < g.bookmarks.size(); i++)
        if (g.bookmarks[i].id == id) { g.bookmarks.erase(g.bookmarks.begin() + i); break; }
    return true;
}

std::vector<Book> DatabaseManager::getHistory() {
    std::vector<Book> h = g.books;
    std::stable_sort(h.begin(), h.end(), newer_first);
    return h;
}

bool DatabaseManager::migrateFromLegacy(const std::string&) { return false; }
