// Game-free placeholder UI on PS5: displays the console's IP, expected runtime version, and live Pit Stop builder
// transfer progress via ImGui, using the runtime's Vulkan presentation path. No game files are loaded here.
#include "ui/menu/ps5_placeholder.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "ui/menu/menu_look.h"

extern "C" {
#include "gfx/nvn/host/gfx.h"
#include "input/input.h"
#include "input/input_map.h"
#include "platform/platform.h"
#include "platform/ps5_recv.h"
#include "ui/menu/menu.h"
void gfx_window_release_vk(void);
}

struct PlaceholderView {
    const char *port_version;
    const char *rt_game_id;
    const char *reason;
    int has_game_code;
    char console_ip[64];
    int recv_started;
};

static void draw_placeholder(float width, float height, void *raw_ctx) {
    auto *ctx = (PlaceholderView *)raw_ctx;
    Ps5RecvStatus st = {};
    ps5_recv_status(&st);
    const float scale = menu_look_scale((uint32_t)height);

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.045f, 0.055f, 0.075f, 1.0f));
    if (ImGui::Begin("##ps5_placeholder", nullptr, flags)) {
        const float card_w = fminf(width * 0.78f, 980.0f * scale);
        const float card_h = fminf(height * 0.76f, 640.0f * scale);
        ImGui::SetCursorPos(ImVec2((width - card_w) * 0.5f, (height - card_h) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.11f, 0.15f, 0.96f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(32.0f * scale, 28.0f * scale));
        if (ImGui::BeginChild("##card", ImVec2(card_w, card_h), ImGuiChildFlags_Borders)) {
            ImGui::SetWindowFontScale(1.45f);
            ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.18f, 1.0f), "Mario Kart World: Pit Stop");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::Spacing();

            const char *heading = ctx->has_game_code ? "Game files out of date or missing" : "Game files missing";
            ImGui::TextColored(ImVec4(0.92f, 0.94f, 0.98f, 1.0f), "%s", heading);
            if (ctx->reason && ctx->reason[0]) {
                ImGui::TextColored(ImVec4(0.68f, 0.74f, 0.84f, 1.0f), "Reason: %s", ctx->reason);
            }
            ImGui::Separator();
            ImGui::Spacing();

            const char *ip = st.ip[0] ? st.ip : ctx->console_ip;
            const char *ver = (ctx->port_version && ctx->port_version[0]) ? ctx->port_version : "unknown";
            const char *gid = (ctx->rt_game_id && ctx->rt_game_id[0]) ? ctx->rt_game_id : "-";

            if (st.state == PS5_RECV_DONE) {
                ImGui::SetWindowFontScale(1.25f);
                ImGui::TextColored(ImVec4(0.32f, 0.88f, 0.48f, 1.0f), "Transfer complete!");
                ImGui::SetWindowFontScale(1.0f);
                ImGui::Spacing();
                ImGui::TextWrapped(
                    "All game files (%llu files, %.1f MiB) were received and verified. Close and relaunch "
                    "Mario Kart World: Pit Stop from the PS5 home screen to start the game.",
                    (unsigned long long)st.files_done, (double)st.bytes_done / (1024.0 * 1024.0));
                ImGui::Spacing();
                ImGui::TextDisabled("Press Cross / A to exit now.");
            } else if (st.state == PS5_RECV_ERROR) {
                ImGui::SetWindowFontScale(1.2f);
                ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "Receive error");
                ImGui::SetWindowFontScale(1.0f);
                ImGui::Spacing();
                ImGui::TextWrapped("%s", st.error[0] ? st.error : "Unknown transfer error.");
                ImGui::Spacing();
                ImGui::TextDisabled("Fix the issue and try again from the PC builder, or press Cross / A to exit.");
            } else {
                ImGui::TextWrapped(
                    "Open the Pit Stop launcher on your PC (Windows or Linux), select your Mario Kart World dump "
                    "folder or .nup file (version 1.8.0), enter this console's IP below, and click Build for PS5.");
                ImGui::Spacing();

                ImGui::SetWindowFontScale(1.35f);
                if (ip && ip[0] && strcmp(ip, "0.0.0.0") != 0) {
                    ImGui::TextColored(ImVec4(0.40f, 0.82f, 1.0f, 1.0f), "Console IP:  %s   (port %u)",
                                       ip, (unsigned)(st.port ? st.port : PS5_RECV_PORT));
                } else {
                    ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.30f, 1.0f),
                                       "Console IP unavailable - check your PS5 network connection (port %u)",
                                       (unsigned)(st.port ? st.port : PS5_RECV_PORT));
                }
                ImGui::SetWindowFontScale(1.0f);
                ImGui::Spacing();

                ImGui::Text("Expected Pit Stop runtime: %s   (game id: %s)", ver, gid);
                ImGui::Text("Required game dump: Mario Kart World 1.8.0 (or 1.0.0), dumped from your own Switch 2");
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (st.state == PS5_RECV_RECEIVING) {
                    float frac = st.bytes_total ? (float)((double)st.bytes_done / (double)st.bytes_total) : 0.0f;
                    if (frac < 0.0f) frac = 0.0f;
                    if (frac > 1.0f) frac = 1.0f;
                    char overlay[96];
                    snprintf(overlay, sizeof overlay, "%llu / %llu files  (%.1f / %.1f MiB)",
                             (unsigned long long)st.files_done, (unsigned long long)st.files_total,
                             (double)st.bytes_done / (1024.0 * 1024.0),
                             (double)st.bytes_total / (1024.0 * 1024.0));
                    ImGui::TextColored(ImVec4(0.45f, 0.85f, 1.0f, 1.0f), "Receiving from PC builder...");
                    ImGui::ProgressBar(frac, ImVec2(-1.0f, 28.0f * scale), overlay);
                    if (st.current_file[0]) ImGui::TextDisabled("Current: %s", st.current_file);
                } else {
                    ImGui::TextColored(ImVec4(0.75f, 0.80f, 0.88f, 1.0f),
                                       ctx->recv_started ? "Waiting for connection from PC Pit Stop builder..."
                                                         : "Failed to open receiver socket.");
                }
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

void ps5_placeholder_run(const char *root_dir, const char *port_version, const char *rt_game_id, int has_game_code,
                         const char *reason) {
    PlaceholderView view = {};
    view.port_version = port_version;
    view.rt_game_id = rt_game_id;
    view.reason = reason;
    view.has_game_code = has_game_code;
    ps5_console_ip(view.console_ip, sizeof view.console_ip);
    view.recv_started = ps5_recv_start(root_dir, 0, port_version, rt_game_id, has_game_code);

    menu_set_placeholder(draw_placeholder, &view);

    InputMenuPad pad = {};
    input_menu_pad(&pad);
    uint32_t prev_buttons = pad.buttons;
    const uint32_t exit_mask = (1u << PAD_SOUTH) | (1u << PAD_EAST);

    for (;;) {
        gfx_window_placeholder_present();

        Ps5RecvStatus st = {};
        ps5_recv_status(&st);

        input_menu_pad(&pad);
        uint32_t pressed = pad.buttons & ~prev_buttons;
        prev_buttons = pad.buttons;
        if ((st.state == PS5_RECV_DONE || st.state == PS5_RECV_ERROR) && (pressed & exit_mask)) break;

        plat_sleep_ns(16666667ull);
    }

    ps5_recv_stop();
    menu_set_placeholder(nullptr, nullptr);
    gfx_window_release_vk();
}
