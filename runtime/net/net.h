// Local play (the Switch's local wireless, nn::ldn) over IP: one player's game hosts a hub, the others join it by
// address. ldn.c is nn::ldn plus the link between the games (TCP: each client keeps one connection to the host,
// which relays); vsock.c is nn::socket on a virtual network: the sockets the game opens for its own traffic exist
// only here, their packets travel inside the link. Addresses are the ones a Switch would use (169.254.x.n, the
// host n = 1), so the game sees the network it expects.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A packet of the virtual network (fields in host byte order, as sent inside the link after the frame header).
enum { VPROTO_UDP = 0, VPROTO_TCP = 1 };
enum { VTCP_DATA = 0, VTCP_SYN, VTCP_SYNACK, VTCP_FIN, VTCP_RST };
typedef struct {
    uint32_t src_ip, dst_ip;
    uint16_t src_port, dst_port;
    uint8_t proto, kind;  // VPROTO_*; for TCP VTCP_*
    uint16_t pad;
} VPacket;

// MHGU_LOG_NET=2: every nn::ldn / nn::socket call (the first 40 of each, then every 1000th), with r0-r2.
#define NTRACE()                                                                                                   \
    do {                                                                                                           \
        const char *nt_ = ENV("MHGU_LOG_NET");                                                                     \
        static int ntn_;                                                                                           \
        if (nt_ && atoi(nt_) >= 2 && (ntn_++ < 40 || ntn_ % 1000 == 0))                                            \
            LOG("[net] %s #%d (%x, %x, %x)\n", __func__, ntn_, ARG(0), ARG(1), ARG(2));                            \
    } while (0)

// ldn.c
bool ldn_net_up(void);           // a network exists (hosting with it created, or joined)
bool ldn_hosting(void);          // we host it
bool ldn_crossplay(void);        // emulator peers (Ryujinx, a Yuzu-family room): port-specific monster / HP synchronization is off
bool ldn_hub_browsing(void);     // the Join Hub screen is open: searching, not joined
int ldn_hub_lists(void);         // how many lists of hubs the last search had
bool ldn_hub_list_goto(int n);   // a tap on a list's number (0 based)
void ldn_hub_list_range(int *first, int *last, int *total); // the hubs of the chosen list: #first to #last of total
int ldn_hub_list_chosen(void);   // the list the next search gives (0 based)
bool ldn_hub_list_pending(void); // one was chosen that is not on the screen yet
bool ldn_hub_list_step(int dir); // the arrows there: the next search gives the previous / next list of hubs (false: one list only)
// The hub's players and their maximum, and an id for it (a hash of its session id: the same in every player's
// game); false: no hub.
bool ldn_party(int *n, int *max, char *id, size_t size);
uint32_t ldn_own_ip(void);        // 0 without a network
uint32_t ldn_broadcast_ip(void);
void ldn_send(const VPacket *p, const void *data, uint32_t len);  // from a local socket: into the network
void ldn_send_aux(const void *data, uint32_t len);  // the port's own message to every other game (reliable)
// The hub server (internet hubs) for the settings menu: its last known state for these settings (ini Server, Key),
// from a search, a hosted hub or a check; ms: the answer's round trip, hubs: listed for the key (-1: not known).
enum { HUB_UNKNOWN, HUB_CHECKING, HUB_OFF, HUB_ONLINE, HUB_NOT_FOUND, HUB_UNREACHABLE, HUB_LOST, HUB_NO_ANSWER,
       HUB_BUSY, HUB_VERSION, HUB_REFUSED };
int ldn_hub_status(const char *server, const char *key, int *ms, int *hubs);  // HUB_*
void ldn_hub_check(const char *server, const char *key);  // asks the server for its hubs, in a thread
// The Join Hub screen's lists of hubs (more found than the game shows at once): runtime/touch.cpp's number strip and
// host_input.c's Retry for the player use these.
bool ldn_hub_browsing(void);
int ldn_hub_lists(void);
int ldn_hub_list_chosen(void);
bool ldn_hub_list_pending(void);
bool ldn_hub_list_goto(int n);
bool ldn_hub_list_step(int dir);
void ldn_hub_list_range(int *first, int *last, int *total);
// The room connections the saved crossplay settings keep (for the settings menu): ROOMS_YUZU, ROOMS_CITRA -> ROOM_*
enum { ROOMS_YUZU, ROOMS_CITRA };
enum { ROOM_OFF, ROOM_JOINING, ROOM_IN };
int ldn_room_state(int kind);

// The Room Browsers (runtime/net/webapi.c): a Web API's public room list (GET <url>/lobby), fetched in a thread.
// WEBROOMS_YUZU: the Eden Web API (Yuzu-family rooms). WEBROOMS_CITRA: the Citra Web APIs (Azahar / Citra rooms, MH XX);
// two URLs, the rooms of both in one list.
typedef struct {
    char name[64], owner[48], game[48], address[64];
    uint16_t port;
    int players, max_players;
    bool locked;  // has a password
    uint32_t net_version;  // the room's netVersion (Citra: 4 for Azahar), 0 if the API says nothing
} WebRoom;
enum { WEBROOMS_YUZU, WEBROOMS_CITRA, WEBROOMS_KINDS };
enum { WEBROOMS_IDLE, WEBROOMS_LOADING, WEBROOMS_OK, WEBROOMS_ERROR };
#define WEBROOMS_MAX 128
void webrooms_refresh(const char *url);             // starts a fetch of the Yuzu list (ignored while one runs)
int webrooms_state(char *err, size_t err_size);     // WEBROOMS_*; err: why, after an error
int webrooms_get(WebRoom *out, int max);            // the last list; its size
void webrooms_refresh_kind(int kind, const char *url, const char *url2);  // url2 may be NULL / empty
int webrooms_state_kind(int kind, char *err, size_t err_size);
int webrooms_get_kind(int kind, WebRoom *out, int max);

// vsock.c
void vsock_deliver(const VPacket *p, const void *data, uint32_t len);  // from the network to the local sockets
void vsock_peer_gone(uint32_t ip);  // a node left: its TCP connections end
void vsock_network_down(void);      // the network is gone: every connection ends, queues empty

// emsync.c: small monsters synced between the players in a zone
void emsync_tick(void);             // each frame, from the game's input polling
void emsync_aux_received(const void *data, uint32_t len);  // an F_AUX message from another game

// hle/hpscale.c: large monster HP by the number of hunters; the host's settings go to the others
void hpscale_tick(void);            // each frame, from the game's input polling
void hpscale_aux_received(const void *data, uint32_t len);

#ifdef __cplusplus
}
#endif
