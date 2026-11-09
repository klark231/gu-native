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

// ---- MHGU as one 3DS in that room, with Monster Hunter XX players (crates/mhgu-crossplay/src/citra_bridge.rs).
// The bridge takes the room's hooks itself: use these instead of the transport's calls above. Network infos are nn::ldn
// NetworkInfos (0x480 bytes); UDS node n is subnet | n, the host is .1.
typedef void (*UdsInfoCb)(const uint8_t *info);  // the network we're in changed; NULL: it is gone
typedef void (*UdsPacketCb)(uint32_t src_ip, uint32_t dst_ip, const uint8_t *data, uint32_t len);  // a game packet (UDP, Pia's port)
void mhgu_uds_set_hooks(UdsInfoCb info, UdsPacketCb packet);
// Joins the room (another nickname if it is taken). true: in it (or already was).
bool mhgu_uds_start(const char *server, const char *nick, const char *password, const char *token, char *err, int err_size);
void mhgu_uds_stop(void);
bool mhgu_uds_ready(void);
void mhgu_uds_mac(uint8_t *mac);  // this member's MAC (a hosted network's BSSID)
uint32_t mhgu_uds_subnet(void);   // the subnet a network this console hosts uses
int mhgu_uds_scan(uint8_t *infos, int max);  // the MHXX networks seen in the room lately
// Joins the network of bssid (blocks up to about 5 s). 0: joined (info, ip filled); 1: not found; 2: no answer / refused
uint32_t mhgu_uds_connect(const uint8_t *bssid, const uint8_t *user_name, uint8_t *info, uint32_t *ip);
bool mhgu_uds_info(uint8_t *info);  // the network we're in now
void mhgu_uds_leave(void);
bool mhgu_uds_host(const uint8_t *info);  // hosts info (node 0: this player, at mhgu_uds_subnet() | 1)
void mhgu_uds_host_update(const uint8_t *info);  // it changed: the local player and our own stations only (calls nothing back)
void mhgu_uds_host_stop(void);
void mhgu_uds_reject(uint32_t ip);
// src_ip: the local player, or (hosting in both) one of our own stations, so MHXX sees who sent it
void mhgu_uds_send(uint32_t src_ip, uint32_t dst_ip, uint16_t dst_port, const uint8_t *data, uint32_t len);
