// TecFlix - a self-contained video sharing web server.
// C++17, no third-party dependencies (Winsock on Windows, BSD sockets elsewhere).
//
// Features: accounts, upload, streaming (HTTP Range), delete, public/private
// visibility, title editing and metadata extraction (MP4/MOV box parser).

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
#define CLOSESOCK closesocket
#define SEND_FLAGS 0
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using sock_t = int;
#define INVALID_SOCKET (-1)
#define CLOSESOCK close
#define SEND_FLAGS MSG_NOSIGNAL
#endif

namespace fs = std::filesystem;
using std::string;
using std::vector;

static const uint64_t MAX_UPLOAD = 4ULL << 30;  // 4 GiB
static const size_t MAX_FORM = 64 * 1024;
static fs::path DATA_DIR = "data";
static fs::path WEB_DIR = "web/dist";  // React build output (see frontend/)
static string gFfmpeg;                  // path to ffmpeg; empty when DASH packaging is unavailable

// ---------------------------------------------------------------- utilities

static string lower(string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
static string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == string::npos ? "" : s.substr(a, b - a + 1);
}
static string urldecode(const string& s) {
    string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && isxdigit((unsigned char)s[i + 1]) &&
            isxdigit((unsigned char)s[i + 2])) {
            o += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else if (s[i] == '+') {
            o += ' ';
        } else {
            o += s[i];
        }
    }
    return o;
}
static std::map<string, string> parseForm(const string& s) {
    std::map<string, string> m;
    size_t i = 0;
    while (i <= s.size()) {
        size_t amp = s.find('&', i);
        if (amp == string::npos) amp = s.size();
        string kv = s.substr(i, amp - i);
        size_t eq = kv.find('=');
        if (!kv.empty())
            m[urldecode(kv.substr(0, eq))] = eq == string::npos ? "" : urldecode(kv.substr(eq + 1));
        i = amp + 1;
    }
    return m;
}
static string jstr(const string& s) {
    string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += (char)c;
                }
        }
    }
    return o + "\"";
}
static string randHex(int bytes) {
    static std::random_device rd;
    static std::mutex m;
    std::lock_guard<std::mutex> g(m);
    string o;
    char b[3];
    for (int i = 0; i < bytes; i++) {
        snprintf(b, sizeof b, "%02x", (unsigned)(rd() & 0xff));
        o += b;
    }
    return o;
}
static string toHex(const string& raw) {
    string o;
    char b[3];
    for (unsigned char c : raw) {
        snprintf(b, sizeof b, "%02x", c);
        o += b;
    }
    return o;
}
static bool isHexId(const string& s) {
    if (s.empty() || s.size() > 32) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return isxdigit(c); });
}

// ------------------------------------------------------------------ SHA-256

static string sha256(const string& in) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    string m = in;
    uint64_t bits = (uint64_t)in.size() * 8;
    m.push_back((char)0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; i--) m.push_back((char)(bits >> (i * 8)));
    auto ror = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t o = 0; o < m.size(); o += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)(unsigned char)m[o + i * 4] << 24) | ((uint32_t)(unsigned char)m[o + i * 4 + 1] << 16) |
                   ((uint32_t)(unsigned char)m[o + i * 4 + 2] << 8) | (uint32_t)(unsigned char)m[o + i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    string out;
    for (int i = 0; i < 8; i++)
        for (int j = 3; j >= 0; j--) out.push_back((char)(h[i] >> (j * 8)));
    return out;
}
static string hashPassword(const string& salt, const string& pw) {
    string h = salt + pw;
    for (int i = 0; i < 20000; i++) h = sha256(h + salt + pw);
    return toHex(h);
}

// ------------------------------------------------------------------- models

struct Video {
    string id, owner, title, visibility, file, orig, mime;
    uint64_t size = 0;
    int64_t uploaded = 0, created = 0;
    double duration = 0;
    int width = 0, height = 0, tracks = 0;
    string vcodec, acodec, brand;
    string dash = "unavailable";  // unavailable | processing | ready | failed
};
struct User {
    string name, salt, hash;
};

static std::mutex gMutex;
static vector<Video> gVideos;
static vector<User> gUsers;
static std::map<string, string> gSessions;  // token -> username

static string clean(string s) {
    for (auto& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}
static vector<string> splitTabs(const string& line) {
    vector<string> f;
    size_t i = 0;
    while (true) {
        size_t t = line.find('\t', i);
        if (t == string::npos) { f.push_back(line.substr(i)); break; }
        f.push_back(line.substr(i, t - i));
        i = t + 1;
    }
    return f;
}
static void saveUsers() {  // caller holds gMutex
    std::ofstream o(DATA_DIR / "users.db", std::ios::trunc);
    for (auto& u : gUsers) o << u.name << '\t' << u.salt << '\t' << u.hash << '\n';
}
static void saveVideos() {  // caller holds gMutex
    std::ofstream o(DATA_DIR / "videos.db", std::ios::trunc);
    for (auto& v : gVideos)
        o << v.id << '\t' << v.owner << '\t' << clean(v.title) << '\t' << v.visibility << '\t' << v.file << '\t'
          << clean(v.orig) << '\t' << v.mime << '\t' << v.size << '\t' << v.uploaded << '\t' << v.created << '\t'
          << v.duration << '\t' << v.width << '\t' << v.height << '\t' << v.tracks << '\t' << v.vcodec << '\t'
          << v.acodec << '\t' << v.brand << '\t' << v.dash << '\n';
}
static void loadDb() {
    std::string line;
    std::ifstream u(DATA_DIR / "users.db");
    while (std::getline(u, line)) {
        auto f = splitTabs(line);
        if (f.size() >= 3) gUsers.push_back({f[0], f[1], f[2]});
    }
    std::ifstream v(DATA_DIR / "videos.db");
    while (std::getline(v, line)) {
        auto f = splitTabs(line);
        if (f.size() < 17) continue;
        Video x;
        x.id = f[0]; x.owner = f[1]; x.title = f[2]; x.visibility = f[3]; x.file = f[4]; x.orig = f[5];
        x.mime = f[6]; x.size = std::stoull(f[7]); x.uploaded = std::stoll(f[8]); x.created = std::stoll(f[9]);
        x.duration = std::stod(f[10]); x.width = std::stoi(f[11]); x.height = std::stoi(f[12]);
        x.tracks = std::stoi(f[13]); x.vcodec = f[14]; x.acodec = f[15]; x.brand = f[16];
        if (f.size() > 17) x.dash = f[17];
        if (x.dash == "processing") x.dash = "failed";  // server stopped mid-packaging
        gVideos.push_back(x);
    }
}

// ------------------------------------------------------------ MP4 metadata

namespace {
struct Mp4Probe {
    std::ifstream f;
    Video& v;
    string handler, codec;
    int tkW = 0, tkH = 0;
    bool haveVideo = false;

    explicit Mp4Probe(const string& path, Video& vid) : f(path, std::ios::binary), v(vid) {}

    bool readAt(uint64_t off, void* buf, size_t n) {
        f.clear();
        f.seekg((std::streamoff)off);
        f.read((char*)buf, (std::streamsize)n);
        return (size_t)f.gcount() == n;
    }
    static uint32_t be32(const unsigned char* p) {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    }
    static uint64_t be64(const unsigned char* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

    void walk(uint64_t start, uint64_t end, int depth) {
        if (depth > 8) return;
        while (start + 8 <= end) {
            unsigned char h[16];
            if (!readAt(start, h, 8)) return;
            uint64_t size = be32(h);
            string type((char*)h + 4, 4);
            uint64_t hdr = 8;
            if (size == 1) {
                if (!readAt(start + 8, h + 8, 8)) return;
                size = be64(h + 8);
                hdr = 16;
            } else if (size == 0) {
                size = end - start;
            }
            if (size < hdr || start + size > end) return;
            uint64_t p = start + hdr, pe = start + size;
            unsigned char b[96];
            if (type == "moov" || type == "mdia" || type == "minf" || type == "stbl") {
                walk(p, pe, depth + 1);
            } else if (type == "trak") {
                handler.clear(); codec.clear(); tkW = tkH = 0;
                walk(p, pe, depth + 1);
                v.tracks++;
                if (handler == "vide") {
                    if (!haveVideo) { v.width = tkW; v.height = tkH; v.vcodec = codec; haveVideo = true; }
                } else if (handler == "soun" && v.acodec.empty()) {
                    v.acodec = codec;
                }
            } else if (type == "ftyp") {
                if (readAt(p, b, 4)) v.brand = string((char*)b, 4);
            } else if (type == "mvhd") {
                if (readAt(p, b, 32)) {
                    uint64_t ct, ts, dur;
                    if (b[0] == 1) { ct = be64(b + 4); ts = be32(b + 20); dur = be64(b + 24); }
                    else { ct = be32(b + 4); ts = be32(b + 12); dur = be32(b + 16); }
                    if (ts) v.duration = (double)dur / (double)ts;
                    if (ct > 2082844800ULL) v.created = (int64_t)(ct - 2082844800ULL);
                }
            } else if (type == "tkhd") {
                if (readAt(p, b, 96)) {
                    int off = b[0] == 1 ? 88 : 76;
                    tkW = (int)(be32(b + off) >> 16);
                    tkH = (int)(be32(b + off + 4) >> 16);
                }
            } else if (type == "hdlr") {
                if (readAt(p, b, 12)) handler = string((char*)b + 8, 4);
            } else if (type == "stsd") {
                if (readAt(p, b, 16)) codec = string((char*)b + 12, 4);
            }
            start += size;
        }
    }
};
}  // namespace

static string mimeFor(const string& ext) {
    static const std::map<string, string> m = {
        {".mp4", "video/mp4"},  {".m4v", "video/mp4"},   {".mov", "video/quicktime"}, {".webm", "video/webm"},
        {".ogv", "video/ogg"},  {".ogg", "video/ogg"},   {".mkv", "video/x-matroska"}};
    auto it = m.find(ext);
    return it == m.end() ? "" : it->second;
}
static void probe(Video& v, const fs::path& path) {
    std::error_code ec;
    v.size = fs::file_size(path, ec);
    string ext = lower(path.extension().string());
    if (ext == ".mp4" || ext == ".m4v" || ext == ".mov") {
        Mp4Probe p(path.string(), v);
        if (p.f) p.walk(0, v.size, 0);
    }
    if (!v.created) {  // fall back to file modification time
        auto t = fs::last_write_time(path, ec);
        if (!ec) {
            auto s = std::chrono::time_point_cast<std::chrono::seconds>(
                t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            v.created = s.time_since_epoch().count();
        }
    }
}

// --------------------------------------------------------------------- HTTP

struct Req {
    string method, path;
    std::map<string, string> q, h;
    string body, pre;
    uint64_t clen = 0;
};

static bool sendAll(sock_t s, const char* d, size_t n) {
    while (n) {
        int r = send(s, d, (int)std::min<size_t>(n, 1 << 20), SEND_FLAGS);
        if (r <= 0) return false;
        d += r; n -= (size_t)r;
    }
    return true;
}
static const char* statusText(int c) {
    switch (c) {
        case 200: return "OK";
        case 201: return "Created";
        case 206: return "Partial Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 416: return "Range Not Satisfiable";
        default: return "Internal Server Error";
    }
}
static string headerBlock(int code, const string& ctype, uint64_t len, const string& extra = "") {
    std::ostringstream o;
    o << "HTTP/1.1 " << code << ' ' << statusText(code) << "\r\nContent-Type: " << ctype
      << "\r\nContent-Length: " << len << "\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n"
      << extra << "\r\n";
    return o.str();
}
static void respond(sock_t s, int code, const string& ctype, const string& body, const string& extra = "") {
    string h = headerBlock(code, ctype, body.size(), extra + "Cache-Control: no-store\r\n");
    if (sendAll(s, h.data(), h.size())) sendAll(s, body.data(), body.size());
}
static void json(sock_t s, int code, const string& body, const string& extra = "") {
    respond(s, code, "application/json; charset=utf-8", body, extra);
}
static void fail(sock_t s, int code, const string& msg) { json(s, code, "{\"error\":" + jstr(msg) + "}"); }

static bool readReq(sock_t s, Req& r) {
    string buf;
    char tmp[8192];
    size_t pos;
    while ((pos = buf.find("\r\n\r\n")) == string::npos) {
        if (buf.size() > 16384) return false;
        int n = recv(s, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        buf.append(tmp, (size_t)n);
    }
    r.pre = buf.substr(pos + 4);
    std::istringstream hs(buf.substr(0, pos));
    string line;
    std::getline(hs, line);
    std::istringstream rl(trim(line));
    string target, ver;
    rl >> r.method >> target >> ver;
    if (r.method.empty() || target.empty() || target[0] != '/') return false;
    size_t qm = target.find('?');
    r.path = urldecode(target.substr(0, qm));
    if (qm != string::npos) r.q = parseForm(target.substr(qm + 1));
    while (std::getline(hs, line)) {
        size_t c = line.find(':');
        if (c != string::npos) r.h[lower(trim(line.substr(0, c)))] = trim(line.substr(c + 1));
    }
    if (r.h.count("content-length")) {
        try { r.clen = std::stoull(r.h["content-length"]); } catch (...) { return false; }
    }
    return true;
}
static bool readBody(sock_t s, Req& r) {
    if (r.clen > MAX_FORM) return false;
    char tmp[8192];
    while (r.pre.size() < r.clen) {
        int n = recv(s, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        r.pre.append(tmp, (size_t)n);
    }
    r.body = r.pre.substr(0, r.clen);
    return true;
}

// ---------------------------------------------------------------- sessions

static string sessionUser(const Req& r) {
    auto it = r.h.find("cookie");
    if (it == r.h.end()) return "";
    size_t p = it->second.find("tf_session=");
    if (p == string::npos) return "";
    p += 11;
    string tok = it->second.substr(p, it->second.find(';', p) - p);
    std::lock_guard<std::mutex> g(gMutex);
    auto s = gSessions.find(tok);
    return s == gSessions.end() ? "" : s->second;
}

// ------------------------------------------------------------------- routes

static string videoJson(const Video& v, const string& me) {
    std::ostringstream o;
    double br = v.duration > 0 ? (double)v.size * 8.0 / v.duration : 0;
    o << "{\"id\":" << jstr(v.id) << ",\"title\":" << jstr(v.title) << ",\"owner\":" << jstr(v.owner)
      << ",\"visibility\":" << jstr(v.visibility) << ",\"uploaded\":" << v.uploaded
      << ",\"isOwner\":" << (v.owner == me ? "true" : "false") << ",\"meta\":{\"filename\":" << jstr(v.orig)
      << ",\"size\":" << v.size << ",\"mime\":" << jstr(v.mime) << ",\"duration\":" << v.duration
      << ",\"width\":" << v.width << ",\"height\":" << v.height << ",\"vcodec\":" << jstr(v.vcodec)
      << ",\"acodec\":" << jstr(v.acodec) << ",\"brand\":" << jstr(v.brand) << ",\"tracks\":" << v.tracks
      << ",\"created\":" << v.created << ",\"bitrate\":" << (long long)br << "},\"dash\":" << jstr(v.dash) << "}";
    return o.str();
}
static bool canSee(const Video& v, const string& me) { return v.visibility == "public" || v.owner == me; }

static void handleAuth(sock_t s, Req& r, bool reg) {
    if (!readBody(s, r)) return fail(s, 400, "Bad request body");
    auto f = parseForm(r.body);
    string name = f["username"], pw = f["password"];
    std::lock_guard<std::mutex> g(gMutex);
    if (reg) {
        bool ok = name.size() >= 3 && name.size() <= 20 &&
                  std::all_of(name.begin(), name.end(), [](unsigned char c) { return isalnum(c) || c == '_'; });
        if (!ok) return fail(s, 400, "Username must be 3-20 letters, digits or underscore");
        if (pw.size() < 6) return fail(s, 400, "Password must have at least 6 characters");
        for (auto& u : gUsers)
            if (lower(u.name) == lower(name)) return fail(s, 409, "Username already taken");
        User u{name, randHex(16), ""};
        u.hash = hashPassword(u.salt, pw);
        gUsers.push_back(u);
        saveUsers();
    } else {
        auto it = std::find_if(gUsers.begin(), gUsers.end(), [&](const User& u) { return u.name == name; });
        if (it == gUsers.end() || hashPassword(it->salt, pw) != it->hash)
            return fail(s, 401, "Invalid username or password");
    }
    string tok = randHex(24);
    gSessions[tok] = name;
    json(s, 200, "{\"username\":" + jstr(name) + "}",
         "Set-Cookie: tf_session=" + tok + "; Path=/; HttpOnly; SameSite=Lax; Max-Age=604800\r\n");
}

// ---- DASH packaging (ffmpeg): data/dash/<id>/manifest.mpd + fMP4 segments ----

static std::mutex gDashMutex;  // package one video at a time

static void packageDash(Video v) {  // runs on a worker thread
    std::lock_guard<std::mutex> one(gDashMutex);
    std::error_code ec;
    fs::path in = fs::absolute(DATA_DIR / "videos" / v.file, ec);
    fs::path out = fs::absolute(DATA_DIR / "dash" / v.id, ec);
    fs::create_directories(out, ec);
    // Re-use H.264 / AAC streams as they are; otherwise transcode to browser-friendly codecs.
    string vc = v.vcodec == "avc1" ? "copy" : "libx264 -preset veryfast -crf 23 -pix_fmt yuv420p -g 96 -sc_threshold 0";
    string ac = v.acodec == "mp4a" ? "copy" : "aac -b:a 128k";
    // ffmpeg writes segment files relative to its working directory, so run it inside the output folder.
#ifdef _WIN32
    string cd = "cd /d \"" + out.string() + "\" && ";
#else
    string cd = "cd \"" + out.string() + "\" && ";
#endif
    string cmd = cd + "\"" + gFfmpeg + "\" -nostdin -y -loglevel error -i \"" + in.string() +
                 "\" -map 0:v:0 -map 0:a:0? -c:v " + vc + " -c:a " + ac +
                 " -f dash -seg_duration 4 -use_template 1 -use_timeline 1 manifest.mpd";
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";  // cmd.exe strips one outer pair of quotes
#endif
    bool ok = std::system(cmd.c_str()) == 0 && fs::exists(out / "manifest.mpd", ec);
    bool found = false;
    {
        std::lock_guard<std::mutex> g(gMutex);
        for (auto& x : gVideos)
            if (x.id == v.id) { x.dash = ok ? "ready" : "failed"; found = true; }
        if (found) saveVideos();
    }
    if (!ok || !found) fs::remove_all(out, ec);  // failed, or the video was deleted meanwhile
}

static void handleUpload(sock_t s, Req& r, const string& me) {
    if (me.empty()) return fail(s, 401, "Log in to upload");
    if (!r.clen) return fail(s, 411, "Content-Length required");
    if (r.clen > MAX_UPLOAD) return fail(s, 413, "File too large (max 4 GiB)");
    string orig = fs::path(r.q["filename"]).filename().string();
    string ext = lower(fs::path(orig).extension().string());
    string mime = mimeFor(ext);
    if (mime.empty()) return fail(s, 400, "Unsupported type. Use mp4, m4v, mov, webm, ogv or mkv");
    string title = trim(clean(r.q["title"]));
    if (title.empty()) title = fs::path(orig).stem().string();
    if (title.size() > 120) title.resize(120);
    string id = randHex(6), file = id + ext;
    fs::path path = DATA_DIR / "videos" / file;
    {
        std::ofstream out(path, std::ios::binary);
        if (!out) return fail(s, 500, "Cannot store file");
        uint64_t got = std::min<uint64_t>(r.pre.size(), r.clen);
        out.write(r.pre.data(), (std::streamsize)got);
        vector<char> buf(256 * 1024);
        while (got < r.clen) {
            int n = recv(s, buf.data(), (int)std::min<uint64_t>(buf.size(), r.clen - got), 0);
            if (n <= 0) { out.close(); std::error_code ec; fs::remove(path, ec); return; }
            out.write(buf.data(), n);
            got += (uint64_t)n;
        }
    }
    Video v;
    v.id = id; v.owner = me; v.title = title; v.file = file; v.orig = clean(orig); v.mime = mime;
    v.visibility = r.q["visibility"] == "public" ? "public" : "private";
    v.uploaded = (int64_t)time(nullptr);
    probe(v, path);
    if (!gFfmpeg.empty()) v.dash = "processing";
    std::lock_guard<std::mutex> g(gMutex);
    gVideos.push_back(v);
    saveVideos();
    if (!gFfmpeg.empty()) std::thread(packageDash, v).detach();
    json(s, 201, videoJson(v, me));
}

static void streamFile(sock_t s, const Req& r, const fs::path& path, const string& mime) {
    std::ifstream f(path, std::ios::binary);
    std::error_code ec;
    uint64_t size = fs::file_size(path, ec);
    if (!f || ec) return fail(s, 404, "File missing");
    uint64_t a = 0, b = size ? size - 1 : 0;
    int code = 200;
    string extra = "Accept-Ranges: bytes\r\n";
    auto it = r.h.find("range");
    if (it != r.h.end() && it->second.rfind("bytes=", 0) == 0) {
        string spec = it->second.substr(6);
        size_t dash = spec.find('-');
        try {
            if (dash == string::npos || spec.find(',') != string::npos) throw 1;
            string x = spec.substr(0, dash), y = spec.substr(dash + 1);
            if (x.empty()) { uint64_t n = std::stoull(y); a = n >= size ? 0 : size - n; }
            else { a = std::stoull(x); if (!y.empty()) b = std::min<uint64_t>(std::stoull(y), size - 1); }
            if (a > b || a >= size) throw 1;
            code = 206;
            extra += "Content-Range: bytes " + std::to_string(a) + "-" + std::to_string(b) + "/" +
                     std::to_string(size) + "\r\n";
        } catch (...) {
            string h = headerBlock(416, "text/plain", 0, "Content-Range: bytes */" + std::to_string(size) + "\r\n");
            sendAll(s, h.data(), h.size());
            return;
        }
    }
    uint64_t len = size ? b - a + 1 : 0;
    string h = headerBlock(code, mime, len, extra + "Cache-Control: private, max-age=3600\r\n");
    if (!sendAll(s, h.data(), h.size())) return;
    f.seekg((std::streamoff)a);
    vector<char> buf(256 * 1024);
    while (len) {
        f.read(buf.data(), (std::streamsize)std::min<uint64_t>(buf.size(), len));
        auto n = (size_t)f.gcount();
        if (!n || !sendAll(s, buf.data(), n)) return;
        len -= n;
    }
}

static void serveStatic(sock_t s, string p) {
    if (p == "/") p = "/index.html";
    if (p.find("..") != string::npos || p.find('\\') != string::npos) return fail(s, 404, "Not found");
    fs::path path = WEB_DIR / p.substr(1);
    static const std::map<string, string> types = {
        {".html", "text/html; charset=utf-8"}, {".css", "text/css; charset=utf-8"},
        {".js", "application/javascript; charset=utf-8"}, {".svg", "image/svg+xml"}, {".ico", "image/x-icon"},
        {".json", "application/json"}, {".map", "application/json"}, {".png", "image/png"},
        {".jpg", "image/jpeg"}, {".woff2", "font/woff2"}, {".woff", "font/woff"}, {".txt", "text/plain"}};
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) && path.extension().empty())  // single-page-app fallback
        path = WEB_DIR / "index.html";
    auto t = types.find(lower(path.extension().string()));
    std::ifstream f(path, std::ios::binary);
    if (t == types.end() || !f || fs::is_directory(path)) return fail(s, 404, "Not found");
    std::ostringstream ss;
    ss << f.rdbuf();
    respond(s, 200, t->second, ss.str());
}

static void route(sock_t s, Req& r) {
    const string me = sessionUser(r);
    const string& p = r.path;
    const string& m = r.method;

    if (p == "/api/register" && m == "POST") return handleAuth(s, r, true);
    if (p == "/api/login" && m == "POST") return handleAuth(s, r, false);
    if (p == "/api/logout" && m == "POST") {
        auto it = r.h.find("cookie");
        if (it != r.h.end()) {
            size_t q = it->second.find("tf_session=");
            if (q != string::npos) {
                q += 11;
                std::lock_guard<std::mutex> g(gMutex);
                gSessions.erase(it->second.substr(q, it->second.find(';', q) - q));
            }
        }
        return json(s, 200, "{}", "Set-Cookie: tf_session=; Path=/; Max-Age=0\r\n");
    }
    if (p == "/api/me" && m == "GET")
        return json(s, 200, me.empty() ? "{\"username\":null}" : "{\"username\":" + jstr(me) + "}");
    if (p == "/api/upload" && m == "POST") return handleUpload(s, r, me);

    if (p == "/api/videos" && m == "GET") {
        bool mine = r.q["scope"] == "mine";
        if (mine && me.empty()) return fail(s, 401, "Log in first");
        std::lock_guard<std::mutex> g(gMutex);
        string out = "[";
        bool first = true;
        for (auto it = gVideos.rbegin(); it != gVideos.rend(); ++it) {
            if (mine ? it->owner != me : it->visibility != "public") continue;
            out += (first ? "" : ",") + videoJson(*it, me);
            first = false;
        }
        return json(s, 200, out + "]");
    }

    // /dash/<id>/<file>: manifest and segments of a packaged video
    if (p.rfind("/dash/", 0) == 0 && (m == "GET" || m == "HEAD")) {
        string rest = p.substr(6);
        size_t sl = rest.find('/');
        string did = rest.substr(0, sl), name = sl == string::npos ? "" : rest.substr(sl + 1);
        bool okName = !name.empty() && name.find("..") == string::npos &&
                      std::all_of(name.begin(), name.end(),
                                  [](unsigned char c) { return isalnum(c) || c == '.' || c == '_' || c == '-'; });
        if (!isHexId(did) || !okName) return fail(s, 404, "Not found");
        {
            std::lock_guard<std::mutex> g(gMutex);
            auto it = std::find_if(gVideos.begin(), gVideos.end(), [&](const Video& v) { return v.id == did; });
            if (it == gVideos.end() || !canSee(*it, me) || it->dash != "ready") return fail(s, 404, "Not found");
        }
        string ext = lower(fs::path(name).extension().string());
        return streamFile(s, r, DATA_DIR / "dash" / did / name, ext == ".mpd" ? "application/dash+xml" : "video/mp4");
    }

    // /api/videos/<id>  and  /video/<id>
    string id;
    bool isApi = p.rfind("/api/videos/", 0) == 0, isStream = p.rfind("/video/", 0) == 0;
    if (isApi) id = p.substr(12);
    else if (isStream) id = p.substr(7);
    if (isApi || isStream) {
        if (!isHexId(id)) return fail(s, 404, "Not found");
        Video copy;
        {
            std::lock_guard<std::mutex> g(gMutex);
            auto it = std::find_if(gVideos.begin(), gVideos.end(), [&](const Video& v) { return v.id == id; });
            if (it == gVideos.end() || !canSee(*it, me)) return fail(s, 404, "Video not found");
            copy = *it;
        }
        if (isStream) return (m == "GET" || m == "HEAD") ? streamFile(s, r, DATA_DIR / "videos" / copy.file, copy.mime) : fail(s, 405, "Method not allowed");
        if (m == "GET") return json(s, 200, videoJson(copy, me));
        if (copy.owner != me) return fail(s, 403, "Only the owner can do that");
        if (m == "DELETE") {
            std::lock_guard<std::mutex> g(gMutex);
            gVideos.erase(std::remove_if(gVideos.begin(), gVideos.end(), [&](const Video& v) { return v.id == id; }),
                          gVideos.end());
            saveVideos();
            std::error_code ec;
            fs::remove(DATA_DIR / "videos" / copy.file, ec);
            fs::remove_all(DATA_DIR / "dash" / id, ec);
            return json(s, 200, "{}");
        }
        if (m == "POST") {  // update title and/or visibility
            if (!readBody(s, r)) return fail(s, 400, "Bad request body");
            auto f = parseForm(r.body);
            std::lock_guard<std::mutex> g(gMutex);
            for (auto& v : gVideos) {
                if (v.id != id) continue;
                if (f.count("visibility")) {
                    if (f["visibility"] != "public" && f["visibility"] != "private")
                        return fail(s, 400, "visibility must be public or private");
                    v.visibility = f["visibility"];
                }
                if (f.count("title")) {
                    string t = trim(clean(f["title"]));
                    if (t.empty() || t.size() > 120) return fail(s, 400, "Title must be 1-120 characters");
                    v.title = t;
                }
                saveVideos();
                return json(s, 200, videoJson(v, me));
            }
        }
        return fail(s, 405, "Method not allowed");
    }

    if (m == "GET") return serveStatic(s, p);
    fail(s, 404, "Not found");
}

static void handleConn(sock_t s) {
#ifdef _WIN32
    DWORD tv = 120000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
#else
    timeval tv{120, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
    try {
        Req r;
        if (readReq(s, r)) route(s, r);
    } catch (const std::exception& e) {
        std::cerr << "request error: " << e.what() << "\n";
    }
    CLOSESOCK(s);
}

int main(int argc, char** argv) {
    if (argc > 1 && string(argv[1]) == "--selftest") {  // SHA-256 known-answer test
        string h = toHex(sha256("abc"));
        std::cout << h << "\n"
                  << (h == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" ? "OK" : "FAIL") << "\n";
        return 0;
    }
    int port = argc > 1 ? std::atoi(argv[1]) : 8080;
    fs::create_directories(DATA_DIR / "videos");
    loadDb();
    {   // DASH packaging needs ffmpeg: TECFLIX_FFMPEG, or "ffmpeg" from PATH
        const char* env = std::getenv("TECFLIX_FFMPEG");
        string cand = env && *env ? env : "ffmpeg";
#ifdef _WIN32
        string probeCmd = "\"\"" + cand + "\" -version >NUL 2>&1\"";
#else
        string probeCmd = "\"" + cand + "\" -version >/dev/null 2>&1";
#endif
        if (std::system(probeCmd.c_str()) == 0) gFfmpeg = cand;
        std::cout << (gFfmpeg.empty() ? "ffmpeg not found: DASH disabled, videos play as plain MP4/WebM\n"
                                      : "ffmpeg found: uploads are packaged for MPEG-DASH\n");
        if (!gFfmpeg.empty()) {
            std::lock_guard<std::mutex> g(gMutex);
            for (auto& v : gVideos)  // package videos uploaded before DASH support existed
                if (v.dash == "unavailable") { v.dash = "processing"; std::thread(packageDash, v).detach(); }
            saveVideos();
        }
    }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((unsigned short)port);
    if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 64) != 0) {
        std::cerr << "Cannot listen on port " << port << "\n";
        return 1;
    }
    std::cout << "TecFlix running at http://localhost:" << port << "  (Ctrl+C to stop)\n";
    while (true) {
        sock_t c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        std::thread(handleConn, c).detach();
    }
}
