// Host preview: MusicBackend/Decoder without gstreamer/faad. Simulated clock.
#include "music_backend.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <time.h>

namespace {

struct SimState {
    bool     has_pipe = false;    // stands in for "pipeline != NULL"
    gint64   wall_base = 0;       // ns, monotonic, when the clock last (re)started
};

// Function-local statics: the app's global `backend` is constructed before our file-scope objects.
std::mutex& mu() { static std::mutex* m = new std::mutex(); return *m; }
std::map<const MusicBackend*, SimState>& states() {
    static std::map<const MusicBackend*, SimState>* s = new std::map<const MusicBackend*, SimState>();
    return *s;
}
#define g_mu mu()
#define g_state states()

gint64 now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (gint64)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

bool is_stream(const char* r) {
    return strncmp(r, "http://", 7) == 0 || strncmp(r, "https://", 8) == 0;
}

} // namespace

// ---------------- Decoder (trivial) ----------------
Decoder::Decoder() : stop_flag(false), running(false), thread_id(), current_filepath(),
    start_time(0), on_error_callback(NULL), error_user_data(NULL), current_stream_pid(0) {}
Decoder::~Decoder() {}
bool Decoder::start(const char* filepath, int st, AudioFormat) {
    current_filepath = filepath ? filepath : ""; start_time = st; running = true; return true;
}
void Decoder::stop() { running = false; }
bool Decoder::is_running() const { return running; }
void Decoder::set_error_callback(ErrorCallback cb, void* ud) { on_error_callback = cb; error_user_data = ud; }
void Decoder::set_stream_pid(pid_t pid) { current_stream_pid = pid; }
void* Decoder::thread_func(void*) { return NULL; }
void Decoder::decode_loop() {}
void Decoder::decode_mp4_file(const char*, int) {}
void Decoder::decode_miniaudio(const char*, int) {}
void Decoder::decode_stream(const char*) {}
AudioFormat Decoder::detect_format(const char*, InputType) { return AudioFormat::UNKNOWN; }
InputType Decoder::detect_input_type(const char* r) { return is_stream(r) ? InputType::STREAM : InputType::FILE; }

// ---------------- MusicBackend ----------------
MusicBackend::MusicBackend()
    : is_playing(false), is_paused(false),
      meta_title(""), meta_artist(""), meta_album(""), cover_art(), chapters(),
      current_samplerate(44100), total_duration(0),
      decoder(new Decoder()),
      pipeline(NULL), bus(NULL), bus_watch_id(0),
      current_filepath_str(""), stopping(false),
      on_eos_callback(NULL), eos_user_data(NULL),
      on_error_callback(NULL), error_user_data(NULL),
      last_position(0)
{
    std::lock_guard<std::mutex> l(g_mu);
    g_state[this] = SimState();
}

MusicBackend::~MusicBackend() {
    stop();
    std::lock_guard<std::mutex> l(g_mu);
    g_state.erase(this);
}

bool MusicBackend::is_shutting_down() const { return stopping; }
const char* MusicBackend::get_current_filepath() { return current_filepath_str.c_str(); }
void MusicBackend::set_eos_callback(EosCallback cb, void* ud) { on_eos_callback = cb; eos_user_data = ud; }
void MusicBackend::set_error_callback(ErrorCallback cb, void* ud) { on_error_callback = cb; error_user_data = ud; }

void MusicBackend::internal_decoder_error_callback(const char* msg, void* ud) {
    MusicBackend* self = static_cast<MusicBackend*>(ud);
    if (self && self->on_error_callback) self->on_error_callback(msg, self->error_user_data);
}
gboolean MusicBackend::bus_callback_func(GstBus*, GstMessage*, gpointer) { return TRUE; }
void MusicBackend::cleanup_pipeline() {
    std::lock_guard<std::mutex> l(g_mu);
    g_state[this].has_pipe = false;
}

gint64 MusicBackend::get_duration() { return total_duration > 0 ? total_duration : 0; }

gint64 MusicBackend::get_position() {
    if (is_paused) return last_position;
    SimState st;
    { std::lock_guard<std::mutex> l(g_mu); st = g_state[this]; }
    if (st.has_pipe && is_playing) {
        gint64 pos = last_position + (now_ns() - st.wall_base);
        if (total_duration > 0 && pos > total_duration) pos = total_duration;
        return pos;
    }
    return last_position;
}

void MusicBackend::read_metadata(const char* filepath, AudioFormat) {
    meta_title.clear(); meta_artist.clear(); meta_album.clear();
    cover_art.clear(); chapters.clear();
    current_samplerate = 44100;
    total_duration = 0;
    metadata_filepath.clear();
    if (filepath == nullptr) return;
    metadata_filepath = filepath;
    if (is_stream(filepath)) return;

    std::string p = filepath;
    size_t slash = p.find_last_of('/');
    std::string base = slash == std::string::npos ? p : p.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) base = base.substr(0, dot);
    meta_title = base;
    meta_artist = "Fake Author";
    meta_album = "Fake Album";
    for (int i = 0; i < 6; i++) {
        Chapter ch;
        ch.timestamp = (uint64_t)i * 600ULL * 10000000ULL;
        char t[64]; snprintf(t, sizeof t, "Chapter %d", i + 1);
        ch.title = t;
        chapters.push_back(ch);
    }
    total_duration = 3600LL * GST_SECOND;
}

void MusicBackend::play_file(const char* filepath, int start_time, AudioFormat forced_format) {
    if (stopping) return;
    if (is_playing || is_paused) stop();

    current_filepath_str = filepath;
    if (is_stream(filepath)) {
        current_samplerate = 44100;
        total_duration = 0;
    } else if (metadata_filepath != filepath || total_duration <= 0) {
        read_metadata(filepath, forced_format);
    }
    if (total_duration <= 0 && !is_stream(filepath)) total_duration = 3600LL * GST_SECOND;

    if (!is_stream(filepath) && (gint64)start_time * (gint64)GST_SECOND >= total_duration)
        total_duration = ((gint64)start_time + 3600) * GST_SECOND;   // keep the fake book longer than the resume point
    printf("Backend(fake): Playing %s from %d\n", filepath, start_time);
    is_playing = true;
    is_paused = false;
    last_position = (gint64)start_time * GST_SECOND;
    std::lock_guard<std::mutex> l(g_mu);
    SimState& st = g_state[this];
    st.has_pipe = true;
    st.wall_base = now_ns();
}

void MusicBackend::pause() {
    bool has;
    { std::lock_guard<std::mutex> l(g_mu); has = g_state[this].has_pipe; }
    if (!has || !is_playing) return;

    if (is_paused) {
        std::lock_guard<std::mutex> l(g_mu);
        g_state[this].wall_base = now_ns();
        is_paused = false;
    } else {
        last_position = get_position();
        is_paused = true;
    }
}

void MusicBackend::stop() {
    if (stopping) return;
    stopping = true;
    decoder->stop();
    cleanup_pipeline();
    stopping = false;
    is_playing = false;
    is_paused = false;
}
