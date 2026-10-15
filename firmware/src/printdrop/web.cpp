#include <vector>
#include "web.h"

#include <Arduino.h>
#include <WebServer.h>
#include <LittleFS.h>
#ifdef USE_SDIO
#include <SD_MMC.h>
#define SD_FS SD_MMC
#else
#include <SD.h>
#define SD_FS SD
#endif
#include <WiFi.h>
#include <lwip/sockets.h>

#include "config.h"
#include "logbuf.h"
#include <esp_core_dump.h>
#include "net.h"
#include "storage.h"
#include "auth.h"
#include "ws.h"
#include "ota.h"
#include "led.h"
#include <Update.h>

namespace web {
namespace {

WebServer server(80);
bool      wantReboot = false;

// Upload state. WebServer drives uploads through repeated callbacks, so it
// lives here; the card lock is taken per card operation inside them.
File   uploadFile;
bool   uploadHoldsLock = false;
// WebServer hands the upload over in ~1.4 KB pieces. Writing each one to the
// card is a separate locked FAT write, so they are collected here and written
// 16 KB at a time. uploadCardUs is the time spent in card writes, logged per
// upload, to tell card time from network time.
constexpr size_t kUploadBufSize = 16384;
uint8_t* uploadBuf    = nullptr;
size_t   uploadBufLen = 0;
uint32_t uploadCardUs = 0;

String uploadError;
String uploadName;
String uploadTmp;     // where the bytes go until the upload completes
size_t uploadBytes = 0;
// HTTPUpload::totalSize counts bytes received so far, not the expected total,
// so it cannot answer "how far along is this?". The request's Content-Length
// can: it overshoots by the multipart preamble and trailer, a few hundred
// bytes, which is why the percentage is clamped rather than trusted at the end.
size_t   uploadExpected  = 0;
uint32_t uploadStartMs   = 0;
uint32_t lastProgressMs  = 0;

// Writes the buffered bytes to the card. Returns false and sets uploadError
// on failure.
bool flushUploadBuf() {
    if (uploadBufLen == 0) return true;
    if (!storage::lock(true, 5000)) { uploadError = "Card busy"; return false; }
    const uint32_t t0 = micros();
    const bool ok = uploadFile.write(uploadBuf, uploadBufLen) == uploadBufLen;
    uploadCardUs += micros() - t0;
    storage::unlock();
    if (!ok) { uploadError = "Write failed — card full?"; return false; }
    uploadBufLen = 0;
    return true;
}

void log(const char* fmt, ...) {
    char buf[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Serial.println(buf);
    logbuf::add(buf);
}

// --- helpers ---------------------------------------------------------------

String jsonEscape(const String& s) {
    String o;
    o.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); ++i) {
        const char c = s[i];
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (static_cast<uint8_t>(c) < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    o += b;
                } else {
                    o += c;
                }
        }
    }
    return o;
}

// Normalises a client-supplied path and refuses anything that tries to escape
// the card root.
// Exactly WebServer's own test (Parsing.cpp: case-sensitive
// startsWith("multipart/")). A looser test, such as one that lowercased the
// value first, let "Multipart/Form-Data" through here while WebServer took
// the raw path, and server.upload() was null again.
bool isMultipart() {
    return server.header("Content-Type").startsWith("multipart/");
}

bool safePath(const String& in, String& out) {
    String p = in;
    p.trim();
    if (p.isEmpty()) { out = "/"; return true; }
    if (!p.startsWith("/")) p = "/" + p;
    // Reject "." and ".." as path parts, backslashes and control characters.
    // A substring test for ".." also refused names like "Wait...jpg", which
    // could then be uploaded but never deleted, renamed or downloaded.
    for (size_t i = 0; i < p.length(); ++i) {
        const char ch = p[i];
        if (ch == '\\' || (uint8_t)ch < 0x20) return false;
    }
    for (int start = 1; start <= (int)p.length();) {
        int end = p.indexOf('/', start);
        if (end < 0) end = p.length();
        const String part = p.substring(start, end);
        if (part == "." || part == "..") return false;
        start = end + 1;
    }
    while (p.indexOf("//") >= 0) p.replace("//", "/");
    if (p.length() > 1 && p.endsWith("/")) p.remove(p.length() - 1);
    out = p;
    return true;
}

String joinPath(const String& dir, const String& name) {
    if (dir.isEmpty() || dir == "/") return "/" + name;
    return dir + "/" + name;
}

// Browsers send Origin on cross-site requests. A page on another site must not
// be able to drive the device with Basic credentials the browser has cached
// (for example to hold the frame's drive off the bus with /api/batch/begin),
// so an Origin is accepted only when it names the host the request was sent
// to. Clients that send no Origin, such as curl and the iOS Shortcut, are not
// a browser acting for another site.
bool originAllowed() {
    String origin = server.header("Origin");
    if (origin.isEmpty()) return true;
    const int scheme = origin.indexOf("://");
    if (scheme < 0) return false;            // includes the opaque "null" origin
    String host = origin.substring(scheme + 3);
    String requested = server.hostHeader();
    host.toLowerCase();
    requested.toLowerCase();
    return !requested.isEmpty() && host == requested;
}

// The UI is served from the device itself, so it never needs CORS. Headers are
// only sent back to an Origin that names this device.
void sendCorsHeaders() {
    String origin = server.header("Origin");
    if (origin.isEmpty() || !originAllowed()) return;
    server.sendHeader("Access-Control-Allow-Origin", origin);
    server.sendHeader("Vary", "Origin");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
    server.sendHeader("Access-Control-Max-Age", "86400");
}

void sendJson(int code, const String& body) {
    sendCorsHeaders();
    server.sendHeader("Cache-Control", "no-store");
    server.send(code, "application/json", body);
}

void sendOk() { sendJson(200, "{\"ok\":true}"); }

void sendError(int code, const String& message) {
    sendJson(code, String("{\"ok\":false,\"error\":\"") + jsonEscape(message) + "\"}");
}

bool requireAuth() {
    if (!originAllowed()) {
        sendError(403, "Cross-site request refused");
        return false;
    }
    if (!auth::isRequired()) return true;
    String hdr = server.header("Authorization");
    if (auth::checkBasicAuth(hdr)) return true;
    // No WWW-Authenticate header: it makes the browser show its own login
    // box on top of the page's prompt. The page and the Shortcut send the
    // Authorization header themselves.
    sendError(401, "Authentication required");
    return false;
}

const char* contentTypeFor(const String& path) {
    if (path.endsWith(".html")) return "text/html";
    if (path.endsWith(".css"))  return "text/css";
    if (path.endsWith(".js"))   return "application/javascript";
    if (path.endsWith(".json")) return "application/json";
    if (path.endsWith(".svg"))  return "image/svg+xml";
    if (path.endsWith(".ico"))  return "image/x-icon";
    if (path.endsWith(".png"))  return "image/png";
    if (path.endsWith(".webp")) return "image/webp";
    if (path.endsWith(".woff2"))return "font/woff2";
    return "text/plain";
}

// --- static UI from LittleFS ----------------------------------------------

// streamFile() loops while the file reports bytes available and never checks
// that a read returned any; a damaged file (for example after a half-written
// UI update) would spin loopTask forever. Same reasoning as handleDownload.
void sendStatic(File& f, const String& type) {
    const size_t total = f.size();
    server.setContentLength(total);
    server.send(200, type, "");
    uint8_t buf[1024];
    size_t sent = 0;
    while (sent < total && server.client().connected()) {
        const int got = f.read(buf, min(sizeof(buf), total - sent));
        if (got <= 0) break;
        server.sendContent(reinterpret_cast<const char*>(buf), (size_t)got);
        sent += (size_t)got;
    }
}

bool serveFromLittleFS(String path) {
    if (path.endsWith("/")) path += "index.html";

    // Prefer a pre-compressed copy when one was shipped.
    const String gz = path + ".gz";
    if (LittleFS.exists(gz)) {
        File f = LittleFS.open(gz, "r");
        if (!f) return false;
        server.sendHeader("Content-Encoding", "gzip");
        server.sendHeader("Cache-Control", "no-cache");
        sendStatic(f, contentTypeFor(path));
        f.close();
        return true;
    }
    if (LittleFS.exists(path)) {
        File f = LittleFS.open(path, "r");
        if (!f) return false;
        server.sendHeader("Cache-Control", "no-cache");
        sendStatic(f, contentTypeFor(path));
        f.close();
        return true;
    }
    return false;
}

void handleNotFound() {
    if (server.method() == HTTP_OPTIONS) {
        sendCorsHeaders();
        server.send(204);
        return;
    }
    if (serveFromLittleFS(server.uri())) return;
    // Single-page app: unknown non-API routes fall back to the shell.
    if (!server.uri().startsWith("/api/") && serveFromLittleFS("/index.html")) return;
    sendError(404, "Not found");
}

// --- API: status -----------------------------------------------------------

void handleStatus() {
    const uint64_t total = storage::totalBytes();
    const uint64_t used  = storage::usedBytes();
    const uint64_t freeB = total > used ? total - used : 0;

    String j = "{";
    j += "\"name\":\"" PRINTDROP_NAME "\",";
    j += "\"version\":\"" PRINTDROP_VERSION "\",";
    j += "\"resetReason\":\"" + String(logbuf::resetReason()) + "\",";
    j += "\"hostname\":\"" + jsonEscape(net::hostname()) + "\",";
    j += "\"ip\":\"" + net::localIp().toString() + "\",";
    j += "\"ssid\":\"" + jsonEscape(net::currentSsid()) + "\",";
    j += "\"rssi\":" + String(net::rssi()) + ",";
    j += "\"mode\":\"" + String(net::isAccessPoint() ? "ap" : "sta") + "\",";
    j += "\"card\":{";
    j +=   "\"present\":" + String(storage::cardMounted() ? "true" : "false") + ",";
    j +=   "\"totalBytes\":" + String(total) + ",";
    j +=   "\"usedBytes\":" + String(used) + ",";
    j +=   "\"freeBytes\":" + String(freeB) + ",";
    // spiHz kept for backwards compatibility; bus* is canonical on feat/sdio.
    j +=   "\"spiHz\":" + String(storage::spiFrequency()) + ",";
    j +=   "\"busHz\":" + String(storage::busFrequency()) + ",";
    j +=   "\"busMode\":\"" + String(storage::busMode()) + "\",";
    j +=   "\"busWidth\":" + String(storage::busWidth());
    j += "},";
    j += "\"usb\":{";
    j +=   "\"hostPresent\":" + String(storage::usbHostPresent() ? "true" : "false") + ",";
    j +=   "\"mediaPresent\":" + String(storage::usbMediaPresent() ? "true" : "false") + ",";
    j +=   "\"batchPending\":" + String(storage::batchPending() ? "true" : "false");
    j += "},";
    j += "\"discovery\":{";
    j +=   "\"mdns\":" + String(PRINTDROP_ENABLE_MDNS ? "true" : "false") + ",";
    j +=   "\"llmnr\":" + String(PRINTDROP_ENABLE_LLMNR ? "true" : "false");
    j += "},";
    j += "\"auth\":{";
    j +=   "\"required\":" + String(auth::isRequired() ? "true" : "false") + ",";
    j +=   "\"user\":\"" + jsonEscape(auth::currentUser()) + "\"";
    j += "},";
    j += "\"wsPort\":81,";
    j += "\"ota\":{";
    j +=   "\"enabled\":" + String(PRINTDROP_ENABLE_OTA ? "true" : "false") + ",";
    j +=   "\"version\":\"" + jsonEscape(ota::currentVersion()) + "\"";
    {
        // /api/status is open and polled every 5 s by each page; checking the
        // card for an update image takes the card lock, so cache it a minute.
        static uint32_t checkedAt = 0;
        static bool     sdAvail   = false;
        static String   sdVer;
        if (checkedAt == 0 || millis() - checkedAt > 60000) {
            String n;
            sdVer = "";
            sdAvail = ota::checkSD(&sdVer, &n);
            checkedAt = millis() | 1;
        }
        j += ",\"sdAvailable\":" + String(sdAvail ? "true" : "false");
        if (sdAvail) { j += ",\"sdVersion\":\"" + jsonEscape(sdVer) + "\""; }
    }
    j += "},";
    j += "\"uptimeMs\":" + String(millis());
    j += "}";
    // Push to WS clients as well
    ws::broadcastStatus(j);
    sendJson(200, j);
}

// --- API: listing ----------------------------------------------------------

void handleList() {
    if (!requireAuth()) return;
    String path;
    if (!safePath(server.arg("path"), path)) return sendError(400, "Invalid path");

    // The card lock is taken per entry, not for the whole listing: each
    // openNextFile() is a FAT lookup from the start of the folder, so a folder
    // of thousands of photos held the lock for long enough to fail the
    // frame's USB reads.
    File dir;
    {
        storage::Guard g(false);
        if (!g.ok()) return sendError(503, "Card busy");
        dir = SD_FS.open(path);
        if (!dir) return sendError(404, "No such folder");
        if (!dir.isDirectory()) { dir.close(); return sendError(400, "Not a folder"); }
    }

    String j = "{\"path\":\"" + jsonEscape(path) + "\",\"entries\":[";
    bool first = true, outOfMemory = false, busy = false;
    for (;;) {
        String name; bool isDir = false; uint32_t size = 0;
        {
            storage::Guard g(false, 5000);
            if (!g.ok()) { busy = true; break; }
            File f = dir.openNextFile();
            if (!f) break;
            name = f.name();
            isDir = f.isDirectory();
            size = (uint32_t)f.size();
            f.close();
        }
        // Some cores hand back a full path; the UI only wants the leaf.
        const int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        String piece = first ? "" : ",";
        first = false;
        piece += "{\"name\":\"" + jsonEscape(name) + "\",\"dir\":" + String(isDir ? "true" : "false") +
                 ",\"size\":" + String(size) + "}";
        // String::concat fails silently when the heap runs out, which used to
        // send truncated JSON the page could not parse.
        if (!j.concat(piece)) { outOfMemory = true; break; }
    }
    {
        storage::Guard g(false, 20000);
        dir.close();
    }
    if (busy) return sendError(503, "Card busy");
    if (outOfMemory || !j.concat("]}")) return sendError(507, "Folder too large to list");
    sendJson(200, j);
}

// --- API: mutations --------------------------------------------------------

void removeRecursive(const String& path) {
    File f = SD_FS.open(path);
    if (!f) return;
    if (!f.isDirectory()) { f.close(); SD_FS.remove(path); return; }
    for (File child = f.openNextFile(); child; child = f.openNextFile()) {
        String name = child.name();
        const int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        const bool isDir = child.isDirectory();
        child.close();
        if (isDir) removeRecursive(joinPath(path, name));
        else       SD_FS.remove(joinPath(path, name));
    }
    f.close();
    SD_FS.rmdir(path);
}

void handleDelete() {
    if (!requireAuth()) return;
    String path;
    if (!safePath(server.arg("path"), path)) return sendError(400, "Invalid path");
    if (path == "/") return sendError(400, "Refusing to delete the card root");

    {
        storage::Guard g(true);
        if (!g.ok()) return sendError(503, "Card busy");
        if (!SD_FS.exists(path)) return sendError(404, "No such file");

        removeRecursive(path);
        if (SD_FS.exists(path)) return sendError(500, "Delete failed");
        log("[web] deleted %s", path.c_str());
    }
    // Auto-refresh printer view so the printer re-reads the FAT without
    // requiring the user to press Eject/refresh manually.
    storage::refreshHostView();
    sendOk();
}

void handleMkdir() {
    if (!requireAuth()) return;
    String path;
    if (!safePath(server.arg("path"), path)) return sendError(400, "Invalid path");
    if (path == "/") return sendError(400, "Invalid folder name");

    {
        storage::Guard g(true);
        if (!g.ok()) return sendError(503, "Card busy");
        if (SD_FS.exists(path)) return sendError(409, "Already exists");
        if (!SD_FS.mkdir(path)) return sendError(500, "Could not create folder");
    }
    storage::refreshHostView();
    sendOk();
}

void handleRename() {
    if (!requireAuth()) return;
    String from, to;
    if (!safePath(server.arg("from"), from) || !safePath(server.arg("to"), to)) {
        return sendError(400, "Invalid path");
    }
    if (from == "/" || to == "/") return sendError(400, "Invalid path");

    {
        storage::Guard g(true);
        if (!g.ok()) return sendError(503, "Card busy");
        if (!SD_FS.exists(from)) return sendError(404, "No such file");
        if (SD_FS.exists(to))    return sendError(409, "Target already exists");
        if (!SD_FS.rename(from, to)) return sendError(500, "Rename failed");
    }
    storage::refreshHostView();
    sendOk();
}

void handleDownload() {
    if (!requireAuth()) return;
    String path;
    if (!safePath(server.arg("path"), path)) return sendError(400, "Invalid path");

    // The card lock is held per 1 KB read, not for the whole response: the
    // frame's USB reads give up after 2 s, and a phone downloading a photo at
    // Wi-Fi speed (or a stalled one) would otherwise starve them throughout.
    File f;
    size_t total = 0;
    {
        storage::Guard g(false, 20000);
        if (!g.ok()) return sendError(503, "Card busy");
        f = SD_FS.open(path, FILE_READ);
        if (!f) return sendError(404, "No such file");
        if (f.isDirectory()) { f.close(); return sendError(400, "Is a folder"); }
        total = f.size();
    }

    String name = path.substring(path.lastIndexOf('/') + 1);
    sendCorsHeaders();
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");

    // Deliberately not server.streamFile(): it hands the File to
    // WiFiClient::write(Stream&), which loops `while (stream.available())` and
    // never checks whether readBytes() actually returned anything. A file whose
    // cluster chain is damaged reads 0 bytes forever, available() never falls,
    // and the loop spins on loopTask -- which takes the web server AND the
    // serial console down until someone power-cycles the board. Reproduced
    // three times; see docs/bugs.md. Send it ourselves so a failed read ends
    // the response instead.
    server.setContentLength(total);
    server.send(200, "application/octet-stream", "");

    uint8_t  buf[1024];
    size_t   sent = 0;
    while (sent < total) {
        if (!server.client().connected()) {
            log("[web] download of %s abandoned by the client at %u/%u bytes",
                path.c_str(), (unsigned)sent, (unsigned)total);
            break;
        }
        const size_t want = (total - sent) < sizeof(buf) ? (total - sent) : sizeof(buf);
        int got = -1;
        if (storage::lock(false, 5000)) {
            got = f.read(buf, want);
            storage::unlock();
        }
        if (got <= 0) {
            // Short body against a declared Content-Length: the client reports a
            // truncated transfer, which is what we want it to see.
            log("[web] read failed on %s at %u/%u bytes, truncating the response",
                path.c_str(), (unsigned)sent, (unsigned)total);
            break;
        }
        server.sendContent(reinterpret_cast<const char*>(buf), (size_t)got);
        sent += (size_t)got;
    }
    if (storage::lock(false, 20000)) { f.close(); storage::unlock(); }
}

void handleEject() {
    if (!requireAuth()) return;
    storage::endBatchNow();
    sendOk();
}

// Multi-file uploads: begin, upload each file, end. The frame sees one
// unplug at begin and one plug-in at end, whatever the number of files.
void handleBatchBegin() {
    if (!requireAuth()) return;
    if (!storage::beginBatch()) return sendError(503, "Card busy");
    sendOk();
}

// Unlike eject, ending a batch that is not open does nothing: the UI and the
// Shortcut send end unconditionally, and that must not unplug the frame.
void handleBatchEnd() {
    if (!requireAuth()) return;
    if (storage::batchPending()) storage::endBatchNow();
    sendOk();
}

// Recovery hatch: once a host has ejected the media it keeps ejecting it, and
// re-presenting the media does not change its mind. Leaving the bus entirely
// does, and that is the only way back short of unplugging the cable.
void handleReattach() {
    if (!requireAuth()) return;
    sendOk();               // answer first; the detach takes the drive away
    storage::reattachHost();
}

// --- API: upload -----------------------------------------------------------

void handleUploadData() {
    // WebServer sends a POST that is not multipart down its raw-body path,
    // which calls this same callback while server.upload() refers to a null
    // pointer. Any client posting the wrong type crashed the board.
    if (!isMultipart()) return;
    HTTPUpload& up = server.upload();

    if (up.status == UPLOAD_FILE_START) {
        // Clear the previous upload's name first: ABORTED must never act on a
        // path this request did not open (an unauthenticated abort used to
        // delete the last uploaded file).
        uploadName = "";
        uploadError = "";
        if (!originAllowed()) {
            uploadError = "Cross-site request refused";
            return;
        }
        if (auth::isRequired() && !auth::checkBasicAuth(server.header("Authorization"))) {
            uploadError = "Authentication required";
            return;
        }
        // A phone that leaves Wi-Fi mid-upload sends no FIN. The patched
        // WebServer abandons the upload after HTTP_MAX_POST_WAIT (5 s) without
        // a byte; these probes are a backstop that closes the socket itself
        // after about 25 s, where lwIP's default keepalive takes over two hours.
        {
            const int fd = server.client().fd();
            if (fd >= 0) {
                int on = 1, idle = 10, interval = 5, count = 3;
                const bool ok =
                    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) == 0 &&
                    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) == 0 &&
                    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) == 0 &&
                    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) == 0;
                if (!ok) log("[web] could not set upload keepalive (errno %d)", errno);
            } else {
                log("[web] upload has no socket to set keepalive on");
            }
        }
        led::setActivity(true);
        uploadBytes = 0;
        uploadStartMs = millis();
        lastProgressMs = 0;
        const int declared = server.clientContentLength();
        uploadExpected = declared > 0 ? (size_t)declared : 0;

        String dir;
        // Query-string arguments are parsed before the multipart body, so this
        // is readable here (see WebServer::_parseRequest).
        if (!safePath(server.arg("path"), dir)) { uploadError = "Invalid path"; return; }

        String name = up.filename;
        const int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        if (name.isEmpty()) { uploadError = "Missing filename"; return; }
        if (name.length() > 200) { uploadError = "Filename too long"; return; }
        uploadName = joinPath(dir, name);
        { String checked; if (!safePath(uploadName, checked)) { uploadError = "Invalid filename"; return; } }

        // The card lock is taken per card operation, not for the whole
        // upload: the frame keeps reading the drive during a batch, and its
        // reads time out after kMscLockTimeoutMs if an upload holds the lock
        // from start to end. uploadHoldsLock now means "this request opened
        // uploadName", which is what decides whether an abort may remove it.
        if (!uploadBuf) uploadBuf = (uint8_t*)malloc(kUploadBufSize);
        if (!uploadBuf) { uploadError = "Out of memory"; return; }
        uploadBufLen = 0;
        uploadCardUs = 0;
        if (!storage::lock(true, 20000)) { uploadError = "Card busy"; return; }
        // Write to a temporary name and rename over the target only once the
        // upload is complete, so a failed re-upload of an existing name does
        // not take the original photo with it.
        uploadTmp = uploadName + ".partial";
        if (SD_FS.exists(uploadTmp)) SD_FS.remove(uploadTmp);
        uploadFile = SD_FS.open(uploadTmp, FILE_WRITE);
        if (!uploadFile) uploadError = "Could not open file for writing";
        else uploadHoldsLock = true;
        storage::unlock();

    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (uploadFile && uploadError.isEmpty()) {
            bool wrote = true;
            size_t off = 0;
            while (wrote && off < up.currentSize) {
                const size_t n = min(kUploadBufSize - uploadBufLen, up.currentSize - off);
                memcpy(uploadBuf + uploadBufLen, up.buf + off, n);
                uploadBufLen += n;
                off += n;
                if (uploadBufLen == kUploadBufSize) wrote = flushUploadBuf();
            }
            if (wrote) {
                uploadBytes += up.currentSize;
                // Every one of these numbers used to be wrong. The rate was
                // timed around the card write alone — 0-1 ms for a 1436-byte
                // chunk — so it reported the card's write speed, ~1.4 MB/s,
                // rather than the ~200 KB/s the upload was actually moving.
                // The percentage divided by HTTPUpload::totalSize, which counts
                // bytes received so far, so it sat at 100 from the first chunk;
                // the ETA compared the same two counters and was always 0.
                const uint32_t now     = millis();
                const uint32_t elapsed = now - uploadStartMs;
                uint32_t rate = 0;   // bytes/s across the whole upload
                if (elapsed > 0) {
                    rate = (uint32_t)((uint64_t)uploadBytes * 1000ULL / elapsed);
                }
                uint8_t pct = 0;
                if (uploadExpected > 0) {
                    const uint64_t p = (uint64_t)uploadBytes * 100ULL / uploadExpected;
                    pct = (uint8_t)(p > 99 ? 99 : p);   // 100 belongs to FILE_END
                }
                uint32_t eta = 0;
                if (rate > 0 && uploadExpected > uploadBytes) {
                    eta = (uint32_t)((uploadExpected - uploadBytes) / rate);
                }
                // One broadcast per chunk is ~140 websocket frames per MB, all
                // of them on the same task doing the upload.
                if (now - lastProgressMs >= UPLOAD_PROGRESS_INTERVAL_MS) {
                    lastProgressMs = now;
                    ws::broadcastProgress(uploadName, pct, rate, eta);
                }
            }
        }

    } else if (up.status == UPLOAD_FILE_END) {
        if (uploadFile && uploadError.isEmpty()) flushUploadBuf();
        if (uploadFile && storage::lock(true, 20000)) {
            uploadFile.close();
            if (uploadHoldsLock) {
                if (uploadError.isEmpty()) {
                    if (SD_FS.exists(uploadName)) SD_FS.remove(uploadName);
                    if (!SD_FS.rename(uploadTmp, uploadName)) uploadError = "Could not finish writing the file";
                }
                // A failed write (card full, card busy) leaves a truncated file
                // the frame would try to show. Remove this request's own.
                if (!uploadError.isEmpty() && SD_FS.exists(uploadTmp)) SD_FS.remove(uploadTmp);
            }
            storage::unlock();
        } else if (uploadFile) {
            uploadError = "Card busy";
        }
        uploadHoldsLock = false;
        led::setActivity(false);
        if (uploadError.isEmpty()) {
            const uint32_t elapsed = millis() - uploadStartMs;
            const uint32_t rate = elapsed ? (uint32_t)((uint64_t)uploadBytes * 1000ULL / elapsed) : 0;
            log("[web] uploaded %s (%u bytes in %u ms, %u KB/s; card writes %u ms)",
                uploadName.c_str(), (unsigned)uploadBytes, (unsigned)elapsed,
                (unsigned)(rate / 1024), (unsigned)(uploadCardUs / 1000));
            ws::broadcastProgress(uploadName, 100, rate, 0);
        }

    } else if (up.status == UPLOAD_FILE_ABORTED) {
        // Do not leave a truncated photo on the card. Only this request's own
        // file, and only if this request opened it: otherwise there is nothing
        // of ours to remove (auth failed, card busy, or the abort came after
        // END).
        // Only a request that opened a file touches the card: a write lock
        // opens a batch, and an unauthenticated POST dropped mid-body would
        // otherwise make the frame re-read the card 30 s later.
        if ((uploadFile || uploadHoldsLock) && storage::lock(true, 20000)) {
            if (uploadFile) uploadFile.close();
            if (uploadHoldsLock && !uploadTmp.isEmpty() && SD_FS.exists(uploadTmp)) {
                SD_FS.remove(uploadTmp);
            }
            storage::unlock();
        }
        uploadHoldsLock = false;
        led::setActivity(false);
        uploadError = "Upload aborted";
    }
}

// Upload state is per request; clear it however the request ends, or the next
// request without a file part reports this one's name or error.
struct UploadStateReset {
    ~UploadStateReset() { uploadName = ""; uploadTmp = ""; uploadError = ""; uploadHoldsLock = false; }
};

void handleUploadDone() {
    UploadStateReset reset;
    if (!isMultipart()) return sendError(400, "Expected multipart/form-data with a file field");
    if (auth::isRequired() && !auth::checkBasicAuth(server.header("Authorization"))) {
        if (uploadFile && storage::lock(true, 5000)) { uploadFile.close(); storage::unlock(); }
        uploadHoldsLock = false;
        return sendError(401, "Authentication required");
    }
    if (!uploadError.isEmpty()) return sendError(500, uploadError);
    // uploadName is reset at UPLOAD_FILE_START; still empty means no file part
    // arrived, and the previous upload's name must not be reported as this one.
    if (uploadName.isEmpty()) return sendError(400, "No file in the request");
    String j = "{\"ok\":true,\"name\":\"" + jsonEscape(uploadName) + "\",";
    j += "\"size\":" + String((uint32_t)uploadBytes) + "}";
    // With a batch open (the normal case) refreshHostView() does nothing and
    // the host is told when the batch closes. Send the JSON first so the
    // client is not kept waiting by a refresh.
    sendJson(200, j);
    storage::refreshHostView();
}

// --- API: Wi-Fi ------------------------------------------------------------

void handleWifiScan() {
    if (!requireAuth()) return;
    const int n = WiFi.scanNetworks();
    String j = "{\"networks\":[";
    for (int i = 0; i < n && i < 25; ++i) {
        if (i) j += ",";
        j += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",";
        j += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
        j += "\"secure\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
    }
    j += "]}";
    WiFi.scanDelete();
    sendJson(200, j);
}

void handleWifiSave() {
    if (!requireAuth()) return;
    net::Config c = net::config();
    if (server.hasArg("ssid"))     c.ssid      = server.arg("ssid");
    if (server.hasArg("password")) c.password  = server.arg("password");
    if (server.hasArg("hostname")) {
        String raw = server.arg("hostname");
        raw.trim();
        if (!raw.isEmpty()) {
            String norm = net::normalizeHostname(raw);
            if (!net::isValidHostname(norm)) {
                return sendError(400, "Invalid hostname (a-z, 0-9, hyphen, 1-63 chars, not starting/ending with -)");
            }
            String cur = net::hostname();
            cur.toLowerCase();
            String lowNorm = norm;
            lowNorm.toLowerCase();
            if (lowNorm != cur && net::isHostnameTaken(norm, 1200)) {
                String free = net::findFreeHostname(norm);
                return sendError(409, "Hostname '" + norm + "' already taken, try '" + free + "'");
            }
            c.hostname = norm;
        }
    }
    if (server.hasArg("useStatic"))c.useStatic = server.arg("useStatic") == "true" ||
                                                 server.arg("useStatic") == "1";
    if (server.hasArg("ip"))       c.ip      = server.arg("ip");
    if (server.hasArg("gw"))       c.gateway = server.arg("gw");
    if (server.hasArg("mask"))     c.mask    = server.arg("mask");
    if (server.hasArg("dns"))      c.dns     = server.arg("dns");

    if (c.ssid.isEmpty()) return sendError(400, "SSID is required");
    if (c.ssid.length() > 32) return sendError(400, "SSID is longer than 32 bytes");
    if (!c.password.isEmpty() && (c.password.length() < 8 || c.password.length() > 63)) {
        return sendError(400, "Wi-Fi password must be 8 to 63 characters, or empty for an open network");
    }
    // A wrong static address still joins the network, so neither the setup
    // network nor the Wi-Fi watchdog would ever bring the device back.
    if (c.useStatic) {
        IPAddress ip, gw, mask, dns;
        if (!ip.fromString(c.ip) || !gw.fromString(c.gateway) || !mask.fromString(c.mask)) {
            return sendError(400, "Static IP, gateway and mask must be IPv4 addresses");
        }
        if (!c.dns.isEmpty() && !dns.fromString(c.dns)) return sendError(400, "DNS must be an IPv4 address");
        const uint32_t m = (uint32_t)mask, i = (uint32_t)ip, g = (uint32_t)gw;
        if (m == 0 || (i & m) != (g & m) || i == g) {
            return sendError(400, "The gateway must be on the same subnet as the static IP");
        }
    }
    if (!net::saveConfig(c)) return sendError(500, "Could not save settings");

    sendJson(200, "{\"ok\":true,\"rebooting\":true}");
    wantReboot = true;
}

// --- API: auth -------------------------------------------------------------

void handleAuthStatus() {
    String j = "{";
    j += "\"required\":" + String(auth::isRequired() ? "true" : "false") + ",";
    j += "\"user\":\"" + jsonEscape(auth::currentUser()) + "\"";
    j += "}";
    sendJson(200, j);
}

void handleAuthSet() {
    if (!requireAuth()) return;
    String user = server.arg("user");
    String pass = server.arg("pass");
    if (user.isEmpty() || pass.isEmpty()) return sendError(400, "user and pass required");
    // Basic auth splits user:pass at the first colon, so a username with one
    // could never log in.
    if (user.length() > 32 || user.indexOf(':') >= 0) return sendError(400, "username must be 1 to 32 characters, without ':'");
    for (size_t i = 0; i < user.length(); ++i) {
        if ((uint8_t)user[i] < 0x20) return sendError(400, "username contains a control character");
    }
    if (pass.length() < 4) return sendError(400, "password too short (min 4)");
    if (!auth::setCredentials(user, pass)) return sendError(500, "NVS error");
    log("[auth] credentials changed for '%s'", user.c_str());
    sendOk();
}

// --- API: OTA --------------------------------------------------------------

String otaError;
bool   otaHoldsLock = false;

void handleOtaStatus() {
    if (!requireAuth()) return;
    String ver, notes;
    bool sdAvail = ota::checkSD(&ver, &notes);
    String j = "{";
    j += "\"current\":\"" + jsonEscape(ota::currentVersion()) + "\",";
    j += "\"sdAvailable\":" + String(sdAvail ? "true" : "false") + ",";
    if (sdAvail) {
        j += "\"sdVersion\":\"" + jsonEscape(ver) + "\",";
        j += "\"sdNotes\":\"" + jsonEscape(notes) + "\",";
    }
    j += "\"updating\":" + String(ota::isUpdating() ? "true" : "false");
    j += "}";
    sendJson(200, j);
}

void handleOtaSDTrigger() {
    if (!requireAuth()) return;
    String err;
    if (!ota::checkSD()) return sendError(404, "No SD OTA image");
    if (!ota::triggerSDUpdate(&err)) return sendError(500, err);
    sendJson(200, "{\"ok\":true,\"rebooting\":true}");
    wantReboot = true;
}

void handleOtaUploadDone() {
    if (!requireAuth()) { ota::abortUpdate(); return; }
    // A failed request (a second file part, a write error, an abort) must not
    // leave the update open: every later OTA would fail until a reboot, and a
    // UI update would leave LittleFS unmounted.
    if (!otaError.isEmpty()) { ota::abortUpdate(); String e=otaError; otaError=""; sendError(500, e); return; }
    String err;
    if (!ota::endUpdate(&err)) return sendError(500, err);
    log("[ota] HTTP OTA complete, rebooting");
    sendJson(200, "{\"ok\":true,\"rebooting\":true}");
    wantReboot = true;
}

void handleOtaUploadData() {
    // WebServer sends a POST that is not multipart down its raw-body path,
    // which calls this same callback while server.upload() refers to a null
    // pointer. Any client posting the wrong type crashed the board.
    if (!isMultipart()) return;
    HTTPUpload& up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        otaError = "";
        // Check credentials here, not only in handleOtaUploadDone: the data
        // callbacks run first, so an unauthenticated POST would otherwise write
        // a whole image into the OTA partition before being turned away.
        if (auth::isRequired() && !auth::checkBasicAuth(server.header("Authorization"))) {
            otaError = "Authentication required";
            return;
        }
        String err;
        size_t total = server.header("Content-Length").toInt();
        // WebServer doesn't give total easily; use UPDATE_SIZE_UNKNOWN if 0
        if (total == 0) total = UPDATE_SIZE_UNKNOWN;
        // ?target=ui writes the web UI image (.pio/littlefs.bin) instead.
        const bool ui = server.arg("target") == "ui";
        if (!ota::beginUpdate(total, &err, ui)) { otaError = err; return; }
        log("[ota] HTTP OTA started (%s)", ui ? "web UI image" : "firmware");
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (!otaError.isEmpty()) return;
        String err;
        if (!ota::writeUpdate(up.buf, up.currentSize, &err)) { otaError = err; ota::abortUpdate(); }
    } else if (up.status == UPLOAD_FILE_END) {
        // handled in done
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        ota::abortUpdate();
        otaError = "Upload aborted";
    }
}

}  // namespace

// Removes "*.partial" files an upload left behind when the board lost power
// or reset mid-upload. Runs once at boot, before the web server starts, so no
// upload can be in flight. Bounded depth; the lock is taken per entry.
void sweepPartials(const String& path, int depth) {
    if (depth > 4 || !storage::cardMounted()) return;
    File dir;
    {
        storage::Guard g(false, 5000);
        if (!g.ok()) return;
        dir = SD_FS.open(path);
        if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
    }
    std::vector<String> partials, subdirs;
    for (;;) {
        storage::Guard g(false, 5000);
        if (!g.ok()) break;
        File f = dir.openNextFile();
        if (!f) break;
        String name = f.name();
        const int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        const bool isDir = f.isDirectory();
        f.close();
        if (isDir && !name.startsWith(".")) subdirs.push_back(joinPath(path, name));
        else if (!isDir && name.endsWith(".partial")) partials.push_back(joinPath(path, name));
        if (partials.size() + subdirs.size() > 256) break;
    }
    { storage::Guard g(false, 5000); dir.close(); }
    if (!partials.empty()) {
        storage::Guard g(true, 5000);
        if (g.ok()) {
            for (const String& p : partials) {
                if (SD_FS.remove(p)) log("[web] removed leftover %s", p.c_str());
            }
        }
    }
    for (const String& d : subdirs) sweepPartials(d, depth + 1);
}

bool begin() {
    auth::begin();
    ota::begin();
    ws::begin();
    sweepPartials("/", 0);
    if (!LittleFS.begin(false)) {
        log("[web] LittleFS mount failed — run 'pio run -t uploadfs'");
        // Still serve the API so the device is configurable without a UI.
    }

    const char* hdrKeys[] = {"Authorization", "Origin", "Host", "Content-Type"};
    server.collectHeaders(hdrKeys, 4);

    server.on("/api/status",    HTTP_GET,  handleStatus);
    // The last panic's core dump summary, from the coredump partition. Decode
    // the addresses with xtensa-esp32s3-elf-addr2line against the build whose
    // ELF SHA-256 matches.
    server.on("/api/crash",     HTTP_GET,  []() {
        if (!requireAuth()) return;
        esp_core_dump_summary_t* sum = (esp_core_dump_summary_t*)calloc(1, sizeof(esp_core_dump_summary_t));
        if (!sum) return sendError(500, "Out of memory");
        String out;
        if (esp_core_dump_image_check() != ESP_OK || esp_core_dump_get_summary(sum) != ESP_OK) {
            out = "no core dump stored\n";
        } else {
            char line[96];
            snprintf(line, sizeof(line), "task %s\npc 0x%08x\ncause %u vaddr 0x%08x\n",
                     sum->exc_task, (unsigned)sum->exc_pc, (unsigned)sum->ex_info.exc_cause,
                     (unsigned)sum->ex_info.exc_vaddr);
            out += line;
            out += "elf ";
            out += (const char*)sum->app_elf_sha256;
            out += "\nbacktrace";
            for (uint32_t i = 0; i < sum->exc_bt_info.depth && i < 16; ++i) {
                snprintf(line, sizeof(line), " 0x%08x", (unsigned)sum->exc_bt_info.bt[i]);
                out += line;
            }
            out += sum->exc_bt_info.corrupted ? " (corrupted)\n" : "\n";
        }
        free(sum);
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "text/plain; charset=utf-8", out);
    });
    server.on("/api/log",       HTTP_GET,  []() {
        if (!requireAuth()) return;
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "text/plain; charset=utf-8", logbuf::dump());
    });
    server.on("/api/list",      HTTP_GET,  handleList);
    server.on("/api/download",  HTTP_GET,  handleDownload);
    server.on("/api/delete",    HTTP_POST, handleDelete);
    server.on("/api/mkdir",     HTTP_POST, handleMkdir);
    server.on("/api/rename",    HTTP_POST, handleRename);
    server.on("/api/eject",     HTTP_POST, handleEject);
    server.on("/api/batch/begin", HTTP_POST, handleBatchBegin);
    server.on("/api/batch/end",   HTTP_POST, handleBatchEnd);
    server.on("/api/usb/reattach", HTTP_POST, handleReattach);
    // Restarting the device is what reliably starts the NIX slideshow: the
    // frame sees the drive plugged in again. The page's Restart slideshow
    // button posts here.
    server.on("/api/restart", HTTP_POST, []() {
        if (!requireAuth()) return;
        log("[web] restart requested");
        sendJson(200, "{\"ok\":true,\"rebooting\":true}");
        wantReboot = true;
    });
    server.on("/api/wifi/scan", HTTP_GET,  handleWifiScan);
    server.on("/api/wifi",      HTTP_POST, handleWifiSave);
    server.on("/api/upload",    HTTP_POST, handleUploadDone, handleUploadData);
    server.on("/api/auth/status", HTTP_GET,  handleAuthStatus);
    server.on("/api/auth/set",    HTTP_POST, handleAuthSet);
    server.on("/api/ota/status",  HTTP_GET,  handleOtaStatus);
    server.on("/api/ota/sd",      HTTP_POST, handleOtaSDTrigger);
    server.on("/api/ota",         HTTP_POST, handleOtaUploadDone, handleOtaUploadData);

    server.onNotFound(handleNotFound);
    server.begin();
    log("[web] listening on port 80, ws on 81, auth %s", auth::isRequired() ? "on" : "off");
    return true;
}

void loop() {
    server.handleClient();
    ws::loop();
    ota::loop();
}
bool rebootRequested() { return wantReboot; }

}  // namespace web
