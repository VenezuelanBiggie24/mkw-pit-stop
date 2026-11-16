// What the Pit Stop's Multiplayer page shows and does (ui/menu/net_page.cpp), from the session net_ldn.c runs. Any
// thread.
#pragma once
#include <stdint.h>

#include "ldn_types.h"
#include "net_lobby.h"

typedef struct { uint32_t session; NetLobbyMeta meta; } NetLobbyEntry;
typedef struct { int node, me; uint32_t ip; char name[LDN_USERNAME]; } NetUiPlayer;

typedef struct {
    int mode;                 // 0 off, 1 LAN, 2 online (MKW_LOCAL_WIRELESS)
    int ldn_state;            // nn::ldn::State
    int hosting, lobby, public_session, locked;
    uint32_t lobby_session;   // the lobby's number for the session this console hosts, 0 none
    char password[11];        // a private session's join password ("" open)
    char invite[32];          // its invite code, "" until the lobby listed it
    int nplayers;
    NetUiPlayer players[LDN_NODES];
    uint32_t rtt_ms[LDN_NODES];   // host: per node; station: [0] to the host
    uint64_t dropped;
    int nlist;
    NetLobbyEntry list[NET_LIST_PAGE];   // the last session list the lobby sent (net_ui_refresh)
} NetUiStatus;

void net_ui_status(NetUiStatus *out);
void net_ui_kick(uint32_t ip);                   // host: remove that player (the game sees a disconnect)
void net_ui_lock(int on);                        // host: no new players
void net_ui_refresh(const NetLobbyFilter *f);    // asks the lobby for its public sessions (answer in net_ui_status)
