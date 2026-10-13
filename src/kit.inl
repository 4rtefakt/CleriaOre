// CleriaOre's UI kit: the look of the F1 panel in the game and of the manager, after Ys Origin's own
// menus (its Option page and pause book): a pale stone page under a dark blue band trimmed in gold, the
// page's name on a black plate set into the band, dark plates with pointed ends for what can be chosen,
// flat dark slabs for what is only read, a serif face, a help bar along the bottom.
//
// The API is the one of CleriaCore's kit (its apps/cleria-view/view_ui.inl: List, the row functions, Panel),
// so a mod's page (cleria.ui.page) is drawn by either. Dear ImGui is the engine underneath (text, clipping,
// input); none of its own widgets are drawn.
#include "imgui_internal.h"   // ImGui::ShadeVertsLinearColorGradientKeepAlpha

namespace ui {

// ---- palette -----------------------------------------------------------------------------------------
// The names CleriaCore's kit has (a mod's HUD layer asks for them by name, host.cpp named_colors): each
// reads on a dark ground.
constexpr ImU32 kDim        = IM_COL32(0, 0, 0, 120);       // over the game behind the panel
constexpr ImU32 kPanel      = IM_COL32(14, 20, 36, 240);
constexpr ImU32 kPanelEdge  = IM_COL32(190, 160, 90, 235);
constexpr ImU32 kRule       = IM_COL32(170, 142, 74, 255);
constexpr ImU32 kDivider    = IM_COL32(60, 68, 92, 255);
constexpr ImU32 kGold       = IM_COL32(222, 192, 112, 255);
constexpr ImU32 kTabOff     = IM_COL32(176, 160, 118, 255);
constexpr ImU32 kTabOn      = IM_COL32(250, 248, 240, 255);
constexpr ImU32 kSection    = IM_COL32(184, 162, 108, 255);
constexpr ImU32 kRow        = IM_COL32(20, 26, 42, 255);
constexpr ImU32 kRowEdge    = IM_COL32(86, 92, 116, 255);
constexpr ImU32 kRowSel     = IM_COL32(40, 62, 124, 255);
constexpr ImU32 kLabel      = IM_COL32(246, 244, 236, 255);
constexpr ImU32 kValue      = IM_COL32(216, 220, 230, 255);
constexpr ImU32 kMuted      = IM_COL32(146, 152, 172, 255);
constexpr ImU32 kDesc       = IM_COL32(228, 230, 236, 255);
constexpr ImU32 kGood       = IM_COL32(158, 220, 138, 255);
constexpr ImU32 kWarn       = IM_COL32(242, 184, 76, 255);
// The skin's own.
constexpr ImU32 kPageTop    = IM_COL32(232, 240, 239, 255);   // the pale stone page
constexpr ImU32 kPageBot    = IM_COL32(196, 213, 221, 255);
constexpr ImU32 kPageLine   = IM_COL32(120, 150, 170, 46);    // its faint rings
constexpr ImU32 kInk        = IM_COL32(26, 36, 66, 255);      // text on the page
constexpr ImU32 kBandTop    = IM_COL32(12, 18, 36, 255);      // the band and the help bar
constexpr ImU32 kBandBot    = IM_COL32(34, 50, 88, 255);
constexpr ImU32 kGoldDark   = IM_COL32(96, 74, 32, 255);
constexpr ImU32 kPlateTop   = IM_COL32(52, 58, 76, 255);      // a plate: lit from above
constexpr ImU32 kPlateBot   = IM_COL32(12, 15, 26, 255);
constexpr ImU32 kPlateSelTop = IM_COL32(86, 118, 190, 255);   // the chosen one is blue, as the game's
constexpr ImU32 kPlateSelBot = IM_COL32(28, 44, 104, 255);
constexpr ImU32 kShadow     = IM_COL32(0, 0, 0, 170);

struct Fonts {
    ImFont* body = nullptr; ImFont* medium = nullptr; ImFont* bold = nullptr;   // Fira Sans: values, descriptions,
    ImFont* cond = nullptr; ImFont* cond_med = nullptr;                         // and a mod's HUD (fonts 0..4)
    ImFont* serif = nullptr; ImFont* serif_bold = nullptr; ImFont* serif_italic = nullptr;   // the skin's face
};
inline Fonts F;
inline float S = 1.0f;          // UI scale: the window height over the 1000-px reference

// Fira Sans / Fira Sans Condensed (SIL OFL) from `dir`; the serif is Windows' own Palatino Linotype (not
// shipped: where it is missing, as under Wine, the sans stands in).
inline bool load_fonts(const fs::path& dir) {
    ImGuiIO& io = ImGui::GetIO();
    auto load = [&](const fs::path& p) -> ImFont* {
        std::error_code ec;
        if (!fs::exists(p, ec)) return nullptr;
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        return io.Fonts->AddFontFromFileTTF(p.string().c_str(), 18.0f, &cfg);
    };
    F.body = load(dir / "FiraSans-Regular.ttf");
    F.medium = load(dir / "FiraSans-Medium.ttf");
    F.bold = load(dir / "FiraSans-SemiBold.ttf");
    F.cond = load(dir / "FiraSansCondensed-SemiBold.ttf");
    F.cond_med = load(dir / "FiraSansCondensed-Medium.ttf");
    ImFont* fallback = io.Fonts->AddFontDefault();
    for (ImFont** f : {&F.body, &F.medium, &F.bold, &F.cond, &F.cond_med}) if (!*f) *f = fallback;
    const char* windir = std::getenv("WINDIR");
    const fs::path sys = fs::path(windir ? windir : "C:\\Windows") / "Fonts";
    F.serif = load(sys / "pala.ttf");
    F.serif_bold = load(sys / "palab.ttf");
    F.serif_italic = load(sys / "palai.ttf");
    if (!F.serif) F.serif = F.body;
    if (!F.serif_bold) F.serif_bold = F.serif == F.body ? F.medium : F.serif;
    if (!F.serif_italic) F.serif_italic = F.serif;
    return F.body != fallback;
}

inline std::string upper(const char* s) {
    std::string u(s);
    for (char& c : u) c = char(std::toupper(static_cast<unsigned char>(c)));
    return u;
}
inline ImVec2 text_size(ImFont* f, float px, const char* t) { return f->CalcTextSizeA(px, FLT_MAX, 0.0f, t); }
inline void text(ImDrawList* dl, ImFont* f, float px, ImVec2 p, ImU32 c, const char* t) { dl->AddText(f, px, p, c, t); }
// Text on a plate: with the game's drop shadow.
inline void text_sh(ImDrawList* dl, ImFont* f, float px, ImVec2 p, ImU32 c, const char* t) {
    dl->AddText(f, px, {p.x + 1.2f * S, p.y + 1.2f * S}, kShadow, t);
    dl->AddText(f, px, p, c, t);
}

// A filled shape lit from above: `top` at a.y down to `bot` at b.y.
inline void shade(ImDrawList* dl, int v0, float y0, float y1, ImU32 top, ImU32 bot) {
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, {0, y0}, {0, y1}, top, bot);
}
// A plate with pointed ends and a gold bevel (the game's menu rows); the chosen one is blue.
inline void plate(ImDrawList* dl, ImVec2 a, ImVec2 b, bool selected) {
    const float cy = (a.y + b.y) * 0.5f, c = (b.y - a.y) * 0.34f;
    const ImVec2 p[6] = {{a.x + c, a.y}, {b.x - c, a.y}, {b.x, cy}, {b.x - c, b.y}, {a.x + c, b.y}, {a.x, cy}};
    const int v0 = dl->VtxBuffer.Size;
    dl->AddConvexPolyFilled(p, 6, IM_COL32_WHITE);
    shade(dl, v0, a.y, b.y, selected ? kPlateSelTop : kPlateTop, selected ? kPlateSelBot : kPlateBot);
    dl->AddPolyline(p, 6, kGoldDark, ImDrawFlags_Closed, 3.0f * S);
    dl->AddPolyline(p, 6, selected ? kGold : kRule, ImDrawFlags_Closed, 1.1f * S);
}
// A flat dark slab in a thin frame (the pause book's stat panel): what is read, not chosen.
inline void slab(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    dl->AddRectFilledMultiColor(a, b, IM_COL32(30, 36, 54, 255), IM_COL32(30, 36, 54, 255), IM_COL32(10, 13, 24, 255), IM_COL32(10, 13, 24, 255));
    dl->AddRect(a, b, kGoldDark, 0.0f, 0, 1.6f * S);
}

// ---- input, read once per frame ----------------------------------------------------------------------
struct Nav {
    bool up = false, down = false, left = false, right = false, ok = false, back = false;
    bool tab_prev = false, tab_next = false;
};
// The mouse over [a, b]. Unclipped: the kit draws outside any ImGui window.
inline bool hovering(const ImVec2& a, const ImVec2& b) { return ImGui::IsMouseHoveringRect(a, b, false); }
inline void hand() { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); }
inline int text_edit_frame = -100;   // while a text_field is edited the keys are typing, not navigation

inline Nav read_nav() {
    Nav n;
    if (ImGui::GetFrameCount() - text_edit_frame <= 1) return n;
    auto pr = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
    n.up = pr(ImGuiKey_UpArrow) || pr(ImGuiKey_W) || pr(ImGuiKey_GamepadDpadUp) || pr(ImGuiKey_GamepadLStickUp);
    n.down = pr(ImGuiKey_DownArrow) || pr(ImGuiKey_S) || pr(ImGuiKey_GamepadDpadDown) || pr(ImGuiKey_GamepadLStickDown);
    n.left = pr(ImGuiKey_LeftArrow) || pr(ImGuiKey_A) || pr(ImGuiKey_GamepadDpadLeft) || pr(ImGuiKey_GamepadLStickLeft);
    n.right = pr(ImGuiKey_RightArrow) || pr(ImGuiKey_D) || pr(ImGuiKey_GamepadDpadRight) || pr(ImGuiKey_GamepadLStickRight);
    n.ok = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
           ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false);
    n.back = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false) ||
             ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
    n.tab_prev = pr(ImGuiKey_Q) || pr(ImGuiKey_GamepadL1) || pr(ImGuiKey_PageUp);
    n.tab_next = pr(ImGuiKey_E) || pr(ImGuiKey_GamepadR1) || pr(ImGuiKey_PageDown) || ImGui::IsKeyPressed(ImGuiKey_Tab, true);
    return n;
}

// ---- the row list of one page --------------------------------------------------------------------------
// Immediate mode: a page calls the row functions every frame in order; the list numbers them, draws them,
// keeps the selected one in view and reports its description.
struct List {
    int sel = 0;                    // the selected row (keyboard / pad focus)
    float scroll = 0, scroll_to = 0;
    // per frame
    ImDrawList* dl = nullptr;
    ImVec2 org{}, size{};
    float y = 0;                    // layout cursor (content space)
    int n = 0;                      // rows laid out so far
    int count_last = 0;             // rows of the previous frame
    float content_h = 0;
    std::string desc;
    Nav nav{};
    bool focus = true;              // the panel has input
    bool mouse_moved = false;
    std::vector<float> row_y;       // each row's content y this frame (for keep-in-view)
    std::vector<bool> focusable, focusable_last;   // read-only rows are skipped by the keys
};

inline float row_h() { return 34.0f * S; }
inline float row_gap() { return 6.0f * S; }

inline void list_begin(List& L, ImDrawList* dl, ImVec2 org, ImVec2 size, const Nav& nav, bool focus) {
    L.dl = dl; L.org = org; L.size = size; L.nav = nav; L.focus = focus;
    L.y = 4.0f * S; L.n = 0; L.desc.clear(); L.row_y.clear(); L.focusable.clear();
    ImGuiIO& io = ImGui::GetIO();
    L.mouse_moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
    if (focus && L.count_last > 0 && int(L.focusable_last.size()) == L.count_last && (nav.up || nav.down)) {
        const int step = nav.up ? L.count_last - 1 : 1;
        int s = L.sel;
        for (int k = 0; k < L.count_last; ++k) {
            s = (s + step) % L.count_last;
            if (L.focusable_last[s]) { L.sel = s; break; }
        }
    }
    if (focus && hovering(org, {org.x + size.x, org.y + size.y})) L.scroll_to -= io.MouseWheel * 60.0f * S;
    dl->PushClipRect(org, {org.x + size.x, org.y + size.y}, true);
}

inline void list_end(List& L) {
    L.dl->PopClipRect();
    L.count_last = L.n;
    L.focusable_last = L.focusable;
    L.content_h = L.y;
    if (L.n > 0) L.sel = std::clamp(L.sel, 0, L.n - 1);
    if (L.n > 0 && !L.focusable[L.sel])                 // land on the first row that takes focus
        for (int k = 0; k < L.n; ++k) if (L.focusable[k]) { L.sel = k; break; }
    if ((L.nav.up || L.nav.down) && L.sel < int(L.row_y.size())) {   // keep the row the keys chose in view
        const float top = L.row_y[L.sel], bot = top + row_h();
        if (top - L.scroll_to < 8.0f * S) L.scroll_to = top - 40.0f * S;
        if (bot - L.scroll_to > L.size.y - 8.0f * S) L.scroll_to = bot - L.size.y + 40.0f * S;
    }
    L.scroll_to = std::clamp(L.scroll_to, 0.0f, std::max(0.0f, L.content_h - L.size.y));
    L.scroll += (L.scroll_to - L.scroll) * 0.35f;
    if (std::fabs(L.scroll_to - L.scroll) < 0.5f) L.scroll = L.scroll_to;
    if (L.content_h > L.size.y + 1.0f) {                // the scroll bar, only when the page overflows
        const float x = L.org.x + L.size.x - 5.0f * S;
        const float h = L.size.y * L.size.y / L.content_h;
        const float t = (L.size.y - h) * (L.scroll / std::max(1.0f, L.content_h - L.size.y));
        L.dl->AddRectFilled({x, L.org.y}, {x + 3.0f * S, L.org.y + L.size.y}, IM_COL32(26, 36, 66, 40));
        L.dl->AddRectFilled({x - 1.0f * S, L.org.y + t}, {x + 4.0f * S, L.org.y + t + h}, kBandBot);
    }
}

// A heading on the page: italic, with a gold rule to the row's end.
inline void section(List& L, const char* title) {
    L.y += (L.n > 0 ? 12.0f : 0.0f) * S;
    const float px = 20.0f * S, y = L.org.y + L.y - L.scroll;
    text(L.dl, F.serif_italic, px, {L.org.x + 4.0f * S, y}, kInk, title);
    const float x0 = L.org.x + 14.0f * S + text_size(F.serif_italic, px, title).x, x1 = L.org.x + L.size.x - 14.0f * S;
    if (x1 > x0) {
        L.dl->AddLine({x0, y + px * 0.62f}, {x1, y + px * 0.62f}, kRule, 1.2f * S);
        L.dl->AddLine({x0, y + px * 0.62f + 2.5f * S}, {x1, y + px * 0.62f + 2.5f * S}, IM_COL32(26, 36, 66, 70), 1.0f * S);
    }
    L.y += px + 9.0f * S;
}

// `pad`: how far in from a.x / b.x the row's texts start (a plate's points take room).
struct RowIo { bool selected = false, activate = false; int delta = 0; ImVec2 a{}, b{}; float pad = 0; };

// Lay out and draw one row's frame; returns its input this frame. A focusable row is a plate, another a slab.
inline RowIo row_frame(List& L, const char* desc, bool enabled, bool focusable = true) {
    RowIo r;
    const int i = L.n++;
    L.row_y.push_back(L.y);
    L.focusable.push_back(focusable);
    const float w = L.size.x - 14.0f * S;
    r.a = {L.org.x, L.org.y + L.y - L.scroll};
    r.b = {r.a.x + w, r.a.y + row_h()};
    r.pad = (focusable ? 22.0f : 13.0f) * S;
    L.y += row_h() + row_gap();
    const bool hover = L.focus && hovering(r.a, r.b) && hovering(L.org, {L.org.x + L.size.x, L.org.y + L.size.y});
    if (hover && L.mouse_moved && focusable) L.sel = i;
    r.selected = i == L.sel;
    if (r.selected && desc) L.desc = desc;
    if (enabled && L.focus) {
        if (hover && focusable) hand();
        if (r.selected) {
            if (L.nav.ok) r.activate = true;
            if (L.nav.left) r.delta = -1;
            if (L.nav.right) r.delta = +1;
        }
        if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { L.sel = i; r.selected = true; r.activate = true; }
        if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { L.sel = i; r.selected = true; r.delta = -1; }
    }
    if (focusable) plate(L.dl, r.a, r.b, r.selected);
    else slab(L.dl, {r.a.x + 7.0f * S, r.a.y}, {r.b.x - 7.0f * S, r.b.y});
    return r;
}

inline void row_texts(List& L, const RowIo& r, const char* label, const char* value, bool enabled) {
    const float lpx = 18.0f * S, vpx = 16.5f * S;
    const float cy = (r.a.y + r.b.y) * 0.5f;
    text_sh(L.dl, F.serif, lpx, {r.a.x + r.pad, cy - lpx * 0.58f}, enabled ? kLabel : kMuted, label);
    if (!value) return;
    ImFont* f = r.selected ? F.medium : F.body;
    const ImVec2 vs = text_size(f, vpx, value);
    text(L.dl, f, vpx, {r.b.x - r.pad - vs.x, cy - vpx * 0.55f}, !enabled ? kMuted : r.selected ? kTabOn : kValue, value);
}

// The game's check box: a stone square, blue with a red tick when on.
inline bool toggle(List& L, const char* label, bool* v, const char* desc = nullptr, bool enabled = true) {
    const RowIo r = row_frame(L, desc, enabled);
    bool changed = false;
    if (r.activate || r.delta != 0) { *v = !*v; changed = true; }
    row_texts(L, r, label, nullptr, enabled);
    const float cy = (r.a.y + r.b.y) * 0.5f, h = 9.0f * S, x1 = r.b.x - r.pad, x0 = x1 - 2.0f * h;
    const ImVec2 a{x0, cy - h}, b{x1, cy + h};
    const int alpha = enabled ? 255 : 110;
    if (*v) L.dl->AddRectFilledMultiColor(a, b, IM_COL32(86, 164, 236, alpha), IM_COL32(86, 164, 236, alpha), IM_COL32(22, 90, 190, alpha), IM_COL32(22, 90, 190, alpha));
    else L.dl->AddRectFilledMultiColor(a, b, IM_COL32(198, 190, 164, alpha), IM_COL32(198, 190, 164, alpha), IM_COL32(140, 130, 104, alpha), IM_COL32(140, 130, 104, alpha));
    L.dl->AddRect(a, b, IM_COL32(236, 236, 228, alpha), 0.0f, 0, 1.3f * S);
    if (*v) {
        const ImU32 red = IM_COL32(232, 44, 36, alpha);
        L.dl->AddLine({x0 + 0.45f * h, cy - 0.1f * h}, {x0 + 0.95f * h, cy + 0.6f * h}, red, 2.6f * S);
        L.dl->AddLine({x0 + 0.95f * h, cy + 0.6f * h}, {x1 - 0.1f * h, cy - 1.15f * h}, red, 2.6f * S);
    }
    return changed;
}

// A stepped value on the game's slider: a teal groove and a steel knob; `fmt` formats it (printf, a float).
inline bool slider(List& L, const char* label, float* v, float lo, float hi, float step, const char* fmt,
                   const char* desc = nullptr, bool enabled = true) {
    const RowIo r = row_frame(L, desc, enabled);
    const float old = *v;
    if (r.delta != 0) *v = std::clamp(*v + step * float(r.delta), lo, hi);
    char buf[48];
    std::snprintf(buf, sizeof buf, fmt, *v);
    row_texts(L, r, label, buf, enabled);
    const float cy = (r.a.y + r.b.y) * 0.5f, x1 = r.b.x - r.pad - 64.0f * S, x0 = x1 - 92.0f * S;
    const float t = hi > lo ? std::clamp((*v - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f, kx = x0 + t * (x1 - x0);
    L.dl->AddRectFilled({x0, cy - 2.5f * S}, {x1, cy + 2.5f * S}, IM_COL32(8, 30, 36, 255));
    L.dl->AddRectFilled({x0, cy - 1.5f * S}, {kx, cy + 1.5f * S}, IM_COL32(44, 150, 150, enabled ? 255 : 110));
    L.dl->AddRect({x0, cy - 2.5f * S}, {x1, cy + 2.5f * S}, IM_COL32(110, 150, 160, 255), 0.0f, 0, 1.0f * S);
    L.dl->AddRectFilledMultiColor({kx - 3.0f * S, cy - 9.0f * S}, {kx + 3.0f * S, cy + 9.0f * S}, IM_COL32(220, 226, 232, 255),
                                  IM_COL32(120, 130, 144, 255), IM_COL32(70, 78, 92, 255), IM_COL32(168, 176, 188, 255));
    return *v != old;
}

inline bool choice(List& L, const char* label, int* idx, const char* const* items, int count,
                   const char* desc = nullptr, bool enabled = true) {
    const RowIo r = row_frame(L, desc, enabled);
    const int old = *idx;
    if (count > 0) {
        if (r.delta != 0) *idx = (*idx + r.delta + count) % count;
        else if (r.activate) *idx = (*idx + 1) % count;
    }
    row_texts(L, r, label, count > 0 ? items[std::clamp(*idx, 0, count - 1)] : "", enabled);
    return *idx != old;
}

// A label / value row whose activation starts something: returns true on OK / click.
inline bool choice_label(List& L, const char* label, const char* value, const char* desc = nullptr, bool enabled = true) {
    const RowIo r = row_frame(L, desc, enabled);
    row_texts(L, r, label, value, enabled);
    return r.activate;
}

// An action: its name centred on the plate.
inline bool button(List& L, const char* label, const char* desc = nullptr, bool enabled = true) {
    const RowIo r = row_frame(L, desc, enabled);
    const float px = 18.0f * S;
    const ImVec2 ts = text_size(F.serif, px, label);
    text_sh(L.dl, F.serif, px, {(r.a.x + r.b.x - ts.x) * 0.5f, (r.a.y + r.b.y) * 0.5f - px * 0.58f}, enabled ? kLabel : kMuted, label);
    return r.activate;
}

// A read-only line (a path, a status).
inline void info(List& L, const char* label, const char* value, ImU32 value_col = kValue, const char* desc = nullptr) {
    const RowIo r = row_frame(L, desc, false, /*focusable*/false);
    const float lpx = 17.0f * S, vpx = 16.0f * S;
    const float cy = (r.a.y + r.b.y) * 0.5f;
    text(L.dl, F.serif, lpx, {r.a.x + r.pad + 7.0f * S, cy - lpx * 0.58f}, IM_COL32(190, 196, 212, 255), label);
    const float maxw = (r.b.x - r.a.x) * 0.62f;
    std::string v = value;
    if (text_size(F.body, vpx, value).x > maxw) {       // keep the tail of long paths
        while (v.size() > 4 && text_size(F.body, vpx, ("..." + v).c_str()).x > maxw) v.erase(0, 1);
        v = "..." + v;
    }
    const ImVec2 vs = text_size(F.body, vpx, v.c_str());
    text(L.dl, F.body, vpx, {r.b.x - r.pad - 7.0f * S - vs.x, cy - vpx * 0.55f}, value_col, v.c_str());
}

// The first row of a sub-page: "<  Mods / Hello Lua". OK / click returns true (go back), as Esc does
// through Page::back.
inline bool back_row(List& L, const char* parent, const char* title, const char* desc = nullptr) {
    const RowIo r = row_frame(L, desc ? desc : "Back. (Esc / Backspace / the pad's Back also return.)", true);
    const float lpx = 18.0f * S, cy = (r.a.y + r.b.y) * 0.5f;
    float x = r.a.x + r.pad;
    L.dl->AddLine({x + 6.0f * S, cy - 6.0f * S}, {x, cy}, kGold, 1.8f * S);   // a chevron pointing back
    L.dl->AddLine({x, cy}, {x + 6.0f * S, cy + 6.0f * S}, kGold, 1.8f * S);
    x += 18.0f * S;
    const std::string p = std::string(parent) + "   /   ";
    text_sh(L.dl, F.serif, lpx, {x, cy - lpx * 0.58f}, kTabOff, p.c_str());
    x += text_size(F.serif, lpx, p.c_str()).x;
    text_sh(L.dl, F.serif_italic, lpx, {x, cy - lpx * 0.58f}, kLabel, title);
    return r.activate;
}

// ---- the panel -----------------------------------------------------------------------------------------
// A tab. `back` (optional): the page shows a sub-page and Esc / Back should return to the page instead of
// closing the panel (returns true when it went back). `open_sub` (optional): open a named sub-page.
struct Page {
    std::string name;
    std::function<void(List&)> draw;
    List list;
    std::function<bool(List&)> back;
    std::function<bool(const std::string&)> open_sub;
};

inline constexpr float kPanelW = 900.0f, kPanelH = 620.0f;   // at S = 1

struct Panel {
    std::vector<Page> pages;
    int tab = 0;
    bool open = false;
    const char* mark = "CLERIAORE";   // the wordmark in the band
    ImVec2 side_a{}, side_b{};        // this frame's description slab, for a page's picture

    // Adds a tab, or replaces the one of that name in place.
    Page& add(const std::string& name, std::function<void(List&)> draw) {
        for (Page& p : pages)
            if (p.name == name) { p.draw = std::move(draw); return p; }
        pages.push_back({name, std::move(draw), {}, {}, {}});
        return pages.back();
    }
    // "Tab" or "Tab:sub": select that tab and open its sub-page.
    bool select(const std::string& spec) {
        const std::size_t c = spec.find(':');
        const std::string tab_name = spec.substr(0, c), sub = c == std::string::npos ? std::string() : spec.substr(c + 1);
        for (int i = 0; i < int(pages.size()); ++i) {
            if (pages[std::size_t(i)].name != tab_name) continue;
            tab = i;
            Page& p = pages[std::size_t(i)];
            if (sub.empty()) {
                if (p.back) p.back(p.list);
                return true;
            }
            return p.open_sub && p.open_sub(sub);
        }
        return false;
    }

    // Draw the panel centred in a w x h screen; returns false when it was closed this frame.
    bool draw(ImDrawList* dl, float w, float h, const Nav& nav) {
        if (!open || pages.empty()) return open;
        if (nav.back) {                              // a sub-page goes back to its page; a page closes the panel
            Page& cur = pages[std::size_t(std::clamp(tab, 0, int(pages.size()) - 1))];
            if (!(cur.back && cur.back(cur.list))) { open = false; return false; }
        }
        dl->AddRectFilled({0, 0}, {w, h}, kDim);
        const float pw = std::min(w - 40.0f * S, kPanelW * S), ph = std::min(h - 40.0f * S, kPanelH * S);
        const ImVec2 a{(w - pw) * 0.5f, (h - ph) * 0.5f}, b{a.x + pw, a.y + ph};
        const float band = 46.0f * S, bar = 30.0f * S;
        // the page: pale stone, the game's rings faintly on it, a thin gold line inside its edge
        dl->AddRectFilledMultiColor(a, b, kPageTop, kPageTop, kPageBot, kPageBot);
        dl->PushClipRect({a.x, a.y + band}, {b.x, b.y - bar}, true);
        for (int k = 0; k < 7; ++k) dl->AddCircle({(a.x + b.x) * 0.5f, b.y + ph * 0.18f}, ph * (0.42f + 0.13f * float(k)), kPageLine, 96, 1.2f * S);
        dl->AddRect({a.x + 7.0f * S, a.y + band + 7.0f * S}, {b.x - 7.0f * S, b.y - bar - 7.0f * S}, IM_COL32(170, 142, 74, 150), 22.0f * S, 0, 1.2f * S);
        dl->PopClipRect();
        // the band, the help bar and the frame
        dl->AddRectFilledMultiColor(a, {b.x, a.y + band}, kBandTop, kBandTop, kBandBot, kBandBot);
        dl->AddLine({a.x, a.y + band}, {b.x, a.y + band}, kGoldDark, 3.0f * S);
        dl->AddLine({a.x, a.y + band}, {b.x, a.y + band}, kGold, 1.1f * S);
        dl->AddRectFilled({a.x, b.y - bar}, b, kBandTop);
        dl->AddLine({a.x, b.y - bar}, {b.x, b.y - bar}, kRule, 1.1f * S);
        dl->AddRect(a, b, kGoldDark, 0.0f, 0, 3.0f * S);
        dl->AddRect(a, b, kGold, 0.0f, 0, 1.1f * S);
        // the tabs: the open page's name on a black plate set into the band, the others beside it
        if (nav.tab_prev) tab = (tab + int(pages.size()) - 1) % int(pages.size());
        if (nav.tab_next) tab = (tab + 1) % int(pages.size());
        const float tpx = 21.0f * S;
        float x = a.x + 18.0f * S;
        for (int i = 0; i < int(pages.size()); ++i) {
            const char* t = pages[std::size_t(i)].name.c_str();
            const ImVec2 ts = text_size(F.serif_italic, tpx, t);
            const ImVec2 ta{x, a.y + 6.0f * S}, tb{x + ts.x + 34.0f * S, a.y + band + 7.0f * S};
            if (hovering(ta, tb)) hand();
            if (hovering(ta, tb) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) tab = i;
            if (i == tab) {
                dl->AddRectFilledMultiColor(ta, tb, IM_COL32(44, 46, 54, 255), IM_COL32(44, 46, 54, 255), IM_COL32(4, 5, 8, 255), IM_COL32(4, 5, 8, 255));
                dl->AddRect(ta, tb, kGoldDark, 0.0f, 0, 3.4f * S);
                dl->AddRect(ta, tb, kGold, 0.0f, 0, 1.2f * S);
            }
            text_sh(dl, F.serif_italic, tpx, {x + 17.0f * S, a.y + 6.0f * S + (tb.y - ta.y - tpx) * 0.42f}, i == tab ? kTabOn : kTabOff, t);
            x = tb.x + 6.0f * S;
        }
        // the close cross, and the wordmark before it, letter-spaced as the game's own
        {
            const ImVec2 c{b.x - 24.0f * S, a.y + band * 0.5f};
            const float r = 6.0f * S;
            const bool hov = hovering({c.x - 14 * S, c.y - 14 * S}, {c.x + 14 * S, c.y + 14 * S});
            if (hov) hand();
            dl->AddLine({c.x - r, c.y - r}, {c.x + r, c.y + r}, hov ? kTabOn : kGold, 1.7f * S);
            dl->AddLine({c.x - r, c.y + r}, {c.x + r, c.y - r}, hov ? kTabOn : kGold, 1.7f * S);
            if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { open = false; return false; }
            const float mpx = 19.0f * S, gap = 5.5f * S;
            float mw = 0;
            for (const char* p = mark; *p; ++p) mw += text_size(F.serif, mpx, std::string(1, *p).c_str()).x + gap;
            float mx = c.x - 26.0f * S - mw;
            if (mx > x + 20.0f * S)
                for (const char* p = mark; *p; ++p) {
                    const std::string ch(1, *p);
                    text_sh(dl, F.serif, mpx, {mx, a.y + (band - mpx) * 0.5f}, IM_COL32(214, 198, 150, 255), ch.c_str());
                    mx += text_size(F.serif, mpx, ch.c_str()).x + gap;
                }
        }
        // the rows on the page (left), the chosen row's description on a slab (right)
        const float top = a.y + band + 22.0f * S, bottom = b.y - bar - 16.0f * S, split = a.x + pw * 0.56f;
        Page& pg = pages[std::size_t(std::clamp(tab, 0, int(pages.size()) - 1))];
        List& L = pg.list;
        const ImVec2 lo{a.x + 22.0f * S, top};
        side_a = {split + 8.0f * S, top + 4.0f * S}; side_b = {b.x - 24.0f * S, bottom};
        list_begin(L, dl, lo, {split - lo.x - 4.0f * S, bottom - top}, nav, true);
        pg.draw(L);
        list_end(L);
        slab(dl, side_a, side_b);
        dl->AddRectFilledMultiColor(side_a, {side_b.x, side_a.y + 34.0f * S}, IM_COL32(255, 255, 255, 26), IM_COL32(255, 255, 255, 26), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
        if (!L.desc.empty()) {
            dl->PushClipRect(side_a, side_b, true);
            dl->AddText(F.body, 16.5f * S, {side_a.x + 16.0f * S, side_a.y + 14.0f * S}, kDesc, L.desc.c_str(), nullptr, side_b.x - side_a.x - 32.0f * S);
            dl->PopClipRect();
        }
        text(dl, F.body, 14.5f * S, {a.x + 18.0f * S, b.y - bar + (bar - 14.5f * S) * 0.42f}, IM_COL32(214, 220, 232, 255),
             "Up / Down  choose      Enter  OK      Left / Right  change      Q / E  page      Esc  back");
        return open;
    }
};

// Bottom-centre: a notice card over the game.
inline void toast(ImDrawList* dl, float w, float h, const char* head, const char* body) {
    const float bw = std::max(340.0f * S, text_size(F.body, 16.0f * S, body).x + 40.0f * S), bh = 74.0f * S;
    const ImVec2 a{(w - bw) * 0.5f, h - 105.0f * S}, b{a.x + bw, a.y + bh};
    dl->AddRectFilledMultiColor(a, b, IM_COL32(34, 50, 88, 240), IM_COL32(34, 50, 88, 240), IM_COL32(10, 14, 28, 240), IM_COL32(10, 14, 28, 240));
    dl->AddRect(a, b, kGoldDark, 0.0f, 0, 3.0f * S);
    dl->AddRect(a, b, kGold, 0.0f, 0, 1.1f * S);
    text_sh(dl, F.serif_italic, 17.0f * S, {a.x + 19.0f * S, a.y + 11.0f * S}, kGold, head);
    text(dl, F.body, 16.0f * S, {a.x + 19.0f * S, a.y + 40.0f * S}, kDesc, body);
}

// ---- a one-line text field (a mod's server / slot / password) --------------------------------------------
// OK / click on the row starts editing; typed text, Ctrl+V pastes, Backspace deletes (Ctrl+Backspace
// clears), Enter or a click elsewhere keeps the text, Esc restores it. While editing, read_nav() yields
// nothing. `secret` shows a bullet per character. Returns true when an edit was kept and changed the value.
struct TextEdit {
    const void* id = nullptr;       // the value being edited
    std::string buf;
    int frame = -100;               // the last frame it was drawn active
};
inline TextEdit text_edit;

inline void utf8_append(std::string& s, unsigned c) {
    if (c < 0x80) s += char(c);
    else if (c < 0x800) { s += char(0xC0 | (c >> 6)); s += char(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { s += char(0xE0 | (c >> 12)); s += char(0x80 | ((c >> 6) & 0x3F)); s += char(0x80 | (c & 0x3F)); }
    else {
        s += char(0xF0 | (c >> 18)); s += char(0x80 | ((c >> 12) & 0x3F));
        s += char(0x80 | ((c >> 6) & 0x3F)); s += char(0x80 | (c & 0x3F));
    }
}
inline void utf8_pop(std::string& s) {
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
    if (!s.empty()) s.pop_back();
}
inline std::size_t utf8_count(const std::string& s) {
    std::size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

inline bool text_field(List& L, const char* label, std::string* v, const char* desc = nullptr, bool secret = false,
                       const char* empty = "(not set)", std::size_t max_len = 128) {
    const RowIo r = row_frame(L, desc, true);
    TextEdit& e = text_edit;
    const int now = ImGui::GetFrameCount();
    bool active = e.id == v && now - e.frame <= 1, kept = false, started = false;
    if (!active && r.activate) {
        e.id = v;
        e.buf = *v;
        active = started = true;
        L.sel = L.n - 1;
    }
    if (active) {
        e.frame = text_edit_frame = now;
        ImGuiIO& io = ImGui::GetIO();
        if (!started) {                                  // the key / click that opened it is not an edit
            for (ImWchar c : io.InputQueueCharacters)
                if (c >= 0x20 && c != 0x7F && e.buf.size() < max_len) utf8_append(e.buf, c);
            if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) { if (io.KeyCtrl) e.buf.clear(); else utf8_pop(e.buf); }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
                if (const char* clip = ImGui::GetClipboardText())
                    for (const char* p = clip; *p && *p != '\r' && *p != '\n' && e.buf.size() < max_len; ++p) e.buf += *p;
            const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
            const bool away = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hovering(r.a, r.b);
            if (enter || away) {
                kept = e.buf != *v;
                *v = e.buf;
                e.id = nullptr;
                active = false;
            } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                e.id = nullptr;                          // the old text stays
                active = false;
            }
        }
    }
    // the value, right-aligned like every row; while editing, the text and a caret in a dark well
    const std::string& src = active ? e.buf : *v;
    std::string shown;
    if (secret) for (std::size_t i = utf8_count(src); i > 0; --i) shown += "\xE2\x80\xA2";
    else shown = src;
    const float lpx = 18.0f * S, vpx = 16.5f * S, cy = (r.a.y + r.b.y) * 0.5f;
    text_sh(L.dl, F.serif, lpx, {r.a.x + r.pad, cy - lpx * 0.58f}, kLabel, label);
    const float maxw = (r.b.x - r.a.x) * 0.55f;
    if (active) {
        const ImVec2 wa{r.b.x - r.pad - maxw, r.a.y + 5.0f * S}, wb{r.b.x - r.pad + 4.0f * S, r.b.y - 5.0f * S};
        L.dl->AddRectFilled(wa, wb, IM_COL32(6, 8, 16, 255));
        L.dl->AddRect(wa, wb, kGold, 0.0f, 0, 1.0f * S);
        std::string t = shown;                           // keep the caret end in view
        while (!t.empty() && text_size(F.body, vpx, t.c_str()).x > maxw - 24.0f * S) t.erase(0, 1);
        while (!t.empty() && (static_cast<unsigned char>(t.front()) & 0xC0) == 0x80) t.erase(0, 1);
        const float tw = text_size(F.body, vpx, t.c_str()).x;
        const float x = wb.x - 10.0f * S - tw;
        text(L.dl, F.body, vpx, {x, cy - vpx * 0.55f}, kTabOn, t.c_str());
        if ((now / 30) % 2 == 0)
            L.dl->AddLine({x + tw + 2.0f * S, cy - vpx * 0.5f}, {x + tw + 2.0f * S, cy + vpx * 0.5f}, kGold, 1.5f * S);
    } else {
        std::string t = shown.empty() ? std::string(empty) : shown;
        const ImU32 col = shown.empty() ? kMuted : (r.selected ? kTabOn : kValue);
        if (text_size(F.body, vpx, t.c_str()).x > maxw) {
            while (t.size() > 4 && text_size(F.body, vpx, (t + "...").c_str()).x > maxw) utf8_pop(t);
            t += "...";
        }
        ImFont* f = r.selected ? F.medium : F.body;
        const ImVec2 vs = text_size(f, vpx, t.c_str());
        text(L.dl, f, vpx, {r.b.x - r.pad - vs.x, cy - vpx * 0.55f}, col, t.c_str());
    }
    return kept;
}

}  // namespace ui
