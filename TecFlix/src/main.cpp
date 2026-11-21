// TecFlix - a self-contained video sharing web server.
// C++17, integrated with BTreeUserRepository, MetadataService/MetadataController,
// FFmpeg DASH packager and OpenTelemetry.

#include "BTreeUserRepository.hpp"
#include "IMetadataRepository.hpp"
#include "IUserRepository.hpp"
#include "MetadataController.hpp"
#include "MetadataService.hpp"
#include "TelemetryManager.hpp"

#ifdef TECFLIX_USE_MONGODB
#include "MongoMetadataRepository.hpp"
#include <mongocxx/instance.hpp>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
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

static const uint64_t MAX_UPLOAD = 4ULL << 30;  // 4 GiB hard ceiling
static const size_t MAX_FORM = 64 * 1024;
static fs::path DATA_DIR = "data";
static fs::path WEB_DIR = "web/dist";
static string gFfmpeg;

// ---------------------------------------------------------------- utilities

static string getEnvOrDefault(const char* name, const string& def) {
    const char* v = std::getenv(name);
    return (v && *v) ? string(v) : def;
}
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
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) &&
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
static vector<string> parseCsvTags(const string& raw) {
    vector<string> out;
    size_t i = 0;
    while (i <= raw.size()) {
        size_t comma = raw.find(',', i);
        if (comma == string::npos) comma = raw.size();
        string tag = trim(raw.substr(i, comma - i));
        if (!tag.empty()) out.push_back(tag);
        i = comma + 1;
    }
    return out;
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
static string extractJsonStringField(const string& body, const string& key) {
    string needle = "\"" + key + "\"";
    size_t kpos = body.find(needle);
    if (kpos == string::npos) return "";
    size_t colon = body.find(':', kpos + needle.size());
    if (colon == string::npos) return "";
    size_t q1 = body.find('"', colon + 1);
    if (q1 == string::npos) return "";
    string val;
    for (size_t i = q1 + 1; i < body.size(); ++i) {
        if (body[i] == '\\' && i + 1 < body.size()) {
            val.push_back(body[++i]);
        } else if (body[i] == '"') {
            return val;
        } else {
            val.push_back(body[i]);
        }
    }
    return "";
}
static string formBodyToJson(const string& formBody) {
    auto f = parseForm(formBody);
    std::ostringstream oss;
    oss << "{";
    bool first = true;
    for (const auto& [k, v] : f) {
        if (!first) oss << ",";
        oss << jstr(k) << ":" << jstr(v);
        first = false;
    }
    oss << "}";
    return oss.str();
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
    string topic;
    vector<string> tags;
};

static std::mutex gMutex;
static vector<Video> gVideos;
static std::map<string, string> gSessions;  // token -> username
static std::unordered_map<std::string, size_t> gVideoIndex;
static std::mutex gQueueMutex;
static std::condition_variable gQueueCondition;
static std::queue<sock_t> gConnectionQueue;
static std::atomic<bool> gServerRunning{true};
static sock_t gListenSocket = INVALID_SOCKET;

// Instancias globales de los repositorios y servicios
static std::shared_ptr<tecflix::users::IUserRepository> gUserRepository;
static std::shared_ptr<tecflix::metadata::MetadataService> gMetadataService;
static std::shared_ptr<tecflix::metadata::MetadataController> gMetadataController;

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
static void saveVideos();

/// Quita un video del indice local y de videos.db (idempotente). Con MongoDB el repositorio remoto
/// borra su documento, pero el indice local seguia guardandolo y al reiniciar se re-sincronizaba a Mongo.
static void eraseVideoLocal(const string& id) {
    std::lock_guard<std::mutex> g(gMutex);
    auto it = gVideoIndex.find(id);
    if (it == gVideoIndex.end()) return;
    gVideos.erase(gVideos.begin() + static_cast<std::ptrdiff_t>(it->second));
    gVideoIndex.clear();
    for (size_t i = 0; i < gVideos.size(); ++i) gVideoIndex[gVideos[i].id] = i;
    saveVideos();
}

static void saveVideos() {  // caller holds gMutex
    std::ofstream o(DATA_DIR / "videos.db", std::ios::trunc);
    for (auto& v : gVideos)
        o << v.id << '\t' << v.owner << '\t' << clean(v.title) << '\t' << v.visibility << '\t' << v.file << '\t'
          << clean(v.orig) << '\t' << v.mime << '\t' << v.size << '\t' << v.uploaded << '\t' << v.created << '\t'
          << v.duration << '\t' << v.width << '\t' << v.height << '\t' << v.tracks << '\t' << v.vcodec << '\t'
          << v.acodec << '\t' << v.brand << '\t' << v.dash << '\n';
}
static void loadVideosDb() {
    std::string line;
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
        if (x.dash == "processing") x.dash = "failed";

        gVideos.push_back(x);
        gVideoIndex[x.id] = gVideos.size() - 1;
    }
}

// ---------------------------------------------- MetadataRepository Adapter

namespace {

std::string mapDashStateForMetadata(const std::string& rawDash) {
    if (rawDash == "ready") return "ready";
    if (rawDash == "processing") return "processing";
    if (rawDash == "failed" || rawDash == "error") return "error";
    return "pending";
}

tecflix::metadata::VideoMetadata toDomainMetadata(const Video& v) {
    tecflix::metadata::VideoMetadata m;
    m.id = v.id;
    m.owner = v.owner;
    m.title = v.title;
    m.topic = v.topic;
    m.tags = v.tags;
    m.visibility = (v.visibility == "private") ? "private" : "public";
    m.file = v.file;
    m.dash = mapDashStateForMetadata(v.dash);
    m.duration = v.duration;
    m.sizeBytes = static_cast<int64_t>(v.size);
    m.uploaded = v.uploaded;
    return m;
}

class EmbeddedMetadataRepository : public tecflix::metadata::IMetadataRepository {
public:
    bool create(const tecflix::metadata::VideoMetadata& video) override {
        std::lock_guard<std::mutex> g(gMutex);
        auto it = gVideoIndex.find(video.id);
        if (it != gVideoIndex.end()) {
            Video& existing = gVideos[it->second];
            existing.title = video.title;
            existing.topic = video.topic;
            existing.tags = video.tags;
            existing.visibility = video.visibility;
            existing.duration = video.duration;
            if (video.sizeBytes > 0) existing.size = static_cast<uint64_t>(video.sizeBytes);
            saveVideos();
            return true;
        }
        Video v;
        v.id = video.id;
        v.owner = video.owner;
        v.title = video.title;
        v.topic = video.topic;
        v.tags = video.tags;
        v.visibility = video.visibility;
        v.file = video.file;
        v.dash = video.dash;
        v.duration = video.duration;
        v.size = static_cast<uint64_t>(std::max<int64_t>(0, video.sizeBytes));
        v.uploaded = video.uploaded;
        gVideos.push_back(v);
        gVideoIndex[v.id] = gVideos.size() - 1;
        saveVideos();
        return true;
    }

    std::optional<tecflix::metadata::VideoMetadata> findById(const std::string& videoId) override {
        std::lock_guard<std::mutex> g(gMutex);
        auto it = gVideoIndex.find(videoId);
        if (it == gVideoIndex.end()) return std::nullopt;
        return toDomainMetadata(gVideos[it->second]);
    }

    tecflix::metadata::PaginatedVideos findPaginated(
        const tecflix::metadata::VideoQueryFilter& filter) override {
        std::vector<tecflix::metadata::VideoMetadata> filtered;
        {
            std::lock_guard<std::mutex> g(gMutex);
            for (const auto& vid : gVideos) {
                if (filter.scope == "mine") {
                    if (vid.owner != filter.requesterUsername) continue;
                } else {
                    if (vid.visibility == "private" && vid.owner != filter.requesterUsername) continue;
                }
                if (!filter.topicFilter.empty() && vid.topic != filter.topicFilter) continue;
                if (!filter.tagFilter.empty()) {
                    bool hasTag = std::find(vid.tags.begin(), vid.tags.end(), filter.tagFilter) != vid.tags.end();
                    if (!hasTag) continue;
                }
                filtered.push_back(toDomainMetadata(vid));
            }
        }

        bool asc = (filter.order == "asc");
        std::sort(filtered.begin(), filtered.end(),
                  [&](const tecflix::metadata::VideoMetadata& a,
                      const tecflix::metadata::VideoMetadata& b) {
                      if (filter.sortBy == "title") {
                          return asc ? (a.title < b.title) : (a.title > b.title);
                      }
                      if (filter.sortBy == "owner" || filter.sortBy == "author") {
                          return asc ? (a.owner < b.owner) : (a.owner > b.owner);
                      }
                      return asc ? (a.uploaded < b.uploaded) : (a.uploaded > b.uploaded);
                  });

        int totalItems = static_cast<int>(filtered.size());
        int totalPages = std::max(1, static_cast<int>(std::ceil(static_cast<double>(totalItems) / filter.limit)));
        int page = std::clamp(filter.page, 1, totalPages);

        size_t startIdx = static_cast<size_t>((page - 1) * filter.limit);
        size_t endIdx = std::min(filtered.size(), startIdx + static_cast<size_t>(filter.limit));

        std::vector<tecflix::metadata::VideoMetadata> pageItems;
        if (startIdx < filtered.size()) {
            pageItems.assign(filtered.begin() + startIdx, filtered.begin() + endIdx);
        }

        return tecflix::metadata::PaginatedVideos{pageItems, page, filter.limit, totalPages, totalItems};
    }

    bool update(const tecflix::metadata::VideoMetadata& video) override {
        std::lock_guard<std::mutex> g(gMutex);
        auto it = gVideoIndex.find(video.id);
        if (it == gVideoIndex.end()) return false;
        Video& v = gVideos[it->second];
        v.title = video.title;
        v.topic = video.topic;
        v.tags = video.tags;
        v.visibility = video.visibility;
        saveVideos();
        return true;
    }

    bool updateDashStatus(const std::string& videoId, const std::string& dashStatus) override {
        std::lock_guard<std::mutex> g(gMutex);
        auto it = gVideoIndex.find(videoId);
        if (it == gVideoIndex.end()) return false;
        gVideos[it->second].dash = (dashStatus == "error") ? "failed" : dashStatus;
        saveVideos();
        return true;
    }

    bool remove(const std::string& videoId) override {
        std::lock_guard<std::mutex> g(gMutex);
        auto it = gVideoIndex.find(videoId);
        if (it == gVideoIndex.end()) return false;
        gVideos.erase(gVideos.begin() + static_cast<std::ptrdiff_t>(it->second));
        gVideoIndex.clear();
        for (size_t i = 0; i < gVideos.size(); ++i) {
            gVideoIndex[gVideos[i].id] = i;
        }
        saveVideos();
        return true;
    }
};

}  // namespace

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
    if (!v.created) {
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
    if (auto hdr = r.h.find("x-user"); hdr != r.h.end() && !hdr->second.empty()) {
        return hdr->second;
    }
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

static void revokeUserSessions(const string& username) {
    std::lock_guard<std::mutex> g(gMutex);
    for (auto it = gSessions.begin(); it != gSessions.end();) {
        if (lower(it->second) == lower(username)) {
            it = gSessions.erase(it);
        } else {
            ++it;
        }
    }
}

// ------------------------------------------------------------------- routes

static string userProfileJson(const tecflix::users::UserRecord& u) {
    uint64_t remaining = (u.used_bytes >= u.quota_bytes) ? 0 : (u.quota_bytes - u.used_bytes);
    std::ostringstream o;
    o << "{\"username\":" << jstr(u.username)
      << ",\"quota_bytes\":" << u.quota_bytes
      << ",\"used_bytes\":" << u.used_bytes
      << ",\"remaining_bytes\":" << remaining << "}";
    return o.str();
}

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
    string name, pw;
    if (!r.body.empty() && r.body.front() == '{') {
        name = extractJsonStringField(r.body, "username");
        pw = extractJsonStringField(r.body, "password");
    } else {
        auto f = parseForm(r.body);
        name = f["username"];
        pw = f["password"];
    }
    name = trim(name);

    if (!gUserRepository) return fail(s, 500, "User repository unavailable");

    tecflix::users::UserRecord profile;
    if (reg) {
        bool ok = name.size() >= 3 && name.size() <= 20 &&
                  std::all_of(name.begin(), name.end(), [](unsigned char c) { return isalnum(c) || c == '_'; });
        if (!ok) return fail(s, 400, "Username must be 3-20 letters, digits or underscore");
        if (pw.size() < 6) return fail(s, 400, "Password must have at least 6 characters");

        const uint64_t defaultQuota = static_cast<uint64_t>(
            std::stoull(getEnvOrDefault("DEFAULT_QUOTA_BYTES",
                                        std::to_string(tecflix::users::DEFAULT_MAX_QUOTA_BYTES))));

        tecflix::users::UserRecord rec;
        std::strncpy(rec.username, name.c_str(), sizeof(rec.username) - 1);
        string salt = randHex(16);
        string hash = hashPassword(salt, pw);
        std::strncpy(rec.salt, salt.c_str(), sizeof(rec.salt) - 1);
        std::strncpy(rec.password_hash, hash.c_str(), sizeof(rec.password_hash) - 1);
        rec.quota_bytes = defaultQuota;
        rec.used_bytes = 0;
        rec.created_at = static_cast<int64_t>(time(nullptr));

        if (!gUserRepository->createUser(rec)) {
            return fail(s, 409, "Username already taken");
        }
        profile = rec;
    } else {
        auto found = gUserRepository->findByUsername(name);
        if (!found || hashPassword(found->salt, pw) != string(found->password_hash)) {
            return fail(s, 401, "Invalid username or password");
        }
        profile = *found;
        name = profile.username;
    }

    string tok = randHex(24);
    {
        std::lock_guard<std::mutex> g(gMutex);
        gSessions[tok] = name;
    }
    json(s, reg ? 201 : 200, userProfileJson(profile),
         "Set-Cookie: tf_session=" + tok + "; Path=/; HttpOnly; SameSite=Lax; Max-Age=604800\r\n");
}

static void handleUsersEndpoint(sock_t s, Req& r, const string& me) {
    const string targetUser = urldecode(r.path.substr(11));  // after "/api/users/"
    if (targetUser.empty() || targetUser.find('/') != string::npos) {
        return fail(s, 400, "Invalid username path");
    }
    if (me.empty()) {
        return fail(s, 401, "Authentication required");
    }
    if (lower(me) != lower(targetUser)) {
        return fail(s, 403, "Forbidden: cannot access another user's profile");
    }
    if (!gUserRepository) {
        return fail(s, 500, "User repository unavailable");
    }

    if (r.method == "GET") {
        auto found = gUserRepository->findByUsername(targetUser);
        if (!found) return fail(s, 404, "User not found");
        return json(s, 200, userProfileJson(*found));
    }

    if (r.method == "PUT") {
        if (!readBody(s, r)) return fail(s, 400, "Bad request body");
        string newPw;
        if (!r.body.empty() && r.body.front() == '{') {
            newPw = extractJsonStringField(r.body, "password");
            if (newPw.empty()) newPw = extractJsonStringField(r.body, "newPassword");
        } else {
            auto f = parseForm(r.body);
            newPw = !f["password"].empty() ? f["password"] : f["newPassword"];
        }
        if (newPw.size() < 6) {
            return fail(s, 400, "Password must have at least 6 characters");
        }
        string newSalt = randHex(16);
        string newHash = hashPassword(newSalt, newPw);
        // quota_bytes es estrictamente inmutable: se ignora cualquier intento de cambiarlo
        if (!gUserRepository->updateCredentials(targetUser, newSalt, newHash)) {
            return fail(s, 404, "User not found");
        }
        auto updated = gUserRepository->findByUsername(targetUser);
        if (!updated) return fail(s, 404, "User not found");
        return json(s, 200, userProfileJson(*updated));
    }

    if (r.method == "DELETE") {
        if (!gUserRepository->deleteUser(targetUser)) {
            return fail(s, 404, "User not found");
        }
        revokeUserSessions(targetUser);
        return json(s, 200, "{\"deleted\":true,\"username\":" + jstr(targetUser) + "}",
                    "Set-Cookie: tf_session=; Path=/; Max-Age=0\r\n");
    }

    return fail(s, 405, "Method not allowed");
}

// ---- DASH packaging (ffmpeg): data/dash/<id>/manifest.mpd + fMP4 segments ----

static std::mutex gDashMutex;
static std::condition_variable gDashCondition;
static int gDashActive = 0;
static const int MAX_DASH_JOBS = 2;

static void packageDash(Video v) {
    tecflix::observability::ScopedSpan span("ffmpeg.packageDash", "", {{"video.id", v.id}});
    {
        std::unique_lock<std::mutex> lock(gDashMutex);
        gDashCondition.wait(lock, [] {
            return gDashActive < MAX_DASH_JOBS;
        });
        ++gDashActive;
    }
    std::error_code ec;
    fs::path in = fs::absolute(DATA_DIR / "videos" / v.file, ec);
    fs::path out = fs::absolute(DATA_DIR / "dash" / v.id, ec);
    fs::create_directories(out, ec);
    string vc = v.vcodec == "avc1" ? "copy" : "libx264 -preset veryfast -crf 23 -pix_fmt yuv420p -g 96 -sc_threshold 0";
    string ac = v.acodec == "mp4a" ? "copy" : "aac -b:a 128k";
#ifdef _WIN32
    string cd = "cd /d \"" + out.string() + "\" && ";
#else
    string cd = "cd \"" + out.string() + "\" && ";
#endif
    string cmd = cd + "\"" + gFfmpeg + "\" -nostdin -y -loglevel error -i \"" + in.string() +
                 "\" -map 0:v:0 -map 0:a:0? -c:v " + vc + " -c:a " + ac +
                 " -f dash -seg_duration 4 -use_template 1 -use_timeline 1 manifest.mpd";
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";
#endif
    bool ok = std::system(cmd.c_str()) == 0 && fs::exists(out / "manifest.mpd", ec);
    bool found = false;
    {
        std::lock_guard<std::mutex> g(gMutex);
        auto indexIt = gVideoIndex.find(v.id);
        if (indexIt != gVideoIndex.end()) {
            Video& x = gVideos[indexIt->second];
            x.dash = ok ? "ready" : "failed";
            found = true;
        }
        if (found) saveVideos();
    }
    if (found && gMetadataService) {
        gMetadataService->updateDashState(v.id, ok ? "ready" : "error");
    }
    if (!ok || !found) fs::remove_all(out, ec);
    span.setStatusCode(ok ? 200 : 500);
    {
        std::lock_guard<std::mutex> lock(gDashMutex);
        --gDashActive;
    }
    gDashCondition.notify_one();
}

static void handleUpload(sock_t s, Req& r, const string& me) {
    if (me.empty()) return fail(s, 401, "Log in to upload");
    if (!r.clen) return fail(s, 411, "Content-Length required");
    if (r.clen > MAX_UPLOAD) return fail(s, 413, "File too large (max 4 GiB)");

    string orig = fs::path(r.q["filename"]).filename().string();
    string ext = lower(fs::path(orig).extension().string());
    string mime = mimeFor(ext);
    if (mime.empty()) return fail(s, 400, "Unsupported type. Use mp4, m4v, mov, webm, ogv or mkv");

    // Invariante de cuota: validar y reservar antes de escribir un solo byte en disco
    if (!gUserRepository) return fail(s, 500, "User repository unavailable");
    auto userOpt = gUserRepository->findByUsername(me);
    if (!userOpt) return fail(s, 401, "User session invalid");
    if (!gUserRepository->reserveQuota(me, r.clen)) {
        return fail(s, 413, "Storage quota exceeded");
    }

    string title = trim(clean(r.q["title"]));
    if (title.empty()) title = fs::path(orig).stem().string();
    if (title.size() > 120) title.resize(120);
    string id = randHex(6), file = id + ext;
    fs::path path = DATA_DIR / "videos" / file;

    {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            gUserRepository->releaseQuota(me, r.clen);
            return fail(s, 500, "Cannot store file");
        }
        uint64_t got = std::min<uint64_t>(r.pre.size(), r.clen);
        out.write(r.pre.data(), (std::streamsize)got);
        vector<char> buf(256 * 1024);
        while (got < r.clen) {
            int n = recv(s, buf.data(), (int)std::min<uint64_t>(buf.size(), r.clen - got), 0);
            if (n <= 0) {
                out.close();
                std::error_code ec;
                fs::remove(path, ec);
                gUserRepository->releaseQuota(me, r.clen);
                return;
            }
            out.write(buf.data(), n);
            got += (uint64_t)n;
        }
    }

    Video v;
    v.id = id;
    v.owner = me;
    v.title = title;
    v.file = file;
    v.orig = clean(orig);
    v.mime = mime;
    v.visibility = r.q["visibility"] == "public" ? "public" : "private";
    v.uploaded = (int64_t)time(nullptr);
    v.topic = trim(clean(r.q["topic"]));
    v.tags = parseCsvTags(r.q["tags"]);
    probe(v, path);
    if (!gFfmpeg.empty()) v.dash = "processing";

    {
        std::lock_guard<std::mutex> g(gMutex);
        gVideos.push_back(v);
        gVideoIndex[v.id] = gVideos.size() - 1;
        saveVideos();
    }

    if (gMetadataService) {
        gMetadataService->registerVideo(toDomainMetadata(v));
    }

    if (!gFfmpeg.empty())
        std::thread(packageDash, v).detach();

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
    static thread_local std::array<char, 256 * 1024> buf;
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
    if (!fs::is_regular_file(path, ec) && path.extension().empty())
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

    // Delegar endpoints de Metadata API al controlador dedicado
    if (gMetadataController && (p == "/api/videos" || p.rfind("/api/videos/", 0) == 0)) {
        if (r.clen > 0 && !readBody(s, r)) {
            return fail(s, 400, "Bad request body");
        }

        tecflix::metadata::HttpRequest metaReq;
        metaReq.method = m;
        metaReq.path = p;
        metaReq.queryParams = r.q;
        metaReq.headers = r.h;
        if (!me.empty()) {
            metaReq.headers["x-user"] = me;
        }

        if (p.rfind("/api/videos/", 0) == 0 && m == "POST") {
            metaReq.method = "PATCH";
            metaReq.body = (!r.body.empty() && r.body.front() != '{') ? formBodyToJson(r.body) : r.body;
        } else {
            metaReq.body = r.body;
        }

        string deletedFile;
        string deletedId;
        string deletedOwner;
        uint64_t deletedSize = 0;
        if (m == "DELETE" && p.rfind("/api/videos/", 0) == 0) {
            deletedId = p.substr(12);
            std::lock_guard<std::mutex> g(gMutex);
            auto idxIt = gVideoIndex.find(deletedId);
            if (idxIt != gVideoIndex.end()) {
                deletedFile = gVideos[idxIt->second].file;
                deletedOwner = gVideos[idxIt->second].owner;
                deletedSize = gVideos[idxIt->second].size;
            }
        }

        auto res = gMetadataController->handleRequest(metaReq);

        if (m == "DELETE" && res.statusCode == 200 && !deletedId.empty()) {
            std::error_code ec;
            if (!deletedFile.empty()) fs::remove(DATA_DIR / "videos" / deletedFile, ec);
            fs::remove_all(DATA_DIR / "dash" / deletedId, ec);
            eraseVideoLocal(deletedId);
            if (gUserRepository && !deletedOwner.empty() && deletedSize > 0) {
                gUserRepository->releaseQuota(deletedOwner, deletedSize);
            }
        }

        return json(s, res.statusCode, res.jsonBody);
    }

    string traceId = r.h.count("x-trace-id") ? r.h["x-trace-id"] : "";
    tecflix::observability::ScopedSpan span(m + " " + p, traceId, {{"http.method", m}, {"http.route", p}});

    if (p == "/health" && m == "GET") {
        span.setStatusCode(200);
        return json(s, 200, "{\"status\":\"healthy\",\"service\":\"tecflix-server\"}");
    }
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
    if (p == "/api/me" && m == "GET") {
        if (me.empty()) return fail(s, 401, "Not authenticated");
        if (!gUserRepository) return json(s, 200, "{\"username\":" + jstr(me) + "}");
        auto found = gUserRepository->findByUsername(me);
        if (!found) return fail(s, 404, "User not found");
        return json(s, 200, userProfileJson(*found));
    }
    if (p.rfind("/api/users/", 0) == 0) {
        return handleUsersEndpoint(s, r, me);
    }
    if (p == "/api/upload" && m == "POST") return handleUpload(s, r, me);

    // /dash/<id>/<file>: manifest and segments of a packaged video
    if (p.rfind("/dash/", 0) == 0 && (m == "GET" || m == "HEAD")) {
        string rest = p.substr(6);
        size_t sl = rest.find('/');
        string did = rest.substr(0, sl), name = sl == string::npos ? "" : rest.substr(sl + 1);
        bool okName = !name.empty() && name.find("..") == string::npos &&
                      std::all_of(name.begin(), name.end(),
                                  [](unsigned char c) { return isalnum(c) || c == '.' || c == '_' || c == '-'; });
        if (!isHexId(did) || !okName) { span.setStatusCode(404); return fail(s, 404, "Not found"); }
        {
            std::lock_guard<std::mutex> g(gMutex);
            auto indexIt = gVideoIndex.find(did);
            if (indexIt == gVideoIndex.end()) { span.setStatusCode(404); return fail(s, 404, "Not found"); }
            Video& video = gVideos[indexIt->second];
            if (!canSee(video, me) || video.dash != "ready") { span.setStatusCode(404); return fail(s, 404, "Not found"); }
        }
        string ext = lower(fs::path(name).extension().string());
        return streamFile(s, r, DATA_DIR / "dash" / did / name, ext == ".mpd" ? "application/dash+xml" : "video/mp4");
    }

    // /video/<id>: streaming directo por rangos HTTP
    if (p.rfind("/video/", 0) == 0) {
        string id = p.substr(7);
        if (!isHexId(id)) { span.setStatusCode(404); return fail(s, 404, "Not found"); }
        Video copy;
        {
            std::lock_guard<std::mutex> g(gMutex);
            auto indexIt = gVideoIndex.find(id);
            if (indexIt == gVideoIndex.end()) { span.setStatusCode(404); return fail(s, 404, "Video not found"); }
            Video& video = gVideos[indexIt->second];
            if (!canSee(video, me)) { span.setStatusCode(404); return fail(s, 404, "Video not found"); }
            copy = video;
        }
        if (m == "GET" || m == "HEAD") {
            return streamFile(s, r, DATA_DIR / "videos" / copy.file, copy.mime);
        }
        span.setStatusCode(405);
        return fail(s, 405, "Method not allowed");
    }

    if (m == "GET") return serveStatic(s, p);
    span.setStatusCode(404);
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
static void workerThread() {
    while (true) {
        sock_t s;
        {
            std::unique_lock<std::mutex> lock(gQueueMutex);
            gQueueCondition.wait(lock, [] {
                return !gConnectionQueue.empty() || !gServerRunning.load();
            });
            if (!gServerRunning.load() && gConnectionQueue.empty()) {
                return;
            }
            s = gConnectionQueue.front();
            gConnectionQueue.pop();
        }
        handleConn(s);
    }
}
static void onShutdownSignal(int /*sig*/) {
    gServerRunning.store(false);
    if (gListenSocket != INVALID_SOCKET) {
        CLOSESOCK(gListenSocket);
        gListenSocket = INVALID_SOCKET;
    }
    gQueueCondition.notify_all();
}

int main(int argc, char** argv) {
    if (argc > 1 && string(argv[1]) == "--selftest") {
        string h = toHex(sha256("abc"));
        std::cout << h << "\n"
                  << (h == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" ? "OK" : "FAIL") << "\n";
        return 0;
    }
    int port = argc > 1 ? std::atoi(argv[1]) : std::stoi(getEnvOrDefault("PORT", "8080"));
    const string otlpHost = getEnvOrDefault("OTLP_HOST", "127.0.0.1");
    const int otlpPort = std::stoi(getEnvOrDefault("OTLP_PORT", "4318"));

    std::signal(SIGINT, onShutdownSignal);
    std::signal(SIGTERM, onShutdownSignal);

    tecflix::observability::TelemetryManager::instance().init("tecflix-server", otlpHost, otlpPort);

    fs::create_directories(DATA_DIR / "videos");
    loadVideosDb();

    // Inicializar repositorio de Usuarios con Árbol B en disco
    gUserRepository = std::make_shared<tecflix::users::BTreeUserRepository>(
        (DATA_DIR / "users.idx").string(),
        (DATA_DIR / "users.db").string());

    // Inicializar capa de Metadata API (MongoDB en despliegue o repositorio embebido en local)
#ifdef TECFLIX_USE_MONGODB
    mongocxx::instance mongoInstance{};
    const string mongoUri = getEnvOrDefault("MONGO_URI", "mongodb://localhost:27017");
    const string mongoDb = getEnvOrDefault("MONGO_DB", "tecflix");
    const string mongoColl = getEnvOrDefault("MONGO_COLLECTION", "videos");
    auto metaRepo = std::make_shared<tecflix::metadata::MongoMetadataRepository>(mongoUri, mongoDb, mongoColl);
    for (const auto& v : gVideos) {
        if (!metaRepo->findById(v.id)) {
            metaRepo->create(toDomainMetadata(v));
        }
    }
#else
    auto metaRepo = std::make_shared<EmbeddedMetadataRepository>();
#endif
    gMetadataService = std::make_shared<tecflix::metadata::MetadataService>(metaRepo);
    gMetadataController = std::make_shared<tecflix::metadata::MetadataController>(gMetadataService);

    {
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
            for (auto& v : gVideos)
                if (v.dash == "unavailable") { v.dash = "processing"; std::thread(packageDash, v).detach(); }
            saveVideos();
        }
    }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    gListenSocket = ls;
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((unsigned short)port);
    if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 64) != 0) {
        std::cerr << "Cannot listen on port " << port << "\n";
        tecflix::observability::TelemetryManager::instance().shutdown();
        return 1;
    }
    std::cout << "TecFlix running at http://localhost:" << port << "  (Ctrl+C to stop)\n";

    const unsigned int numWorkers = std::max(2u, std::thread::hardware_concurrency());
    std::vector<std::thread> workers;
    workers.reserve(numWorkers);
    for (unsigned int i = 0; i < numWorkers; ++i) {
        workers.emplace_back(workerThread);
    }

    while (gServerRunning.load()) {
        sock_t c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            if (!gServerRunning.load()) break;
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(gQueueMutex);
            gConnectionQueue.push(c);
        }
        gQueueCondition.notify_one();
    }

    gServerRunning.store(false);
    gQueueCondition.notify_all();
    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    tecflix::observability::TelemetryManager::instance().shutdown();
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}