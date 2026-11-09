// nn::ldn (local wireless play) over IP. The game that creates a network (the hub's host, an "access point") listens
// on a TCP port (ini [Multiplayer] Port); the others ("stations") find it with Scan - a LAN broadcast plus the
// addresses in ini [Multiplayer] Join - and Connect to it. Each station keeps that one connection: the host hands
// out node numbers and addresses (169.254.x.n, the host n = 1), sends the network's NetworkInfo whenever it changes
// (players joining or leaving, the hub's advertise data), and relays the game's packets between the players
// (net.h, vsock.c). Only the host needs a reachable port.
//
// Internet hubs (ini [Multiplayer] Server, server/ in the repository): with a hub server, a created hub is also listed
// there, for players with the same matchmaking key (ini Key, empty: public) and port version, and Scan adds the
// server's hubs to the LAN's. Joining one goes through the server, which splices the player's connection to one the
// host opens for it: the same link as a direct connection, so nobody forwards a port. Game packets go by UDP through
// the server too ("MHGR" relay prefix), else through the link - or, when both players allow it (ini Direct), straight
// between them once hole punching gets through (see "Direct UDP" below), with the relay as the fallback.
//
// Link frames: u32 size (of what follows), u8 type, payload. Structures are the SDK's (sizes and offsets below).
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <string.h>

#include "../../updater/updater.h"  // (sha256)
#include "../config.h"
#include "../hle.h"
#include "../kernel.h"
#include "../overlay.h"
#include "../settings_locale.h"
#include "citra_room.h"
#include "net.h"
// Each caller thread owns one snapshot of the saved crossplay settings.
static __thread Config xp_config;
static void crossplay_snapshot(void) { config_live_crossplay(&xp_config); }

// ---- crossplay: the Rust modules (crates/mhgu-crossplay), their C API
typedef struct { uint32_t src_ip, dst_ip; uint16_t src_port, dst_port; uint32_t proto; } RyuProxy;
extern bool mhgu_ryu_enabled(void);
extern void mhgu_ryu_set_config(const char *servers, const char *host, const char *pass);
extern void mhgu_ryu_set_self(const char *addr);
extern void mhgu_ryu_set_hooks(void (*lobby)(const uint8_t *), void (*data)(const RyuProxy *, const uint8_t *, uint32_t));
extern int mhgu_ryu_server_count(void);
extern int mhgu_ryu_scan(const uint8_t *filter, uint8_t *infos, int *server_of, int max);
extern uint32_t mhgu_ryu_create(int server, const uint8_t *net, const uint8_t *sec, const uint8_t *user,
                                const uint8_t *adv, uint32_t adv_size, uint8_t *info, uint32_t *ip, uint32_t *mask);
extern uint32_t mhgu_ryu_connect(int server, const uint8_t *info, const uint8_t *sec, const uint8_t *user, uint16_t lcv,
                                 uint8_t *out, uint32_t *ip, uint32_t *mask);
extern void mhgu_ryu_leave(void);
extern bool mhgu_ryu_in_lobby(void);
extern void mhgu_ryu_set_advertise(const uint8_t *data, uint32_t len);
extern void mhgu_ryu_set_accept_policy(uint8_t policy);
extern bool mhgu_ryu_reject(uint32_t node_id, uint32_t reason);
extern void mhgu_ryu_send(const RyuProxy *p, const uint8_t *data, uint32_t len);
extern bool mhgu_ryusrv_start(const char *addr, uint16_t port, char *err, int err_size);
extern void mhgu_ryusrv_stop(void);
extern bool mhgu_ryusrv_running(void);

typedef void (*RoomLdnCb)(uint8_t, const uint8_t *, const uint8_t *, bool, const uint8_t *, uint32_t);
typedef void (*RoomProxyCb)(const uint8_t *, uint16_t, const uint8_t *, uint16_t, uint8_t, bool, const uint8_t *, uint32_t);
extern void mhgu_room_set_hooks(RoomLdnCb ldn, RoomProxyCb proxy, void (*lost)(void), void (*members)(const uint8_t (*)[4], int));
extern bool mhgu_room_join(const char *server, const char *nick, const char *password, uint8_t *ip, char *err, int err_size);
extern void mhgu_room_leave(void);
extern bool mhgu_room_connected(void);
extern void mhgu_room_own_ip(uint8_t *ip);
extern void mhgu_room_send_ldn(uint8_t type, const uint8_t *lip, const uint8_t *rip, bool bc, const uint8_t *data, uint32_t len);
extern void mhgu_room_send_proxy(const uint8_t *sip, uint16_t sport, const uint8_t *dip, uint16_t dport, uint8_t proto, bool bc,
                                 const uint8_t *data, uint32_t len);
extern bool mhgu_roomsrv_start(int port, const char *name, const char *password, const char *host_nick, char *err, int err_size);
extern void mhgu_roomsrv_stop(void);
extern bool mhgu_roomsrv_running(void);

// "All Rooms": a hosted hub let into the Yuzu room too (crates/mhgu-crossplay/src/yuzu_door.rs)
extern void mhgu_ydoor_set_hooks(void (*info)(const uint8_t *), void (*packet)(uint32_t, uint32_t, uint16_t, uint16_t, const uint8_t *, uint32_t));
extern bool mhgu_ydoor_host(const uint8_t *hub);
extern void mhgu_ydoor_update(const uint8_t *hub);
extern void mhgu_ydoor_stop(void);
extern void mhgu_ydoor_lost(void);
extern void mhgu_ydoor_ldn(uint8_t type, const uint8_t *lip, const uint8_t *data, uint32_t len);
extern void mhgu_ydoor_proxy(const uint8_t *sip, uint16_t sport, const uint8_t *dip, uint16_t dport, uint8_t proto, bool bc,
                             const uint8_t *data, uint32_t len);
extern void mhgu_ydoor_members(const uint8_t (*ips)[4], int n);
extern void mhgu_ydoor_reject(uint32_t ip);
extern void mhgu_ydoor_send(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport, const uint8_t *data, uint32_t len);

extern void mhgu_xb_begin(const uint8_t *info, uint32_t host_ip, uint32_t mask);
extern void mhgu_xb_end(void);
extern bool mhgu_xb_active(void);
extern int mhgu_xb_players(void);
extern void mhgu_xb_lobby(const uint8_t *info);
extern void mhgu_xb_room_ldn(uint8_t type, const uint8_t *lip, const uint8_t *rip, bool bc, const uint8_t *data, uint32_t len);
extern void mhgu_xb_room_members(const uint8_t (*ips)[4], int n);
extern void mhgu_xb_room_proxy(const uint8_t *sip, uint16_t sport, const uint8_t *dip, uint16_t dport, uint8_t proto, bool bc,
                               const uint8_t *data, uint32_t len);


#ifdef _WIN32
typedef SOCKET hsock;
#define BADSOCK INVALID_SOCKET
#define hclose closesocket
static void hnet_init(void) {
    static int done;
    if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = 1; }
}
static void hset_nonblock(hsock s, bool on) { u_long v = on; ioctlsocket(s, FIONBIO, &v); }
static void hset_timeout(hsock s, int ms) {
    DWORD v = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&v, sizeof v);
}
#else
typedef int hsock;
#define BADSOCK (-1)
#define hclose close
static void hnet_init(void) {}
static void hset_nonblock(hsock s, bool on) {
    int f = fcntl(s, F_GETFL);
    fcntl(s, F_SETFL, on ? f | O_NONBLOCK : f & ~O_NONBLOCK);
}
static void hset_timeout(hsock s, int ms) {
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}
#endif

// ---- SDK structures (bytes)
#define NETINFO_SIZE 0x480
enum {
    NI_LOCAL_COMM_ID = 0x00, NI_SCENE_ID = 0x0A, NI_SESSION_ID = 0x10, NI_BSSID = 0x20, NI_SSID = 0x26,
    NI_CHANNEL = 0x48, NI_LINK_LEVEL = 0x4A, NI_NETWORK_TYPE = 0x4B, NI_SEC_PARAM = 0x50, NI_SEC_MODE = 0x60,
    NI_ACCEPT_POLICY = 0x62, NI_NODE_MAX = 0x66, NI_NODE_COUNT = 0x67, NI_NODES = 0x68, NI_ADV_SIZE = 0x26A,
    NI_ADV = 0x26C, NI_AUTH_ID = 0x478,
};
#define NODE_SIZE 0x40
enum { NODE_IP = 0x00, NODE_MAC = 0x04, NODE_ID = 0x0A, NODE_CONNECTED = 0x0B, NODE_NAME = 0x0C, NODE_LCV = 0x2E };
#define MAX_NODES 8
#define ADV_MAX 0x180
// Yuzu-family room LDN packets (crates/mhgu-crossplay/src/room.rs)
enum { RLDN_SCAN, RLDN_SCAN_RESP, RLDN_CONNECT, RLDN_SYNC_NETWORK, RLDN_DISCONNECT, RLDN_DESTROY_NETWORK };
// NetworkConfig (0x20): IntentId 0x10, u16 channel, u8 node count max, u8 -, u16 local communication version
// SecurityConfig (0x44): u16 mode, u16 passphrase size, passphrase[0x40]; UserConfig (0x30): user name[0x21]
// SecurityParameter (0x20): data[0x10], session id[0x10]
// ScanFilter (0x60): NetworkId 0x20, u32 network type, bssid[6], ssid 0x22, -[0x10], u32 flags
enum { SF_LOCAL_COMM_ID = 1, SF_SESSION_ID = 2, SF_NETWORK_TYPE = 4, SF_BSSID = 8, SF_SSID = 0x10, SF_SCENE_ID = 0x20 };

enum { ST_NONE, ST_INITIALIZED, ST_ACCESS_POINT, ST_AP_CREATED, ST_STATION, ST_STATION_CONNECTED, ST_ERROR };
enum { DR_NONE, DR_BY_USER, DR_BY_SYSTEM, DR_DESTROYED_BY_USER, DR_DESTROYED_BY_SYSTEM, DR_REJECTED, DR_SIGNAL_LOST };
#define LDN_RESULT(d) MAKE_RESULT(203, d)
#define R_INVALID_STATE LDN_RESULT(32)
#define R_CONNECT_FAILURE LDN_RESULT(64)
#define R_CONNECT_NOT_FOUND LDN_RESULT(65)
#define R_CONNECT_TIMEOUT LDN_RESULT(66)
#define R_CONNECT_REJECTED LDN_RESULT(67)
#define R_VERSION_TOO_LOW LDN_RESULT(113)
#define R_VERSION_TOO_HIGH LDN_RESULT(114)
#define R_TOO_MANY_PLAYERS LDN_RESULT(144)

enum { DOOR_NONE, DOOR_CITRA, DOOR_YUZU };
enum { ORIGIN_HUB, ORIGIN_CITRA, ORIGIN_YUZU };  // where a packet into the hosted hub came from

// ---- link protocol
#define PROTO_MAGIC "MHGL"
#define PROTO_VERSION 2  // the one used when nothing says otherwise (gu-native's own)
// Link versions spoken: 2 (this port, gu-native) and 3 (the MHGURecomp PC build). The frames are laid out the same in both, but
// a host only accepts a hello of its own version and a hub server only lists hubs of the asker's version. So we speak both: a
// hub we host is listed under both versions, a search asks each version, and a found hub remembers the one it speaks.
static const uint16_t LINK_VERSIONS[] = {2, 3};
#define N_LINK_VERSIONS 2
static bool link_version_ok(uint16_t v) { return v == LINK_VERSIONS[0] || v == LINK_VERSIONS[1]; }
// The room of Android players without a key on the MHGURecomp servers (-separate-phones, on by default): searches add it.
#define PHONE_PUBLIC_KEY "public-phone"
enum { F_HELLO = 1, F_INFO, F_WELCOME, F_REFUSE, F_PACKET, F_DESTROY, F_AUX };  // F_AUX: the port's own messages (emsync.c, hle/hpscale.c)
typedef struct {
    char magic[4];
    uint16_t version;
    uint8_t scan_only, pad;
    uint16_t lcv;
    uint8_t mac[6];
    char name[0x21];
    uint8_t pad2[3];
} Hello;
#define MAX_FRAME (0x10000 + 64)

// The game's packets go by UDP when it gets through (Pia, the game's network library, expects datagrams: over TCP
// one lost packet would hold up everything behind it): datagrams "MHGD" + token + kind [+ VPacket + data] between
// a station and the host's port. A station says hello every 2 s (keeping NAT mappings open), the host answers;
// without UDP (blocked, not forwarded) or after 15 s without any, the packets go through the TCP link instead.
// A hello carries the station's clock (u64 ns) and the answer echoes it: the round trip (MHGU_LDN_RTT=1 logs it).
#define UDP_MAGIC "MHGD"
enum { U_DATA = 0, U_HELLO, U_HELLO_ACK, U_PUNCH };
typedef struct { char magic[4]; uint32_t token; uint8_t kind, flags, pad[2]; } UHdr;
#define UF_DIRECT_OK 1  // (a direct hello) the station gets the host's direct datagrams
#define UDP_MAX_PAYLOAD 8192
#define UDP_SILENCE_NS 15000000000ull

// Direct UDP in an internet hub (hole punching; ini Direct on both ends): over the relay socket each end asks the hub
// server for the other's public UDP address ("MHGQ" query); the server answers ("MHGP") once both have asked, so a
// player with Direct = Off never has its address shown, and an older game (which never asks) stays on the relay. The
// answer also carries the pair's direct key: direct datagrams start "MHGR", pair id, direct key (as relayed ones do
// with the sender's secret), and anything else arriving straight from the internet is dropped. Then the host sends
// punches and the station hellos straight to the other's address, each opening its own router's way for the other's
// datagrams; once the station's hellos are answered both ways, game packets go directly. The relay stays up beside
// it (its hellos go on, and the direct hellos every second keep the routers' mappings open): after DIRECT_SILENCE_NS
// without proof that the direct path works both ways, the packets go through the server again (symmetric /
// carrier-grade NAT never get a direct path at all), and the ends ask the server again at once (a new address after a
// network change). Coming back to direct after such a fall takes DIRECT_REUP answers in a row (no flapping).
#define PEER_QUERY "MHGQ"
#define PEER_ANSWER "MHGP"
#define PEER_MSG_SIZE (RELAY_PREFIX + 16)  // the query: prefix, "MHGQ", 0[12]; the answer: "MHGP", pair, secret,
                                           // ip[4], port (BE), 0[2], direct key (u64): never bigger than the query
#define DIRECT_SILENCE_NS 3500000000ull  // (direct hellos every second: three lost in a row)
#define DIRECT_HELLO_NS 1000000000ull
#define DIRECT_REUP 3
#define PUNCH_FAST_NS 15000000000ull  // punches / hellos every 200 ms this long after the address came, then every 2 s
#define QUERY_FAST_NS 30000000000ull  // queries every second this long after the link came up or direct fell, then every 15 s

// The hub server (server/server.go has the protocol): control frames like the link's; requests start with a header
// "MHGS", u16 LOBBY_PROTO, u16 link version (2 or 3), room[16] (the matchmaking key's hash).
#define LOBBY_PROTO 1
#define LOBBY_PORT 39510
#define LOBBY_HEADER 24
enum { S_LIST = 1, S_HOST, S_ATTACH, S_JOIN, S_UPDATE, S_PING, S_HUBS = 16, S_HOSTED, S_INCOMING, S_SPLICED, S_ERROR };
enum { SE_NOT_FOUND = 1, SE_NO_ANSWER, SE_BUSY, SE_VERSION };
#define LOBBY_PING_NS 15000000000ull
#define RELAY_MAGIC "MHGR"
#define RELAY_PREFIX 16  // magic, u32 pair, u64 secret

typedef struct {
    hsock s;
    pthread_mutex_t wmu;  // one writer at a time; s is closed only with it held (alive cleared first)
    pthread_t th;
    bool alive, has_thread;
    uint32_t ip;
    uint32_t token;               // UDP: identifies the station's datagrams
    struct sockaddr_in uaddr;     // host side: where the station's datagrams come from
    volatile bool udp_up;
    volatile uint64_t udp_rx_ns;  // last datagram from the other end
    uint32_t relay_pair;          // through the hub server (0: direct): its pair id and our secret for UDP
    uint64_t relay_secret;
    // relayed links: direct UDP past the server (hole punching)
    bool direct_ok;               // ini Direct when the link came up
    struct sockaddr_in paddr;     // the other end's public address (from the server)
    volatile bool p_known, p_up;  // p_up: direct datagrams get through both ways, game packets go that way
    volatile uint64_t p_rx_ns;    // last proof that the direct path works both ways
    uint64_t dkey;                // the pair's direct key (from the server's answer; 0: none yet)
    int p_good;                   // station: direct hello answers in a row (DIRECT_REUP after a fall)
    bool p_ever, p_noanswer_said, p_nopunch_said;  // (for the log)
    uint64_t born_ns, q_since, p_since, next_query, next_punch;  // (the UDP thread's timers)
} Link;

static struct {
    pthread_mutex_t mu;
    int state, reason;
    uint8_t info[NETINFO_SIZE];
    uint32_t own_ip, subnet;
    uint8_t mac[6];
    uint8_t adv[ADV_MAX];
    uint32_t adv_size;
    int accept_policy;
    uint16_t lcv;
    KEvent *ev;
    uint32_t ev_r;
    // host
    hsock listen_s, disc_s;
    pthread_t host_th;
    bool hosting;
    Link nodes[MAX_NODES];  // [0] unused (the host itself)
    // station
    Link up;
    hsock us;  // UDP socket connected to the host
    pthread_t uth;
    volatile bool uth_run;
    // host: the hub's listing on the hub server
    pthread_t lobby_th;
    volatile bool lobby_run;
    volatile uint32_t lobby_seq;  // counts the NetworkInfo's changes: each listing thread sends it when it moved on
    struct sockaddr_in lobby_addr;
    // crossplay
    bool ryu;                    // the network is a lobby on a RyuLdn server
    uint32_t ryu_mask;
    bool room;                   // the network is a hub in a Yuzu-family room
    bool room_client[MAX_NODES]; // host: the room players in the hub, by node
    uint8_t room_cip[MAX_NODES][4];
    bool citra;                  // the network is a 3DS one in a Citra-family room (MHXX players; citra_bridge.rs)
    bool yuzu;                   // host, All Rooms: the hub is in the Yuzu room too (yuzu_door.rs)
    uint8_t door_of[MAX_NODES];  // host: whose player is in each slot (DOOR_*; our own stations are DOOR_NONE)
} L = {.mu = PTHREAD_MUTEX_INITIALIZER, .listen_s = BADSOCK, .disc_s = BADSOCK, .us = BADSOCK};

// Scan results: where each found network's host is (hub: on the hub server at addr, 0: the host's own address).
#define MAX_FOUND 512  // (a list holds up to HUB_LIST_MAX hubs; the rooms' networks are among them)
// (ver, key: what the hub was listed under; info, ryu, room, citra: crossplay networks, joined through their bridge)
static struct { uint8_t bssid[6], session[16]; struct sockaddr_in addr; uint64_t hub; char server[256], key[64]; uint16_t ver; uint8_t info[NETINFO_SIZE]; int ryu; bool room, citra; } found[MAX_FOUND];
static int nfound;

static void signal_change(void) {
    if (!L.ev) return;
    pthread_mutex_lock(&g_klock);
    kernel_signal(L.ev);
    pthread_mutex_unlock(&g_klock);
}

static void rand_bytes(void *p, size_t n) {
    for (size_t i = 0; i < n; i++) ((uint8_t *)p)[i] = (uint8_t)arc4random();
}
static const char *ipstr(uint32_t ip) {
    static __thread char b[2][20];
    static __thread int k;
    char *s = b[k++ & 1];
    snprintf(s, 20, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return s;
}
static uint8_t *node_at(uint8_t *info, int i) { return info + NI_NODES + i * NODE_SIZE; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

// A NetworkInfo from the network (LAN, hub server, host), before the game sees it: the game trusts it as it came from
// the system. Counts and sizes in range, strings terminated, node ids = their index (free entries all zero, as ours).
static bool info_sane(uint8_t *info) {
    uint8_t max = info[NI_NODE_MAX], n = info[NI_NODE_COUNT];
    if (info[NI_SSID] > 32 || max < 1 || max > MAX_NODES || n < 1 || n > max || rd16(info + NI_ADV_SIZE) > ADV_MAX)
        return false;
    info[NI_SSID + 0x21] = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        uint8_t *nd = node_at(info, i);
        if (!nd[NODE_CONNECTED]) { memset(nd, 0, NODE_SIZE); continue; }
        nd[NODE_CONNECTED] = 1;
        nd[NODE_ID] = (uint8_t)i;
        nd[NODE_NAME + 0x20] = 0;
    }
    return true;
}

#define ONCE(...) do { static int once_; if (!once_++) LOG(__VA_ARGS__); } while (0)

// ---- frames
static bool send_all(hsock s, const void *b, size_t n) {
    for (const char *p = b; n;) {
        int k = send(s, p, (int)n, 0);
        if (k <= 0) return false;
        p += k, n -= (size_t)k;
    }
    return true;
}
static bool recv_all(hsock s, void *b, size_t n) {
    for (char *p = b; n;) {
        int k = recv(s, p, (int)n, 0);
        if (k <= 0) return false;
        p += k, n -= (size_t)k;
    }
    return true;
}
static bool frame_send(hsock s, uint8_t type, const void *a, uint32_t alen, const void *b, uint32_t blen) {
    uint8_t h[5];
    uint32_t size = 1 + alen + blen;
    memcpy(h, &size, 4);
    h[4] = type;
    return send_all(s, h, 5) && (!alen || send_all(s, a, alen)) && (!blen || send_all(s, b, blen));
}
static bool link_send(Link *l, uint8_t type, const void *a, uint32_t alen, const void *b, uint32_t blen) {
    pthread_mutex_lock(&l->wmu);
    bool ok = l->alive && frame_send(l->s, type, a, alen, b, blen);
    pthread_mutex_unlock(&l->wmu);
    return ok;
}
// -1: closed / error; else the payload size (buf holds MAX_FRAME bytes)
static int frame_recv(hsock s, uint8_t *type, uint8_t *buf) {
    uint32_t size;
    if (!recv_all(s, &size, 4) || size < 1 || size > MAX_FRAME + 1 || !recv_all(s, type, 1)) return -1;
    return recv_all(s, buf, size - 1) ? (int)size - 1 : -1;
}
static void link_close(Link *l) {  // (caller: not holding L.mu if l has a thread to join)
    pthread_mutex_lock(&l->wmu);
    if (l->alive) {
        l->alive = false;
        shutdown(l->s, 2);
        hclose(l->s);
    }
    pthread_mutex_unlock(&l->wmu);
}

static hsock connect_to(const struct sockaddr_in *a, int timeout_ms) {
    hsock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == BADSOCK) return BADSOCK;
    hset_nonblock(s, true);
    connect(s, (const struct sockaddr *)a, sizeof *a);
    fd_set w, e;
    FD_ZERO(&w);
    FD_ZERO(&e);
    FD_SET(s, &w);
    FD_SET(s, &e);
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int err = 1;
    socklen_t el = sizeof err;
    if (select((int)s + 1, NULL, &w, &e, &tv) <= 0 || !FD_ISSET(s, &w) ||
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &el) || err) {
        hclose(s);
        return BADSOCK;
    }
    hset_nonblock(s, false);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    return s;
}

// Through the hub server, a datagram starts with "MHGR", the pair and our secret; the server passes on the rest.
static uint32_t relay_prefix(uint8_t *buf, const Link *l) {
    if (!l->relay_pair) return 0;
    memcpy(buf, RELAY_MAGIC, 4);
    wr32(buf + 4, l->relay_pair);
    memcpy(buf + 8, &l->relay_secret, 8);
    return RELAY_PREFIX;
}
// Straight to the other end of a relayed link (hole punched): "MHGR", the pair and the pair's direct key.
static uint32_t direct_prefix(uint8_t *buf, const Link *l) {
    if (!l->relay_pair) return 0;
    memcpy(buf, RELAY_MAGIC, 4);
    wr32(buf + 4, l->relay_pair);
    memcpy(buf + 8, &l->dkey, 8);
    return RELAY_PREFIX;
}
// The host tells the server where its end of a relayed link is (until the station's datagrams come through).
static void relay_register(hsock s, const Link *l) {
    uint8_t buf[RELAY_PREFIX];
    sendto(s, (const char *)buf, (int)relay_prefix(buf, l), 0, (const struct sockaddr *)&l->uaddr, sizeof l->uaddr);
}

// "host[:port]" (port: def without one) -> its IPv4 address
static bool resolve_host(const char *text, int def, struct sockaddr_in *out) {
    char host[256];
    int port = def;
    snprintf(host, sizeof host, "%s", text);
    char *colon = strrchr(host, ':');
    if (colon) { *colon = 0; port = atoi(colon + 1); }
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (port < 1 || port > 65535 || getaddrinfo(host, NULL, &hints, &res) || !res) return false;
    *out = *(struct sockaddr_in *)res->ai_addr;
    out->sin_port = htons((uint16_t)port);
    freeaddrinfo(res);
    return true;
}

// One datagram on link l: header [+ packet] [+ data], to the server's relay (relayed) or straight to to (a relayed
// link's direct path, or a LAN / address link).
static bool udp_send(hsock s, const struct sockaddr_in *to, const Link *l, bool relayed, uint8_t kind, uint8_t flags,
                     const VPacket *p, const void *data, uint32_t len) {
    uint8_t buf[RELAY_PREFIX + sizeof(UHdr) + sizeof(VPacket) + UDP_MAX_PAYLOAD];
    if (len > UDP_MAX_PAYLOAD) return false;
    UHdr h = {{0}, l->token, kind, flags, {0}};
    memcpy(h.magic, UDP_MAGIC, 4);
    uint32_t n = relayed ? relay_prefix(buf, l) : direct_prefix(buf, l);
    memcpy(buf + n, &h, sizeof h);
    n += sizeof h;
    if (p) {
        memcpy(buf + n, p, sizeof *p);
        n += sizeof *p;
    }
    if (len) memcpy(buf + n, data, len);
    n += len;
    return sendto(s, (const char *)buf, (int)n, 0, (const struct sockaddr *)to, sizeof *to) == (int)n;
}

static bool same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

// Windows reports an ICMP "port unreachable" (a punch that met a closed router) as an error on the socket's next
// receive: not wanted on a socket that talks to several addresses.
static void udp_no_connreset(hsock s) {
#ifdef _WIN32
    DWORD off = 0, got = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof off, NULL, 0, &got, NULL, NULL);
#else
    (void)s;
#endif
}

// ---- direct UDP past the hub server (hole punching)
// MHGU_LDN_BLOCK_DIRECT=<s>[-<t>] (testing the fallback): direct datagrams are ignored from s seconds after a link
// came up on (0: always, as behind a symmetric NAT), until t seconds if given (a passing outage).
static bool direct_blocked(const Link *l) {
    static int64_t from_ns = -2, to_ns = -1;
    if (from_ns == -2) {
        const char *e = getenv("MHGU_LDN_BLOCK_DIRECT"), *dash = e ? strchr(e, '-') : NULL;
        from_ns = e && *e ? (int64_t)atoi(e) * 1000000000ll : -1;
        to_ns = dash ? (int64_t)atoi(dash + 1) * 1000000000ll : -1;
        if (from_ns >= 0) LOG("[ldn] MHGU_LDN_BLOCK_DIRECT: direct datagrams ignored %s s after a link starts\n", e);
    }
    int64_t t = (int64_t)(plat_now_ns() - l->born_ns);
    return from_ns >= 0 && t >= from_ns && (to_ns < 0 || t < to_ns);
}

static void direct_reset(Link *l, bool ok) {
    l->direct_ok = ok && l->relay_pair;
    l->p_known = l->p_up = l->p_ever = l->p_noanswer_said = l->p_nopunch_said = false;
    l->p_rx_ns = l->p_since = l->next_query = l->next_punch = l->dkey = 0;
    l->p_good = 0;
    l->born_ns = l->q_since = plat_now_ns();
}

// Asks the server for the other end's address (every second for 30 s after the link came up or direct fell, then
// every 15 s, until direct works), and says in the log why there is no direct connection when it can tell.
static void peer_query(hsock s, Link *l, uint64_t now, const char *who) {
    if (!l->direct_ok || l->p_up) return;
    if (!l->p_known && !l->p_noanswer_said && now - l->born_ns > QUERY_FAST_NS) {
        l->p_noanswer_said = true;
        LOG("[ldn] %s: no direct connection: the other end does not offer one (its Direct = Off, an older version, or an "
            "older hub server); game packets stay on the hub server\n", who);
    }
    if (l->p_known && !l->p_nopunch_said && now - l->p_since > PUNCH_FAST_NS) {
        l->p_nopunch_said = true;
        LOG("[ldn] %s: no direct connection after %d s of trying (a router that gives each destination its own port, as "
            "symmetric / carrier-grade NAT, or a firewall): game packets stay on the hub server, still trying every 2 s\n",
            who, (int)(PUNCH_FAST_NS / 1000000000ull));
    }
    if (now < l->next_query) return;
    l->next_query = now + (now - l->q_since < QUERY_FAST_NS ? 1000000000ull : 15000000000ull);
    uint8_t buf[PEER_MSG_SIZE] = {0};
    relay_prefix(buf, l);
    memcpy(buf + RELAY_PREFIX, PEER_QUERY, 4);
    sendto(s, (const char *)buf, sizeof buf, 0, (const struct sockaddr *)&l->uaddr, sizeof l->uaddr);
}

// The server's answer (from l->uaddr, checked by the caller): the other end's address. true: it was one.
static bool peer_answer(Link *l, const uint8_t *b, int n, const char *who) {
    if (n != PEER_MSG_SIZE || memcmp(b, PEER_ANSWER, 4) || !l->direct_ok || rd32(b + 4) != l->relay_pair ||
        rd64(b + 8) != l->relay_secret)
        return false;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    memcpy(&a.sin_addr.s_addr, b + 16, 4);
    memcpy(&a.sin_port, b + 20, 2);
    l->dkey = rd64(b + 24);
    if (l->p_known && same_addr(&a, &l->paddr)) return true;
    if (l->p_known) LOG("[ldn] %s: the other end's address changed (a network change?)\n", who);
    l->paddr = a;  // (never logged: the other player's address)
    l->p_since = plat_now_ns(), l->next_punch = 0;
    if (!l->p_known) LOG("[ldn] %s: the hub server gave the other end's address, trying a direct connection\n", who);
    l->p_known = true;
    return true;
}

// A datagram straight from the other end (not through the server): from its address as the server gave it (the
// port may differ: the latest wins). What keeps the direct path alive (p_rx_ns) is only what proves it works both
// ways: on the station the answers to its own hellos, on the host the station's hellos (which say whether ours get
// through) and packets. A one-way path (punches still arriving, our datagrams lost) falls back like a dead one.
static bool direct_from(Link *l, const struct sockaddr_in *from) {
    if (!l->p_known || from->sin_addr.s_addr != l->paddr.sin_addr.s_addr || direct_blocked(l)) return false;
    l->paddr.sin_port = from->sin_port;
    return true;
}
static bool direct_usable(const Link *l) { return l->p_up && plat_now_ns() - l->p_rx_ns < DIRECT_SILENCE_NS; }
static void direct_check_silence(Link *l, uint64_t now, const char *who) {
    if (l->p_up && now - l->p_rx_ns > DIRECT_SILENCE_NS) {
        l->p_up = false;
        l->p_good = 0;
        l->p_since = l->q_since = now, l->next_query = 0, l->p_nopunch_said = false;  // (punch and ask fast again for a while)
        LOG("[ldn] %s: the direct connection went quiet (%.1f s without proof it works both ways): game packets go "
            "through the hub server again\n", who, (double)(now - l->p_rx_ns) / 1e9);
    }
}

// ---- MHGU_LDN_RTT=1: the station logs the round trip of its UDP hellos every 10 s, by path (the game's packets
// take the same ones), and says hello every 250 ms instead of every 2 s.
static int rtt_on = -1;
static struct { uint32_t n; double sum, min, max; } rtt_acc[2];  // [0] relay / LAN, [1] direct
static uint64_t rtt_next_log;
static void pkt_report(const char *who);
static bool rtt_enabled(void) {
    if (rtt_on < 0) {
        const char *e = getenv("MHGU_LDN_RTT");
        rtt_on = e && atoi(e) > 0;
    }
    return rtt_on;
}
static void rtt_add(int path, const uint8_t *stamp, int len) {
    if (!rtt_enabled() || len < 8) return;
    uint64_t t0 = rd64(stamp), now = plat_now_ns();
    if (t0 > now || now - t0 > 10000000000ull) return;
    double ms = (double)(now - t0) / 1e6;
    if (!rtt_acc[path].n || ms < rtt_acc[path].min) rtt_acc[path].min = ms;
    if (!rtt_acc[path].n || ms > rtt_acc[path].max) rtt_acc[path].max = ms;
    rtt_acc[path].n++, rtt_acc[path].sum += ms;
}
static void rtt_report(bool relayed, uint64_t now) {
    if (!rtt_enabled()) return;
    if (!rtt_next_log) rtt_next_log = now + 10000000000ull;
    if (now < rtt_next_log) return;
    rtt_next_log = now + 10000000000ull;
    char line[2][96];
    for (int i = 0; i < 2; i++) {
        if (rtt_acc[i].n)
            snprintf(line[i], sizeof line[i], "%u, avg %.2f, min %.2f, max %.2f ms", rtt_acc[i].n,
                     rtt_acc[i].sum / rtt_acc[i].n, rtt_acc[i].min, rtt_acc[i].max);
        else
            snprintf(line[i], sizeof line[i], "none");
    }
    LOG("[ldn] round trip (UDP hello -> answer, last 10 s): %s %s | direct %s\n",
        relayed ? "through the hub server" : "to the host (LAN / address)", line[0], line[1]);
    memset(rtt_acc, 0, sizeof rtt_acc);
    pkt_report("station");
}
static uint64_t hello_every(void) { return rtt_enabled() ? 250000000ull : 2000000000ull; }

// (with MHGU_LDN_RTT) the game's packets by path, sent and received: [0] direct, [1] UDP through the hub server or to
// the LAN / address host, [2] the TCP link. Logged with the round trips (station) or every 10 s (host).
enum { PATH_DIRECT, PATH_UDP, PATH_TCP };
static uint32_t pkt_tx[3], pkt_rx[3];
static void pkt_count(uint32_t *a, int path) { if (rtt_enabled()) __atomic_add_fetch(&a[path], 1, __ATOMIC_RELAXED); }
static void pkt_report(const char *who) {
    uint32_t t[3], r[3];
    for (int i = 0; i < 3; i++)
        t[i] = __atomic_exchange_n(&pkt_tx[i], 0, __ATOMIC_RELAXED), r[i] = __atomic_exchange_n(&pkt_rx[i], 0, __ATOMIC_RELAXED);
    LOG("[ldn] %s: game packets in the last 10 s, sent / received: direct %u / %u, UDP via server or LAN %u / %u, TCP %u / %u\n",
        who, t[0], r[0], t[1], r[1], t[2], r[2]);
}

static Hello make_hello(bool scan_only, const uint8_t *user_name, uint16_t version) {
    Hello h = {0};
    memcpy(h.magic, PROTO_MAGIC, 4);
    h.version = version;
    h.scan_only = scan_only;
    h.lcv = L.lcv;
    memcpy(h.mac, L.mac, 6);
    if (user_name) memcpy(h.name, user_name, 0x20);
    return h;
}

// ---- the virtual network's packets (vsock.c -> here)
uint32_t ldn_own_ip(void) { return L.own_ip; }
uint32_t ldn_broadcast_ip(void) { return L.subnet ? L.subnet | (L.ryu && L.ryu_mask ? ~L.ryu_mask : 0xFFu) : 0xFFFFFFFFu; }
bool ldn_net_up(void) { return L.state == ST_AP_CREATED || L.state == ST_STATION_CONNECTED; }
bool ldn_hosting(void) { return L.state == ST_AP_CREATED; }
bool ldn_crossplay(void) { return L.ryu || L.room || L.citra || L.yuzu; }
bool ldn_party(int *n, int *max, char *id, size_t size) {
    pthread_mutex_lock(&L.mu);
    bool up = ldn_net_up();
    if (up) {
        *n = L.info[NI_NODE_COUNT], *max = L.info[NI_NODE_MAX];
        Sha256 h;  // (a hash of the session id: Discord shows the party id to others, the network's own id stays here)
        char hex[65];
        sha256_init(&h);
        sha256_update(&h, "mhgu-party", 10);
        sha256_update(&h, L.info + NI_SESSION_ID, 16);
        sha256_hex(&h, hex);
        snprintf(id, size, "%.32s", hex);
    }
    pthread_mutex_unlock(&L.mu);
    return up;
}

static bool udp_usable(const Link *l) { return l->udp_up && plat_now_ns() - l->udp_rx_ns < UDP_SILENCE_NS; }

// Host: deliver to the host's own sockets and/or forward to the stations (from: the sending node, 0 = the host).
// Hosting in a Citra room / a Yuzu room too (All Rooms): what is for everyone, or for none of our own stations, also
// goes to the doors (each passes on what is for its players; origin: the door it came from, it doesn't go back).
static void host_route_ex(int from, int origin, const VPacket *p, const void *data, uint32_t len) {
    bool all = p->dst_ip == ldn_broadcast_ip() || p->dst_ip == 0xFFFFFFFFu, ours = false;
    if (all || p->dst_ip == L.own_ip) vsock_deliver(p, data, len);
    for (int i = 1; i < MAX_NODES; i++) {
        Link *l = &L.nodes[i];
        ours |= l->alive && l->ip == p->dst_ip;
        if (i == from || !l->alive || !(all || l->ip == p->dst_ip)) continue;
        hsock ds = L.disc_s;
        if (ds != BADSOCK && direct_usable(l) && udp_send(ds, &l->paddr, l, false, U_DATA, 0, p, data, len)) {
            pkt_count(pkt_tx, PATH_DIRECT);
        } else if (ds != BADSOCK && udp_usable(l) && udp_send(ds, &l->uaddr, l, true, U_DATA, 0, p, data, len)) {
            pkt_count(pkt_tx, PATH_UDP);
        } else {
            link_send(l, F_PACKET, p, sizeof *p, data, len);
            pkt_count(pkt_tx, PATH_TCP);
        }
    }
    bool out = p->proto == VPROTO_UDP && (all || (!ours && p->dst_ip != L.own_ip));
    if (out && L.citra && origin != ORIGIN_CITRA) mhgu_uds_send(p->src_ip, p->dst_ip, p->dst_port, data, len);
    if (out && L.yuzu && origin != ORIGIN_YUZU) mhgu_ydoor_send(p->src_ip, p->dst_ip, p->src_port, p->dst_port, data, len);
}
static void host_route(int from, const VPacket *p, const void *data, uint32_t len) { host_route_ex(from, ORIGIN_HUB, p, data, len); }

static void ip_bytes(uint32_t ip, uint8_t b[4]) { b[0] = (uint8_t)(ip >> 24); b[1] = (uint8_t)(ip >> 16); b[2] = (uint8_t)(ip >> 8); b[3] = (uint8_t)ip; }
static void ryu_xmit(const VPacket *p, const void *data, uint32_t len) {
    bool all = p->dst_ip == ldn_broadcast_ip() || p->dst_ip == 0xFFFFFFFFu;
    RyuProxy r = {p->src_ip, all ? ldn_broadcast_ip() : p->dst_ip, p->src_port, p->dst_port, 17};
    mhgu_ryu_send(&r, data, len);
}
static void room_xmit(const VPacket *p, const void *data, uint32_t len) {
    bool all = p->dst_ip == ldn_broadcast_ip() || p->dst_ip == 0xFFFFFFFFu;
    uint8_t s[4], d[4];
    ip_bytes(p->src_ip, s);
    ip_bytes(p->dst_ip, d);
    mhgu_room_send_proxy(s, p->src_port, d, p->dst_port, 4, all, data, len);
}
void ldn_send(const VPacket *p, const void *data, uint32_t len) {
    if (L.citra && L.state != ST_AP_CREATED) {  // (joined in the Citra room: the bridge plays this game's packets as a 3DS's)
        if (ldn_net_up() && p->proto == VPROTO_UDP) mhgu_uds_send(p->src_ip, p->dst_ip, p->dst_port, data, len);
        return;
    }
    if (L.ryu || L.room) {  // (an emulator's network: its server or room carries the packets)
        if (!ldn_net_up()) return;
        if (L.ryu) ryu_xmit(p, data, len); else room_xmit(p, data, len);
        return;
    }
    if (L.state == ST_AP_CREATED) { host_route(0, p, data, len); return; }
    if (L.state != ST_STATION_CONNECTED) return;
    bool all = p->dst_ip == ldn_broadcast_ip() || p->dst_ip == 0xFFFFFFFFu;
    if (all || p->dst_ip == L.own_ip) vsock_deliver(p, data, len);  // (a broadcast reaches the sender too)
    if (p->dst_ip == L.own_ip) return;
    hsock us = L.us;
    if (us != BADSOCK && direct_usable(&L.up) && udp_send(us, &L.up.paddr, &L.up, false, U_DATA, 0, p, data, len)) {
        pkt_count(pkt_tx, PATH_DIRECT);
    } else if (us != BADSOCK && udp_usable(&L.up) && udp_send(us, &L.up.uaddr, &L.up, true, U_DATA, 0, p, data, len)) {
        pkt_count(pkt_tx, PATH_UDP);
    } else {
        link_send(&L.up, F_PACKET, p, sizeof *p, data, len);
        pkt_count(pkt_tx, PATH_TCP);
    }
}

// The port's own messages to every other game in the network (reliable, in order: the TCP link).
void ldn_send_aux(const void *data, uint32_t len) {
    if (L.ryu || L.room || (L.citra && L.state != ST_AP_CREATED)) return;  // (the emulators' players don't speak them)
    if (L.state == ST_AP_CREATED) {
        for (int i = 1; i < MAX_NODES; i++)
            if (L.nodes[i].alive) link_send(&L.nodes[i], F_AUX, data, len, NULL, 0);
    } else if (L.state == ST_STATION_CONNECTED) {
        link_send(&L.up, F_AUX, data, len, NULL, 0);
    }
}

// ---- MHGU_LDN_LAG=<ms>: received packets wait that long before they go on (testing high ping: set it on both
// ends, the round trip grows by twice the value). Kept in arrival order by one delivery thread.
typedef struct Delayed { struct Delayed *next; uint64_t due; int from; VPacket p; uint32_t len; uint8_t data[]; } Delayed;
static Delayed *lag_head, *lag_tail;
static pthread_mutex_t lag_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lag_cv = PTHREAD_COND_INITIALIZER;

static void *lag_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&lag_mu);
    for (;;) {
        while (!lag_head) pthread_cond_wait(&lag_cv, &lag_mu);
        Delayed *d = lag_head;
        uint64_t now = plat_now_ns();
        if (now < d->due) {
            pthread_mutex_unlock(&lag_mu);
            plat_sleep_until_ns(d->due);
            pthread_mutex_lock(&lag_mu);
            continue;
        }
        lag_head = d->next;
        if (!lag_head) lag_tail = NULL;
        pthread_mutex_unlock(&lag_mu);
        if (d->from > 0) host_route(d->from, &d->p, d->data, d->len);
        else vsock_deliver(&d->p, d->data, d->len);
        free(d);
        pthread_mutex_lock(&lag_mu);
    }
    return NULL;
}

// A packet from the link: from = the sending station (host side) or 0 (a station: from the host).
// MHGU_LDN_LOSS=<percent>: that share of the received game packets is dropped (testing a lossy connection).
static void link_received(int from, const VPacket *p, const void *data, uint32_t len) {
    static int lag_ms = -1, loss_pct;
    if (lag_ms < 0) {
        const char *e = getenv("MHGU_LDN_LAG");
        lag_ms = e ? atoi(e) : 0;
        if (lag_ms > 0) {
            pthread_t t;
            pthread_create(&t, NULL, lag_thread, NULL);
            LOG("[ldn] MHGU_LDN_LAG: received packets are held back %d ms\n", lag_ms);
        }
        if ((e = getenv("MHGU_LDN_LOSS")) && atoi(e) > 0) {
            loss_pct = atoi(e);
            LOG("[ldn] MHGU_LDN_LOSS: dropping %d%% of the received packets\n", loss_pct);
        }
    }
    if (loss_pct && (int)(arc4random() % 100) < loss_pct) return;
    if (lag_ms <= 0) {
        if (from > 0) host_route(from, p, data, len);
        else vsock_deliver(p, data, len);
        return;
    }
    Delayed *d = malloc(sizeof *d + len);
    if (!d) return;
    d->next = NULL, d->due = plat_now_ns() + (uint64_t)lag_ms * 1000000ull, d->from = from, d->p = *p, d->len = len;
    memcpy(d->data, data, len);
    pthread_mutex_lock(&lag_mu);
    if (lag_tail) lag_tail->next = d; else lag_head = d;
    lag_tail = d;
    pthread_cond_signal(&lag_cv);
    pthread_mutex_unlock(&lag_mu);
}

// ---- crossplay: a lobby on a RyuLdn server (Ryujinx/Yuzu family)
static void ryu_lobby_changed(const uint8_t *info) {
    pthread_mutex_lock(&L.mu);
    if (!L.ryu) { pthread_mutex_unlock(&L.mu); return; }
    if (info) {
        bool ok = info_sane((uint8_t *)info);
        if (ok) memcpy(L.info, info, NETINFO_SIZE);
        LOG("[ldn] RyuLdn lobby update: %d of %d players%s\n", info[NI_NODE_COUNT], info[NI_NODE_MAX], ok ? "" : " (rejected as malformed)");
    } else if (L.state == ST_AP_CREATED) {
        L.state = ST_ACCESS_POINT, L.reason = DR_DESTROYED_BY_SYSTEM;
        LOG("[ldn] the RyuLdn server closed the hub\n");
    } else if (L.state == ST_STATION_CONNECTED) {
        L.state = ST_ERROR, L.reason = DR_SIGNAL_LOST;
        LOG("[ldn] connection to the RyuLdn lobby ended\n");
    }
    bool gone = !info;
    pthread_mutex_unlock(&L.mu);
    if (mhgu_xb_active()) mhgu_xb_lobby(info);
    if (gone) vsock_network_down();
    signal_change();
}

static void ryu_data_received(const RyuProxy *r, const uint8_t *data, uint32_t len) {
    if (!L.ryu || r->proto != 17) return;
    VPacket p = {.src_ip = r->src_ip, .dst_ip = r->dst_ip, .src_port = r->src_port, .dst_port = r->dst_port,
                 .proto = VPROTO_UDP, .kind = 0};
    vsock_deliver(&p, data, len);
}

static void ryu_stop(void) {
    if (!L.ryu) return;
    mhgu_xb_end();
    mhgu_ryu_leave();
    L.ryu = false;
    L.ryu_mask = 0;
    vsock_network_down();
}

// ---- crossplay doors (Citra Room, All Rooms): players of a room in the hub this game hosts. ldn.c keeps the slots:
// each door is told the hub without its own players (it sees the others as reachable through us) and hands back the
// hub with them; they go in the slots no one else holds. Two taking one slot at once: the first keeps it.
static void push_info_locked(int except);
static bool link_slot_locked(int i) { return L.nodes[i].alive || L.nodes[i].has_thread; }
static void door_recount_locked(void) {
    int n = 1;
    for (int i = 1; i < MAX_NODES; i++) n += node_at(L.info, i)[NODE_CONNECTED] ? 1 : 0;
    L.info[NI_NODE_COUNT] = (uint8_t)n;
}
// the door's hub: its players into free slots (or its own); returns how many of its players lost a clash (their IPs in clash)
static int door_merge_locked(int door, const uint8_t *info, uint32_t *clash) {
    uint8_t copy[NETINFO_SIZE];
    memcpy(copy, info, NETINFO_SIZE);
    if (!info_sane(copy)) return 0;
    int nclash = 0;
    for (int i = 1; i < MAX_NODES; i++) {
        uint8_t *mine = node_at(L.info, i), *theirs = node_at(copy, i);
        bool held = link_slot_locked(i) || (L.door_of[i] != DOOR_NONE && L.door_of[i] != door);
        if (held) {
            if (theirs[NODE_CONNECTED] && memcmp(theirs + NODE_MAC, mine + NODE_MAC, 6)) clash[nclash++] = rd32(theirs + NODE_IP);
            continue;
        }
        memcpy(mine, theirs, NODE_SIZE);
        L.door_of[i] = mine[NODE_CONNECTED] ? (uint8_t)door : DOOR_NONE;
    }
    door_recount_locked();
    return nclash;
}
static void door_drop_locked(int door) {  // (its room is gone)
    for (int i = 1; i < MAX_NODES; i++)
        if (L.door_of[i] == door) memset(node_at(L.info, i), 0, NODE_SIZE), L.door_of[i] = DOOR_NONE;
    door_recount_locked();
}
// every door, of the hub without its own players (the doors hand nothing back here: safe under L.mu)
static void door_sync_locked(void) {
    if (L.state != ST_AP_CREATED) return;
    for (int door = DOOR_CITRA; door <= DOOR_YUZU; door++) {
        if (door == DOOR_CITRA ? !L.citra : !L.yuzu) continue;
        uint8_t view[NETINFO_SIZE];
        memcpy(view, L.info, NETINFO_SIZE);
        int n = 1;
        for (int i = 1; i < MAX_NODES; i++) {
            if (L.door_of[i] == door) memset(node_at(view, i), 0, NODE_SIZE);
            n += node_at(view, i)[NODE_CONNECTED] ? 1 : 0;
        }
        view[NI_NODE_COUNT] = (uint8_t)n;
        if (door == DOOR_CITRA) mhgu_uds_host_update(view);
        else mhgu_ydoor_update(view);
    }
}

// ---- the Yuzu room as a door (All Rooms)
static void ydoor_info_changed(const uint8_t *info) {
    uint32_t clash[MAX_NODES];
    int nclash = 0;
    pthread_mutex_lock(&L.mu);
    if (L.yuzu && info && L.state == ST_AP_CREATED) {
        nclash = door_merge_locked(DOOR_YUZU, info, clash);
        push_info_locked(0);
    }
    pthread_mutex_unlock(&L.mu);
    for (int i = 0; i < nclash; i++) mhgu_ydoor_reject(clash[i]);
    signal_change();
}
static void ydoor_packet(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport, const uint8_t *data, uint32_t len) {
    if (!L.yuzu || L.state != ST_AP_CREATED) return;
    VPacket p = {.src_ip = src, .dst_ip = dst, .src_port = sport, .dst_port = dport, .proto = VPROTO_UDP, .kind = 0};
    host_route_ex(0, ORIGIN_YUZU, &p, data, len);
}
static void ydoor_lost(void) {
    mhgu_ydoor_lost();
    pthread_mutex_lock(&L.mu);
    bool was = L.yuzu;
    if (was) {
        L.yuzu = false;
        door_drop_locked(DOOR_YUZU);
        push_info_locked(0);
    }
    pthread_mutex_unlock(&L.mu);
    if (was) { LOG("[ldn] the Yuzu room is gone: its players left, the hub goes on\n"); signal_change(); }
}

// ---- crossplay: a Yuzu-family room
static uint32_t ip_u32(const uint8_t b[4]) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
static void room_mac(uint8_t mac[6]) { uint8_t ip[4]; mhgu_room_own_ip(ip); mac[0] = 0x02, mac[1] = 0; memcpy(mac + 2, ip, 4); }

static struct { uint8_t info[NETINFO_SIZE]; } room_scan[MAX_FOUND];
static int room_nscan;
static bool room_scanning, room_info_dirty;
static pthread_mutex_t room_scan_mu = PTHREAD_MUTEX_INITIALIZER;

static void room_sync_all(void) {
    uint8_t own[4], info[NETINFO_SIZE];
    mhgu_room_own_ip(own);
    pthread_mutex_lock(&L.mu);
    memcpy(info, L.info, NETINFO_SIZE);
    pthread_mutex_unlock(&L.mu);
    for (int i = 1; i < MAX_NODES; i++)
        if (L.room_client[i]) mhgu_room_send_ldn(RLDN_SYNC_NETWORK, own, L.room_cip[i], false, info, NETINFO_SIZE);
}

static void room_stop(void) {
    if (!L.room) return;
    if (L.state == ST_AP_CREATED) {
        uint8_t own[4];
        mhgu_room_own_ip(own);
        for (int i = 1; i < MAX_NODES; i++)
            if (L.room_client[i]) mhgu_room_send_ldn(RLDN_DESTROY_NETWORK, own, L.room_cip[i], false, NULL, 0);
    } else if (L.state == ST_STATION_CONNECTED) {
        uint8_t own[4], host[4];
        mhgu_room_own_ip(own);
        ip_bytes(rd32(node_at(L.info, 0) + NODE_IP), host);
        uint8_t node[NODE_SIZE] = {0};
        wr32(node + NODE_IP, L.own_ip);
        room_mac(node + NODE_MAC);
        mhgu_room_send_ldn(RLDN_DISCONNECT, own, host, false, node, NODE_SIZE);
    }
    memset(L.room_client, 0, sizeof L.room_client);
    L.room = false;
    vsock_network_down();
}

static bool room_from_host_locked(const uint8_t ip[4]) { return ip_u32(ip) == rd32(node_at(L.info, 0) + NODE_IP); }
static bool room_in_hub_locked(uint32_t ip) {
    for (int i = 0; i < MAX_NODES; i++)
        if (node_at(L.info, i)[NODE_CONNECTED] && rd32(node_at(L.info, i) + NODE_IP) == ip) return true;
    return false;
}

static void room_ldn_received(uint8_t type, const uint8_t lip[4], const uint8_t rip[4], bool bc, const uint8_t *data, uint32_t len) {
    if (L.yuzu) {  // (All Rooms: the hub is the door's to show)
        if (type == RLDN_SCAN || type == RLDN_CONNECT || type == RLDN_DISCONNECT) mhgu_ydoor_ldn(type, lip, data, len);
        return;
    }
    if (mhgu_xb_active() && (type == RLDN_SCAN || type == RLDN_CONNECT || type == RLDN_DISCONNECT)) {
        mhgu_xb_room_ldn(type, lip, rip, bc, data, len);
        return;
    }
    uint8_t own[4];
    mhgu_room_own_ip(own);
    switch (type) {
    case RLDN_SCAN: {
        uint8_t info[NETINFO_SIZE];
        bool host;
        pthread_mutex_lock(&L.mu);
        host = L.room && L.state == ST_AP_CREATED;
        if (host) memcpy(info, L.info, NETINFO_SIZE);
        pthread_mutex_unlock(&L.mu);
        if (host) mhgu_room_send_ldn(RLDN_SCAN_RESP, own, lip, false, info, NETINFO_SIZE);
        break;
    }
    case RLDN_SCAN_RESP:
        if (len >= NETINFO_SIZE && info_sane((uint8_t *)data)) {
            pthread_mutex_lock(&room_scan_mu);
            if (room_scanning) {
                int i = 0;
                while (i < room_nscan && memcmp(room_scan[i].info + NI_BSSID, (const uint8_t *)data + NI_BSSID, 6)) i++;
                if (i < MAX_FOUND) { memcpy(room_scan[i].info, data, NETINFO_SIZE); if (i == room_nscan) room_nscan++; }
            }
            pthread_mutex_unlock(&room_scan_mu);
        }
        break;
    case RLDN_CONNECT: {
        if (len < NODE_SIZE) break;
        bool changed = false, refused = false, again = false;
        pthread_mutex_lock(&L.mu);
        if (L.room && L.state == ST_AP_CREATED) {
            uint16_t lcv = rd16((const uint8_t *)data + NODE_LCV);
            for (int k = 1; k < MAX_NODES; k++) again |= L.room_client[k] && !memcmp(L.room_cip[k], lip, 4);
            refused = !again && (L.info[NI_NODE_COUNT] >= L.info[NI_NODE_MAX] || L.info[NI_ACCEPT_POLICY] == 1 ||
                                 (lcv && lcv != L.lcv));
        }
        if (L.room && L.state == ST_AP_CREATED && !again && !refused) {
            int i = 1;
            while (i < MAX_NODES && node_at(L.info, i)[NODE_CONNECTED]) i++;
            refused = i == MAX_NODES;
            if (i < MAX_NODES) {
                uint8_t *nd = node_at(L.info, i);
                memcpy(nd, data, NODE_SIZE);
                nd[NODE_ID] = (uint8_t)i, nd[NODE_CONNECTED] = 1;
                L.room_client[i] = true;
                memcpy(L.room_cip[i], lip, 4);
                L.info[NI_NODE_COUNT]++;
                changed = true;
                LOG("[ldn] room member %u.%u.%u.%u joined the hub as node %d\n", lip[0], lip[1], lip[2], lip[3], i);
            }
        }
        pthread_mutex_unlock(&L.mu);
        if (refused) mhgu_room_send_ldn(RLDN_DESTROY_NETWORK, own, lip, false, NULL, 0);
        if (changed || again) room_sync_all();
        if (changed) signal_change();
        break;
    }
    case RLDN_DISCONNECT: {
        if (len < NODE_SIZE) break;
        bool changed = false;
        pthread_mutex_lock(&L.mu);
        if (L.room && L.state == ST_AP_CREATED)
            for (int i = 1; i < MAX_NODES; i++)
                if (L.room_client[i] && !memcmp(L.room_cip[i], lip, 4)) {
                    memset(node_at(L.info, i), 0, NODE_SIZE);
                    node_at(L.info, i)[NODE_ID] = (uint8_t)i;
                    L.room_client[i] = false;
                    if (L.info[NI_NODE_COUNT] > 1) L.info[NI_NODE_COUNT]--;
                    changed = true;
                }
        pthread_mutex_unlock(&L.mu);
        if (changed) { room_sync_all(); signal_change(); }
        break;
    }
    case RLDN_SYNC_NETWORK:
        if (len >= NETINFO_SIZE && info_sane((uint8_t *)data)) {
            pthread_mutex_lock(&L.mu);
            if (L.room && (L.state == ST_STATION || L.state == ST_STATION_CONNECTED) && room_from_host_locked(lip)) {
                memcpy(L.info, data, NETINFO_SIZE);
                L.state = ST_STATION_CONNECTED, L.reason = DR_NONE;
            }
            pthread_mutex_unlock(&L.mu);
            signal_change();
        }
        break;
    case RLDN_DESTROY_NETWORK: {
        bool gone = false;
        pthread_mutex_lock(&L.mu);
        if (L.room && L.state == ST_STATION_CONNECTED && room_from_host_locked(lip)) {
            L.state = ST_STATION, L.reason = DR_DESTROYED_BY_SYSTEM;
            L.own_ip = L.subnet = 0, gone = true;
            L.room = false;
        }
        pthread_mutex_unlock(&L.mu);
        if (gone) { LOG("[ldn] the room's hub was closed by its host\n"); vsock_network_down(); signal_change(); }
        break;
    }
    }
}

static void room_proxy_received(const uint8_t sip[4], uint16_t sport, const uint8_t dip[4], uint16_t dport, uint8_t proto, bool bc,
                                const uint8_t *data, uint32_t len) {
    if (L.yuzu) { mhgu_ydoor_proxy(sip, sport, dip, dport, proto, bc, data, len); return; }
    if (mhgu_xb_active()) { mhgu_xb_room_proxy(sip, sport, dip, dport, proto, bc, data, len); return; }
    if (!L.room || proto != 4) return;
    pthread_mutex_lock(&L.mu);
    bool ours = L.room && ldn_net_up() && room_in_hub_locked(ip_u32(sip));
    pthread_mutex_unlock(&L.mu);
    if (!ours) return;
    VPacket p = {.src_ip = ip_u32(sip), .dst_ip = ip_u32(dip), .src_port = sport, .dst_port = dport, .proto = VPROTO_UDP, .kind = 0};
    vsock_deliver(&p, data, len);
}

static void room_members_changed(const uint8_t (*ips)[4], int n) {
    if (L.yuzu) { mhgu_ydoor_members(ips, n); return; }
    if (mhgu_xb_active()) { mhgu_xb_room_members(ips, n); return; }
    bool changed = false;
    pthread_mutex_lock(&L.mu);
    if (L.room && L.state == ST_AP_CREATED) {
        for (int i = 1; i < MAX_NODES; i++) {
            if (!L.room_client[i]) continue;
            bool present = false;
            for (int j = 0; j < n; j++) present |= !memcmp(L.room_cip[i], ips[j], 4);
            if (present) continue;
            memset(node_at(L.info, i), 0, NODE_SIZE);
            node_at(L.info, i)[NODE_ID] = (uint8_t)i;
            L.room_client[i] = false;
            if (L.info[NI_NODE_COUNT] > 1) L.info[NI_NODE_COUNT]--;
            changed = true;
        }
    }
    pthread_mutex_unlock(&L.mu);
    if (changed) { room_sync_all(); signal_change(); }
}
static void room_lost(void) {
    LOG("[room] the room connection ended\n");
    if (L.yuzu) { ydoor_lost(); return; }
    mhgu_xb_end();
    bool gone = false;
    pthread_mutex_lock(&L.mu);
    if (L.room) {
        if (L.state == ST_AP_CREATED) L.state = ST_ACCESS_POINT, L.reason = DR_DESTROYED_BY_SYSTEM, gone = true;
        else if (L.state == ST_STATION_CONNECTED) L.state = ST_ERROR, L.reason = DR_SIGNAL_LOST, gone = true;
        if (gone) { memset(L.room_client, 0, sizeof L.room_client); L.room = false; }
    }
    pthread_mutex_unlock(&L.mu);
    if (gone) { vsock_network_down(); signal_change(); }
}

// ---- crossplay: a Citra-family room (Azahar, Citra): Monster Hunter XX players' 3DS networks. The bridge
// (crates/mhgu-crossplay/src/citra_bridge.rs) is one 3DS in the room: it lists MHXX players' networks, joins them,
// and hosts this game's network for them; it hands us an nn::ldn network for each.
// The bridge's network: hosting, its MHXX players go in the slots our own stations don't hold (a clash, both taking
// one slot at once: ours keeps it, the MHXX player is sent away); joined, it is the network.
static void citra_info_changed(const uint8_t *info) {
    bool gone = false;
    uint32_t clash[MAX_NODES];
    int nclash = 0;
    pthread_mutex_lock(&L.mu);
    if (L.citra && info && L.state == ST_AP_CREATED) {
        nclash = door_merge_locked(DOOR_CITRA, info, clash);
        push_info_locked(0);  // (our stations, the hub server's list and the other door hear of it)
    } else if (L.citra && info) {
        uint8_t copy[NETINFO_SIZE];
        memcpy(copy, info, NETINFO_SIZE);
        if (ldn_net_up() && info_sane(copy)) memcpy(L.info, copy, NETINFO_SIZE);
    } else if (L.citra && L.state == ST_AP_CREATED) {
        // the room is gone: its players with it; the network goes on for everyone else
        L.citra = false;
        door_drop_locked(DOOR_CITRA);
        push_info_locked(0);
        LOG("[ldn] the Citra room is gone: its players left, the hub goes on\n");
    } else if (L.citra && L.state == ST_STATION_CONNECTED) {
        L.state = ST_ERROR, L.reason = DR_SIGNAL_LOST, gone = true;
        L.citra = false, L.own_ip = L.subnet = 0;
    }
    pthread_mutex_unlock(&L.mu);
    for (int i = 0; i < nclash; i++) mhgu_uds_reject(clash[i]);
    if (gone) { LOG("[ldn] the network in the Citra room is gone\n"); vsock_network_down(); }
    signal_change();
}
static void citra_packet(uint32_t src, uint32_t dst, const uint8_t *data, uint32_t len) {
    if (!L.citra || !ldn_net_up()) return;
    VPacket p = {.src_ip = src, .dst_ip = dst, .src_port = 12345, .dst_port = 12345, .proto = VPROTO_UDP, .kind = 0};
    if (L.state == ST_AP_CREATED) host_route_ex(0, ORIGIN_CITRA, &p, data, len);  // (to us, our stations, the other door)
    else vsock_deliver(&p, data, len);
}
static void host_stop(uint32_t reason);
static void citra_stop(void) {  // leaves the network (the room stays); hosting, our own host closes too
    if (!L.citra) return;
    bool host = L.state == ST_AP_CREATED;
    if (host) mhgu_uds_host_stop();
    else mhgu_uds_leave();
    L.citra = false;
    if (host) host_stop(DR_DESTROYED_BY_USER);
    vsock_network_down();
}
static bool citra_configured(void) {
    return (xp_config.crossplay == XP_CITRA || xp_config.crossplay == XP_ALL) && xp_config.citra_room_server[0];
}

static pthread_t citra_keep_th;
static pthread_mutex_t citra_keep_mu = PTHREAD_MUTEX_INITIALIZER;
static bool citra_keep_started;
static int citra_wanted;
static Config citra_keep_config;
static void *citra_keeper(void *arg) {
    (void)arg;
    xp_config = citra_keep_config;  // (the settings applied when it started)
    uint64_t next = 0;
    while (__atomic_load_n(&citra_wanted, __ATOMIC_ACQUIRE)) {
        uint64_t now = plat_now_ns();
        if (now >= next && citra_configured() && !mhgu_uds_ready()) {
            char err[96] = "";
            if (!mhgu_uds_start(xp_config.citra_room_server, xp_config.room_name, xp_config.citra_room_password,
                                xp_config.citra_token, err, sizeof err)) {
                LOG("[ldn] joining the Citra room %s failed: %s\n", xp_config.citra_room_server, err);
                next = now + 15000000000ull;
            }
        }
        plat_sleep_until_ns(plat_now_ns() + 500000000ull);
    }
    return NULL;
}
static void citra_start(void) {
    if (!citra_configured()) return;
    pthread_mutex_lock(&citra_keep_mu);
    if (!citra_keep_started) {
        citra_keep_config = xp_config;
        __atomic_store_n(&citra_wanted, 1, __ATOMIC_RELEASE);
        if (!pthread_create(&citra_keep_th, NULL, citra_keeper, NULL)) citra_keep_started = true;
        else __atomic_store_n(&citra_wanted, 0, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&citra_keep_mu);
}
static void citra_shutdown(void) {
    pthread_mutex_lock(&citra_keep_mu);
    __atomic_store_n(&citra_wanted, 0, __ATOMIC_RELEASE);
    if (citra_keep_started) {
        pthread_join(citra_keep_th, NULL);
        citra_keep_started = false;
    }
    citra_stop();
    mhgu_uds_stop();
    pthread_mutex_unlock(&citra_keep_mu);
}

static void mixed_bridge_start(void) {
    pthread_mutex_lock(&L.mu);
    if (L.ryu && L.state == ST_AP_CREATED && mhgu_room_connected() && !mhgu_xb_active())
        mhgu_xb_begin(L.info, L.own_ip, L.ryu_mask);
    pthread_mutex_unlock(&L.mu);
}
static bool ryu_configured(void) {
    return (xp_config.crossplay == XP_RYU || xp_config.crossplay == XP_MIXED) &&
           (xp_config.ryu_servers[0] || xp_config.ryu_host);
}
static bool room_configured(void) {
    return (xp_config.crossplay == XP_ROOM || xp_config.crossplay == XP_MIXED || xp_config.crossplay == XP_ALL) &&
           (xp_config.room_server[0] || xp_config.room_host);
}
static char roomsrv_self[32];
static pthread_mutex_t roomsrv_addr_mu = PTHREAD_MUTEX_INITIALIZER;
static const char *room_server(void) {
    static __thread char addr[sizeof xp_config.room_server];
    pthread_mutex_lock(&roomsrv_addr_mu);
    snprintf(addr, sizeof addr, "%s", roomsrv_self[0] ? roomsrv_self : xp_config.room_server);
    pthread_mutex_unlock(&roomsrv_addr_mu);
    return addr;
}
static bool room_enabled(void) { return room_configured() && (room_server()[0] != 0); }

static pthread_t room_keep_th;
static pthread_mutex_t room_keep_mu = PTHREAD_MUTEX_INITIALIZER;
static bool room_keep_started;
static int room_wanted;
static Config room_keep_config;
static void *room_keeper(void *arg) {
    (void)arg;
    xp_config = room_keep_config;  // keep the connection on the settings applied when it started
    uint64_t next = 0;
    while (__atomic_load_n(&room_wanted, __ATOMIC_ACQUIRE)) {
        uint64_t now = plat_now_ns();
        if (now >= next && room_enabled() && !mhgu_room_connected()) {
            uint8_t ip[4];
            char err[96];
            if (!mhgu_room_join(room_server(), xp_config.room_name, xp_config.room_password, ip, err, sizeof err)) {
                LOG("[ldn] room join failed: %s\n", err);
                next = now + 15000000000ull;
            }
        }
        mixed_bridge_start();
        plat_sleep_until_ns(plat_now_ns() + 500000000ull);
    }
    return NULL;
}
int ldn_room_state(int kind) {
    if (kind == ROOMS_CITRA) return mhgu_uds_ready() ? ROOM_IN : __atomic_load_n(&citra_wanted, __ATOMIC_ACQUIRE) ? ROOM_JOINING : ROOM_OFF;
    return mhgu_room_connected() ? ROOM_IN : __atomic_load_n(&room_wanted, __ATOMIC_ACQUIRE) ? ROOM_JOINING : ROOM_OFF;
}

static void room_start(void) {
    if (!room_enabled()) return;
    pthread_mutex_lock(&room_keep_mu);
    if (room_keep_started) { pthread_mutex_unlock(&room_keep_mu); return; }
    static bool hooked;
    if (!hooked) {
        hooked = true;
        mhgu_room_set_hooks(room_ldn_received, room_proxy_received, room_lost, room_members_changed);
    }
    room_keep_config = xp_config;
    __atomic_store_n(&room_wanted, 1, __ATOMIC_RELEASE);
    if (!pthread_create(&room_keep_th, NULL, room_keeper, NULL)) room_keep_started = true;
    else __atomic_store_n(&room_wanted, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&room_keep_mu);
}
static void room_shutdown(void) {
    pthread_mutex_lock(&room_keep_mu);
    __atomic_store_n(&room_wanted, 0, __ATOMIC_RELEASE);
    if (room_keep_started) {
        pthread_join(room_keep_th, NULL);
        room_keep_started = false;
    }
    room_stop();
    mhgu_room_leave();
    pthread_mutex_unlock(&room_keep_mu);
}

static void ryusrv_wanted(void) {
    if (!xp_config.ryu_host || mhgu_ryusrv_running()) return;
    int port = xp_config.ryu_port > 0 ? xp_config.ryu_port : 30456;
    char err[96] = "";
    if (!mhgu_ryusrv_start("0.0.0.0", (uint16_t)port, err, sizeof err)) {
        LOG("[ldn] can't run the RyuLdn server on port %d: %s\n", port, err);
        return;
    }
    char addr[32];
    snprintf(addr, sizeof addr, "127.0.0.1:%d", port);
    mhgu_ryu_set_self(addr);
    LOG("[ldn] Ryujinx players can use this PC as their LDN server (port %d)\n", port);
}
static void ryusrv_unwanted(void) {
    if (!mhgu_ryusrv_running()) return;
    mhgu_ryu_set_self(NULL);
    mhgu_ryusrv_stop();
}
static void roomsrv_wanted(void) {
    if (!xp_config.room_host || mhgu_roomsrv_running()) return;
    int port = xp_config.room_port > 0 ? xp_config.room_port : 24872;
    char err[96] = "";
    if (!mhgu_roomsrv_start(port, xp_config.room_name, xp_config.room_password, xp_config.room_name, err, sizeof err)) {
        LOG("[ldn] can't run the room server on port %d: %s\n", port, err);
        return;
    }
    pthread_mutex_lock(&roomsrv_addr_mu);
    snprintf(roomsrv_self, sizeof roomsrv_self, "127.0.0.1:%d", port);
    pthread_mutex_unlock(&roomsrv_addr_mu);
    LOG("[ldn] Yuzu-family players can join this PC's room (port %d)\n", port);
}
static void roomsrv_unwanted(void) {
    if (!mhgu_roomsrv_running()) return;
    pthread_mutex_lock(&roomsrv_addr_mu);
    roomsrv_self[0] = 0;
    pthread_mutex_unlock(&roomsrv_addr_mu);
    mhgu_roomsrv_stop();
}

// Saved settings apply before the next search or hub creation. Existing connections
// retain their transport until they leave; each reconnect reads the saved snapshot.
static void crossplay_prepare(void) {
    static Config applied;
    static bool prepared;
    static pthread_mutex_t prepare_mu = PTHREAD_MUTEX_INITIALIZER;
    crossplay_snapshot();
    pthread_mutex_lock(&prepare_mu);
    pthread_mutex_lock(&L.mu);
    bool active = L.state == ST_AP_CREATED || L.state == ST_STATION_CONNECTED;
    pthread_mutex_unlock(&L.mu);
    bool room_changed = prepared && (applied.crossplay != xp_config.crossplay ||
        applied.room_host != xp_config.room_host || applied.room_port != xp_config.room_port ||
        strcmp(applied.room_server, xp_config.room_server) || strcmp(applied.room_name, xp_config.room_name) ||
        strcmp(applied.room_password, xp_config.room_password));
    bool citra_changed = prepared && (applied.crossplay != xp_config.crossplay ||
        strcmp(applied.citra_room_server, xp_config.citra_room_server) || strcmp(applied.room_name, xp_config.room_name) ||
        strcmp(applied.citra_room_password, xp_config.citra_room_password) || strcmp(applied.citra_token, xp_config.citra_token));
    bool ryu_changed = prepared && (applied.crossplay != xp_config.crossplay ||
        applied.ryu_host != xp_config.ryu_host || applied.ryu_port != xp_config.ryu_port ||
        strcmp(applied.ryu_servers, xp_config.ryu_servers) || strcmp(applied.ryu_password, xp_config.ryu_password));
    if (!active) {
        if (room_changed) { room_shutdown(); roomsrv_unwanted(); }
        if (citra_changed) citra_shutdown();
        if (ryu_changed) ryusrv_unwanted();
        applied = xp_config;
        prepared = true;
    } else if (prepared) {
        xp_config = applied;
    }
    mhgu_ryu_set_config(xp_config.ryu_servers, "", xp_config.ryu_password);
    if (ryu_configured()) ryusrv_wanted();
    if (room_configured()) {
        roomsrv_wanted();
        room_start();
    }
    if (citra_configured()) citra_start();
    pthread_mutex_unlock(&prepare_mu);
}

// ---- host
static void push_info_locked(int except) {  // to every station (and the hub server's list): the network changed
    for (int i = 1; i < MAX_NODES; i++)
        if (i != except && L.nodes[i].alive) link_send(&L.nodes[i], F_INFO, L.info, NETINFO_SIZE, NULL, 0);
    L.lobby_seq++;
    door_sync_locked();  // (and the doors' players: Citra room, Yuzu room)
}

// A station's connection: accepted here, or opened to the hub server for a player it announced (relay_pair != 0).
typedef struct { hsock s; struct sockaddr_in from; uint32_t relay_pair; uint64_t relay_secret; } Accepted;

static void *station_thread(void *arg) {
    Accepted acc = *(Accepted *)arg;
    free(arg);
    static __thread uint8_t buf[MAX_FRAME];
    uint8_t type;
    hset_timeout(acc.s, 5000);
    int n = frame_recv(acc.s, &type, buf);
    Hello h;
    if (n < (int)sizeof h || type != F_HELLO) { hclose(acc.s); return NULL; }
    memcpy(&h, buf, sizeof h);
    if (memcmp(h.magic, PROTO_MAGIC, 4) || !link_version_ok(h.version)) {
        uint32_t r = R_CONNECT_REJECTED;
        frame_send(acc.s, F_REFUSE, &r, 4, NULL, 0);
        hclose(acc.s);
        return NULL;
    }
    pthread_mutex_lock(&L.mu);
    if (h.scan_only || L.state != ST_AP_CREATED) {
        uint8_t info[NETINFO_SIZE];
        memcpy(info, L.info, sizeof info);
        bool up = L.state == ST_AP_CREATED;
        pthread_mutex_unlock(&L.mu);
        if (up) frame_send(acc.s, F_INFO, info, sizeof info, NULL, 0);
        hclose(acc.s);
        return NULL;
    }
    uint32_t refuse = 0;
    int slot = 0;
    if (h.lcv < L.lcv) refuse = R_VERSION_TOO_LOW;
    else if (h.lcv > L.lcv) refuse = R_VERSION_TOO_HIGH;
    else if (L.accept_policy == 1) refuse = R_CONNECT_REJECTED;  // RejectAll
    else if (L.info[NI_NODE_COUNT] >= L.info[NI_NODE_MAX]) refuse = R_TOO_MANY_PLAYERS;
    else {
        for (slot = 1; slot < MAX_NODES && (L.nodes[slot].alive || L.nodes[slot].has_thread ||
                                            L.door_of[slot] != DOOR_NONE); slot++) {}
        if (slot >= MAX_NODES || slot >= L.info[NI_NODE_MAX]) refuse = R_TOO_MANY_PLAYERS;
    }
    if (refuse) {
        pthread_mutex_unlock(&L.mu);
        frame_send(acc.s, F_REFUSE, &refuse, 4, NULL, 0);
        hclose(acc.s);
        LOG("[ldn] refused a player%s (result %#x)\n", acc.relay_pair ? " through the hub server" : "", refuse);
        return NULL;
    }
    Link *l = &L.nodes[slot];
    l->s = acc.s, l->ip = L.subnet | (uint32_t)(slot + 1), l->alive = true, l->has_thread = true;
    l->th = pthread_self();
    l->token = arc4random() | 1, l->udp_up = false, l->udp_rx_ns = 0;
    l->relay_pair = acc.relay_pair, l->relay_secret = acc.relay_secret;
    l->uaddr = acc.from;  // (relayed: the server, whose UDP port is its TCP port)
    direct_reset(l, config_live_ldn_direct());
    if (l->relay_pair && L.disc_s != BADSOCK) relay_register(L.disc_s, l);
    uint8_t *nd = node_at(L.info, slot);
    memset(nd, 0, NODE_SIZE);
    wr32(nd + NODE_IP, l->ip);
    memcpy(nd + NODE_MAC, h.mac, 6);
    nd[NODE_ID] = (uint8_t)slot;
    nd[NODE_CONNECTED] = 1;
    memcpy(nd + NODE_NAME, h.name, 0x20);
    wr16(nd + NODE_LCV, h.lcv);
    L.info[NI_NODE_COUNT]++;
    uint8_t welcome[8 + NETINFO_SIZE] = {(uint8_t)slot};  // node, -, -, -, u32 UDP token, NetworkInfo
    wr32(welcome + 4, l->token);
    memcpy(welcome + 8, L.info, NETINFO_SIZE);
    link_send(l, F_WELCOME, welcome, sizeof welcome, NULL, 0);
    push_info_locked(slot);
    pthread_mutex_unlock(&L.mu);
    hset_timeout(acc.s, 0);
    LOG("[ldn] %.32s joined %s as %s (node %d)\n", h.name, acc.relay_pair ? "through the hub server" : "from the LAN / by address",
        ipstr(l->ip), slot);
    signal_change();

    while ((n = frame_recv(acc.s, &type, buf)) >= 0) {
        if (type == F_PACKET && n >= (int)sizeof(VPacket)) {
            VPacket p;
            memcpy(&p, buf, sizeof p);
            p.src_ip = l->ip;  // (a station speaks for itself only)
            pkt_count(pkt_rx, PATH_TCP);
            link_received(slot, &p, buf + sizeof p, (uint32_t)n - sizeof p);
        } else if (type == F_AUX && n > 0) {  // to the host's own game and on to the other stations
            emsync_aux_received(buf, (uint32_t)n);
            hpscale_aux_received(buf, (uint32_t)n);
            for (int i = 1; i < MAX_NODES; i++)
                if (i != slot && L.nodes[i].alive) link_send(&L.nodes[i], F_AUX, buf, (uint32_t)n, NULL, 0);
        }
    }

    pthread_mutex_lock(&L.mu);
    bool was = l->alive;
    link_close(l);
    if (was && L.state == ST_AP_CREATED) {
        memset(node_at(L.info, slot), 0, NODE_SIZE);
        if (L.info[NI_NODE_COUNT] > 1) L.info[NI_NODE_COUNT]--;
        push_info_locked(0);
    }
    l->has_thread = false;
    pthread_mutex_unlock(&L.mu);
    LOG("[ldn] node %d (%s) left\n", slot, ipstr(l->ip));
    vsock_peer_gone(l->ip);
    signal_change();
    return NULL;
}

// Accepts stations and answers LAN discovery probes until the listening sockets close.
static void *host_thread(void *arg) {
    (void)arg;
    uint64_t next_register = 0, next_report = plat_now_ns() + 10000000000ull;
    for (;;) {
        hsock ls = L.listen_s, ds = L.disc_s;
        if (ls == BADSOCK) break;
        uint64_t now = plat_now_ns();
        if (now >= next_report) {
            next_report = now + 10000000000ull;
            bool any = false;
            for (int i = 1; i < MAX_NODES; i++) any |= L.nodes[i].alive;
            if (any && rtt_enabled()) pkt_report("host");
        }
        if (ds != BADSOCK && now >= next_register) {  // relayed links: until UDP from the station gets through
            next_register = now + 1000000000ull;
            for (int i = 1; i < MAX_NODES; i++)
                if (L.nodes[i].alive && L.nodes[i].relay_pair && !udp_usable(&L.nodes[i])) relay_register(ds, &L.nodes[i]);
        }
        for (int i = 1; ds != BADSOCK && i < MAX_NODES; i++) {  // relayed links: direct UDP (hole punching)
            Link *l = &L.nodes[i];
            if (!l->alive || !l->direct_ok) continue;
            char who[16];
            snprintf(who, sizeof who, "node %d", i);
            direct_check_silence(l, now, who);
            peer_query(ds, l, now, who);
            if (l->p_known && !l->p_up && now >= l->next_punch) {  // (opens our router for the station's hellos)
                l->next_punch = now + (now - l->p_since < PUNCH_FAST_NS ? 200000000ull : 2000000000ull);
                udp_send(ds, &l->paddr, l, false, U_PUNCH, 0, NULL, NULL, 0);
            }
        }
        fd_set r;
        FD_ZERO(&r);
        FD_SET(ls, &r);
        if (ds != BADSOCK) FD_SET(ds, &r);
        struct timeval tv = {0, 100000};
        int k = select((int)(ls > ds || ds == BADSOCK ? ls : ds) + 1, &r, NULL, NULL, &tv);
        if (!L.hosting) break;
        if (k <= 0) continue;
        if (FD_ISSET(ls, &r)) {
            Accepted *a = calloc(1, sizeof *a);
            socklen_t al = sizeof a->from;
            a->s = accept(ls, (struct sockaddr *)&a->from, &al);
            if (a->s == BADSOCK) { free(a); continue; }
            int one = 1;
            setsockopt(a->s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
            pthread_t t;
            if (pthread_create(&t, NULL, station_thread, a)) { hclose(a->s); free(a); }
            else pthread_detach(t);
        }
        if (ds != BADSOCK && FD_ISSET(ds, &r)) {
            static uint8_t probe[RELAY_PREFIX + sizeof(UHdr) + sizeof(VPacket) + UDP_MAX_PAYLOAD];
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int n = recvfrom(ds, (char *)probe, sizeof probe, 0, (struct sockaddr *)&from, &fl);
            uint8_t *d = probe;  // past a direct datagram's prefix
            Link *dl = NULL;     // its link: straight from a station, with the pair id and the pair's direct key
            if (n >= RELAY_PREFIX + (int)sizeof(UHdr) && !memcmp(probe, RELAY_MAGIC, 4)) {
                for (int i = 1; i < MAX_NODES && !dl; i++) {
                    Link *l = &L.nodes[i];
                    if (l->alive && l->relay_pair && l->dkey && l->relay_pair == rd32(probe + 4) && l->dkey == rd64(probe + 8) &&
                        !same_addr(&from, &l->uaddr))
                        dl = l;
                }
                if (!dl) continue;  // (anything else on the port: dropped)
                d = probe + RELAY_PREFIX, n -= RELAY_PREFIX;
            }
            if (n >= (int)sizeof(UHdr) && !memcmp(d, UDP_MAGIC, 4)) {  // a station's datagram
                UHdr h;
                memcpy(&h, d, sizeof h);
                int slot = 1;
                while (slot < MAX_NODES && !(L.nodes[slot].alive && L.nodes[slot].token == h.token)) slot++;
                if (slot == MAX_NODES || (dl && dl != &L.nodes[slot])) continue;
                Link *l = &L.nodes[slot];
                int extra = n - (int)sizeof h;  // (a hello's clock, echoed: at most 8 bytes, never more than came)
                if (l->relay_pair && !same_addr(&from, &l->uaddr)) {  // straight from the station (hole punched)
                    if (!dl || !direct_from(l, &from)) continue;
                    if (h.kind == U_HELLO || h.kind == U_DATA) l->p_rx_ns = plat_now_ns();
                    if (h.kind == U_HELLO) {
                        bool ok = h.flags & UF_DIRECT_OK;
                        if (ok != l->p_up)
                            LOG(ok ? "[ldn] node %d: direct connection up: game packets go straight to the player\n"
                                   : "[ldn] node %d: the player gets no direct datagrams: game packets go through the hub server\n", slot);
                        l->p_up = ok;
                        udp_send(ds, &from, l, false, U_HELLO_ACK, 0, NULL, d + sizeof h, extra < 8 ? (uint32_t)extra : 8);
                    } else if (h.kind == U_DATA && n >= (int)(sizeof h + sizeof(VPacket))) {
                        if (!l->p_up) LOG("[ldn] node %d: direct connection up: game packets go straight to the player\n", slot);
                        l->p_up = true;  // (the station sends them directly only once it gets ours)
                        VPacket p;
                        memcpy(&p, d + sizeof h, sizeof p);
                        p.src_ip = l->ip;
                        pkt_count(pkt_rx, PATH_DIRECT);
                        link_received(slot, &p, d + sizeof h + sizeof p, (uint32_t)n - sizeof h - sizeof p);
                    }
                    continue;
                }
                l->uaddr = from;  // (NAT may change it: the latest wins)
                l->udp_rx_ns = plat_now_ns();
                if (h.kind == U_HELLO) {
                    if (!l->udp_up) LOG("[ldn] node %d: game packets go by UDP%s\n", slot, l->relay_pair ? " through the hub server" : "");
                    l->udp_up = true;
                    udp_send(ds, &from, l, true, U_HELLO_ACK, 0, NULL, d + sizeof h, extra < 8 ? (uint32_t)extra : 8);
                } else if (h.kind == U_DATA && n >= (int)(sizeof h + sizeof(VPacket))) {
                    VPacket p;
                    memcpy(&p, d + sizeof h, sizeof p);
                    p.src_ip = l->ip;
                    pkt_count(pkt_rx, PATH_UDP);
                    link_received(slot, &p, d + sizeof h + sizeof p, (uint32_t)n - sizeof h - sizeof p);
                }
                continue;
            }
            if (n == PEER_MSG_SIZE && !memcmp(probe, PEER_ANSWER, 4)) {  // the hub server: a station's address
                for (int i = 1; i < MAX_NODES; i++) {
                    Link *l = &L.nodes[i];
                    char who[16];
                    snprintf(who, sizeof who, "node %d", i);
                    if (l->alive && l->relay_pair && same_addr(&from, &l->uaddr) && peer_answer(l, probe, n, who)) break;
                }
                continue;
            }
            if (n >= 6 && !memcmp(probe, PROTO_MAGIC, 4) && link_version_ok(rd16(probe + 4))) {
                uint8_t reply[8 + NETINFO_SIZE];
                memcpy(reply, PROTO_MAGIC, 4);
                wr16(reply + 4, rd16(probe + 4));  // (the asker's version)
                wr16(reply + 6, (uint16_t)g_config.ldn_port);
                pthread_mutex_lock(&L.mu);
                memcpy(reply + 8, L.info, NETINFO_SIZE);
                pthread_mutex_unlock(&L.mu);
                sendto(ds, (const char *)reply, sizeof reply, 0, (struct sockaddr *)&from, fl);
            }
        }
    }
    return NULL;
}

// ---- the hub server (internet hubs)
// ini Server: empty = the default server (LDN_DEFAULT_SERVER, config.h), "Off" = none. It can also be a list of up to
// MAX_HUBS servers, separated by commas or spaces: the hubs you create are listed on all of them, a search asks all of
// them (and the LAN) and a hub is joined through the server that listed it. That is how two communities' servers are
// "stitched" together for you. A name that starts with '-' is switched off but kept in the list. A web address is
// accepted too: "http://name/" is the server "name". Server and Key are live settings (config_live_ldn): a change
// applies from the next search, join or connection to a server on.
#define LOBBY_NAME sizeof g_config.ldn_server
#define MAX_HUBS 8
// One name as typed -> the server's name (lower case: names are compared as they are); false: nothing in it
static bool lobby_clean(const char *s, size_t n, char *out) {
    char t[LOBBY_NAME];
    if (n >= sizeof t) n = sizeof t - 1;
    memcpy(t, s, n);
    t[n] = 0;
    char *p = t;
    if (!strncmp(p, "http://", 7) || !strncmp(p, "HTTP://", 7)) p += 7;
    else if (!strncmp(p, "https://", 8) || !strncmp(p, "HTTPS://", 8)) p += 8;
    p[strcspn(p, "/?")] = 0;  // (a web address's path is not part of the server)
    size_t k = 0;
    for (; p[k]; k++) out[k] = (p[k] >= 'A' && p[k] <= 'Z') ? (char)(p[k] + 32) : p[k];
    out[k] = 0;
    return k > 0;
}
// The servers named in an ini Server text, into out; how many (0: no internet hubs)
static int lobby_servers_of(const char *s, char out[MAX_HUBS][LOBBY_NAME]) {
    if ((s[0] | 0x20) == 'o' && (s[1] | 0x20) == 'f' && (s[2] | 0x20) == 'f' && !s[3]) return 0;
    if (!*s) s = LDN_DEFAULT_SERVER;
    int n = 0;
    while (*s && n < MAX_HUBS) {
        while (*s == ' ' || *s == ',' || *s == '\t') s++;
        const char *e = s;
        while (*e && *e != ' ' && *e != ',' && *e != '\t') e++;
        char name[LOBBY_NAME];
        if (e > s && *s != '-' && lobby_clean(s, (size_t)(e - s), name)) {
            bool dup = false;
            for (int i = 0; i < n; i++) dup |= !strcmp(out[i], name);
            if (!dup) memcpy(out[n++], name, sizeof name);
        }
        s = e;
    }
    return n;
}
static int lobby_servers(char out[MAX_HUBS][LOBBY_NAME]) {
    char s[LOBBY_NAME];
    config_live_ldn(s, sizeof s, NULL, 0);
    return lobby_servers_of(s, out);
}

// A request's header. The room is a hash of the matchmaking key (key NULL: ini Key; spaces around it ignored): hubs
// are listed only for players whose key matches, all of them without one. The server never sees the key itself.
static bool lobby_header(uint8_t *h, const char *key, uint16_t ver) {
    static const char salt[] = "MHGURecomp matchmaking key:";
    char live[sizeof g_config.ldn_key];
    if (!key) config_live_ldn(NULL, 0, live, sizeof live), key = live;
    memcpy(h, "MHGS", 4);
    wr16(h + 4, LOBBY_PROTO);
    wr16(h + 6, ver);
    memset(h + 8, 0, 16);
    const char *k = key;
    while (*k == ' ') k++;
    size_t n = strlen(k);
    while (n && k[n - 1] == ' ') n--;
    if (!n) return false;
    Sha256 s;
    char hex[65];
    sha256_init(&s);
    sha256_update(&s, salt, sizeof salt - 1);
    sha256_update(&s, k, n);
    sha256_hex(&s, hex);
    for (int i = 0; i < 16; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        h[8 + i] = (uint8_t)v;
    }
    return true;
}

static const char *lobby_error(uint32_t code) {
    switch (code) {
    case SE_NOT_FOUND: return "the hub is gone";
    case SE_NO_ANSWER: return "the host did not answer";
    case SE_BUSY: return "the server is busy (too many hubs or requests from this address)";
    case SE_VERSION: return "the server speaks another protocol version";
    default: return "refused";
    }
}
static int lobby_state(uint32_t code) {  // a server's error as a HUB_* state (net.h)
    return code == SE_VERSION ? HUB_VERSION : code == SE_BUSY ? HUB_BUSY : HUB_REFUSED;
}

// What the player sees of the hub server: its last known state, for the settings menu (ldn_hub_status), and a notice
// on screen (overlay_notice) when it fails: hosting (the hub is not listed online), searching (only the LAN's hubs
// are found) or joining. Hosting notices come when the state changes; a search's every time, but not twice within
// 10 s. Texts in UI_TEXT are translated as the settings menu's (settings_locale.h,
// tests/test_settings_locale.py).
#define UI_TEXT(s) s
enum { NOTE_HOST, NOTE_SEARCH, NOTE_JOIN };
static pthread_mutex_t hub_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { int state, ms, hubs; char server[LOBBY_NAME], key[sizeof g_config.ldn_key]; uint32_t check; } hub_now;
static int note_last[3];  // HUB_*: HUB_UNKNOWN and HUB_ONLINE are fine
static uint64_t note_at[3];

static void hub_set_locked(const char *srv, const char *key, int state, int ms, int hubs) {
    hub_now.state = state, hub_now.ms = ms, hub_now.hubs = hubs;
    snprintf(hub_now.server, sizeof hub_now.server, "%s", srv);
    snprintf(hub_now.key, sizeof hub_now.key, "%s", key);
}

static void hub_report(int what, const char *srv, int state, int ms, int hubs) {
    char key[sizeof g_config.ldn_key];
    config_live_ldn(NULL, 0, key, sizeof key);
    uint64_t now = plat_now_ns();
    pthread_mutex_lock(&hub_mu);
    if (hub_now.state != HUB_CHECKING && what != NOTE_JOIN) hub_set_locked(srv, key, state, ms, hubs);
    bool show = state == HUB_ONLINE ? what == NOTE_HOST && note_last[what] > HUB_ONLINE
                                    : what == NOTE_JOIN || note_last[what] != state || now - note_at[what] > 10000000000ull;
    note_last[what] = state;
    if (show) note_at[what] = now;
    pthread_mutex_unlock(&hub_mu);
    if (!show) return;
    const char *title;
    switch (state) {
    case HUB_ONLINE: title = UI_TEXT("Connected to the hub server %s"); break;
    case HUB_NOT_FOUND: title = UI_TEXT("Hub server %s not found"); break;
    case HUB_UNREACHABLE: title = UI_TEXT("Can't reach the hub server %s"); break;
    case HUB_LOST: title = UI_TEXT("Lost the connection to the hub server %s"); break;
    case HUB_BUSY: title = UI_TEXT("The hub server %s is busy"); break;
    case HUB_VERSION: title = UI_TEXT("The hub server %s runs another version"); break;
    case HUB_REFUSED: title = UI_TEXT("The hub server %s refused the request"); break;
    default: title = UI_TEXT("The hub server %s did not answer"); break;
    }
    static const char *const next[] = {UI_TEXT("Your hub is not listed online. Trying again..."),
                                       UI_TEXT("Only hubs on your local network are shown."),
                                       UI_TEXT("Could not join the hub.")};
    int lang = config_language();
    char line[LOBBY_NAME + 128];
    snprintf(line, sizeof line, settings_text(lang, title), srv);
    const char *text = state == HUB_ONLINE ? UI_TEXT("Your hub is listed online again.") : next[what];
    if (what == NOTE_SEARCH && g_config.ldn_join[0]) text = UI_TEXT("Only hubs on your local network and Join addresses are shown.");
    overlay_notice(line, settings_text(lang, text));
}

// Host: one thread per created network keeps it listed and opens a connection for each player the server announces.
// Threads of a network that is gone (lobby_gen moved on) end by themselves; nobody waits for them.
static volatile uint32_t lobby_gen;
static volatile int lobby_up[MAX_HUBS];  // how many link versions of a server's slot are listed now (a failure is told only when none is)
typedef struct { uint32_t gen; struct sockaddr_in addr; uint64_t ticket; uint16_t ver; bool phone; } LobbyTicket;
typedef struct { uint32_t gen; int slot; uint16_t ver; bool phone; } LobbyArg;  // (phone: listed in the phones' public room too)
static bool live_key_empty(void) {
    char k[sizeof g_config.ldn_key];
    config_live_ldn(NULL, 0, k, sizeof k);
    for (const char *c = k; *c; c++)
        if (*c != ' ') return false;
    return true;
}  // (a thread per server of the list: the slot is its place in it)

static bool lobby_wait(uint32_t gen, int ms) {  // false: the network is gone
    for (uint64_t end = plat_now_ns() + (uint64_t)ms * 1000000ull; plat_now_ns() < end && lobby_gen == gen;)
        plat_sleep_until_ns(plat_now_ns() + 100000000ull);
    return lobby_gen == gen;
}

static void *lobby_attach_thread(void *arg) {
    LobbyTicket t = *(LobbyTicket *)arg;
    free(arg);
    static __thread uint8_t buf[MAX_FRAME];
    hsock s = connect_to(&t.addr, 3000);
    if (s == BADSOCK) return NULL;
    uint8_t req[LOBBY_HEADER + 8], type;
    lobby_header(req, t.phone ? PHONE_PUBLIC_KEY : NULL, t.ver);
    memcpy(req + LOBBY_HEADER, &t.ticket, 8);
    hset_timeout(s, 5000);
    int n;
    if (!frame_send(s, S_ATTACH, req, sizeof req, NULL, 0) || (n = frame_recv(s, &type, buf)) < 12 || type != S_SPLICED ||
        lobby_gen != t.gen) {
        LOG("[ldn] a player's connection through the hub server failed\n");
        hclose(s);
        return NULL;
    }
    Accepted *a = calloc(1, sizeof *a);
    a->s = s, a->from = t.addr, a->relay_pair = rd32(buf), a->relay_secret = rd64(buf + 4);
    return station_thread(a);
}

static void *lobby_thread(void *arg) {
    LobbyArg la = *(LobbyArg *)arg;
    free(arg);
    uint32_t gen = la.gen;
    static __thread uint8_t buf[MAX_FRAME];
    uint8_t req[LOBBY_HEADER + NETINFO_SIZE], type;
    while (lobby_gen == gen) {
        char srv[LOBBY_NAME];
        struct sockaddr_in addr;
        hsock s = BADSOCK;
        char list[MAX_HUBS][LOBBY_NAME];
        if (lobby_servers(list) <= la.slot) break;  // (taken out of the list: this thread is done)
        if (la.phone && !live_key_empty()) {  // (the phones' public room is for a hub without a key of its own)
            if (!lobby_wait(gen, 5000)) break;
            continue;
        }
        snprintf(srv, sizeof srv, "%s", list[la.slot]);
        bool named = resolve_host(srv, LOBBY_PORT, &addr);
        if (named) s = connect_to(&addr, 3000);
        if (s == BADSOCK) {
            LOG("[ldn] hub server %s %s: this hub is not listed online (v%u, trying again in 30 s)\n", srv,
                named ? "unreachable" : "not found", (unsigned)la.ver);
            if (lobby_gen == gen && !lobby_up[la.slot]) hub_report(NOTE_HOST, srv, named ? HUB_UNREACHABLE : HUB_NOT_FOUND, -1, -1);
            if (!lobby_wait(gen, 30000)) break;
            continue;
        }
        bool keyed = lobby_header(req, la.phone ? PHONE_PUBLIC_KEY : NULL, la.ver);
        pthread_mutex_lock(&L.mu);
        memcpy(req + LOBBY_HEADER, L.info, NETINFO_SIZE);
        uint32_t seen = L.lobby_seq;
        pthread_mutex_unlock(&L.mu);
        hset_timeout(s, 5000);
        int n = -1;
        uint64_t asked = plat_now_ns();
        if (frame_send(s, S_HOST, req, sizeof req, NULL, 0)) n = frame_recv(s, &type, buf);
        if (n < 8 || type != S_HOSTED) {
            bool error = n >= 4 && type == S_ERROR;
            LOG("[ldn] hub server %s: can't list this hub as v%u%s (%s); trying again in 30 s\n", srv, (unsigned)la.ver, la.phone ? " in the phones' room" : "",
                error ? lobby_error(rd32(buf)) : "no answer");
            hclose(s);
            if (lobby_gen == gen && !lobby_up[la.slot]) hub_report(NOTE_HOST, srv, error ? lobby_state(rd32(buf)) : HUB_NO_ANSWER, -1, -1);
            if (!lobby_wait(gen, 30000)) break;
            continue;
        }
        LOG("[ldn] hub listed on the hub server %s (link v%u%s): %s\n", srv, (unsigned)la.ver, la.phone ? ", phones' room" : "",
            keyed ? "players with the same matchmaking key see it" : "public, every player sees it");
        if (lobby_gen == gen) __sync_fetch_and_add(&lobby_up[la.slot], 1);
        hub_report(NOTE_HOST, srv, HUB_ONLINE, (int)((plat_now_ns() - asked) / 1000000), -1);
        uint64_t now = plat_now_ns(), last_rx = now, next_ping = now + LOBBY_PING_NS;
        while (lobby_gen == gen) {
            now = plat_now_ns();
            if (L.lobby_seq != seen) {
                uint8_t info[NETINFO_SIZE];
                pthread_mutex_lock(&L.mu);
                memcpy(info, L.info, NETINFO_SIZE);
                seen = L.lobby_seq;
                pthread_mutex_unlock(&L.mu);
                if (!frame_send(s, S_UPDATE, info, NETINFO_SIZE, NULL, 0)) break;
            }
            if (now >= next_ping) {
                if (!frame_send(s, S_PING, NULL, 0, NULL, 0)) break;
                next_ping = now + LOBBY_PING_NS;
            }
            if (now - last_rx > 3 * LOBBY_PING_NS) break;
            fd_set r;
            FD_ZERO(&r);
            FD_SET(s, &r);
            struct timeval tv = {0, 200000};
            if (select((int)s + 1, &r, NULL, NULL, &tv) <= 0) continue;
            if ((n = frame_recv(s, &type, buf)) < 0) break;
            last_rx = plat_now_ns();
            if (type == S_INCOMING && n >= 8) {
                LobbyTicket *t = malloc(sizeof *t);
                pthread_t th;
                t->gen = gen, t->addr = addr, t->ticket = rd64(buf), t->ver = la.ver, t->phone = la.phone;
                if (pthread_create(&th, NULL, lobby_attach_thread, t)) free(t);
                else pthread_detach(th);
            }
        }
        hclose(s);
        if (lobby_gen != gen) break;
        __sync_fetch_and_sub(&lobby_up[la.slot], 1);
        LOG("[ldn] lost the hub server (v%u): this hub is not listed online (trying again in 10 s)\n", (unsigned)la.ver);
        if (!lobby_up[la.slot]) hub_report(NOTE_HOST, srv, HUB_LOST, -1, -1);
        if (!lobby_wait(gen, 10000)) break;
    }
    return NULL;
}

static void lobby_host_start(void) {
    uint32_t gen = ++lobby_gen;
    char list[MAX_HUBS][LOBBY_NAME];
    pthread_mutex_lock(&hub_mu);
    note_last[NOTE_HOST] = HUB_ONLINE;  // (each hub tells of its own first failure)
    pthread_mutex_unlock(&hub_mu);
    int n = lobby_servers(list);
    for (int i = 0; i < MAX_HUBS; i++) lobby_up[i] = 0;
    for (int i = 0; i < n; i++) {  // (listed on every server of the list, under each link version)
        // (without a key of our own: the public room under each version - gu-native 0.1.6 asks v2, the PC build v3 - and
        // the phones' public room under v3, where the MHGURecomp Android build looks: PCs and phones of both builds see it)
        for (int v = 0; v < N_LINK_VERSIONS + 1; v++) {
            pthread_t th;
            LobbyArg *a = malloc(sizeof *a);
            if (!a) continue;
            a->gen = gen, a->slot = i, a->ver = LINK_VERSIONS[v < N_LINK_VERSIONS ? v : 1], a->phone = v == N_LINK_VERSIONS;
            if (pthread_create(&th, NULL, lobby_thread, a)) free(a);
            else pthread_detach(th);
        }
    }
}
static void lobby_host_stop(void) { lobby_gen++; }

// Station: the hubs the server lists for a key (NULL: ini Key). A HUB_* state; ms: the request's round trip.
#define LOBBY_LIST_MAX 512
typedef struct {
    uint8_t infos[LOBBY_LIST_MAX][NETINFO_SIZE];
    uint64_t ids[LOBBY_LIST_MAX];
    uint16_t vers[LOBBY_LIST_MAX];  // the link version each hub was listed under
    uint8_t phone[LOBBY_LIST_MAX];  // 1: listed in the phones' public room (key PHONE_PUBLIC_KEY)
    int n, state, ms;
    struct sockaddr_in addr;
    char server[LOBBY_NAME];
} LobbyList;

// One request: the hubs of one room and link version, appended to r (r->n is not reset: lobby_list_all does that)
static int lobby_list(const char *srv, const char *key, uint16_t ver, int connect_ms, LobbyList *r) {
    static __thread uint8_t buf[MAX_FRAME];
    r->ms = -1;
    if (!resolve_host(srv, LOBBY_PORT, &r->addr)) return HUB_NOT_FOUND;
    hsock s = connect_to(&r->addr, connect_ms);
    if (s == BADSOCK) return HUB_UNREACHABLE;
    uint8_t req[LOBBY_HEADER], type;
    lobby_header(req, key, ver);
    hset_timeout(s, 2000);
    uint64_t asked = plat_now_ns();
    int n = frame_send(s, S_LIST, req, sizeof req, NULL, 0) ? frame_recv(s, &type, buf) : -1;
    r->ms = (int)((plat_now_ns() - asked) / 1000000);
    hclose(s);
    if (n >= 4 && type == S_ERROR) return lobby_state(rd32(buf));
    if (n < 4 || type != S_HUBS) return HUB_NO_ANSWER;
    uint32_t count = rd32(buf);
    for (uint32_t i = 0; i < count && r->n < LOBBY_LIST_MAX && 4 + (i + 1) * (8 + NETINFO_SIZE) <= (uint32_t)n; i++) {
        const uint8_t *e = buf + 4 + i * (8 + NETINFO_SIZE);
        bool dup = false;  // (a hub listed under several link versions / rooms arrives once per listing: keep the first)
        for (int j = 0; j < r->n && !dup; j++) dup = !memcmp(r->infos[j] + NI_BSSID, e + 8 + NI_BSSID, 6);
        if (dup) continue;
        r->ids[r->n] = rd64(e);
        r->vers[r->n] = ver;
        r->phone[r->n] = key && !strcmp(key, PHONE_PUBLIC_KEY);
        memcpy(r->infos[r->n++], e + 8, NETINFO_SIZE);
    }
    return HUB_ONLINE;
}

// Every link version we speak, and (no key of our own) the phones' public room too (v3 only). Online if any of them answered; the
// state of a failure is the most telling one (a server that does not serve a version says so, but that is not "down").
#define FULL_ROUNDS 6  // the most times FullHubList asks one server for its hubs
static int lobby_list_all(const char *srv, const char *key, int connect_ms, LobbyList *r) {
    char live[sizeof g_config.ldn_key];
    if (!key) config_live_ldn(NULL, 0, live, sizeof live), key = live;
    const char *keys[2] = {key, PHONE_PUBLIC_KEY};
    bool keyless = true;
    for (const char *k = key; *k; k++) keyless &= *k == ' ';
    int nk = keyless ? 2 : 1, best = HUB_UNKNOWN, ms = -1;
    bool any = false;
    r->n = 0;
    // ini FullHubList: a server may answer each request with only a sample of its hubs, so every room / version is asked
    // again (lobby_list keeps each hub once) until a whole round brings nothing new, at most FULL_ROUNDS times. A server
    // that answers with all its hubs ends after the second round, so the cost is one extra round of quick requests.
    int rounds = config_live_hub_full_list() ? FULL_ROUNDS : 1;
    for (int round = 0; round < rounds; round++) {
        int before = r->n;
        for (int ki = 0; ki < nk; ki++) {
            for (int v = 0; v < N_LINK_VERSIONS; v++) {
                if (ki == 1 && LINK_VERSIONS[v] < 3) continue;  // (nobody lists in the phones' room under v2)
                int st = lobby_list(srv, keys[ki], LINK_VERSIONS[v], connect_ms, r);
                if (st == HUB_NOT_FOUND || st == HUB_UNREACHABLE) {
                    if (!any) best = st, ms = -1;
                    r->ms = any ? ms : -1;
                    return any ? HUB_ONLINE : st;  // (no point asking again)
                }
                if (st == HUB_ONLINE) {
                    if (!any || r->ms < ms) ms = r->ms;
                    any = true;
                } else if (!any && (best == HUB_UNKNOWN || best == HUB_NO_ANSWER)) {
                    best = st;  // (a refusal says more than silence)
                }
            }
        }
        if (!any || (round > 0 && r->n == before) || r->n >= LOBBY_LIST_MAX) break;  // (nothing new: that is all of them)
    }
    r->ms = ms;
    return any ? HUB_ONLINE : best == HUB_UNKNOWN ? HUB_NO_ANSWER : best;
}

static void *lobby_list_thread(void *arg) {  // (while the LAN is searched; r->server set)
    LobbyList *r = arg;
    static int last = HUB_ONLINE;
    r->state = lobby_list_all(r->server, NULL, 1500, r);
    if (r->state != last && r->state != HUB_ONLINE)
        LOG("[ldn] hub server %s: %s: only LAN / Join hubs are searched\n", r->server,
            r->state == HUB_NOT_FOUND ? "not found" : r->state == HUB_UNREACHABLE ? "unreachable" : "no list");
    last = r->state;
    hub_report(NOTE_SEARCH, r->server, r->state, r->ms, r->state == HUB_ONLINE ? r->n : -1);
    return NULL;
}

// The settings menu's check (net.h): the server and key being edited, in a thread of its own.
typedef struct { uint32_t check; char server[LOBBY_NAME], key[sizeof g_config.ldn_key]; } HubCheck;

static void *hub_check_thread(void *arg) {
    HubCheck *c = arg;
    char list[MAX_HUBS][LOBBY_NAME];
    int ns = lobby_servers_of(c->server, list);
    LobbyList *r = malloc(sizeof *r);
    // every server of the list is asked: online if any answers (the quickest one's time, all their hubs counted)
    int state = HUB_UNKNOWN, bad = HUB_UNKNOWN, ms = -1, hubs = 0, up = 0;
    const char *shown = ns ? list[0] : c->server;
    for (int i = 0; i < ns && r; i++) {
        int st = lobby_list_all(list[i], c->key, 3000, r);
        LOG("[ldn] hub server check: %s: state %d, %d ms, %d hub(s) for this key\n", list[i], st, r->ms,
            st == HUB_ONLINE ? r->n : -1);
        if (st == HUB_ONLINE) {
            if (!up || r->ms < ms) ms = r->ms, shown = list[i];
            up++, hubs += r->n;
        } else if (bad == HUB_UNKNOWN) {
            bad = st;
        }
    }
    state = up ? HUB_ONLINE : bad;
    pthread_mutex_lock(&hub_mu);
    if (hub_now.check == c->check) hub_set_locked(shown, c->key, state, up ? ms : -1, up ? hubs : -1);
    pthread_mutex_unlock(&hub_mu);
    free(r);
    free(c);
    return NULL;
}

void ldn_hub_check(const char *server, const char *key) {
    HubCheck *c = calloc(1, sizeof *c);
    pthread_t th;
    if (!c) return;
    hnet_init();
    char list[MAX_HUBS][LOBBY_NAME];
    bool on = lobby_servers_of(server, list) > 0;
    snprintf(c->server, sizeof c->server, "%s", server);  // (the whole text: the thread asks each server in it)
    snprintf(c->key, sizeof c->key, "%s", key);
    pthread_mutex_lock(&hub_mu);
    c->check = ++hub_now.check;
    hub_set_locked(on ? list[0] : c->server, c->key, on ? HUB_CHECKING : HUB_OFF, -1, -1);
    pthread_mutex_unlock(&hub_mu);
    if (on && !pthread_create(&th, NULL, hub_check_thread, c)) {
        pthread_detach(th);
        return;
    }
    if (on) {
        pthread_mutex_lock(&hub_mu);
        hub_set_locked(list[0], c->key, HUB_UNKNOWN, -1, -1);
        pthread_mutex_unlock(&hub_mu);
    }
    free(c);
}

int ldn_hub_status(const char *server, const char *key, int *ms, int *hubs) {
    char list[MAX_HUBS][LOBBY_NAME];
    int n = lobby_servers_of(server, list);
    if (!n) return HUB_OFF;
    pthread_mutex_lock(&hub_mu);
    bool known = false;  // (the last state belongs to one of the servers in the list)
    for (int i = 0; i < n; i++) known |= !strcmp(hub_now.server, list[i]);
    // (the server's state holds whatever the key; the hub count only for the key it was for)
    int state = known ? hub_now.state : HUB_UNKNOWN;
    if (ms) *ms = hub_now.ms;
    if (hubs) *hubs = strcmp(hub_now.key, key) ? -1 : hub_now.hubs;
    pthread_mutex_unlock(&hub_mu);
    return state;
}

// Joining a listed hub: the server asks its host to connect and splices the two. 0, else an nn::ldn result.
static uint32_t lobby_join(const struct sockaddr_in *addr, const char *server, const char *key, uint16_t ver, uint64_t hub, hsock *out, uint32_t *pair, uint64_t *secret) {
    static uint8_t buf[MAX_FRAME];
    char srv[LOBBY_NAME];  // (the server that listed the hub: the one of the list it was found on)
    snprintf(srv, sizeof srv, "%s", *server ? server : inet_ntoa(addr->sin_addr));
    hsock s = connect_to(addr, 3000);
    if (s == BADSOCK) {
        LOG("[ldn] can't reach the hub server\n");
        hub_report(NOTE_JOIN, srv, HUB_UNREACHABLE, -1, -1);
        return R_CONNECT_TIMEOUT;
    }
    uint8_t req[LOBBY_HEADER + 8], type;
    lobby_header(req, *key ? key : NULL, ver);  // (the room the hub was listed in: not always our own key)
    memcpy(req + LOBBY_HEADER, &hub, 8);
    hset_timeout(s, 12000);  // (the server waits up to 10 s for the host)
    int n = frame_send(s, S_JOIN, req, sizeof req, NULL, 0) ? frame_recv(s, &type, buf) : -1;
    if (n >= 12 && type == S_SPLICED) {
        *out = s, *pair = rd32(buf), *secret = rd64(buf + 4);
        return 0;
    }
    hclose(s);
    uint32_t code = n >= 4 && type == S_ERROR ? rd32(buf) : 0;
    LOG("[ldn] joining through the hub server failed: %s\n", code ? lobby_error(code) : "no answer");
    if (code == SE_NO_ANSWER) {  // (a hub that is gone: the game says so itself)
        int lang = config_language();
        overlay_notice(settings_text(lang, UI_TEXT("Could not join the hub.")),
                       settings_text(lang, UI_TEXT("The host did not answer through the hub server.")));
    } else if (code != SE_NOT_FOUND) {
        hub_report(NOTE_JOIN, srv, code ? lobby_state(code) : HUB_NO_ANSWER, -1, -1);
    }
    return code == SE_NOT_FOUND ? R_CONNECT_NOT_FOUND : code == SE_BUSY ? R_CONNECT_REJECTED : R_CONNECT_TIMEOUT;
}

static bool host_start(void) {
    hnet_init();
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)g_config.ldn_port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    hsock ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    bool bound = ls != BADSOCK && !bind(ls, (struct sockaddr *)&a, sizeof a);
    bool any_port = false;  // the port is taken: still hosting through the hub server, on a port of the system's choice
    char srv[MAX_HUBS][LOBBY_NAME];
    bool online = lobby_servers(srv) > 0;
    if (ls != BADSOCK && !bound && online) {
        a.sin_port = 0;
        any_port = bound = !bind(ls, (struct sockaddr *)&a, sizeof a);
    }
    if (!bound || listen(ls, 8)) {
        LOG("[ldn] can't listen on TCP port %d (in use by another program?): nobody can join this hub\n", g_config.ldn_port);
        if (ls != BADSOCK) hclose(ls);
        return false;
    }
    hsock ds = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ds != BADSOCK && bind(ds, (struct sockaddr *)&a, sizeof a)) {
        a.sin_port = 0;  // (for the hub server's relay; LAN search needs the port)
        if (!online || bind(ds, (struct sockaddr *)&a, sizeof a)) { hclose(ds); ds = BADSOCK; }
        if (!any_port) LOG("[ldn] UDP port %d is in use: no LAN search (only Join addresses)\n", g_config.ldn_port);
    }
    if (ds != BADSOCK) udp_no_connreset(ds);  // (direct UDP: a punch that meets a closed router is no error here)
    L.listen_s = ls, L.disc_s = ds, L.hosting = true;
    pthread_create(&L.host_th, NULL, host_thread, NULL);
    if (any_port) LOG("[ldn] port %d is in use: hosting only through the hub server\n", g_config.ldn_port);
    else LOG("[ldn] hosting: players join this computer's address, port %d (TCP, and UDP for LAN search and game packets)\n",
             g_config.ldn_port);
    lobby_host_start();
    return true;
}

static void host_stop(uint32_t reason) {  // (caller: not holding L.mu)
    if (L.yuzu) { mhgu_ydoor_stop(); L.yuzu = false; }
    memset(L.door_of, 0, sizeof L.door_of);
    lobby_host_stop();
    if (L.hosting) {
        L.hosting = false;
        pthread_join(L.host_th, NULL);
        hclose(L.listen_s);
        if (L.disc_s != BADSOCK) hclose(L.disc_s);
        L.listen_s = L.disc_s = BADSOCK;
    }
    for (int i = 1; i < MAX_NODES; i++) {
        Link *l = &L.nodes[i];
        if (l->alive) link_send(l, F_DESTROY, &reason, 4, NULL, 0);
        link_close(l);
    }
    for (int i = 1; i < MAX_NODES; i++)  // (their threads see the closed sockets and end)
        for (int t = 0; t < 200 && L.nodes[i].has_thread; t++) plat_sleep_until_ns(plat_now_ns() + 5000000);
}

// ---- station
static void *uplink_thread(void *arg) {
    (void)arg;
    static __thread uint8_t buf[MAX_FRAME];
    uint8_t type;
    int n, reason = DR_SIGNAL_LOST;
    while ((n = frame_recv(L.up.s, &type, buf)) >= 0) {
        if (type == F_PACKET && n >= (int)sizeof(VPacket)) {
            VPacket p;
            memcpy(&p, buf, sizeof p);
            pkt_count(pkt_rx, PATH_TCP);
            link_received(0, &p, buf + sizeof p, (uint32_t)n - sizeof p);
        } else if (type == F_INFO && n >= NETINFO_SIZE && info_sane(buf)) {
            pthread_mutex_lock(&L.mu);
            memcpy(L.info, buf, NETINFO_SIZE);
            pthread_mutex_unlock(&L.mu);
            signal_change();
        } else if (type == F_DESTROY && n >= 4) {
            reason = (int)rd32(buf);
        } else if (type == F_AUX && n > 0) {
            emsync_aux_received(buf, (uint32_t)n);
            hpscale_aux_received(buf, (uint32_t)n);
        }
    }
    L.uth_run = false;
    pthread_mutex_lock(&L.mu);
    bool lost = L.up.alive;  // (else: we disconnected ourselves)
    link_close(&L.up);
    if (lost && L.state == ST_STATION_CONNECTED) {
        L.state = ST_ERROR, L.reason = reason;
        LOG("[ldn] connection to the host ended (reason %d)\n", reason);
    }
    pthread_mutex_unlock(&L.mu);
    if (lost) { vsock_network_down(); signal_change(); }
    return NULL;
}

// The station's datagrams: hello (every 2 s once answered, else every 0.5 s) and the host's packets.
static void *station_udp_thread(void *arg) {
    (void)arg;
    static __thread uint8_t buf[RELAY_PREFIX + sizeof(UHdr) + sizeof(VPacket) + UDP_MAX_PAYLOAD];
    hsock us = L.us;
    Link *l = &L.up;
    bool relayed = l->relay_pair != 0;
    uint64_t next_hello = 0, next_dhello = 0;
    hset_timeout(us, 100);
    while (L.uth_run) {
        uint64_t now = plat_now_ns();
        if (now >= next_hello) {  // (also through the server while direct works: the fallback stays ready)
            udp_send(us, &l->uaddr, l, true, U_HELLO, 0, NULL, &now, 8);
            next_hello = now + (l->udp_up ? hello_every() : 500000000ull);
        }
        if (l->direct_ok) {
            direct_check_silence(l, now, "station");
            peer_query(us, l, now, "station");
            if (l->p_known && now >= next_dhello) {
                udp_send(us, &l->paddr, l, false, U_HELLO, l->p_up ? UF_DIRECT_OK : 0, NULL, &now, 8);
                next_dhello = now + (l->p_up ? (rtt_enabled() ? hello_every() : DIRECT_HELLO_NS)
                                             : now - l->p_since < PUNCH_FAST_NS ? 200000000ull : 2000000000ull);
            }
        }
        rtt_report(relayed, now);
        if (l->udp_up && now - l->udp_rx_ns > UDP_SILENCE_NS) {
            l->udp_up = false;
            LOG("[ldn] nothing by UDP from the host for 15 s: game packets go by TCP\n");
        }
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(us, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) { plat_sleep_until_ns(plat_now_ns() + 5000000); continue; }  // (timeout)
        bool direct = false;
        uint8_t *d = buf;  // (past a direct datagram's prefix)
        if (!same_addr(&from, &l->uaddr)) {  // straight from the host: its pair id and the pair's direct key, else dropped
            if (!relayed || !l->dkey || n < RELAY_PREFIX || memcmp(buf, RELAY_MAGIC, 4) || rd32(buf + 4) != l->relay_pair ||
                rd64(buf + 8) != l->dkey || !direct_from(l, &from))
                continue;
            direct = true;
            d = buf + RELAY_PREFIX, n -= RELAY_PREFIX;
        } else if (relayed && peer_answer(l, buf, n, "station")) {
            next_dhello = 0;
            continue;
        }
        UHdr h;
        if (n < (int)sizeof h || memcmp(d, UDP_MAGIC, 4)) continue;
        memcpy(&h, d, sizeof h);
        if (h.token != l->token) continue;
        if (direct) {
            if (h.kind == U_HELLO_ACK) {
                rtt_add(1, d + sizeof h, n - (int)sizeof h);
                l->p_rx_ns = plat_now_ns();
                if (!l->p_up && ++l->p_good >= (l->p_ever ? DIRECT_REUP : 1)) {  // (after a fall: a few answers first)
                    l->p_up = true;
                    next_dhello = 0;  // (tells the host at once)
                    LOG("[ldn] direct connection to the host %s: game packets go straight to it, not through the hub server\n",
                        l->p_ever ? "back" : "up");
                    l->p_ever = true;
                }
            } else if (h.kind == U_PUNCH) {
                if (!l->p_up) next_dhello = 0;  // (the host's way is open: say hello now)
            } else if (h.kind == U_DATA && n >= (int)(sizeof h + sizeof(VPacket))) {
                VPacket p;
                memcpy(&p, d + sizeof h, sizeof p);
                pkt_count(pkt_rx, PATH_DIRECT);
                link_received(0, &p, d + sizeof h + sizeof p, (uint32_t)n - sizeof h - sizeof p);
            }
            continue;
        }
        l->udp_rx_ns = plat_now_ns();
        if (h.kind == U_HELLO_ACK) rtt_add(0, d + sizeof h, n - (int)sizeof h);
        if (h.kind == U_HELLO_ACK && !l->udp_up) {
            l->udp_up = true;
            LOG("[ldn] UDP to the host works: game packets go by UDP%s\n", relayed ? " through the hub server" : "");
        } else if (h.kind == U_DATA && n >= (int)(sizeof h + sizeof(VPacket))) {
            VPacket p;
            memcpy(&p, d + sizeof h, sizeof p);
            pkt_count(pkt_rx, PATH_UDP);
            link_received(0, &p, d + sizeof h + sizeof p, (uint32_t)n - sizeof h - sizeof p);
        }
    }
    return NULL;
}

static void station_leave(void) {  // (caller: not holding L.mu)
    if (L.up.has_thread) {
        link_close(&L.up);
        pthread_join(L.up.th, NULL);
        L.up.has_thread = false;
    }
    if (L.us != BADSOCK) {
        L.uth_run = false;
        pthread_join(L.uth, NULL);
        hclose(L.us);
        L.us = BADSOCK;
    }
    L.up.udp_up = false;
    L.up.p_up = L.up.p_known = false;
    vsock_network_down();
}

#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <ifaddrs.h>
#include <jni.h>
#include <net/if.h>
// Android's Wi-Fi drops broadcasts to save battery unless an app asks otherwise: on while the local play service is up
// (GameActivity.lanSearch)
static void lan_wifi(bool on) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    if (!act) return;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = cls ? (*env)->GetStaticMethodID(env, cls, "lanSearch", "(Z)V") : NULL;
    if (m) (*env)->CallStaticVoidMethod(env, cls, m, (jboolean)on);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
}
// The probe to each network's own broadcast address as well: 255.255.255.255 follows the default route, and on a phone
// that can be mobile data, or nothing when it is the Wi-Fi hotspot
static void probe_networks(hsock u, const uint8_t *probe, size_t size, struct sockaddr_in to) {
    struct ifaddrs *all = NULL;
    if (getifaddrs(&all)) return;
    int sent = 0;
    for (struct ifaddrs *i = all; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_BROADCAST) || !(i->ifa_flags & IFF_UP) ||
            !i->ifa_broadaddr)
            continue;
        to.sin_addr = ((struct sockaddr_in *)(void *)i->ifa_broadaddr)->sin_addr;
        sendto(u, (const char *)probe, size, 0, (struct sockaddr *)&to, sizeof to);
        ONCE("[ldn] searching on %s (%s)\n", i->ifa_name, inet_ntoa(to.sin_addr));
        sent++;
    }
    freeifaddrs(all);
    ONCE("[ldn] %d local network(s) to search\n", sent);
}
#else
static void lan_wifi(bool on) { (void)on; }
#endif

// Hosts to ask: ini Join (comma separated host[:port]) and this computer.
static int scan_addresses(struct sockaddr_in *out, int max) {
    int n = 0;
    char list[sizeof g_config.ldn_join];
    snprintf(list, sizeof list, "%s", g_config.ldn_join);
    for (char *t = strtok(list, ", ;"); t && n < max; t = strtok(NULL, ", ;")) {
        if (resolve_host(t, g_config.ldn_port, &out[n])) n++;
        else LOG("[ldn] Join address \"%s\" not found\n", t);
    }
    return n;
}

#define SEARCH_NS 1500000000ull      // how long a LAN search listens
#define SEARCH_ASK_NS 500000000ull   // and how often it asks
// Collects the networks of hosts that answer: by address (TCP) and by LAN broadcast (UDP).
static int discover(uint8_t (*infos)[NETINFO_SIZE], struct sockaddr_in *addrs, uint16_t *vers, int max) {
    hnet_init();
    int n = 0;
    static __thread uint8_t buf[MAX_FRAME];
    struct sockaddr_in targets[8];
    int nt = scan_addresses(targets, 8);
    for (int i = 0; i < nt && n < max; i++) {
        for (int v = 0; v < N_LINK_VERSIONS; v++) {  // (a host answers only a hello of its own version: ask each)
            hsock s = connect_to(&targets[i], 1500);
            if (s == BADSOCK) break;
            Hello h = make_hello(true, NULL, LINK_VERSIONS[v]);
            hset_timeout(s, 1500);
            uint8_t type;
            int k;
            bool got = frame_send(s, F_HELLO, &h, sizeof h, NULL, 0) && (k = frame_recv(s, &type, buf)) >= NETINFO_SIZE &&
                       type == F_INFO;
            if (got) {
                memcpy(infos[n], buf, NETINFO_SIZE);
                vers[n] = LINK_VERSIONS[v];
                addrs[n++] = targets[i];
            }
            hclose(s);
            if (got) break;
        }
    }
    hsock u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (u == BADSOCK) return n;
    int one = 1;
    setsockopt(u, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
    uint8_t probe[N_LINK_VERSIONS][8];  // (one probe per link version)
    for (int v = 0; v < N_LINK_VERSIONS; v++) {
        memcpy(probe[v], PROTO_MAGIC, 4);
        wr16(probe[v] + 4, LINK_VERSIONS[v]);
        wr16(probe[v] + 6, 0);
    }
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)g_config.ldn_port);
    // Asked more than once, and given time: on Wi-Fi a broadcast waits at the router for the next beacon that wakes a
    // device saving power (a phone's reply took longer than the 400 ms this used to wait)
    uint64_t next_ask = 0, end = plat_now_ns() + SEARCH_NS;
    for (uint64_t now; (now = plat_now_ns()) < end && n < max;) {
        if (now >= next_ask) {
            next_ask = now + SEARCH_ASK_NS;
            for (int v = 0; v < N_LINK_VERSIONS; v++) {
                to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
                sendto(u, (const char *)probe[v], sizeof probe[v], 0, (struct sockaddr *)&to, sizeof to);
#ifdef __ANDROID__
                probe_networks(u, probe[v], sizeof probe[v], to);
#endif
                to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                sendto(u, (const char *)probe[v], sizeof probe[v], 0, (struct sockaddr *)&to, sizeof to);
            }
        }
        fd_set r;
        FD_ZERO(&r);
        FD_SET(u, &r);
        uint64_t left = ((next_ask < end ? next_ask : end) - now) / 1000;
        struct timeval tv = {(long)(left / 1000000), (long)(left % 1000000)};
        if (select((int)u + 1, &r, NULL, NULL, &tv) <= 0) continue;
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int k = recvfrom(u, (char *)buf, MAX_FRAME, 0, (struct sockaddr *)&from, &fl);
        if (k < 8 + NETINFO_SIZE || memcmp(buf, PROTO_MAGIC, 4) || !link_version_ok(rd16(buf + 4))) continue;
        bool dup = false;
        for (int i = 0; i < n; i++) dup |= !memcmp(infos[i] + NI_BSSID, buf + 8 + NI_BSSID, 6);
        if (dup) continue;
        memcpy(infos[n], buf + 8, NETINFO_SIZE);
        from.sin_port = htons(rd16(buf + 6));
        vers[n] = rd16(buf + 4);
        addrs[n++] = from;
    }
    hclose(u);
    return n;
}

// ---- nn::ldn

HLE_FUNC("_ZN2nn3ldn10InitializeEv", ldn_initialize) { NTRACE();
    lan_wifi(true);
    pthread_mutex_lock(&L.mu);
    if (!L.ev) L.ev = kernel_create_event_pair(&(uint32_t){0}, &L.ev_r);
    static bool hooked;
    if (!hooked) {
        hooked = true;
        mhgu_ryu_set_hooks(ryu_lobby_changed, ryu_data_received);
        mhgu_uds_set_hooks(citra_info_changed, citra_packet);
        mhgu_ydoor_set_hooks(ydoor_info_changed, ydoor_packet);
    }
    crossplay_snapshot();
    mhgu_ryu_set_config(xp_config.ryu_servers, "", xp_config.ryu_password);
    if (L.state == ST_NONE) {
        ryusrv_wanted();
        roomsrv_wanted();
        room_start();
        citra_start();
        L.state = ST_INITIALIZED, L.reason = DR_NONE;
        rand_bytes(L.mac, 6);
        L.mac[0] = (uint8_t)((L.mac[0] & 0xFC) | 0x02);  // locally administered, unicast
    }
    pthread_mutex_unlock(&L.mu);
    ONCE("[ldn] local play over IP: host port %d, join %s (state event %x)\n", g_config.ldn_port,
         g_config.ldn_join[0] ? g_config.ldn_join : "(LAN search only)", L.ev_r);
    RET(0);
}

static void browse_touch(void);  // (Join Hub screen open, see ldn_hub_browsing)
static void browse_end(void);
static void to_initialized(void) {
    int st = L.state;
    if (L.ryu || L.room || L.citra) {
        if (L.ryu) ryu_stop();
        else if (L.room) room_stop();
        else citra_stop();
        L.own_ip = L.subnet = 0;
        return;
    }
    if (st == ST_AP_CREATED || st == ST_ACCESS_POINT) host_stop(DR_DESTROYED_BY_USER);
    if (st == ST_STATION_CONNECTED || st == ST_STATION || st == ST_ERROR) station_leave();
    if (st == ST_AP_CREATED) vsock_network_down();
    L.own_ip = L.subnet = 0;
}

HLE_FUNC("_ZN2nn3ldn8FinalizeEv", ldn_finalize) { NTRACE();
    lan_wifi(false);
    browse_end();
    to_initialized();
    room_shutdown();
    citra_shutdown();
    roomsrv_unwanted();
    ryusrv_unwanted();
    L.state = ST_NONE;
    (void)c;
}
HLE_FUNC("_ZN2nn3ldn8GetStateEv", ldn_get_state) { NTRACE(); RET(L.state); }
HLE_FUNC("_ZN2nn3ldn19GetDisconnectReasonEv", ldn_get_reason) { NTRACE(); RET(L.reason); }

// AttachStateChangeEvent(SystemEventType *): the event fires on every change of the state or the network
HLE_FUNC("_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", ldn_attach_event) { NTRACE();
    if (!L.ev) L.ev = kernel_create_event_pair(&(uint32_t){0}, &L.ev_r);
    c->r[1] = L.ev_r;
    c->r[2] = 0;  // not managed: the handle stays ours
    c->r[3] = 1;  // auto clear
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
}

HLE_FUNC("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", ldn_get_network_info) { NTRACE();
    if (!ldn_net_up()) { RET(R_INVALID_STATE); return; }
    pthread_mutex_lock(&L.mu);
    memcpy(ARGP(0), L.info, NETINFO_SIZE);
    pthread_mutex_unlock(&L.mu);
    RET(0);
}

HLE_FUNC("_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", ldn_get_ipv4) { NTRACE();
    if (!ldn_net_up()) { RET(R_INVALID_STATE); return; }
    if (ARG(0)) ST32(ARG(0), L.own_ip);
    if (ARG(1)) ST32(ARG(1), L.ryu && L.ryu_mask ? L.ryu_mask : 0xFFFFFF00u);
    RET(0);
}

HLE_FUNC("_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", ldn_get_sec_param) { NTRACE();
    if (!ldn_net_up()) { RET(R_INVALID_STATE); return; }
    memcpy(ARGP(0), L.info + NI_SEC_PARAM, 0x10);
    memcpy((uint8_t *)ARGP(0) + 0x10, L.info + NI_SESSION_ID, 0x10);
    RET(0);
}

HLE_FUNC("_ZN2nn3ldn16GetNetworkConfigEPNS0_13NetworkConfigE", ldn_get_net_config) { NTRACE();
    if (!ldn_net_up()) { RET(R_INVALID_STATE); return; }
    uint8_t *o = ARGP(0);
    memset(o, 0, 0x20);
    memcpy(o, L.info, 0x10);  // IntentId
    wr16(o + 0x10, rd16(L.info + NI_CHANNEL));
    o[0x12] = L.info[NI_NODE_MAX];
    wr16(o + 0x14, L.lcv);
    RET(0);
}

// ---- access point (host)
HLE_FUNC("_ZN2nn3ldn15OpenAccessPointEv", ldn_open_ap) { NTRACE();
    if (L.state != ST_INITIALIZED) { RET(R_INVALID_STATE); return; }
    L.state = ST_ACCESS_POINT;
    signal_change();
    RET(0);
}
HLE_FUNC("_ZN2nn3ldn16CloseAccessPointEv", ldn_close_ap) { NTRACE();
    if (L.state != ST_ACCESS_POINT && L.state != ST_AP_CREATED) { RET(R_INVALID_STATE); return; }
    to_initialized();
    L.state = ST_INITIALIZED;
    signal_change();
    RET(0);
}

static uint32_t create_ryu(const uint8_t *cfg, const uint8_t *sec, const uint8_t *user) {
    uint8_t info[NETINFO_SIZE];
    uint32_t ip = 0, mask = 0;
    L.ryu = true;
    uint32_t r = mhgu_ryu_create(mhgu_ryusrv_running() ? -1 : 0, cfg, sec, user, L.adv, L.adv_size, info, &ip, &mask);
    if (r) {
        L.ryu = false;
        LOG("[ldn] creating the hub on the RyuLdn server failed (result %#x)\n", r);
        return r;
    }
    pthread_mutex_lock(&L.mu);
    if (info_sane(info)) memcpy(L.info, info, NETINFO_SIZE);
    L.own_ip = ip, L.ryu_mask = mask, L.subnet = ip & mask, L.lcv = rd16(cfg + 0x14);
    L.state = ST_AP_CREATED, L.reason = DR_NONE;
    pthread_mutex_unlock(&L.mu);
    LOG("[ldn] hub created on the RyuLdn server: %s, up to %d players\n", ipstr(L.own_ip), L.info[NI_NODE_MAX]);
    if (room_configured()) mixed_bridge_start();
    signal_change();
    return 0;
}

static uint32_t create_network(const uint8_t *cfg, const uint8_t *sec, const uint8_t *sec_param, const uint8_t *user) {
    browse_end();  // (hosting: not browsing)
    if (L.state != ST_ACCESS_POINT) return R_INVALID_STATE;
    crossplay_prepare();
    if (ryu_configured()) return create_ryu(cfg, sec, user);
    // All Rooms: our own host, and each room we're in (none: the LAN / hub server only)
    const bool all = xp_config.crossplay == XP_ALL;
    bool via_room = !all && room_configured() && mhgu_room_connected();
    if (!all && room_configured() && !via_room) return R_CONNECT_FAILURE;  // (a Yuzu Room hub needs the room)
    if (via_room) room_mac(L.mac);
    bool via_citra = !via_room && citra_configured() && (!all || mhgu_uds_ready());
    if (via_citra && !mhgu_uds_ready()) return R_CONNECT_FAILURE;  // (a Citra Room network needs the room)
    if (via_citra) mhgu_uds_mac(L.mac);  // (its beacons come from this member's MAC)
    bool via_yuzu = all && room_configured() && mhgu_room_connected();
    pthread_mutex_lock(&L.mu);
    memset(L.info, 0, NETINFO_SIZE);
    memcpy(L.info, cfg, 0x10);  // IntentId
    if (sec_param) {
        memcpy(L.info + NI_SEC_PARAM, sec_param, 0x10);
        memcpy(L.info + NI_SESSION_ID, sec_param + 0x10, 0x10);
    } else {
        rand_bytes(L.info + NI_SEC_PARAM, 0x10);
        rand_bytes(L.info + NI_SESSION_ID, 0x10);
    }
    memcpy(L.info + NI_BSSID, L.mac, 6);
    char ssid[33];
    snprintf(ssid, sizeof ssid, "MHGURecomp%08x%08x", arc4random(), arc4random());
    L.info[NI_SSID] = (uint8_t)strlen(ssid);
    memcpy(L.info + NI_SSID + 1, ssid, strlen(ssid));
    uint16_t ch = rd16(cfg + 0x10);
    wr16(L.info + NI_CHANNEL, ch ? ch : 6);
    L.info[NI_LINK_LEVEL] = 3;
    L.info[NI_NETWORK_TYPE] = 2;  // ldn
    wr16(L.info + NI_SEC_MODE, rd16(sec));
    L.info[NI_ACCEPT_POLICY] = (uint8_t)L.accept_policy;
    uint8_t max = cfg[0x12];
    L.info[NI_NODE_MAX] = max >= 1 && max <= MAX_NODES ? max : MAX_NODES;
    L.info[NI_NODE_COUNT] = 1;
    L.lcv = rd16(cfg + 0x14);
    if (via_room) {
        uint8_t ip[4];
        mhgu_room_own_ip(ip);
        L.own_ip = ip_u32(ip), L.subnet = L.own_ip & 0xFFFFFF00u;
        memset(L.room_client, 0, sizeof L.room_client);
        memcpy(L.info + NI_SSID, "\x12YuzuFakeSsidForLdn", 19);  // (what the emulators' scans expect)
    } else if (via_citra) {
        L.subnet = mhgu_uds_subnet();  // (169.254.x.0 from our MAC: the phones that join work it out the same way)
        L.own_ip = L.subnet | 1;
    } else {
        L.subnet = 0xA9FE0000u | (uint32_t)(1 + arc4random() % 254) << 8;  // 169.254.x.0/24
        L.own_ip = L.subnet | 1;
    }
    uint8_t *nd = node_at(L.info, 0);
    wr32(nd + NODE_IP, L.own_ip);
    memcpy(nd + NODE_MAC, L.mac, 6);
    nd[NODE_ID] = 0;
    nd[NODE_CONNECTED] = 1;
    memcpy(nd + NODE_NAME, user, 0x20);
    wr16(nd + NODE_LCV, L.lcv);
    wr16(L.info + NI_ADV_SIZE, (uint16_t)L.adv_size);
    memcpy(L.info + NI_ADV, L.adv, L.adv_size);
    uint64_t auth = ((uint64_t)arc4random() << 32) | arc4random();
    memcpy(L.info + NI_AUTH_ID, &auth, 8);
    memset(L.nodes, 0, sizeof L.nodes);
    for (int i = 0; i < MAX_NODES; i++) pthread_mutex_init(&L.nodes[i].wmu, NULL);
    uint8_t hosted[NETINFO_SIZE];
    memcpy(hosted, L.info, NETINFO_SIZE);
    pthread_mutex_unlock(&L.mu);
    memset(L.door_of, 0, sizeof L.door_of);
    bool hosted_here = false;
    if (via_room) L.room = true;
    else {
        // our own host (LAN, hub server), and the room doors: one network, every kind of player
        if (via_citra) L.citra = mhgu_uds_host(hosted);
        if (via_yuzu) L.yuzu = mhgu_ydoor_host(hosted);
        hosted_here = host_start();
        if (!hosted_here && (L.citra || L.yuzu)) LOG("[ldn] no LAN / hub server players: only the rooms'\n");
    }
    if (via_citra && !L.citra && !all) return R_CONNECT_FAILURE;
    L.state = ST_AP_CREATED;
    LOG("[ldn] network created%s%s%s%s: %s, up to %d players, version %u\n", via_room ? " in the Yuzu room" : "",
        hosted_here ? " on the LAN / hub server" : "", L.citra ? ", in the Citra room" : "", L.yuzu ? ", in the Yuzu room" : "",
        ipstr(L.own_ip), L.info[NI_NODE_MAX], L.lcv);
    signal_change();
    return 0;
}

// CreateNetwork(const NetworkConfig &, const SecurityConfig &, const UserConfig &)
HLE_FUNC("_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", ldn_create) { NTRACE();
    RET(create_network(ARGP(0), ARGP(1), NULL, ARGP(2)));
}
// CreateNetworkPrivate(config, security, SecurityParameter, user, address entry count, const AddressEntry *)
HLE_FUNC("_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE",
         ldn_create_private) {
    RET(create_network(ARGP(0), ARGP(1), ARGP(2), ARGP(3)));
}

HLE_FUNC("_ZN2nn3ldn14DestroyNetworkEv", ldn_destroy) { NTRACE();
    if (L.state != ST_AP_CREATED) { RET(R_INVALID_STATE); return; }
    if (L.ryu) ryu_stop();
    else if (L.room) room_stop();
    else if (L.citra) citra_stop();
    else host_stop(DR_DESTROYED_BY_USER);
    vsock_network_down();
    L.state = ST_ACCESS_POINT;
    L.own_ip = L.subnet = 0;
    LOG("[ldn] network destroyed\n");
    signal_change();
    RET(0);
}

HLE_FUNC("_ZN2nn3ldn16SetAdvertiseDataEPKvj", ldn_set_advertise) { NTRACE();
    uint32_t n = ARG(1) < ADV_MAX ? ARG(1) : ADV_MAX;
    pthread_mutex_lock(&L.mu);
    memcpy(L.adv, ARGP(0), n);
    L.adv_size = n;
    if (L.state == ST_AP_CREATED) {
        wr16(L.info + NI_ADV_SIZE, (uint16_t)n);
        memset(L.info + NI_ADV, 0, ADV_MAX);
        memcpy(L.info + NI_ADV, L.adv, n);
        if (L.ryu) mhgu_ryu_set_advertise(L.adv, n);
        else if (L.room) room_info_dirty = true;
        else push_info_locked(0);
    }
    pthread_mutex_unlock(&L.mu);
    if (room_info_dirty) { room_info_dirty = false; room_sync_all(); }
    RET(0);
}

HLE_FUNC("_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", ldn_set_accept) { NTRACE();
    pthread_mutex_lock(&L.mu);
    L.accept_policy = (int)ARG(0);
    if (L.state == ST_AP_CREATED) {
        L.info[NI_ACCEPT_POLICY] = (uint8_t)ARG(0);
        if (L.ryu) mhgu_ryu_set_accept_policy((uint8_t)ARG(0));
        else if (L.room) room_info_dirty = true;
        else push_info_locked(0);
    }
    pthread_mutex_unlock(&L.mu);
    if (room_info_dirty) { room_info_dirty = false; room_sync_all(); }
    RET(0);
}

// Reject(Ipv4Address): the host sends a player away
HLE_FUNC("_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", ldn_reject) { NTRACE();
    if (L.citra) mhgu_uds_reject(ARG(0));  // (an MHXX player; one of our own stations below)
    if (L.yuzu) mhgu_ydoor_reject(ARG(0));  // (a Yuzu room player)
    if (L.ryu) {
        for (int i = 1; i < MAX_NODES; i++)
            if (node_at(L.info, i)[NODE_CONNECTED] && rd32(node_at(L.info, i) + NODE_IP) == ARG(0))
                mhgu_ryu_reject((uint32_t)i, DR_REJECTED);
        RET(0);
        return;
    }
    if (L.room) {
        uint8_t own[4], to[MAX_NODES][4];
        int n = 0;
        mhgu_room_own_ip(own);
        pthread_mutex_lock(&L.mu);
        for (int i = 1; i < MAX_NODES; i++)
            if (L.room_client[i] && rd32(node_at(L.info, i) + NODE_IP) == ARG(0)) {
                memcpy(to[n++], L.room_cip[i], 4);
                memset(node_at(L.info, i), 0, NODE_SIZE);
                node_at(L.info, i)[NODE_ID] = (uint8_t)i;
                L.room_client[i] = false;
                if (L.info[NI_NODE_COUNT] > 1) L.info[NI_NODE_COUNT]--;
            }
        pthread_mutex_unlock(&L.mu);
        for (int i = 0; i < n; i++) mhgu_room_send_ldn(RLDN_DESTROY_NETWORK, own, to[i], false, NULL, 0);
        if (n) { room_sync_all(); signal_change(); }
        RET(0);
        return;
    }
    uint32_t r = DR_REJECTED;
    for (int i = 1; i < MAX_NODES; i++)
        if (L.nodes[i].alive && L.nodes[i].ip == ARG(0)) {
            link_send(&L.nodes[i], F_DESTROY, &r, 4, NULL, 0);
            link_close(&L.nodes[i]);
        }
    RET(0);
}

// ---- station (joining)
HLE_FUNC("_ZN2nn3ldn11OpenStationEv", ldn_open_station) { NTRACE();
    if (L.state != ST_INITIALIZED) { RET(R_INVALID_STATE); return; }
    L.state = ST_STATION;
    browse_touch();
    signal_change();
    RET(0);
}
HLE_FUNC("_ZN2nn3ldn12CloseStationEv", ldn_close_station) { NTRACE();
    if (L.state != ST_STATION && L.state != ST_STATION_CONNECTED && L.state != ST_ERROR) { RET(R_INVALID_STATE); return; }
    to_initialized();
    L.state = ST_INITIALIZED;
    signal_change();
    RET(0);
}

static bool filter_ok(const uint8_t *info, const uint8_t *f) {
    uint32_t flags = rd32(f + 0x5C);
    if ((flags & SF_LOCAL_COMM_ID) && memcmp(info + NI_LOCAL_COMM_ID, f, 8)) return false;
    if ((flags & SF_SCENE_ID) && rd16(info + NI_SCENE_ID) != rd16(f + 0x0A)) return false;
    if ((flags & SF_SESSION_ID) && memcmp(info + NI_SESSION_ID, f + 0x10, 0x10)) return false;
    if ((flags & SF_BSSID) && memcmp(info + NI_BSSID, f + 0x24, 6)) return false;
    return true;
}

// A hub server answers one search with only some of its hubs (a few picked at random when it has many), so the hubs of
// the last searches are kept and listed together: the list of a Retry grows until it holds them all.
#define SEEN_MAX MAX_FOUND
#define HUB_LIST_MAX 500  // the most hubs one list (all its pages) holds
#define HUB_PAGE 18       // hubs the game's Join Hub screen lists at once (it takes more, but shows 3 pages of 6)
static int list_per = HUB_PAGE, list_total = 0;  // hubs in a list; hubs found
static int list_next = -1, list_given, list_pages = 1, list_selected = 0;  // the list the next search gives (-1: the one after the last); the last one given; how many lists
#define SEEN_KEEP_NS (25ull * 1000000000ull)
static struct {
    uint8_t info[NETINFO_SIZE];
    uint64_t id, at;
    struct sockaddr_in addr;
    char server[LOBBY_NAME];
    uint16_t ver;
    uint8_t phone;
} seen[SEEN_MAX];
static bool seen_live(int i, uint64_t now) { return seen[i].at && now - seen[i].at <= SEEN_KEEP_NS; }
static void seen_add(const uint8_t *info, uint64_t id, uint16_t ver, uint8_t phone, const struct sockaddr_in *addr,
                     const char *server, uint64_t now) {
    int idx = -1, empty = -1, oldest = 0;
    for (int i = 0; i < SEEN_MAX; i++) {
        if (!seen_live(i, now)) {
            if (empty < 0) empty = i;
            continue;
        }
        if (!memcmp(seen[i].info + NI_BSSID, info + NI_BSSID, 6)) { idx = i; break; }
        if (seen[i].at < seen[oldest].at) oldest = i;
    }
    if (idx < 0) idx = empty >= 0 ? empty : oldest;
    memcpy(seen[idx].info, info, NETINFO_SIZE);
    seen[idx].id = id, seen[idx].ver = ver, seen[idx].phone = phone, seen[idx].addr = *addr, seen[idx].at = now;
    snprintf(seen[idx].server, sizeof seen[idx].server, "%s", server);
}

// Scan(NetworkInfo *out, int *count, int max, const ScanFilter &, int channel)
HLE_FUNC("_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ldn_scan) { NTRACE();
    uint32_t out = ARG(0), pcount = ARG(1);
    int max = (int)ARG(2);
    const uint8_t *filter = ARGP(3);
    if (L.state != ST_STATION && L.state != ST_ACCESS_POINT) { RET(R_INVALID_STATE); return; }
    browse_touch();
    crossplay_prepare();
    static uint8_t infos[MAX_FOUND][NETINFO_SIZE];
    static LobbyList online[MAX_HUBS];
    // (static, like infos: 512 entries are too much for a game thread's stack; a scan never runs twice at once)
    static struct sockaddr_in addrs[MAX_FOUND];
    static uint64_t hubs[MAX_FOUND];
    static uint16_t vers[MAX_FOUND];  // the link version each found network speaks
    static uint8_t phone[MAX_FOUND];  // found in the phones' public room
    static const char *names[MAX_FOUND];  // (the hub server a found hub is on; LAN hubs: none)
    static int order[MAX_FOUND];  // found[i] is infos[order[i]]
    static int ryu_of[MAX_FOUND];  // crossplay networks: a RyuLdn server's lobby (its index + 1), the Yuzu room's, the Citra room's
    static bool room_of[MAX_FOUND], citra_of[MAX_FOUND];
    memset(hubs, 0, sizeof hubs), memset(phone, 0, sizeof phone), memset(names, 0, sizeof names);
    memset(ryu_of, 0, sizeof ryu_of), memset(room_of, 0, sizeof room_of), memset(citra_of, 0, sizeof citra_of);
    char list[MAX_HUBS][LOBBY_NAME];
    pthread_t lt[MAX_HUBS];
    bool started[MAX_HUBS] = {false};
    hnet_init();  // (before the thread's first socket call)
    int ns = lobby_servers(list);
    for (int s = 0; s < ns; s++) {  // every server of the list is asked at once, while the LAN is searched
        snprintf(online[s].server, sizeof online[s].server, "%s", list[s]);
        started[s] = !pthread_create(&lt[s], NULL, lobby_list_thread, &online[s]);
    }
    int n = discover(infos, addrs, vers, 16), k = 0, listed = 0;
    uint64_t now = plat_now_ns();
    for (int s = 0; s < ns; s++) {  // the servers' hubs after the LAN's (a hub found twice is joined by the first way)
        if (!started[s]) continue;
        pthread_join(lt[s], NULL);
        if (online[s].state != HUB_ONLINE) continue;
        listed += online[s].n;
        for (int j = 0; j < online[s].n; j++)
            seen_add(online[s].infos[j], online[s].ids[j], online[s].vers[j], online[s].phone[j], &online[s].addr,
                     online[s].server, now);
    }
    for (int c = 0; c < SEEN_MAX && n < MAX_FOUND; c++) {  // the hubs of this search and of the last ones, after the LAN's
        if (!seen_live(c, now)) continue;
        bool dup = false;
        for (int i = 0; i < n; i++) dup |= !memcmp(infos[i] + NI_BSSID, seen[c].info + NI_BSSID, 6);
        if (dup) continue;
        memcpy(infos[n], seen[c].info, NETINFO_SIZE);
        addrs[n] = seen[c].addr;
        names[n] = seen[c].server;
        vers[n] = seen[c].ver;
        phone[n] = seen[c].phone;
        hubs[n++] = seen[c].id;
    }
    if (ryu_configured() && mhgu_ryu_server_count() > 0 && n < MAX_FOUND) {  // RyuLdn servers' lobbies
        static uint8_t rinfo[MAX_FOUND][NETINFO_SIZE];
        int rsrv[MAX_FOUND];
        int rn = mhgu_ryu_scan(filter, (uint8_t *)rinfo, rsrv, MAX_FOUND - n);
        for (int j = 0; j < rn; j++) {
            memcpy(infos[n], rinfo[j], NETINFO_SIZE);
            memset(&addrs[n], 0, sizeof addrs[n]);
            vers[n] = PROTO_VERSION;
            ryu_of[n++] = rsrv[j] + 1;
        }
    }
    if ((xp_config.crossplay == XP_ROOM || xp_config.crossplay == XP_ALL) && room_enabled() && mhgu_room_connected() &&
        n < MAX_FOUND) {  // the room's hubs
        uint8_t own[4], all[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        mhgu_room_own_ip(own);
        pthread_mutex_lock(&room_scan_mu);
        room_nscan = 0, room_scanning = true;
        pthread_mutex_unlock(&room_scan_mu);
        mhgu_room_send_ldn(RLDN_SCAN, own, all, true, NULL, 0);
        plat_sleep_until_ns(plat_now_ns() + 700000000ull);
        pthread_mutex_lock(&room_scan_mu);
        room_scanning = false;
        for (int j = 0; j < room_nscan && n < MAX_FOUND; j++) {
            memcpy(infos[n], room_scan[j].info, NETINFO_SIZE);
            memset(&addrs[n], 0, sizeof addrs[n]);
            vers[n] = PROTO_VERSION;
            room_of[n++] = true;
        }
        pthread_mutex_unlock(&room_scan_mu);
    }
    if (citra_configured() && mhgu_uds_ready() && n < MAX_FOUND) {  // MHXX players' networks in the Citra room
        static uint8_t cinfo[MAX_FOUND][NETINFO_SIZE];
        int cn = mhgu_uds_scan((uint8_t *)cinfo, MAX_FOUND - n);
        for (int j = 0; j < cn && n < MAX_FOUND; j++) {
            memcpy(infos[n], cinfo[j], NETINFO_SIZE);
            memset(&addrs[n], 0, sizeof addrs[n]);
            vers[n] = PROTO_VERSION;
            citra_of[n++] = true;
        }
    }
    nfound = 0;
    for (int i = 0; i < n && nfound < HUB_LIST_MAX; i++) {
        if (!info_sane(infos[i]) || !filter_ok(infos[i], filter)) continue;
        found[nfound].ryu = ryu_of[i];
        found[nfound].room = room_of[i];
        found[nfound].citra = citra_of[i];
        memcpy(found[nfound].info, infos[i], NETINFO_SIZE);
        memcpy(found[nfound].bssid, infos[i] + NI_BSSID, 6);
        memcpy(found[nfound].session, infos[i] + NI_SESSION_ID, 16);
        found[nfound].hub = hubs[i];
        snprintf(found[nfound].server, sizeof found[nfound].server, "%s", names[i] ? names[i] : "");
        snprintf(found[nfound].key, sizeof found[nfound].key, "%s", phone[i] ? PHONE_PUBLIC_KEY : "");
        found[nfound].ver = vers[i];
        found[nfound].addr = addrs[i];
        order[nfound++] = i;
    }
    // One fixed order: the LAN's hubs first, then the hubs of the servers by their BSSID (stable for as long as a hub
    // lives, whatever order a server or a search answered in). A stable insertion sort of order[].
    for (int i = 1; i < nfound; i++) {
        int v = order[i], j = i - 1;
        while (j >= 0) {
            int a = order[j];
            int c = (hubs[a] != 0) - (hubs[v] != 0);
            if (!c) c = memcmp(infos[a] + NI_BSSID, infos[v] + NI_BSSID, 6);
            if (c <= 0) break;
            order[j + 1] = order[j], j--;
        }
        order[j + 1] = v;
    }
    // Pages: the game takes only `max` networks per search, so the first search gives the first `max` of the list, the
    // next one (Retry) the next `max`, and so on up to HUB_LIST_MAX hubs; after the last page it starts over. The order
    // is the same from one search to the next (the fixed order above), so no hub is skipped or shown twice.
    int per = max < HUB_PAGE ? max : HUB_PAGE;  // (what the game's Join Hub screen shows: 3 pages of 6)
    int pages = per > 0 ? (nfound + per - 1) / per : 1;
    if (pages < 1) pages = 1;
    if (list_selected >= pages) list_selected = 0;
    if (list_next >= 0) list_selected = list_next, list_next = -1;  // (a tap on a list's number: that one)
    if (list_selected >= pages) list_selected = 0;
    int cur_list = list_selected, first = cur_list * per;
    list_given = cur_list, list_pages = pages, list_per = per > 0 ? per : HUB_PAGE, list_total = nfound;
    list_selected = (cur_list + 1) % pages;  // (a plain Retry gives the next list)
    for (int t = first; t < nfound && k < per; t++)
        memcpy((uint8_t *)G2H(out) + (size_t)k++ * NETINFO_SIZE, infos[order[t]], NETINFO_SIZE);
    if (pcount) ST32(pcount, k);
    for (int t = 0; t < k; t++) {  // what the game was given, to compare with its Join Hub list (which hubs it drops, and why)
        int i = order[first + t];
        uint8_t *nd = node_at(infos[i], 0);
        LOG("[ldn] scan give %d: %d/%d players, adv %u bytes, host '%.32s', %s v%u\n", t, infos[i][NI_NODE_COUNT],
            infos[i][NI_NODE_MAX], (unsigned)rd16(infos[i] + NI_ADV_SIZE), (const char *)(nd + NODE_NAME),
            hubs[i] ? (names[i] ? names[i] : "hub") : ryu_of[i] ? "RyuLdn" : room_of[i] ? "Yuzu room" : citra_of[i] ? "Citra room" : "LAN",
            (unsigned)vers[i]);
    }
    {  // (every search: the page it gave is part of the line)
        LOG("[ldn] scan: %d network(s) found (%d listed on the hub servers), page from #%d: %d given to the game (room for %d)\n",
            nfound, listed, first, k, max);
    }
    RET(0);
}

// The Join Hub screen is open (a station that has not joined): the list buttons (runtime/touch.cpp) change the hub list.
// (the game may leave the station state once its table of hubs is up, so the screen counts as open for a while after
// the last search or station call, until a hub is joined, hosted or the game shuts the LAN down)
static uint64_t browse_ns;  // plat_now_ns() of the last Join Hub search / OpenStation; 0: not browsing
#define BROWSE_HOLD_NS (90ull * 1000000000ull)
static void browse_touch(void) { browse_ns = plat_now_ns(); }
static void browse_end(void) { browse_ns = 0; }
bool ldn_hub_browsing(void) {
    if (L.state == ST_STATION) return true;
    if (!browse_ns || (L.state != ST_INITIALIZED && L.state != ST_ACCESS_POINT)) return false;
    return plat_now_ns() - browse_ns < BROWSE_HOLD_NS;
}
int ldn_hub_lists(void) { return list_pages; }  // how many lists the last search had
// The next search gives the list before / after the one chosen last (dir -1 / +1); several steps in a row add up.
// false: only one list, nothing to change.
bool ldn_hub_list_step(int dir) {
    if (list_pages <= 1) return false;
    int base = list_next >= 0 ? list_next : list_given;
    list_next = ((base + (dir < 0 ? -1 : 1)) % list_pages + list_pages) % list_pages;
    return true;
}
// The next search gives list `n` (0 based), as a tap on its number does. false: no such list.
bool ldn_hub_list_goto(int n) {
    if (n < 0 || n >= list_pages) return false;
    list_next = n;
    return true;
}
// Which hubs the chosen list holds: #first to #last (1 based) of total.
void ldn_hub_list_range(int *first, int *last, int *total) {
    int c = list_next >= 0 ? list_next : list_given, f = c * list_per + 1, l = (c + 1) * list_per;
    *first = f, *last = l < list_total ? l : list_total, *total = list_total;
}
// The list chosen now (the one the next search gives): what the buttons show lit.
int ldn_hub_list_chosen(void) { return list_next >= 0 ? list_next : list_given; }
// Whether a list was chosen that the Join Hub screen does not show yet (a search by itself puts it there).
bool ldn_hub_list_pending(void) { return list_next >= 0 && list_next != list_given; }

static uint32_t connect_network(const uint8_t *info, const uint8_t *user, uint16_t lcv) {
    browse_end();  // (a hub is being joined: the list is left)
    if (L.state != ST_STATION) return R_INVALID_STATE;
    int i = 0;
    while (i < nfound && memcmp(found[i].bssid, info + NI_BSSID, 6)) i++;
    if (i == nfound) return R_CONNECT_NOT_FOUND;
    if (found[i].ryu) {  // a lobby on a RyuLdn server
        uint8_t sec[0x44] = {0}, out[NETINFO_SIZE];
        uint32_t ip = 0, mask = 0;
        memcpy(sec, found[i].info + NI_SEC_MODE, 2);
        L.ryu = true;
        uint32_t r = mhgu_ryu_connect(found[i].ryu - 1, found[i].info, sec, user, lcv, out, &ip, &mask);
        if (r) {
            L.ryu = false;
            LOG("[ldn] joining the RyuLdn lobby failed (result %#x)\n", r);
            return r;
        }
        pthread_mutex_lock(&L.mu);
        if (info_sane(out)) memcpy(L.info, out, NETINFO_SIZE);
        L.lcv = lcv, L.own_ip = ip, L.ryu_mask = mask, L.subnet = ip & mask;
        L.state = ST_STATION_CONNECTED, L.reason = DR_NONE;
        pthread_mutex_unlock(&L.mu);
        LOG("[ldn] joined a RyuLdn lobby as %s\n", ipstr(L.own_ip));
        signal_change();
        return 0;
    }
    if (found[i].citra) {  // an MHXX player's network in the Citra room
        if (!mhgu_uds_ready()) return R_CONNECT_FAILURE;
        uint8_t out[NETINFO_SIZE];
        uint32_t ip = 0;
        uint32_t r = mhgu_uds_connect(found[i].bssid, user, out, &ip);
        if (r) {
            LOG("[ldn] joining the MHXX network failed (%s)\n", r == 1 ? "it is gone" : "its host did not take us");
            return r == 1 ? R_CONNECT_NOT_FOUND : R_CONNECT_TIMEOUT;
        }
        pthread_mutex_lock(&L.mu);
        if (info_sane(out)) memcpy(L.info, out, NETINFO_SIZE);
        L.citra = true, L.lcv = lcv, L.own_ip = ip, L.subnet = ip & 0xFFFFFF00u;
        L.state = ST_STATION_CONNECTED, L.reason = DR_NONE;
        pthread_mutex_unlock(&L.mu);
        if (mhgu_uds_info(out) && info_sane(out)) {  // (an update that came while we were setting up)
            pthread_mutex_lock(&L.mu);
            if (L.citra) memcpy(L.info, out, NETINFO_SIZE);
            pthread_mutex_unlock(&L.mu);
        }
        LOG("[ldn] joined an MHXX player's network in the Citra room as %s\n", ipstr(L.own_ip));
        signal_change();
        return 0;
    }
    if (found[i].room) {  // a hub in a Yuzu-family room
        if (!mhgu_room_connected()) return R_CONNECT_FAILURE;
        uint8_t ipb[4], hostb[4];
        mhgu_room_own_ip(ipb);
        pthread_mutex_lock(&L.mu);
        memcpy(L.info, found[i].info, NETINFO_SIZE);
        L.room = true, L.own_ip = ip_u32(ipb), L.subnet = L.own_ip & 0xFFFFFF00u, L.lcv = lcv;
        pthread_mutex_unlock(&L.mu);
        uint8_t node[NODE_SIZE] = {0};
        wr32(node + NODE_IP, L.own_ip);
        room_mac(node + NODE_MAC);
        node[NODE_CONNECTED] = 1;
        memcpy(node + NODE_NAME, user, 0x20);
        wr16(node + NODE_LCV, lcv);
        ip_bytes(rd32(node_at(found[i].info, 0) + NODE_IP), hostb);
        mhgu_room_send_ldn(RLDN_CONNECT, ipb, hostb, false, node, NODE_SIZE);
        for (int w = 0; w < 300 && L.state != ST_STATION_CONNECTED; w++) plat_sleep_until_ns(plat_now_ns() + 10000000ull);
        if (L.state != ST_STATION_CONNECTED) {
            L.room = false, L.own_ip = L.subnet = 0;
            LOG("[ldn] the room's host did not answer the join\n");
            return R_CONNECT_TIMEOUT;
        }
        LOG("[ldn] joined a hub in the room as %s\n", ipstr(L.own_ip));
        signal_change();
        return 0;
    }
    hsock s;
    uint32_t pair = 0;
    uint64_t secret = 0;
    if (found[i].hub) {
        uint32_t r = lobby_join(&found[i].addr, found[i].server, found[i].key, found[i].ver, found[i].hub, &s, &pair, &secret);
        if (r) return r;
    } else if ((s = connect_to(&found[i].addr, 3000)) == BADSOCK) {
        LOG("[ldn] can't reach the host at %s\n", inet_ntoa(found[i].addr.sin_addr));
        return R_CONNECT_TIMEOUT;
    }
    L.lcv = lcv;
    Hello h = make_hello(false, user, found[i].ver);
    static uint8_t buf[MAX_FRAME];
    uint8_t type;
    int n;
    hset_timeout(s, 5000);
    if (!frame_send(s, F_HELLO, &h, sizeof h, NULL, 0) || (n = frame_recv(s, &type, buf)) < 4) {
        hclose(s);
        return R_CONNECT_TIMEOUT;
    }
    if (type == F_REFUSE) {
        hclose(s);
        uint32_t r = rd32(buf);
        LOG("[ldn] the host refused (result %#x)\n", r);
        return (r & 0x1FF) == 203 ? r : R_CONNECT_REJECTED;  // (an nn::ldn result only)
    }
    int node = buf[0];
    if (type != F_WELCOME || n < 8 + NETINFO_SIZE || node < 1 || node >= MAX_NODES || !info_sane(buf + 8) ||
        !node_at(buf + 8, node)[NODE_CONNECTED]) {
        hclose(s);
        return R_CONNECT_FAILURE;
    }
    hset_timeout(s, 0);
    pthread_mutex_lock(&L.mu);
    memcpy(L.info, buf + 8, NETINFO_SIZE);
    L.own_ip = rd32(node_at(L.info, node) + NODE_IP);
    L.subnet = L.own_ip & 0xFFFFFF00u;
    pthread_mutex_init(&L.up.wmu, NULL);
    L.up.s = s, L.up.alive = true, L.up.ip = L.subnet | 1;
    L.up.token = rd32(buf + 4), L.up.udp_up = false, L.up.udp_rx_ns = 0;
    L.up.relay_pair = pair, L.up.relay_secret = secret;
    L.up.uaddr = found[i].addr;
    direct_reset(&L.up, config_live_ldn_direct());
    L.state = ST_STATION_CONNECTED, L.reason = DR_NONE;
    pthread_mutex_unlock(&L.mu);
    L.up.has_thread = pthread_create(&L.up.th, NULL, uplink_thread, NULL) == 0;
    // UDP to the same address and port as the TCP connection (the host's hello answers tell whether it gets through);
    // not connected: in an internet hub the host's own address may answer too (direct UDP)
    L.us = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in any = {0};
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    if (L.us != BADSOCK && !bind(L.us, (const struct sockaddr *)&any, sizeof any)) {
        udp_no_connreset(L.us);
        L.uth_run = true;
        if (pthread_create(&L.uth, NULL, station_udp_thread, NULL)) { hclose(L.us); L.us = BADSOCK; }
    } else if (L.us != BADSOCK) {
        hclose(L.us);
        L.us = BADSOCK;
    }
    if (pair) LOG("[ldn] joined a hub through the hub server as %s (node %d)%s\n", ipstr(L.own_ip), node,
                  L.up.direct_ok ? "" : " (Direct = Off: game packets only through the server)");
    else LOG("[ldn] joined %s's network as %s (node %d)\n", inet_ntoa(found[i].addr.sin_addr), ipstr(L.own_ip), node);
    signal_change();
    return 0;
}

// Connect(const NetworkInfo &, const SecurityConfig &, const UserConfig &, int version, ConnectOption)
HLE_FUNC("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", ldn_connect) { NTRACE();
    RET(connect_network(ARGP(0), ARGP(2), (uint16_t)ARG(3)));
}
HLE_FUNC("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEi", ldn_connect2) { NTRACE();
    RET(connect_network(ARGP(0), ARGP(2), (uint16_t)ARG(3)));
}
// ConnectPrivate(config, security, SecurityParameter, user, int version, ConnectOption): a network known by its
// session id (from the scan list)
static uint32_t connect_private(Cpu *c) {
    const uint8_t *session = (const uint8_t *)ARGP(2) + 0x10;
    for (int i = 0; i < nfound; i++)
        if (!memcmp(found[i].session, session, 0x10)) {
            uint8_t info[NETINFO_SIZE] = {0};
            if (found[i].ryu) memcpy(info, found[i].info, NETINFO_SIZE);
            memcpy(info + NI_BSSID, found[i].bssid, 6);
            return connect_network(info, ARGP(3), (uint16_t)STK(0));
        }
    LOG("[ldn] ConnectPrivate: no scanned network has that session\n");
    return R_CONNECT_NOT_FOUND;
}
HLE_FUNC("_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE",
         ldn_connect_private) {
    RET(connect_private(c));
}

HLE_FUNC("_ZN2nn3ldn10DisconnectEv", ldn_disconnect) { NTRACE();
    if (L.state != ST_STATION_CONNECTED && L.state != ST_ERROR) { RET(R_INVALID_STATE); return; }
    if (L.ryu) ryu_stop();
    else if (L.room) room_stop();
    else if (L.citra) citra_stop();
    else station_leave();
    L.state = ST_STATION, L.reason = DR_BY_USER;
    L.own_ip = L.subnet = 0;
    signal_change();
    RET(0);
}
