// Vector controller glyphs: independent of fonts, textures and platform assets.
#pragma once
#include "imgui.h"
namespace SettingsIcons {
enum { Confirm, Back, Save, Undo, Left, Right, Dpad };
inline void draw(ImDrawList *d, ImVec2 p, float size, int layout, int action, int physical) {
    const ImU32 ink = IM_COL32(242,246,255,255), edge = IM_COL32(160,184,211,255);
    ImVec2 c(p.x + size*.5f, p.y + size*.5f);
    if (action == Dpad) {
        d->AddRectFilled(ImVec2(c.x-size*.38f,c.y-size*.12f),ImVec2(c.x+size*.38f,c.y+size*.12f),ink,1);
        d->AddRectFilled(ImVec2(c.x-size*.12f,c.y-size*.38f),ImVec2(c.x+size*.12f,c.y+size*.38f),ink,1);
        return;
    }
    if (action == Left || action == Right) {
        const char *name = layout == 0 ? (action == Left ? "LB" : "RB") :
                           layout == 1 ? (action == Left ? "L" : "R") : (action == Left ? "L1" : "R1");
        d->AddRect(p, ImVec2(p.x+size*1.5f,p.y+size), edge, size*.2f, 1.5f, 0);
        ImVec2 t=ImGui::CalcTextSize(name);
        d->AddText(ImVec2(p.x+(size*1.5f-t.x)*.5f,c.y-t.y*.5f), ink, name);
        return;
    }
    d->AddCircleFilled(c, size*.47f, IM_COL32(23,34,48,255));
    d->AddCircle(c, size*.47f, edge, 0, 1.5f);
    if (layout != 2) {
        const char *namesXbox[] = {"A","B","X","Y"}, *namesSwitch[] = {"B","A","Y","X"};
        const char *name = layout == 1 ? namesSwitch[physical] : namesXbox[physical];
        ImVec2 t=ImGui::CalcTextSize(name);
        d->AddText(ImVec2(c.x-t.x*.5f,c.y-t.y*.5f), ink, name);
    } else {
        float r=size*.23f;
        if (physical == 0) {
            d->AddLine(ImVec2(c.x-r,c.y-r),ImVec2(c.x+r,c.y+r),IM_COL32(125,198,255,255),2);
            d->AddLine(ImVec2(c.x-r,c.y+r),ImVec2(c.x+r,c.y-r),IM_COL32(125,198,255,255),2);
        } else if (physical == 1) d->AddCircle(c,r,IM_COL32(255,157,158,255),0,2);
        else if (physical == 2) d->AddRect(ImVec2(c.x-r,c.y-r),ImVec2(c.x+r,c.y+r),IM_COL32(244,166,230,255),0.0f,2.0f,0);
        else d->AddTriangle(ImVec2(c.x,c.y-r),ImVec2(c.x+r,c.y+r),ImVec2(c.x-r,c.y+r),IM_COL32(139,235,202,255),2);
    }
}
// The keyboard's and the mouse's, in the same colours: a key's cap as wide as its name (Enter, Esc, Ctrl+S; "" draws
// the arrow keys up and down), and the mouse with its left button lit (a click) or its wheel (scrolling). Each returns
// its width; a size tall
inline float key_width(float size, const char *name) {
    if (!*name) return size * 1.45f;
    float w = ImGui::GetFont()->CalcTextSizeA(size * .74f, FLT_MAX, 0, name).x + size * .6f;
    return w > size ? w : size;
}
inline float key(ImDrawList *d, ImVec2 p, float size, const char *name) {
    const ImU32 ink = IM_COL32(242,246,255,255), edge = IM_COL32(160,184,211,255);
    float w = key_width(size, name), r = size * .18f;
    ImVec2 q(p.x + w, p.y + size);
    d->AddRectFilled(p, q, IM_COL32(23,34,48,255), r);
    d->AddRect(p, q, edge, r, 0, 1.5f);
    d->AddLine(ImVec2(p.x + r, q.y - size*.14f), ImVec2(q.x - r, q.y - size*.14f), IM_COL32(160,184,211,90), 1.f);  // (a key's front)
    if (!*name) {  // (up and down)
        float cy = p.y + size*.44f, a = size*.2f, x1 = p.x + w*.33f, x2 = p.x + w*.67f;
        d->AddTriangleFilled(ImVec2(x1, cy-a), ImVec2(x1+a, cy+a*.8f), ImVec2(x1-a, cy+a*.8f), ink);
        d->AddTriangleFilled(ImVec2(x2, cy+a), ImVec2(x2-a, cy-a*.8f), ImVec2(x2+a, cy-a*.8f), ink);
        return w;
    }
    float ts = size * .74f;
    ImVec2 t = ImGui::GetFont()->CalcTextSizeA(ts, FLT_MAX, 0, name);
    d->AddText(ImGui::GetFont(), ts, ImVec2(p.x + (w - t.x) * .5f, p.y + (size*.88f - t.y) * .5f), ink, name);
    return w;
}
inline float mouse(ImDrawList *d, ImVec2 p, float size, bool wheel) {
    const ImU32 ink = IM_COL32(242,246,255,255), edge = IM_COL32(160,184,211,255);
    float w = size * .64f, h = size * .98f, x0 = p.x + (size - w) * .5f, y0 = p.y + (size - h) * .5f, r = w * .48f;
    float cx = x0 + w * .5f, split = y0 + h * .44f;
    d->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), IM_COL32(23,34,48,255), r);
    if (wheel)
        d->AddRectFilled(ImVec2(cx - w*.11f, y0 + h*.12f), ImVec2(cx + w*.11f, y0 + h*.34f), ink, w*.11f);
    else  // (the left button)
        d->AddRectFilled(ImVec2(x0 + 1.5f, y0 + 1.5f), ImVec2(cx, split), ink, r - 1.5f, ImDrawFlags_RoundCornersTopLeft);
    d->AddRect(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), edge, r, 0, 1.5f);
    d->AddLine(ImVec2(x0, split), ImVec2(x0 + w, split), edge, 1.f);
    if (!wheel) d->AddLine(ImVec2(cx, y0), ImVec2(cx, split), edge, 1.f);
    return size;
}
}
