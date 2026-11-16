#include <Pch.h>
#include "Cache.h"

#include "Combat/Aimbot.h"
#include "../Render/RenderMenu.hpp"
#include "../sdk.hpp"

// player-state based chain, per the reference implementation
static constexpr uintptr_t kGameStatePlayerArray = 0x480;
static constexpr uintptr_t kSpawnedCharacter = 0xBC8;
static constexpr float kHeadOffset = 90.0f;
static constexpr float kFootOffset = -100.0f;

static bool sane_pos(double x, double y, double z)
{
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
        fabs(x) < 200000.0 && fabs(y) < 200000.0 && fabs(z) < 200000.0 &&
        (x != 0.0 || y != 0.0 || z != 0.0);
}

void Cache::EntityLoop()
{
    if (!GameCache::PersistentLevel || !GameCache::PlayerCameraManager || !GameCache::GameState) return;

    auto scatter_handle = mem.CreateScatterHandle();

    mem.AddScatterReadRequest(scatter_handle, GameCache::PlayerCameraManager + offsets::ViewInfo, &GameCache::LocalCamera, sizeof(Camera));
    mem.ExecuteReadScatter(scatter_handle);

    if (GameCache::LocalCamera.FieldOfView <= 1.0f || GameCache::LocalCamera.FieldOfView > 170.0f) { mem.CloseScatterHandle(scatter_handle); return; }

    uintptr_t player_array = mem.Read<uintptr_t>(GameCache::GameState + kGameStatePlayerArray);
    int count = mem.Read<int>(GameCache::GameState + kGameStatePlayerArray + 0x8);
    if (!player_array || count <= 0 || count > 64) { mem.CloseScatterHandle(scatter_handle); return; }

    const double cam[3] = { GameCache::LocalCamera.Location[0], GameCache::LocalCamera.Location[1], GameCache::LocalCamera.Location[2] };
    const double rot[3] = { GameCache::LocalCamera.Rotation[0], GameCache::LocalCamera.Rotation[1], GameCache::LocalCamera.Rotation[2] };

    // camera forward vector - used to reject targets behind us
    const double pr = rot[0] * 3.14159265358979 / 180.0;
    const double yr = rot[1] * 3.14159265358979 / 180.0;
    const double fwd[3] = { std::cos(pr) * std::cos(yr), std::cos(pr) * std::sin(yr), std::sin(pr) };

    // ---- pass 1: player states ----
    std::vector<uintptr_t> states(count, 0);
    for (int i = 0; i < count; i++)
        mem.AddScatterReadRequest(scatter_handle, player_array + i * sizeof(uintptr_t), &states[i], sizeof(uintptr_t));
    mem.ExecuteReadScatter(scatter_handle);

    // ---- pass 2: pawn from each player state ----
    std::vector<uintptr_t> spawned(count, 0), pawnpriv(count, 0), pawns(count, 0);
    for (int i = 0; i < count; i++) {
        if (!CheckValidVA(states[i])) continue;
        mem.AddScatterReadRequest(scatter_handle, states[i] + kSpawnedCharacter, &spawned[i], sizeof(uintptr_t));
        mem.AddScatterReadRequest(scatter_handle, states[i] + offsets::PawnPrivate, &pawnpriv[i], sizeof(uintptr_t));
    }
    mem.ExecuteReadScatter(scatter_handle);

    for (int i = 0; i < count; i++)
        pawns[i] = spawned[i] ? spawned[i] : pawnpriv[i];

    // ---- pass 3: damage handler, root component, ally flag ----
    std::vector<uintptr_t> dh(count, 0), root(count, 0);
    std::vector<BYTE> ally(count, 0);

    for (int i = 0; i < count; i++) {
        if (!CheckValidVA(pawns[i]) || pawns[i] == GameCache::LocalPawn) continue;
        mem.AddScatterReadRequest(scatter_handle, pawns[i] + offsets::damageHandler, &dh[i], sizeof(uintptr_t));
        mem.AddScatterReadRequest(scatter_handle, pawns[i] + offsets::RootComponent, &root[i], sizeof(uintptr_t));
        mem.AddScatterReadRequest(scatter_handle, pawns[i] + offsets::WasAlly, &ally[i], sizeof(BYTE));
    }
    mem.ExecuteReadScatter(scatter_handle);

    // ---- pass 4: health + relative location (3 doubles) ----
    std::vector<float> hp(count, 0.0f);
    std::vector<double> pos(count * 3, 0.0);

    for (int i = 0; i < count; i++) {
        if (CheckValidVA(dh[i]))
            mem.AddScatterReadRequest(scatter_handle, dh[i] + offsets::Health, &hp[i], sizeof(float));
        if (CheckValidVA(root[i]))
            mem.AddScatterReadRequest(scatter_handle, root[i] + offsets::RootPosition, &pos[i * 3], sizeof(double) * 3);
    }
    mem.ExecuteReadScatter(scatter_handle);

    mem.CloseScatterHandle(scatter_handle);

    int c_scan = count, c_skip = 0, c_ally = 0, c_nohp = 0, c_behind = 0, c_off = 0, c_proj = 0, c_drawn = 0;
    std::string dbg;

    Vector2 Closest2DHead{};
    float min_distance = std::numeric_limits<float>::max();
    bool have_target = false;

    const ImVec2 screenCenter = ImVec2(settings::width / 2.0f, settings::height);
    const float fov_rad = GameCache::LocalCamera.FieldOfView * 3.14159265f / 360.0f;
    const float px_scale = (settings::width / 2.0f) / tanf(fov_rad);

    for (int i = 0; i < count; i++) {
        if (!pawns[i] || pawns[i] == GameCache::LocalPawn) { c_skip++; continue; }
        if (ally[i]) { c_ally++; continue; }
        if (hp[i] <= 0.0f || hp[i] > 200.0f) { c_nohp++; continue; }
        if (!CheckValidVA(root[i])) { c_skip++; continue; }

        const double px = pos[i * 3 + 0];
        const double py = pos[i * 3 + 1];
        const double pz = pos[i * 3 + 2];
        if (!sane_pos(px, py, pz)) { c_skip++; continue; }

        const double dxw = px - cam[0];
        const double dyw = py - cam[1];
        const double dzw = pz - cam[2];

        const double depth = dxw * fwd[0] + dyw * fwd[1] + dzw * fwd[2];
        if (depth < 10.0) { c_behind++; continue; }

        const Vector3 Head3D((float)px, (float)py, (float)(pz + kHeadOffset));
        const Vector3 Foot3D((float)px, (float)py, (float)(pz + kFootOffset));

        const Vector2 Head2D = w2s(GameCache::LocalCamera, Head3D);
        const Vector2 Foot2D = w2s(GameCache::LocalCamera, Foot3D);

        if (!std::isfinite(Head2D.x) || !std::isfinite(Head2D.y) || !std::isfinite(Foot2D.x) || !std::isfinite(Foot2D.y)) { c_off++; continue; }
        if (Head2D.x < -200.0f || Head2D.x > settings::width + 200.0f ||
            Head2D.y < -200.0f || Head2D.y > settings::height + 200.0f) {
            c_off++; continue;
        }

        float height = Foot2D.y - Head2D.y;
        if (height < 4.0f || height > settings::height) { c_off++; continue; }

        const double dist = std::sqrt(dxw * dxw + dyw * dyw + dzw * dzw);
        const float expected = (float)(190.0 * px_scale / dist);
        if (height < expected * 0.45f || height > expected * 1.8f) { c_off++; continue; }

        float width = height * 0.4f;
        c_proj++;

        if (c_proj <= 3) {
            dbg += std::format(" [ok d{:.0f} p=({:.0f},{:.0f},{:.0f}) s=({:.0f},{:.0f}) h={:.0f} e={:.0f}]",
                dist, px, py, pz, Head2D.x, Head2D.y, height, expected);
        }

        ImColor boxcolor = ImColor(0, 255, 0, 255);
        const Vector2 Bottom2D = Vector2(Head2D.x, Foot2D.y);

        if (settings::visuals::bBox) { DrawBox(Head2D.x - (width / 2), Head2D.y, width, height, boxcolor, 1); c_drawn++; }
        if (settings::visuals::bLines)  DrawLine(Bottom2D, screenCenter, boxcolor);
        if (settings::visuals::bHealth) DrawHealth(Head2D, Bottom2D, hp[i], width, height);

        float dx = Head2D.x - (settings::width / 2.0f);
        float dy = Head2D.y - (settings::height / 2.0f);
        float screen_dist = std::sqrt(dx * dx + dy * dy);

        if (screen_dist <= settings::aimbot::fov && screen_dist < min_distance) {
            min_distance = screen_dist;
            Closest2DHead = Head2D;
            have_target = true;
        }
    }

    if (have_target) {
        std::lock_guard<std::mutex> lock(target_mutex);
        target_position2d = Closest2DHead;
    }

    static ULONGLONG last_diag = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last_diag >= 1000) {
        last_diag = now;
        LOG("[esp] n=%d skip=%d ally=%d nohp=%d behind=%d off=%d proj=%d drawn=%d%s\n",
            c_scan, c_skip, c_ally, c_nohp, c_behind, c_off, c_proj, c_drawn, dbg.c_str());
    }

    if (mem.GetKeyboard() &&
        (mem.GetKeyboard()->IsKeyDown(settings::aimbot::aimkey1) ||
            mem.GetKeyboard()->IsKeyDown(settings::aimbot::aimkey2)))
        aim.StartAimbotThread();
    else
        aim.StopAimbotThread();
}

WPARAM render_loop() {
    MSG message = { 0 };

    while (message.message != WM_QUIT) {
        if (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
        else {
            ImGuiIO& io = ImGui::GetIO();
            POINT cursorPos;
            GetCursorPos(&cursorPos);
            io.MousePos = ImVec2(static_cast<float>(cursorPos.x), static_cast<float>(cursorPos.y));
            io.MouseDown[0] = GetAsyncKeyState(VK_LBUTTON) != 0;

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            render_menu();
            Entity.EntityLoop();
            ImGui::EndFrame();

            float clearColor[4] = { 0.0f, 0.0f, 0.0f, settings::misc::bTransparent ? 0.0f : 1.0f };
            p_device_context->ClearRenderTargetView(p_render_target_view, clearColor);

            ImGui::Render();
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

            HRESULT result = p_swap_chain->Present(0, 0);
            if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
                ImGui_ImplDX11_InvalidateDeviceObjects();
                p_device->Release();
                p_device_context->Release();
                p_swap_chain->Release();
                p_render_target_view->Release();

                if (FAILED(directx_init())) {
                    return E_FAIL;
                }
                ImGui_ImplDX11_CreateDeviceObjects();
            }
        }
    }

    return message.wParam;
}

bool init() {
    return SUCCEEDED(directx_init());
}