// The Room Browsers' data (Yuzu: the Eden Web API; Citra: the Citra Web APIs): a Web API's public room list. Eden and Suyu (both Yuzu-based) rooms are announced
// there; GET <WebApiUrl>/lobby answers {"rooms":[{name, address, port, hasPassword, maxPlayers, owner,
// preferredGameName, players:[...]}]}. http:// goes over a plain socket (what Eden's own client does for a bare
// host); https:// needs TLS, which only Android has (GameActivity.webGet).
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <jni.h>
#endif

#include "net.h"

#define API_VERSION "1"
#define BODY_MAX (4 << 20)

static struct {
    pthread_mutex_t mu;
    int state;
    char err[96];
    WebRoom rooms[WEBROOMS_MAX];
    int n;
} Wk[WEBROOMS_KINDS] = {{.mu = PTHREAD_MUTEX_INITIALIZER}, {.mu = PTHREAD_MUTEX_INITIALIZER}};

// ---- a small JSON reader: walks the document once, handing every key of the objects in "rooms" to a callback
typedef struct { const char *p, *end; bool bad; } Jr;
static void ws(Jr *j) { while (j->p < j->end && (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t')) j->p++; }
// a string into out (cut at size - 1); the cursor after its closing quote
static bool jstr(Jr *j, char *out, size_t size) {
    size_t n = 0;
    if (j->p >= j->end || *j->p != '"') return j->bad = true, false;
    j->p++;
    while (j->p < j->end && *j->p != '"') {
        unsigned c = (unsigned char)*j->p++;
        if (c == '\\' && j->p < j->end) {
            c = (unsigned char)*j->p++;
            if (c == 'n' || c == 't' || c == 'r') c = ' ';
            else if (c == 'u') {  // (\uXXXX: ASCII as it is, anything else a '?')
                unsigned v = 0;
                for (int k = 0; k < 4 && j->p < j->end; k++, j->p++) v = v * 16 + (unsigned)((*j->p & 15) + (*j->p > '9' ? 9 : 0));
                c = v < 0x80 && v >= 0x20 ? v : '?';
            }
        }
        if (out && size && n + 1 < size) out[n++] = (char)c;
    }
    if (j->p >= j->end) return j->bad = true, false;
    j->p++;
    if (out && size) out[n] = 0;
    return true;
}
static void jskip(Jr *j);
static void jskip_container(Jr *j, char open, char close) {
    (void)open;
    j->p++;
    ws(j);
    if (j->p < j->end && *j->p == close) { j->p++; return; }
    while (j->p < j->end && !j->bad) {
        ws(j);
        if (close == '}') { if (!jstr(j, NULL, 0)) return; ws(j); if (j->p >= j->end || *j->p != ':') { j->bad = true; return; } j->p++; }
        jskip(j);
        ws(j);
        if (j->p < j->end && *j->p == ',') { j->p++; continue; }
        if (j->p < j->end && *j->p == close) { j->p++; return; }
        j->bad = true;
    }
    j->bad = true;
}
static void jskip(Jr *j) {
    ws(j);
    if (j->p >= j->end) { j->bad = true; return; }
    if (*j->p == '"') jstr(j, NULL, 0);
    else if (*j->p == '{') jskip_container(j, '{', '}');
    else if (*j->p == '[') jskip_container(j, '[', ']');
    else while (j->p < j->end && *j->p != ',' && *j->p != '}' && *j->p != ']') j->p++;
}
static int jcount_array(Jr j) {  // the elements of the array at the cursor
    int n = 0;
    j.p++;
    ws(&j);
    if (j.p < j.end && *j.p == ']') return 0;
    while (j.p < j.end && !j.bad) {
        jskip(&j);
        n++;
        ws(&j);
        if (j.p < j.end && *j.p == ',') { j.p++; continue; }
        break;
    }
    return j.bad ? 0 : n;
}

static void parse_room(Jr *j, WebRoom *r) {
    memset(r, 0, sizeof *r);
    j->p++;  // {
    ws(j);
    if (j->p < j->end && *j->p == '}') { j->p++; return; }
    while (j->p < j->end && !j->bad) {
        char key[40];
        ws(j);
        if (!jstr(j, key, sizeof key)) return;
        ws(j);
        if (j->p >= j->end || *j->p != ':') { j->bad = true; return; }
        j->p++;
        ws(j);
        if (!strcmp(key, "name")) jstr(j, r->name, sizeof r->name);
        else if (!strcmp(key, "owner")) jstr(j, r->owner, sizeof r->owner);
        else if (!strcmp(key, "preferredGameName")) jstr(j, r->game, sizeof r->game);
        else if (!strcmp(key, "address")) jstr(j, r->address, sizeof r->address);
        else if (!strcmp(key, "port")) r->port = (uint16_t)atoi(j->p), jskip(j);
        else if (!strcmp(key, "maxPlayers")) r->max_players = atoi(j->p), jskip(j);
        else if (!strcmp(key, "netVersion")) r->net_version = (uint32_t)atoi(j->p), jskip(j);
        else if (!strcmp(key, "hasPassword")) r->locked = !strncmp(j->p, "true", 4), jskip(j);
        else if (!strcmp(key, "players") && j->p < j->end && *j->p == '[') r->players = jcount_array(*j), jskip(j);
        else jskip(j);
        ws(j);
        if (j->p < j->end && *j->p == ',') { j->p++; continue; }
        if (j->p < j->end && *j->p == '}') { j->p++; return; }
        j->bad = true;
    }
}
// the "rooms" array of the reply; the number read, -1: not what the API answers
static int parse_rooms(const char *body, size_t len, WebRoom *out, int max) {
    Jr j = {body, body + len, false};
    ws(&j);
    if (j.p >= j.end || *j.p != '{') return -1;
    j.p++;
    while (j.p < j.end && !j.bad) {
        char key[40];
        ws(&j);
        if (!jstr(&j, key, sizeof key)) return -1;
        ws(&j);
        if (j.p >= j.end || *j.p != ':') return -1;
        j.p++;
        ws(&j);
        if (!strcmp(key, "rooms") && j.p < j.end && *j.p == '[') {
            int n = 0;
            j.p++;
            ws(&j);
            if (j.p < j.end && *j.p == ']') return 0;
            while (j.p < j.end && !j.bad) {
                ws(&j);
                if (j.p >= j.end || *j.p != '{') return -1;
                WebRoom r;
                parse_room(&j, &r);
                if (j.bad) return -1;
                if (r.address[0] && r.port && n < max) out[n++] = r;
                ws(&j);
                if (j.p < j.end && *j.p == ',') { j.p++; continue; }
                break;
            }
            return j.bad ? -1 : n;
        }
        jskip(&j);
        ws(&j);
        if (j.p < j.end && *j.p == ',') j.p++;
    }
    return -1;
}

// ---- GET
// "[http[s]://]host[:port][/base]" -> the pieces; port 0: the scheme's
static bool split_url(const char *url, bool *tls, char *host, size_t hs, int *port, char *base, size_t bs) {
    *tls = false;
    if (!strncasecmp(url, "https://", 8)) *tls = true, url += 8;
    else if (!strncasecmp(url, "http://", 7)) url += 7;
    size_t hl = strcspn(url, "/:");
    if (!hl || hl >= hs) return false;
    memcpy(host, url, hl), host[hl] = 0;
    url += hl;
    *port = 0;
    if (*url == ':') *port = atoi(url + 1), url += strcspn(url, "/");
    snprintf(base, bs, "%s", url);
    size_t n = strlen(base);
    while (n && base[n - 1] == '/') base[--n] = 0;
    return !(*port < 0 || *port > 65535);
}

static char *http_get(const char *host, int port, const char *path, size_t *len, char *err, size_t es) {
    struct addrinfo hints = {0}, *res = NULL;
    char ps[8];
    hints.ai_family = AF_INET, hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof ps, "%d", port ? port : 80);
    if (getaddrinfo(host, ps, &hints, &res) || !res) return snprintf(err, es, "can't find %s", host), NULL;
    int s = (int)socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) { freeaddrinfo(res); return snprintf(err, es, "no socket"), NULL; }
    struct timeval tv = {8, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof tv);
    if (connect(s, res->ai_addr, (int)res->ai_addrlen)) {
        freeaddrinfo(res);
        close(s);
        return snprintf(err, es, "can't reach %s", host), NULL;
    }
    freeaddrinfo(res);
    char req[768];
    int rn = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\napi-version: " API_VERSION "\r\nAccept: application/json\r\nUser-Agent: mhgu\r\nConnection: close\r\n\r\n", path, host);
    if (send(s, req, (size_t)rn, 0) != rn) { close(s); return snprintf(err, es, "send failed"), NULL; }
    size_t cap = 65536, n = 0;
    char *buf = malloc(cap + 1);
    for (;;) {
        if (n == cap) {
            if (cap >= BODY_MAX) break;
            cap *= 2;
            buf = realloc(buf, cap + 1);
        }
        ssize_t k = recv(s, buf + n, cap - n, 0);
        if (k <= 0) break;
        n += (size_t)k;
    }
    close(s);
    buf[n] = 0;
    char *hdr_end = strstr(buf, "\r\n\r\n");
    if (n < 12 || !hdr_end) { free(buf); return snprintf(err, es, "no answer"), NULL; }
    int status = atoi(buf + 9);
    if (status >= 400 || status < 200) { free(buf); return snprintf(err, es, "the server answered %d", status), NULL; }
    *hdr_end = 0;
    bool chunked = strcasestr(buf, "transfer-encoding: chunked") != NULL;
    char *body = hdr_end + 4;
    size_t bl = n - (size_t)(body - buf);
    if (chunked) {  // decode in place
        char *in = body, *out = body, *end = body + bl;
        while (in < end) {
            char *e;
            unsigned long c = strtoul(in, &e, 16);
            if (e == in) break;
            in = strstr(e, "\r\n");
            if (!in) break;
            in += 2;
            if (!c || in + c > end) { if (c && in < end) c = (unsigned long)(end - in); else break; }
            memmove(out, in, c), out += c, in += c + 2;
        }
        bl = (size_t)(out - body);
    }
    memmove(buf, body, bl);
    buf[bl] = 0;
    *len = bl;
    return buf;
}

#ifdef __ANDROID__
static char *https_get(const char *url, size_t *len, char *err, size_t es) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    if (!act) return snprintf(err, es, "no activity"), NULL;
    char *out = NULL;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = cls ? (*env)->GetStaticMethodID(env, cls, "webGet", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;") : NULL;
    if (m) {
        jstring ju = (*env)->NewStringUTF(env, url), jv = (*env)->NewStringUTF(env, API_VERSION);
        jstring r = (jstring)(*env)->CallStaticObjectMethod(env, cls, m, ju, jv);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        if (r) {
            const char *c = (*env)->GetStringUTFChars(env, r, NULL);
            if (c && *c) out = strdup(c), *len = strlen(c);
            if (c) (*env)->ReleaseStringUTFChars(env, r, c);
            (*env)->DeleteLocalRef(env, r);
        }
        (*env)->DeleteLocalRef(env, ju);
        (*env)->DeleteLocalRef(env, jv);
    }
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
    if (!out) snprintf(err, es, "the https request failed");
    return out;
}
#endif

// one Web API's /lobby: the rooms read (>= 0) into out, or -1 with err
static int fetch_one(const char *url, WebRoom *out, int max, char *err, size_t es) {
    char host[128], base[128], path[160], *body = NULL;
    size_t len = 0;
    bool tls;
    int port;
    if (!split_url(url, &tls, host, sizeof host, &port, base, sizeof base)) {
        snprintf(err, es, "the Web API URL is not valid");
        return -1;
    }
    if (tls) {
#ifdef __ANDROID__
        char full[400];
        if (port) snprintf(full, sizeof full, "https://%s:%d%s/lobby", host, port, base);
        else snprintf(full, sizeof full, "https://%s%s/lobby", host, base);
        body = https_get(full, &len, err, es);
#else
        snprintf(err, es, "https needs Android here: use an http:// URL");
#endif
    } else {
        snprintf(path, sizeof path, "%s/lobby", base);
        body = http_get(host, port, path, &len, err, es);
    }
    if (!body) return -1;
    int n = parse_rooms(body, len, out, max);
    free(body);
    if (n < 0) snprintf(err, es, "the answer is not a room list");
    return n;
}

typedef struct { int kind; char url[256], url2[256]; } Fetch;

static void *fetch_thread(void *arg) {
    Fetch *f = arg;
    char err[96] = "", err2[96] = "";
    static WebRoom tmp[WEBROOMS_KINDS][WEBROOMS_MAX], part[WEBROOMS_KINDS][WEBROOMS_MAX];
    WebRoom *t = tmp[f->kind], *p = part[f->kind];
    int n = -1, got = 0;
    if (f->url[0]) {
        int k = fetch_one(f->url, t, WEBROOMS_MAX, err, sizeof err);
        if (k >= 0) n = k;
    } else snprintf(err, sizeof err, "no Web API URL is set");
    if (f->url2[0]) {  // a second API: its rooms are added (the ones the first already lists are skipped)
        int k = fetch_one(f->url2, p, WEBROOMS_MAX, err2, sizeof err2);
        if (k >= 0) {
            if (n < 0) n = 0;
            for (int i = 0; i < k && n < WEBROOMS_MAX; i++) {
                bool dup = false;
                for (int j = 0; j < n && !dup; j++) dup = t[j].port == p[i].port && !strcmp(t[j].address, p[i].address);
                if (!dup) t[n++] = p[i];
            }
        } else if (n < 0) snprintf(err, sizeof err, "%s", err2);
    }
    (void)got;
    pthread_mutex_lock(&Wk[f->kind].mu);
    if (n >= 0) memcpy(Wk[f->kind].rooms, t, sizeof(WebRoom) * (size_t)n), Wk[f->kind].n = n, Wk[f->kind].state = WEBROOMS_OK, Wk[f->kind].err[0] = 0;
    else Wk[f->kind].state = WEBROOMS_ERROR, snprintf(Wk[f->kind].err, sizeof Wk[f->kind].err, "%s", err);
    pthread_mutex_unlock(&Wk[f->kind].mu);
    free(f);
    return NULL;
}

void webrooms_refresh_kind(int kind, const char *url, const char *url2) {
    if (kind < 0 || kind >= WEBROOMS_KINDS) return;
    pthread_mutex_lock(&Wk[kind].mu);
    if (Wk[kind].state == WEBROOMS_LOADING) { pthread_mutex_unlock(&Wk[kind].mu); return; }
    Wk[kind].state = WEBROOMS_LOADING;
    pthread_mutex_unlock(&Wk[kind].mu);
    Fetch *f = calloc(1, sizeof *f);
    f->kind = kind;
    snprintf(f->url, sizeof f->url, "%s", url ? url : "");
    snprintf(f->url2, sizeof f->url2, "%s", url2 ? url2 : "");
    pthread_t t;
    if (pthread_create(&t, NULL, fetch_thread, f)) {
        free(f);
        pthread_mutex_lock(&Wk[kind].mu);
        Wk[kind].state = WEBROOMS_ERROR, snprintf(Wk[kind].err, sizeof Wk[kind].err, "can't start the request");
        pthread_mutex_unlock(&Wk[kind].mu);
        return;
    }
    pthread_detach(t);
}
int webrooms_state_kind(int kind, char *err, size_t err_size) {
    if (kind < 0 || kind >= WEBROOMS_KINDS) return WEBROOMS_IDLE;
    pthread_mutex_lock(&Wk[kind].mu);
    int s = Wk[kind].state;
    if (err && err_size) snprintf(err, err_size, "%s", Wk[kind].err);
    pthread_mutex_unlock(&Wk[kind].mu);
    return s;
}
int webrooms_get_kind(int kind, WebRoom *out, int max) {
    if (kind < 0 || kind >= WEBROOMS_KINDS) return 0;
    pthread_mutex_lock(&Wk[kind].mu);
    int n = Wk[kind].n < max ? Wk[kind].n : max;
    memcpy(out, Wk[kind].rooms, sizeof(WebRoom) * (size_t)n);
    pthread_mutex_unlock(&Wk[kind].mu);
    return n;
}

// the Yuzu (Eden) list: one URL
void webrooms_refresh(const char *url) { webrooms_refresh_kind(WEBROOMS_YUZU, url, NULL); }
int webrooms_state(char *err, size_t err_size) { return webrooms_state_kind(WEBROOMS_YUZU, err, err_size); }
int webrooms_get(WebRoom *out, int max) { return webrooms_get_kind(WEBROOMS_YUZU, out, max); }
