// A Citra-family (Azahar, Citra) room as a member: crates/mhgu-crossplay/src/citra.rs. The transport only: the room
// carries 802.11 frames between its members, addressed by MAC; their content (UDS) is the caller's.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// A frame from the room: type (0 beacon, 1 data, 2 authentication, 3 association response, 4 deauthentication,
// 5 node map), channel, transmitter MAC, destination MAC, the frame, its size.
typedef void (*CitraFrameCb)(uint8_t type, uint8_t channel, const uint8_t *tx, const uint8_t *dst, const uint8_t *data, uint32_t len);
typedef void (*CitraLostCb)(void);
typedef void (*CitraMembersCb)(const uint8_t (*macs)[6], int n);  // the room's members' MACs (this one included)

void mhgu_citra_set_hooks(CitraFrameCb frame, CitraLostCb lost, CitraMembersCb members);
// host[:port] (default 24872); token may be "" (anonymous). true: joined, mac (6 bytes) is the address the room gave.
bool mhgu_citra_join(const char *server, const char *nick, const char *password, const char *token, uint8_t *mac, char *err, int err_size);
void mhgu_citra_leave(void);
bool mhgu_citra_connected(void);
void mhgu_citra_own_mac(uint8_t *mac);
// dst: one member's MAC, or ff:ff:ff:ff:ff:ff for all the others
void mhgu_citra_send_frame(uint8_t type, uint8_t channel, const uint8_t *tx, const uint8_t *dst, const uint8_t *data, uint32_t len);
