// nn::ldn's structures as the game passes them (local wireless play), byte offsets only, no packing pragmas: the
// session engine (net_session.c), the nn::ldn replacements (net_ldn.c) and the lobby read and write them through these
// offsets. Layouts from Ryujinx src/Ryujinx.HLE/HOS/Services/Ldn/Types/*.cs @ 6af327bd (MIT, Copyright (c) Ryujinx Team
// and Contributors), which match libnx's ldn.h; the Switch 2 SDK in MKW keeps the same public calls (q.py imports
// nn::ldn: 25, the Switch 1 names) and the same "ldn:u" service.
#pragma once
#include <stdint.h>
#include <string.h>

enum {
    LDN_NODES = 8,                 // NodeCountMax: the host and 7 stations
    LDN_ADVERTISE_MAX = 0x180,
    LDN_USERNAME = 0x21,           // 32 bytes and a NUL
    LDN_PASSPHRASE_MAX = 0x40,

    LDN_NETWORK_INFO = 0x480,      // NetworkId 0x20, CommonNetworkInfo 0x30, LdnNetworkInfo 0x430
    LDN_NETWORK_CONFIG = 0x20,
    LDN_SECURITY_CONFIG = 0x44,
    LDN_SECURITY_PARAMETER = 0x20,
    LDN_USER_CONFIG = 0x30,
    LDN_SCAN_FILTER = 0x60,
    LDN_NODE_INFO = 0x40,

    // NetworkInfo
    NI_INTENT = 0x00,              // IntentId: u64 LocalCommunicationId, u16, u16 SceneId, u32
    NI_LCID = 0x00, NI_SCENE = 0x0a,
    NI_SESSION_ID = 0x10,          // 16 bytes
    NI_MAC = 0x20,                 // CommonNetworkInfo
    NI_SSID_LEN = 0x26, NI_SSID = 0x27,   // u8 length, 33 bytes
    NI_CHANNEL = 0x48, NI_LINK_LEVEL = 0x4a, NI_NETWORK_TYPE = 0x4b,
    NI_SEC_PARAM = 0x50,           // LdnNetworkInfo: SecurityParameter data, 16 bytes
    NI_SEC_MODE = 0x60, NI_ACCEPT_POLICY = 0x62, NI_NODE_MAX = 0x66, NI_NODE_COUNT = 0x67,
    NI_NODES = 0x68,               // 8 NodeInfo
    NI_ADV_SIZE = 0x26a, NI_ADV = 0x26c,
    NI_AUTH_ID = 0x478,

    // NodeInfo
    NODE_IP = 0x00, NODE_MAC = 0x04, NODE_ID = 0x0a, NODE_CONNECTED = 0x0b, NODE_NAME = 0x0c, NODE_LCV = 0x2e,

    // NetworkConfig
    NC_INTENT = 0x00, NC_CHANNEL = 0x10, NC_NODE_MAX = 0x12, NC_LCV = 0x14,
    // SecurityConfig
    SC_MODE = 0x00, SC_PASS_SIZE = 0x02, SC_PASS = 0x04,
    // SecurityParameter
    SP_DATA = 0x00, SP_SESSION_ID = 0x10,
    // ScanFilter
    SF_NETWORK_ID = 0x00, SF_NETWORK_TYPE = 0x20, SF_MAC = 0x24, SF_SSID = 0x2a, SF_FLAGS = 0x5c,
};

// ScanFilter flags
enum { SF_LCID = 1, SF_SESSION = 2, SF_NETTYPE = 4, SF_BY_MAC = 8, SF_BY_SSID = 16, SF_SCENE = 32 };
// nn::ldn::State
enum { LDN_STATE_NONE, LDN_STATE_INITIALIZED, LDN_STATE_AP, LDN_STATE_AP_CREATED, LDN_STATE_STATION,
       LDN_STATE_STATION_CONNECTED, LDN_STATE_ERROR };
// nn::ldn::DisconnectReason
enum { LDN_DR_NONE, LDN_DR_USER, LDN_DR_SYSTEM, LDN_DR_DESTROYED_USER, LDN_DR_DESTROYED_SYSTEM, LDN_DR_REJECTED,
       LDN_DR_SIGNAL_LOST };
// nn::ldn::AcceptPolicy
enum { LDN_ACCEPT_ALL, LDN_REJECT_ALL, LDN_BLACKLIST, LDN_WHITELIST };

static inline uint16_t ld16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t ld32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t ld64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline void st16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static inline void st32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void st64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static inline uint8_t *ni_node(uint8_t *ni, int i) { return ni + NI_NODES + LDN_NODE_INFO * i; }
static inline const uint8_t *ni_node_c(const uint8_t *ni, int i) { return ni + NI_NODES + LDN_NODE_INFO * i; }
