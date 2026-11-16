// The Multiplayer tab (net_page.h). Joining happens in the game's own Wireless Play room search, where the local
// network's rooms, the lobby's public ones and the invited one show as rooms; this page sets local wireless up and
// shows what it is doing. No player's address is ever shown.
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "ui/menu/net_page.h"
#include "ui/menu/option_rows.h"

extern "C" {
#include "cfg.h"
#include "net_ui.h"
#include "ui_text.h"
}

static const char *state_text(const NetUiStatus &s, char *buf, size_t n) {
    if (!s.mode) return menu_text(UI_NET_OFF_NOTE);
    static const char *const states[] = {"not started by the game", "ready", "opening a room", "hosting", "searching",
                                         "in a room", "error"};
    const char *st = s.ldn_state >= 0 && s.ldn_state <= 6 ? states[s.ldn_state] : "?";
    if (s.nplayers) snprintf(buf, n, "%s, %d player%s%s", st, s.nplayers, s.nplayers == 1 ? "" : "s", s.locked ? ", closed" : "");
    else snprintf(buf, n, "%s", st);
    return buf;
}

void net_page_rows(void) {
    NetUiStatus s;
    net_ui_status(&s);
    opt_group(UI_NET_GROUP_SETUP);
    static const UiTextId modes[] = {UI_NET_MODE_OFF, UI_NET_MODE_LAN, UI_NET_MODE_ONLINE};
    opt_choice("MKW_LOCAL_WIRELESS", UI_NET_MODE, modes, 3, UI_NET_MODE_DESC);
    if (!strcmp(cfg_value("MKW_LOCAL_WIRELESS"), "online")) {
        static const UiTextId hosts[] = {UI_NET_HOST_PRIVATE, UI_NET_HOST_PUBLIC};
        opt_choice("MKW_NET_HOST", UI_NET_HOST, hosts, 2, UI_NET_HOST_DESC);
        static const char *const regions[] = {"any", "eu", "na", "sa", "asia", "oc", "af"};
        static const char *const region_names[] = {"Any", "Europe", "North America", "South America", "Asia", "Oceania", "Africa"};
        opt_choice_list("MKW_NET_REGION", UI_NET_REGION, regions, region_names, 7, UI_NET_REGION_DESC);
        opt_toggle("MKW_NET_RELAY_ONLY", UI_NET_RELAY_ONLY, UI_NET_RELAY_ONLY_DESC);
        if (!s.lobby && s.mode == 2) opt_text(UI_NET_NO_LOBBY, true);
    }

    opt_group(UI_NET_GROUP_SESSION);
    char buf[96];
    opt_button("##net_status", UI_NET_STATUS, UI_NET_STATUS_DESC, state_text(s, buf, sizeof buf));
    if (s.hosting && s.invite[0] && opt_button("##net_invite", UI_NET_INVITE, UI_NET_INVITE_DESC, s.invite))
        ImGui::SetClipboardText(s.invite);
#ifndef MKW_PS5_NATIVE
    if (s.mode == 2) {
        const char *join = cfg_get("MKW_NET_JOIN");
        if (opt_button("##net_paste", UI_NET_PASTE, UI_NET_PASTE_DESC, join && *join ? join : "")) {
            const char *clip = ImGui::GetClipboardText();
            uint32_t session;
            char pw[NET_PASSWORD_LETTERS + 1];
            if (clip && strlen(clip) < 64 && net_invite_read(clip, &session, pw)) {
                cfg_set("MKW_NET_JOIN", clip);
                opt_save_soon();
            }
        }
    }
#endif
    if (s.hosting && opt_button("##net_lock", UI_NET_LOCK, UI_NET_LOCK_DESC, menu_text(s.locked ? UI_MENU_ON : UI_MENU_OFF)))
        net_ui_lock(!s.locked);
    for (int i = 0; i < s.nplayers; i++) {
        const NetUiPlayer &p = s.players[i];
        char id[16], value[64];
        snprintf(id, sizeof id, "##net_p%d", p.node);
        uint32_t rtt = s.hosting ? s.rtt_ms[p.node] : (p.node == 0 ? s.rtt_ms[0] : 0);
        if (p.me) snprintf(value, sizeof value, "%s (you)", p.name);
        else if (rtt) snprintf(value, sizeof value, "%s  %u ms", p.name, rtt);
        else snprintf(value, sizeof value, "%s", p.name);
        if (opt_button(id, UI_NET_PLAYER, UI_NET_PLAYER_DESC, value) && s.hosting && !p.me) net_ui_kick(p.ip);
    }

    if (s.mode == 2 && s.lobby) {
        opt_group(UI_NET_GROUP_ROOMS);
        if (opt_button("##net_refresh", UI_NET_REFRESH, UI_NET_REFRESH_DESC)) {
            NetLobbyFilter f = {};
            net_ui_refresh(&f);
        }
        static const char *const region_short[] = {"", "EU", "NA", "SA", "Asia", "OC", "AF"};
        for (int i = 0; i < s.nlist; i++) {
            const NetLobbyMeta &m = s.list[i].meta;
            char id[24], value[96];
            snprintf(id, sizeof id, "##net_room%08x", s.list[i].session);
            snprintf(value, sizeof value, "%.32s  %d/%d %s%s%s", m.name, m.players, m.slots,
                     m.region <= 6 ? region_short[m.region] : "", m.flags & NET_ADV_PASSWORD ? "  (invite)" : "",
                     m.flags & (NET_ADV_LOCKED | NET_ADV_FULL) ? "  (closed)" : "");
            opt_button(id, UI_NET_ROOM, UI_NET_ROOM_DESC, value);
        }
    }
}
