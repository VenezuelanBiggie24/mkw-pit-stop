// Pit Stop draws over the presented game and uses physical buttons independently of game bindings.
#include "imgui.h"
#include "imgui_internal.h"   // ImClamp, ImMin, ImMax
#include "ui/glyphs/pad_icons.h"
#include "ui/menu/binds_page.h"
#include "ui/menu/menu_look.h"
#include "ui/menu/option_rows.h"
#include "ui/menu/settings_tabs.h"

extern "C" {
#include "cfg.h"
#include "input_map.h"
#include "platform/platform.h"
#include "ui/glyphs/glyphs.h"
}
#include "ui/menu/pit_stop.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

enum { HOME, ALL, DISPLAY, GRAPHICS, CONTROLS, AUDIO, GENERAL, ADVANCED, HACKS, MULTI,
       PAD_BINDS, KEY_BINDS, QUIT, RESET, RESET_BINDS, CAPTURE, PAGE_COUNT };

struct Category { int tab; UiTextId title, description; };
static const Category categories[] = {
    {TAB_SCREEN, UI_PIT_DISPLAY, UI_PIT_DISPLAY_DESC},
    {TAB_GRAPHICS, UI_MENU_TAB_GRAPHICS, UI_PIT_GRAPHICS_DESC},
    {TAB_CONTROLS, UI_MENU_TAB_CONTROLS, UI_PIT_CONTROLS_DESC},
    {TAB_SOUND, UI_PIT_AUDIO, UI_PIT_AUDIO_DESC},
    {TAB_GENERAL, UI_MENU_TAB_GENERAL, UI_PIT_GAME_DESC},
    {TAB_ADVANCED, UI_MENU_TAB_ADVANCED, UI_PIT_ADV_DESC},
    {TAB_HACKS, UI_MENU_TAB_HACKS, UI_PIT_HACKS_DESC},
    {TAB_MULTI, UI_MENU_TAB_MULTI, UI_PIT_MULTI_DESC},
};
static_assert(sizeof categories / sizeof *categories == MULTI - DISPLAY + 1);
static bool is_category(int page) { return page >= DISPLAY && page <= MULTI; }

static constexpr uint32_t button(int pad) { return 1u << pad; }
enum class Command { None, Up, Down, Left, Right, Select, Back, Previous, Next, Clear };
struct PadNav {
    uint32_t held, generation, pressed;
    int stick, direction;
    uint64_t repeat_at;
    bool armed;
};
static struct {
    OptPitPage pages[PAGE_COUNT];
    PadNav nav;
    int screen, binding_page, icon_kind, accept, back;
    bool pad_prompts, key_wait;
    uint64_t capture_end, cancel_at;
} pit;
static ImFont *heading_font;

void pit_load_fonts(float scale) {
    heading_font = menu_look_font_at(72.0f * scale);
}

void pit_open(void) {
    pit = {};
    pit.pad_prompts = input_last_device() == INPUT_DEVICE_PAD;
    binds_pit_cancel();
}

static int stick_direction(int previous, int32_t x, int32_t y) {
    int along = previous == PAD_DPAD_LEFT ? -x : previous == PAD_DPAD_RIGHT ? x
              : previous == PAD_DPAD_UP ? y : previous == PAD_DPAD_DOWN ? -y : 0;
    if (along > 11468) return previous;   // 0.35 of full travel
    if (ImMax(abs(x), abs(y)) < 16384) return PAD_NONE;   // 0.50 of full travel
    if (abs(x) > abs(y)) return x < 0 ? PAD_DPAD_LEFT : PAD_DPAD_RIGHT;
    return y > 0 ? PAD_DPAD_UP : PAD_DPAD_DOWN;
}

static Command pad_command(const InputMenuPad &pad, uint64_t now) {
    PadNav &n = pit.nav;
    if (!pad.connected || n.generation != pad.generation) {
        n = {};
        n.generation = pad.generation;
        pit.cancel_at = 0;
    }
    if (!pad.connected) return Command::None;
    n.stick = stick_direction(n.stick, pad.lx, pad.ly);
    n.pressed = n.armed ? pad.buttons & ~n.held : 0;
    n.held = pad.buttons;
    if (!n.armed) {
        if (!n.held && n.stick == PAD_NONE) n.armed = true;
        return Command::None;
    }

    int dir = n.stick;
    static constexpr uint32_t dpad = button(PAD_DPAD_LEFT) | button(PAD_DPAD_UP) |
                                     button(PAD_DPAD_RIGHT) | button(PAD_DPAD_DOWN);
    if (uint32_t digital = n.held & dpad) dir = __builtin_ctz(digital);
    bool step = dir != PAD_NONE && (dir != n.direction || now >= n.repeat_at);
    if (step) n.repeat_at = now + (dir == n.direction ? 95000000ull : 340000000ull);
    n.direction = dir;
    if (n.pressed & button(pit.back)) return Command::Back;
    if (n.pressed & button(pit.accept)) return Command::Select;
    if (n.pressed & button(PAD_L)) return Command::Previous;
    if (n.pressed & button(PAD_R)) return Command::Next;
    if (n.pressed & button(PAD_NORTH)) return Command::Clear;
    if (!step) return Command::None;
    static constexpr Command dirs[] = {Command::Left, Command::Up, Command::Right, Command::Down};
    return dirs[dir - PAD_DPAD_LEFT];
}

static bool nav_key_held(void) {
    static const ImGuiKey keys[] = {ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_LeftArrow, ImGuiKey_RightArrow,
                                  ImGuiKey_Enter, ImGuiKey_Space, ImGuiKey_Escape, ImGuiKey_Backspace,
                                  ImGuiKey_Q, ImGuiKey_E, ImGuiKey_Delete};
    for (ImGuiKey key : keys)
        if (ImGui::IsKeyDown(key)) return true;
    return false;
}

static Command key_command(void) {
    using namespace ImGui;
    if (pit.key_wait && nav_key_held()) return Command::None;
    pit.key_wait = false;
    if (IsKeyPressed(ImGuiKey_Escape, false) || IsKeyPressed(ImGuiKey_Backspace, false)) return Command::Back;
    if (IsKeyPressed(ImGuiKey_Enter, false) || IsKeyPressed(ImGuiKey_Space, false)) return Command::Select;
    if (IsKeyPressed(ImGuiKey_Q, false)) return Command::Previous;
    if (IsKeyPressed(ImGuiKey_E, false)) return Command::Next;
    if (IsKeyPressed(ImGuiKey_Delete, false)) return Command::Clear;
    if (IsKeyPressed(ImGuiKey_UpArrow)) return Command::Up;
    if (IsKeyPressed(ImGuiKey_DownArrow)) return Command::Down;
    if (IsKeyPressed(ImGuiKey_LeftArrow)) return Command::Left;
    if (IsKeyPressed(ImGuiKey_RightArrow)) return Command::Right;
    return Command::None;
}

static void go(int page) {
    pit.screen = page;
    pit.nav.armed = !pit.nav.held && pit.nav.stick == PAD_NONE;
    pit.key_wait = nav_key_held();
}

static PitResult back(void) {
    switch (pit.screen) {
    case HOME: return PitResult::Resume;
    case ALL: go(HOME); break;
    case QUIT: go(ALL); break;
    case RESET: go(ADVANCED); break;
    case RESET_BINDS: go(pit.binding_page); break;
    case PAD_BINDS: case KEY_BINDS: go(CONTROLS); break;
    case CAPTURE:
        binds_pit_cancel();
        go(pit.binding_page);
        break;
    default:
        go(ALL);
        break;
    }
    return PitResult::None;
}

static const char *screen_title(int page) {
    if (is_category(page)) return menu_text(categories[page - DISPLAY].title);
    switch (page) {
    case HOME: return menu_text(UI_PIT_TITLE);
    case ALL: return menu_text(UI_PIT_ALL);
    case PAD_BINDS: return menu_text(UI_PIT_BIND_PAD);
    case KEY_BINDS: return menu_text(UI_PIT_BIND_KEYS);
    case QUIT: return menu_text(UI_PIT_QUIT_ASK);
    case RESET: return menu_text(UI_MENU_RESET_CONFIRM);
    case RESET_BINDS: return menu_text(UI_PIT_BIND_RESET_ASK);
    default: return "";
    }
}

struct Geometry {
    float scale, left, right, top, bottom, list_top, list_w, row_h, gap, foot;
    ImVec2 help;
    float help_w, help_h;
    int visible;
};

static Geometry geometry(float w, float h) {
    float s = ImMin(h / 1080, w / 900);
    bool stacked = w < h * 1.3f;
    float margin = 96 * s, top = 66 * s, bottom = h - 68 * s;
    float list_top = top + 170 * s, foot = bottom - 65 * s;
    float width = w - 2 * margin, row_h = ImMax(1.0f, floorf(68 * s)), gap = floorf(12 * s);
    float help_h = 230 * s, help_w = ImMin(570 * s, width * 0.39f);
    float list_w = width - help_w - 64 * s;
    if (stacked) help_w = list_w = width;
    ImVec2 help(stacked ? margin : w - margin - help_w, foot - help_h - 26 * s);
    float list_end = stacked ? help.y - 30 * s : foot - 40 * s;
    int visible = ImClamp((int)((list_end - list_top + gap) / (row_h + gap)), 1, 6);
    return {s, margin, w - margin, top, bottom, list_top, list_w, row_h, gap, foot, help, help_w, help_h, visible};
}

static void quad(ImDrawList *dl, float x, float y, float w, float h, float slope, ImU32 color) {
    dl->AddQuadFilled(ImVec2(x, y), ImVec2(x + w, y), ImVec2(x + w + slope, y + h),
                      ImVec2(x + slope, y + h), color);
}

static void backdrop(float w, float h, const Geometry &g) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float s = g.scale;
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(6, 15, 32, 136));
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, h), IM_COL32(10, 26, 51, 236),
        IM_COL32(9, 23, 44, 24), IM_COL32(9, 23, 44, 42), IM_COL32(10, 26, 51, 242));
    float edge = w - 355 * s, y = h * 0.61f, depth = h - y, slope = -depth * 0.78f;
    quad(dl, edge, y, 390 * s, depth, slope, IM_COL32(26, 65, 114, 195));
    quad(dl, edge, y, 9 * s, depth, slope, IM_COL32(169, 231, 255, 185));
    for (float dy = 0; dy < depth; dy += 72 * s) {
        float dash = ImMin(33 * s, depth - dy);
        quad(dl, edge + 192 * s - dy * 0.78f, y + dy, 9 * s, dash, -dash * 0.78f,
             IM_COL32(169, 231, 255, 116));
    }
    for (int row = 0; row < 2; ++row)
        for (int col = row; col < 5; col += 2)
            quad(dl, g.left + (col * 9 - row * 2) * s, g.top + row * 9 * s, 9 * s, 9 * s, -2 * s, kPitIce);
}

static void text_fit(ImDrawList *dl, ImVec2 p, float width, float size, ImU32 color, const char *text, ImFont *font) {
    float tw = font->CalcTextSizeA(size, FLT_MAX, 0, text).x;
    if (tw > width) size *= width / tw;
    dl->AddText(font, size, p, color, text);
}

static void heading(const Geometry &g, int screen) {
    float s = g.scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = ImGui::GetFont();
    const char *title = screen_title(screen);
    char capture[220];
    if (screen == CAPTURE) {
        binds_pit_title(capture, sizeof capture);
        title = capture;
    }
    dl->AddText(font, 22 * s, ImVec2(g.left + 60 * s, g.top - 1 * s), kPitIce, "MARIO KART WORLD");
    ImVec2 pos(g.left, g.top + 31 * s);
    ImFont *large = heading_font ? heading_font : font;
    text_fit(dl, ImVec2(pos.x + 4 * s, pos.y + 5 * s), g.right - pos.x - 4 * s, 62 * s,
             IM_COL32(28, 66, 119, 255), title, large);
    text_fit(dl, pos, g.right - pos.x - 4 * s, 62 * s, kPitWhite, title, large);
    if (screen == HOME) return;
    const char *parent = screen == ALL ? menu_text(UI_PIT_TITLE) :
                         screen == PAD_BINDS || screen == KEY_BINDS ? menu_text(UI_MENU_TAB_CONTROLS) :
                         screen == CAPTURE || screen == RESET_BINDS ? screen_title(pit.binding_page) :
                         screen == RESET ? menu_text(UI_MENU_TAB_ADVANCED) : menu_text(UI_PIT_ALL);
    dl->AddText(font, 23 * s, ImVec2(g.left, g.top + 109 * s), kPitIce, parent);
}

static void help_panel(const Geometry &g, const OptPitDetail &detail) {
    if (!detail.label[0]) return;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = ImGui::GetFont();
    float s = g.scale, pad = 20 * s, room = g.help_w - 2 * pad, fs = 22 * s;
    float body_h = font->CalcTextSizeA(fs, FLT_MAX, room, detail.text).y;
    float h = body_h + 2 * pad + 33 * s;
    float bottom = g.help.y + g.help_h;
    float top = ImMax(g.help.y, bottom - h);
    if (h > g.help_h) {
        fs *= (g.help_h - 2 * pad - 33 * s) / body_h;
        top = g.help.y;
    }
    ImVec2 p(g.help.x, top), end(g.help.x + g.help_w, bottom);
    dl->AddRectFilled(p, end, IM_COL32(16, 39, 73, 255), 9 * s);
    dl->AddRect(p, end, kPitLine, 9 * s);
    dl->AddRectFilled(p, ImVec2(end.x, top + 5 * s), kPitIce, 5 * s, ImDrawFlags_RoundCornersTop);
    text_fit(dl, ImVec2(p.x + pad, top + pad), room, 26 * s, kPitIce, detail.label, font);
    dl->PushClipRect(p, end, true);
    dl->AddText(font, fs, ImVec2(p.x + pad, top + pad + 33 * s), kPitWhite, detail.text, nullptr, room);
    dl->PopClipRect();
}

static const char *face_label(int pad) {
    switch (input_pad_label(pad)) {
    case PAD_LABEL_A: return "A";
    case PAD_LABEL_B: return "B";
    case PAD_LABEL_X: return "X";
    case PAD_LABEL_Y: return "Y";
    default: return pad == pit.accept ? "A" : pad == pit.back ? "B" : "X";
    }
}

static void footer(const Geometry &g, int screen, const OptPitDetail &d) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = ImGui::GetFont();
    const float s = g.scale, fs = 21 * s, y = g.foot + 25 * s;
    dl->AddLine(ImVec2(g.left, g.foot), ImVec2(g.right, g.foot), kPitLine, 2 * s);
    struct Prompt { UiTextId label; int first, second; const char *key1, *key2; };
    Prompt prompts[6];
    int n = 0;
    if (pit.pages[screen].count > 1)
        prompts[n++] = {UI_PIT_MOVE, PAD_DPAD_UP, PAD_DPAD_DOWN, "^", "v"};
    if (d.adjust) prompts[n++] = {UI_PIT_ADJUST, PAD_DPAD_LEFT, PAD_DPAD_RIGHT, "<", ">"};
    if (d.select) prompts[n++] = {UI_PIT_SELECT, pit.accept, PAD_NONE, "Enter", nullptr};
    if (d.clear) prompts[n++] = {UI_PIT_CLEAR, PAD_NORTH, PAD_NONE, "Del", nullptr};
    if (is_category(screen)) prompts[n++] = {UI_PIT_CATEGORY, PAD_L, PAD_R, "Q", "E"};
    if (screen != CAPTURE || !pit.pad_prompts || binds_pit_keyboard())
        prompts[n++] = {screen == HOME ? UI_PIT_CLOSE : UI_LAUNCH_BACK, pit.back, PAD_NONE, "Esc", nullptr};

    auto glyph = [&](int pad, const char *key, float x, float baseline, bool paint) {
        float h = 23 * s, cy = baseline + fs * 0.5f;
        bool controller = pit.pad_prompts && pad != PAD_NONE;
        bool decorated = controller && pit.icon_kind != GLYPHS_NINTENDO;
        const char *caption = controller && !decorated && pad >= PAD_SOUTH && pad <= PAD_NORTH ? face_label(pad)
                            : controller && !decorated && pad == PAD_L ? "L"
                            : controller && !decorated && pad == PAD_R ? "R" : key;
        float w = decorated ? h * (pad == PAD_L || pad == PAD_R ? 1.6f : 1.24f)
                            : ImMax(h, font->CalcTextSizeA(fs * 0.77f, FLT_MAX, 0, caption).x + 10 * s);
        if (paint && !(decorated && pad_icon_draw(dl, ImVec2(x + w * 0.5f, cy), h, pad, pit.icon_kind))) {
            dl->AddRectFilled(ImVec2(x, cy - h * 0.5f), ImVec2(x + w, cy + h * 0.5f), kPitWhite, 4 * s);
            float tw = font->CalcTextSizeA(fs * 0.77f, FLT_MAX, 0, caption).x;
            dl->AddText(font, fs * 0.77f, ImVec2(x + (w - tw) * 0.5f, cy - fs * 0.38f), kPitInk, caption);
        }
        return w + 6 * s;
    };
    float x = g.left, line = 0;
    for (int i = 0; i < n; ++i) {
        const Prompt &p = prompts[i];
        const char *label = menu_text(p.label);
        float icons = glyph(p.first, p.key1, 0, 0, false);
        if (p.key2) icons += glyph(p.second, p.key2, 0, 0, false);
        float tw = font->CalcTextSizeA(fs, FLT_MAX, 0, label).x;
        if (x + icons + tw > g.right && x > g.left) { x = g.left; line += 29 * s; }
        x += glyph(p.first, p.key1, x, y + line, true);
        if (p.key2) x += glyph(p.second, p.key2, x, y + line, true);
        dl->AddText(font, fs, ImVec2(x, y + line), kPitWhite, label);
        x += tw + 24 * s;
    }
}

static void show_confirmation(int page) {
    pit.pages[page] = {};
    go(page);
}

static PitResult draw_rows(int screen) {
    auto action = [](UiTextId title, UiTextId desc, bool enter = false) {
        return opt_pit_action(menu_text(title), menu_text(desc), enter);
    };
    switch (screen) {
    case HOME:
        if (action(UI_PIT_RESUME, UI_PIT_RESUME_DESC)) return PitResult::Resume;
        settings_pit_quick();
        if (action(UI_PIT_ALL, UI_PIT_ALL_DESC, true)) go(ALL);
        break;
    case ALL:
        for (int i = DISPLAY; i <= MULTI; ++i)
            if (action(categories[i - DISPLAY].title, categories[i - DISPLAY].description, true)) go(i);
        if (action(UI_MENU_QUIT, UI_PIT_QUIT_DESC, true)) show_confirmation(QUIT);
        break;
    case PAD_BINDS: case KEY_BINDS:
        if (binds_pit_rows(screen == KEY_BINDS, pit.icon_kind)) {
            pit.binding_page = screen;
            show_confirmation(RESET_BINDS);
        } else if (binds_pit_active()) {
            pit.binding_page = screen;
            pit.capture_end = plat_now_ns() + 5000000000ull;
            pit.cancel_at = 0;
            input_capture_key(1, nullptr);
            go(CAPTURE);
        }
        break;
    case QUIT:
        if (action(UI_PIT_QUIT_CANCEL, UI_PIT_KEPT)) back();
        if (action(UI_MENU_QUIT, UI_PIT_QUIT_DESC)) return PitResult::Quit;
        break;
    case RESET:
        if (action(UI_PIT_KEEP_SETTINGS, UI_PIT_RETURN_ADV)) back();
        if (action(UI_MENU_RESET_YES, UI_MENU_RESET_DESC)) {
            settings_reset_all();
            back();
        }
        break;
    case RESET_BINDS:
        if (action(UI_MENU_RESET_NO, UI_PIT_BIND_DESC)) back();
        if (action(UI_MENU_RESET_YES, UI_PIT_BIND_RESET_DESC)) {
            binds_reset();
            back();
        }
        break;
    default:
        if (settings_rows(categories[screen - DISPLAY].tab, true)) show_confirmation(RESET);
        if (screen == CONTROLS) {
            if (action(UI_PIT_BIND_PAD, UI_PIT_BIND_DESC, true)) go(PAD_BINDS);
#ifndef MKW_PS5_NATIVE
            if (action(UI_PIT_BIND_KEYS, UI_PIT_BIND_DESC, true)) go(KEY_BINDS);
#endif
        }
        break;
    }
    return PitResult::None;
}

static void capture(const InputMenuPad &pad, uint64_t now) {
    int id = 0;
    int got = input_capture_key(0, &id);
    if (got == INPUT_CAPTURE_CANCEL || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || now >= pit.capture_end) {
        back();
        return;
    }
    bool keyboard = binds_pit_keyboard();
    if (!pad.connected || !pit.nav.armed) pit.cancel_at = 0;
    uint32_t pressed = pit.nav.pressed;
    if ((pad.buttons & (button(PAD_START) | button(PAD_BACK))) == (button(PAD_START) | button(PAD_BACK)))
        pressed &= ~(button(PAD_START) | button(PAD_BACK));
    uint32_t cancel = button(pit.back);
    if (keyboard && (pressed & cancel)) {
        back();
        return;
    }
    if (pressed & cancel) pit.cancel_at = now;
    if (pit.cancel_at && now - pit.cancel_at >= 600000000ull) {
        back();
    } else if (keyboard && got == INPUT_CAPTURE_KEY) {
        binds_pit_apply(id);
        go(pit.binding_page);
    } else if (!keyboard && pit.cancel_at && !(pad.buttons & cancel)) {
        binds_pit_apply(pit.back);
        go(pit.binding_page);
    } else if (!keyboard && !pit.cancel_at && pressed) {
        binds_pit_apply(__builtin_ctz(pressed));
        go(pit.binding_page);
    }
}

PitResult pit_frame(float width, float height, const InputMenuPad &pad, int test_page) {
    if (width <= 0 || height <= 0) return PitResult::None;
    uint64_t now = plat_now_ns();
    pit.accept = pad.east_confirm ? PAD_EAST : PAD_SOUTH;
    pit.back = pad.east_confirm ? PAD_SOUTH : PAD_EAST;
    // The prompts' icons follow the buttons' labels, not the layout: a DualSense in the Nintendo layout shows its shapes.
    pit.icon_kind = input_pad_label(PAD_SOUTH) == PAD_LABEL_CROSS ? GLYPHS_PLAYSTATION
                  : input_pad_label(PAD_EAST) == PAD_LABEL_A    ? GLYPHS_NINTENDO
                                                                : GLYPHS_XBOX;
    Command key = Command::None, cmd = Command::None;
    if (test_page >= 0) {
        for (int i = DISPLAY; i <= MULTI; ++i)
            if (categories[i - DISPLAY].tab == test_page) pit.screen = i;
    } else {
        Command physical = pad_command(pad, now);
        key = key_command();
        cmd = key != Command::None ? key : physical;
        if (cmd != Command::None) pit.pad_prompts = key == Command::None;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::GetIO().MouseWheel) pit.pad_prompts = false;
        if (!pad.connected) pit.pad_prompts = false;
        if (pit.screen == CAPTURE) {
            capture(pad, now);
            cmd = Command::None;
        } else if (cmd == Command::Back) {
            PitResult result = back();
            if (result != PitResult::None) return result;
            cmd = Command::None;
        } else if (is_category(pit.screen) && (cmd == Command::Previous || cmd == Command::Next)) {
            int next = ImClamp(pit.screen + (cmd == Command::Next ? 1 : -1), (int)DISPLAY, (int)MULTI);
            if (next != pit.screen) {
                pit.pages[ALL].row = next - DISPLAY;
                go(next);
            }
            cmd = Command::None;
        }
    }

    int screen = pit.screen;
    Geometry g = geometry(width, height);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##pit_stop", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    backdrop(width, height, g);
    heading(g, screen);
    ImGui::PushID(screen);
    OptPitPage &page = pit.pages[screen];
    OptPitInput in = {cmd == Command::Up ? -1 : cmd == Command::Down ? 1 : 0,
                      cmd == Command::Left ? -1 : cmd == Command::Right ? 1 : 0,
                      cmd == Command::Select, cmd == Command::Clear};
    if (ImGui::GetIO().MouseWheel && screen != CAPTURE)
        in.move = ImGui::GetIO().MouseWheel > 0 ? -1 : 1;
    opt_pit_begin(page, {ImVec2(g.left, g.list_top), g.list_w, g.row_h, g.gap, g.scale, g.visible}, in);
    PitResult result = PitResult::None;
    if (screen == CAPTURE) {
        char label[140], hold[192], hint[256];
        int left = ImMax(1, (int)(pit.capture_end > now ? (pit.capture_end - now + 999999999ull) / 1000000000ull : 0));
        snprintf(label, sizeof label, menu_text(UI_BINDS_CANCEL_HINT), left);
        if (!binds_pit_keyboard()) {
            const char *cancel = pit.icon_kind == GLYPHS_PLAYSTATION ? menu_text(UI_PAD_CIRCLE) : "B";
            snprintf(hold, sizeof hold, menu_text(UI_PIT_BIND_CANCEL), cancel);
            snprintf(hint, sizeof hint, "%s\n\n%s", hold, label);
        } else {
            snprintf(hint, sizeof hint, "%s", label);
        }
        if (opt_pit_action(menu_text(UI_MENU_RESET_NO), hint)) back();
    } else {
        result = draw_rows(screen);
    }
    OptPitDetail detail = opt_pit_end();
    if (screen == CAPTURE) detail.select = false;
    help_panel(g, detail);
    if (page.count > g.visible) {
        char range[48];
        snprintf(range, sizeof range, menu_text(UI_PIT_ARGS_RANGE), page.top + 1,
                 ImMin(page.top + g.visible, page.count), page.count);
        ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), 20 * g.scale,
            ImVec2(g.left, g.list_top + g.visible * (g.row_h + g.gap)), kPitIce, range);
    }
    footer(g, screen, detail);
    if (!cfg_last_save_ok()) {
        float fs = 19 * g.scale;
        ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), fs, ImVec2(g.left, g.bottom + 23 * g.scale),
            IM_COL32(255, 172, 150, 255), menu_text(UI_PIT_SAVE_FAILED), nullptr, g.right - g.left);
    }
    ImGui::PopID();
    ImGui::End();
    ImGui::PopStyleVar();
    return result;
}

// The boot screen's page: Pit Stop's backdrop and heading over an opaque base (no game image behind it yet), one row
// per boot task styled as Pit Stop's option rows, the running one in the focus colour with its bar along the bottom.
void pit_loading_frame(float width, float height, const char *title, const PitLoadingRow *rows, int n,
                       const char *foot, const char *hint) {
    Geometry g = geometry(width, height);
    const float s = g.scale;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##pit_loading", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = ImGui::GetFont();
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(width, height), IM_COL32(6, 15, 32, 255));
    backdrop(width, height, g);

    dl->AddText(font, 22 * s, ImVec2(g.left + 60 * s, g.top - 1 * s), kPitIce, "MARIO KART WORLD");
    ImVec2 pos(g.left, g.top + 31 * s);
    ImFont *large = heading_font ? heading_font : font;
    text_fit(dl, ImVec2(pos.x + 4 * s, pos.y + 5 * s), g.right - pos.x - 4 * s, 62 * s, IM_COL32(28, 66, 119, 255),
             title, large);
    text_fit(dl, pos, g.right - pos.x - 4 * s, 62 * s, kPitWhite, title, large);

    const double t = ImGui::GetTime();
    const float r = 7 * s, pad = 20 * s, fs = 30 * s, status_fs = 23 * s;
    for (int i = 0; i < n; ++i) {
        const PitLoadingRow &row = rows[i];
        bool run = row.state == 1, done = row.state == 2;
        ImVec2 p(g.left, g.list_top + i * (g.row_h + g.gap)), end(g.left + g.list_w, p.y + g.row_h);
        dl->AddRectFilled(ImVec2(p.x, p.y + 3 * s), ImVec2(end.x, end.y + 3 * s),
                          run ? IM_COL32(171, 117, 25, 255) : IM_COL32(5, 15, 29, 255), r);
        dl->AddRectFilled(p, end, run ? kPitFocus : kPitNavy, r);
        dl->AddRect(p, end, run ? kPitFocus : kPitLine, r);
        dl->AddRectFilled(p, ImVec2(p.x + 6 * s, end.y), run ? IM_COL32(255, 248, 212, 255) : done ? kPitIce : kPitLine,
                          r, ImDrawFlags_RoundCornersLeft);
        ImU32 ink = run ? kPitInk : done ? kPitWhite : IM_COL32(150, 165, 190, 255);
        float center = (p.y + end.y) * 0.5f;
        float status_w = row.status ? font->CalcTextSizeA(status_fs, FLT_MAX, 0, row.status).x : 0;
        text_fit(dl, ImVec2(p.x + pad + 6 * s, center - fs * 0.5f), end.x - p.x - 3 * pad - status_w, fs, ink,
                 row.label, font);
        if (row.status)
            dl->AddText(font, status_fs, ImVec2(end.x - pad - status_w, center - status_fs * 0.5f),
                        run ? kPitInk : done ? kPitIce : IM_COL32(150, 165, 190, 255), row.status);
        if (run) {   // the bar along the row's bottom edge, inside its border
            float x0 = p.x + 6 * s, x1 = end.x - r, y0 = end.y - 7 * s, y1 = end.y - 3 * s, span = x1 - x0;
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(171, 117, 25, 255), 2 * s);
            if (row.progress >= 0) {
                dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + span * ImClamp(row.progress, 0.0f, 1.0f), y1), kPitInk,
                                  2 * s);
            } else {   // the total is unknown: a segment a quarter as wide sweeps across every 1.6 s
                float phase = (float)fmod(t / 1.6, 1.0), seg = span * 0.25f;
                float a = x0 + (span + seg) * phase - seg;
                dl->AddRectFilled(ImVec2(ImMax(a, x0), y0), ImVec2(ImMin(a + seg, x1), y1), kPitInk, 2 * s);
            }
        }
    }

    dl->AddLine(ImVec2(g.left, g.foot), ImVec2(g.right, g.foot), kPitLine, 2 * s);
    float foot_w = foot && *foot ? font->CalcTextSizeA(21 * s, FLT_MAX, 0, foot).x : 0;
    if (foot_w) dl->AddText(font, 21 * s, ImVec2(g.left, g.foot + 25 * s), kPitIce, foot);
    if (hint && *hint) {   // at the right of the footer line; under it when both do not fit side by side
        float hint_w = font->CalcTextSizeA(21 * s, FLT_MAX, 0, hint).x;
        bool beside = g.left + foot_w + 40 * s + hint_w <= g.right;
        ImVec2 at = beside ? ImVec2(g.right - hint_w, g.foot + 25 * s) : ImVec2(g.left, g.foot + 52 * s);
        dl->AddText(font, 21 * s, at, kPitFocus, hint);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}
