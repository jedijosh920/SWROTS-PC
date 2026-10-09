// Free camera: detaches the view from the game's camera and flies it with the keyboard, mouse or a
// controller, while the player's input is held back. The game keeps running in whatever state it is
// in (set timeScale 0 to freeze it, hud 0 to hide the HUD); its camera effects (shake, zoom) are kept
// off the flown view. Outside a flight nothing is changed.

#include "game/freecam.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "input/controls.h"

namespace swrots::game {

namespace {

struct Matrix {
    float m[4][4];
};

struct Vec3 {
    float x, y, z;
};

Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
Vec3 operator*(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }

// Row-vector 4x4 product a * b.
Matrix Multiply(const Matrix& a, const Matrix& b)
{
    Matrix out = {};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k)
                out.m[r][c] += a.m[r][k] * b.m[k][c];
    return out;
}

// A camera matrix with its unused fourth column set to (0, 0, 0, 1).
Matrix Affine(const Matrix& m)
{
    Matrix out = m;
    out.m[0][3] = out.m[1][3] = out.m[2][3] = 0.0f;
    out.m[3][3] = 1.0f;
    return out;
}

// The inverse of a camera matrix whose axes are orthonormal: transposed axes, position taken back.
Matrix InverseRigid(const Matrix& m)
{
    Matrix out = {};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out.m[r][c] = m.m[c][r];
    for (int c = 0; c < 3; ++c)
        out.m[3][c] = -(m.m[3][0] * out.m[0][c] + m.m[3][1] * out.m[1][c] + m.m[3][2] * out.m[2][c]);
    out.m[3][3] = 1.0f;
    return out;
}

float RotationDifference(const Matrix& a, const Matrix& b)
{
    float d = 0.0f;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            d += std::fabs(a.m[r][c] - b.m[r][c]);
    return d;
}

// Hooks a function whose first `length` bytes are position-independent instructions; returns the
// original (those bytes copied to a stub that continues after them).
void* Detour(uint32_t address, const uint8_t* prologue, uint32_t length, const void* hook)
{
    uint8_t* stub = AllocStub(length + 5);
    std::memcpy(stub, prologue, length);
    stub[length] = 0xE9;
    int32_t back = int32_t(address + length) - int32_t(uintptr_t(stub) + length + 5);
    std::memcpy(stub + length + 1, &back, 4);
    PatchJump(address, hook);
    return stub;
}

// --- The flight ------------------------------------------------------------------------------------
// The master camera (IMasterCamera.cpp, IMasterCameraVader.cpp: the one camera that drives the view;
// gameplay, duel and scripted cameras feed it) hands its placement over every frame through
// SetTransform (vtable +0x1F4, thiscall (const Matrix*), ret 4), which only the two master camera
// classes use. The matrix's rows are the camera's right, up and forward axes and its position; up is
// +Y, and right x up = forward (as in Direct3D). It runs while the game is frozen (timeScale 0) too.
constexpr uint32_t kSetTransform = 0x00129990;
constexpr uint8_t kSetTransformPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x4C };
using SetTransformFn = void(__fastcall*)(void* camera, void* edx, const Matrix* transform);
SetTransformFn g_OriginalSetTransform = nullptr;

// Speeds in world units (a character is roughly 180 tall) per second.
constexpr float kBaseSpeed = 400.0f;
constexpr float kSpeedStep = 1.25f;   // per wheel notch or D-pad press
constexpr float kMouseTurn = 0.0025f; // radians per mouse count
constexpr float kStickTurn = 2.0f;    // radians per second at full deflection
constexpr float kPitchLimit = 1.55f;  // just short of straight up or down

std::atomic<bool> g_Wanted = false; // the console's choice
bool g_Flying = false;              // taken over from the game's camera
bool g_HaveGameView = false;
Matrix g_GameView = {};             // the master camera's latest placement from the game
Matrix g_ShownView = {};            // the same as shown (adjusted, see SetCameraAdjuster)
CameraAdjuster g_Adjuster = nullptr;
bool g_Adjusted = false;            // g_ShownView was adjusted last time
float g_GameFov = 0.0f;             // the field of view the game gave the renderer (below)
bool g_HaveGameFov = false;
Matrix g_FlownView = {};            // this frame's flown placement
Vec3 g_Position = {};
float g_Yaw = 0.0f, g_Pitch = 0.0f;
float g_Speed = kBaseSpeed;
LARGE_INTEGER g_LastTick = {};
DWORD g_AutoStartMs = 0, g_AutoStartTick = 0; // SWROTS_FREECAM

int g_CameraPass = 0, g_FovPass = 0; // the renderer's calls this frame (below)

// Starts from the game camera's view.
void TakeOver()
{
    const float(*m)[4] = g_GameView.m;
    g_Position = { m[3][0], m[3][1], m[3][2] };
    g_Yaw = std::atan2(m[2][0], m[2][2]);
    g_Pitch = std::asin(std::clamp(m[2][1], -1.0f, 1.0f));
    QueryPerformanceCounter(&g_LastTick);
    g_Flying = true;
    input::HoldPlayerInput(true);
    LOG_INFO("Free camera: on at %.0f %.0f %.0f", g_Position.x, g_Position.y, g_Position.z);
}

void Fly(Matrix& out)
{
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const float dt = std::min(float(now.QuadPart - g_LastTick.QuadPart) / float(frequency.QuadPart), 0.1f);
    g_LastTick = now;

    const input::FreeCameraControls c = input::ReadFreeCameraControls();
    if (c.speedSteps)
        g_Speed = std::clamp(g_Speed * std::pow(kSpeedStep, float(c.speedSteps)), kBaseSpeed / 20.0f, kBaseSpeed * 20.0f);
    g_Yaw += c.mouseDx * kMouseTurn + c.lookX * kStickTurn * dt;
    g_Pitch = std::clamp(g_Pitch - c.mouseDy * kMouseTurn + c.lookY * kStickTurn * dt, -kPitchLimit, kPitchLimit);

    const float cp = std::cos(g_Pitch), sp = std::sin(g_Pitch), cy = std::cos(g_Yaw), sy = std::sin(g_Yaw);
    const Vec3 forward = { cp * sy, sp, cp * cy };
    const Vec3 right = { cy, 0.0f, -sy };       // world up x forward
    const Vec3 up = { -sp * sy, cp, -sp * cy }; // forward x right
    const float step = g_Speed * c.speed * dt;
    g_Position = g_Position + forward * (c.forward * step) + right * (c.right * step) + Vec3{ 0.0f, c.up * step, 0.0f };

    std::memset(&out, 0, sizeof(out));
    const Vec3 rows[4] = { right, up, forward, g_Position };
    for (int r = 0; r < 4; ++r) {
        out.m[r][0] = rows[r].x;
        out.m[r][1] = rows[r].y;
        out.m[r][2] = rows[r].z;
    }
    out.m[3][3] = 1.0f;
}

void __fastcall SetTransformHook(void* camera, void* edx, const Matrix* transform)
{
    g_CameraPass = g_FovPass = 0; // a new frame
    if (g_AutoStartMs && GetTickCount() - g_AutoStartTick >= g_AutoStartMs) {
        g_AutoStartMs = 0;
        g_Wanted = true;
    }
    const bool wanted = g_Wanted;
    if (!g_Flying && transform) {
        g_GameView = *transform;
        g_HaveGameView = true;
    }
    if (wanted && !g_Flying && g_HaveGameView) {
        TakeOver();
    } else if (!wanted && g_Flying) {
        g_Flying = false;
        input::HoldPlayerInput(false);
        LOG_INFO("Free camera: off");
    }
    if (!g_Flying) {
        // The game sometimes places the camera where it already is (while paused): that is the adjusted
        // placement already, which must not be adjusted again (the camera ran off a little more each time).
        const bool again = transform && g_Adjusted && std::memcmp(transform, &g_ShownView, sizeof(Matrix)) == 0;
        if (transform && !again)
            g_ShownView = *transform;
        g_Adjusted = false;
        if (transform && !again && g_Adjuster && g_Adjuster(&g_ShownView.m[0][0], g_HaveGameFov ? g_GameFov : 0.0f)) {
            g_Adjusted = true;
            g_OriginalSetTransform(camera, edx, &g_ShownView);
            return;
        }
        g_Adjusted = again;
        g_OriginalSetTransform(camera, edx, transform);
        return;
    }
    Fly(g_FlownView);
    g_OriginalSetTransform(camera, edx, &g_FlownView);
}

// --- What the renderer gets ----------------------------------------------------------------------
// Between the master camera and the renderer the game adds its camera effects: a shake (a small
// position offset) and a zoom (the field of view). While flying both are kept off the view.
//
// The renderer's camera (a method of the render interface, vtable 0x59CA48; thiscall (int kind,
// const Matrix*), ret 8), kind 0, is called once per render pass (from the scene's render, 0x85FD0,
// through the engine API, 0x18510) with a view matrix: the inverse of the camera's placement in the
// level, which is the master camera's taken into the level (a duel arena's is mirrored and offset)
// with the shake added. Before a flight, view = X * inverse(master) is measured on the frame's main
// pass (the first). A frame can also have passes with cameras of their own (a far background at
// another scale). While flying, a pass whose view the flown camera reproduces (the same rotation, a
// position within a shake's reach) gets the flown position; the others keep the game's, which follows
// the flown camera itself.
constexpr uint32_t kRenderCamera = 0x002116A0;
constexpr uint8_t kRenderCameraPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0xC4, 0x00, 0x00, 0x00 };
using RenderCameraFn = char(__fastcall*)(void* renderer, void* edx, int kind, const Matrix* matrix);
RenderCameraFn g_OriginalRenderCamera = nullptr;
constexpr float kShakeReach = 25.0f; // world units
Matrix g_ToView = {};
bool g_HaveToView = false;

char __fastcall RenderCameraHook(void* renderer, void* edx, int kind, const Matrix* matrix)
{
    if (kind != 0 || !matrix || !g_HaveGameView)
        return g_OriginalRenderCamera(renderer, edx, kind, matrix);
    const int pass = g_CameraPass++;
    if (!g_Flying) {
        if (pass == 0) {
            g_ToView = Multiply(Affine(*matrix), Affine(g_GameView));
            g_HaveToView = true;
        }
        return g_OriginalRenderCamera(renderer, edx, kind, matrix);
    }
    if (!g_HaveToView)
        return g_OriginalRenderCamera(renderer, edx, kind, matrix);
    const Matrix flownView = Multiply(g_ToView, InverseRigid(Affine(g_FlownView)));
    float gap = 0.0f;
    for (int c = 0; c < 3; ++c)
        gap = std::max(gap, std::fabs(flownView.m[3][c] - matrix->m[3][c]));
    if (RotationDifference(flownView, *matrix) > 0.05f || gap > kShakeReach)
        return g_OriginalRenderCamera(renderer, edx, kind, matrix); // another pass's own camera
    Matrix view = *matrix;
    for (int c = 0; c < 3; ++c)
        view.m[3][c] = flownView.m[3][c];
    return g_OriginalRenderCamera(renderer, edx, kind, &view);
}

// The renderer's field of view (a method of the render interface, vtable 0x59CA60; thiscall (float
// radians), ret 4), set once a frame, zoom included. While flying it keeps the value it had before
// the flight began.
constexpr uint32_t kRenderFov = 0x00211C30;
constexpr uint8_t kRenderFovPrologue[] = { 0xA1, 0xF4, 0x93, 0x61, 0x00, 0x85, 0xC0 }; // mov eax, [0x6193F4]; test eax, eax
using RenderFovFn = char(__fastcall*)(void* renderer, void* edx, float fov);
RenderFovFn g_OriginalRenderFov = nullptr;
char __fastcall RenderFovHook(void* renderer, void* edx, float fov)
{
    if (g_FovPass++ != 0) // one a frame; any other call is left as it is
        return g_OriginalRenderFov(renderer, edx, fov);
    if (!g_Flying) {
        g_GameFov = fov;
        g_HaveGameFov = true;
        return g_OriginalRenderFov(renderer, edx, fov);
    }
    return g_OriginalRenderFov(renderer, edx, g_HaveGameFov ? g_GameFov : fov);
}

} // namespace

bool GameCameraPlacement(float rows[16], float& fov)
{
    if (!g_HaveGameView)
        return false;
    std::memcpy(rows, g_ShownView.m, sizeof(g_ShownView.m));
    fov = g_HaveGameFov ? g_GameFov : 0.0f;
    return true;
}

void SetCameraAdjuster(CameraAdjuster adjuster)
{
    g_Adjuster = adjuster;
}

void InstallFreeCamera()
{
    // Every level change or restart reboots the game: the free camera is off again. Development aid:
    // SWROTS_FREECAM=<seconds> turns it on that long after the start, for unattended tests.
    g_Wanted = false;
    char delay[16] = {};
    g_AutoStartMs = GetEnvironmentVariableA("SWROTS_FREECAM", delay, sizeof(delay)) ? DWORD(atof(delay) * 1000.0) : 0;
    g_AutoStartTick = GetTickCount();
    g_Flying = false;
    g_HaveGameView = g_HaveToView = g_HaveGameFov = false;
    input::HoldPlayerInput(false);
    const auto matches = [](uint32_t address, const uint8_t* bytes, size_t length) {
        return std::memcmp(reinterpret_cast<const void*>(uintptr_t(address)), bytes, length) == 0;
    };
    if (!matches(kSetTransform, kSetTransformPrologue, sizeof(kSetTransformPrologue)) ||
        !matches(kRenderCamera, kRenderCameraPrologue, sizeof(kRenderCameraPrologue)) ||
        !matches(kRenderFov, kRenderFovPrologue, sizeof(kRenderFovPrologue))) {
        LOG_WARN("Free camera: the camera code is not as expected; no free camera");
        return;
    }
    g_OriginalSetTransform = reinterpret_cast<SetTransformFn>(Detour(kSetTransform, kSetTransformPrologue,
        sizeof(kSetTransformPrologue), reinterpret_cast<const void*>(&SetTransformHook)));
    g_OriginalRenderCamera = reinterpret_cast<RenderCameraFn>(Detour(kRenderCamera, kRenderCameraPrologue,
        sizeof(kRenderCameraPrologue), reinterpret_cast<const void*>(&RenderCameraHook)));
    g_OriginalRenderFov = reinterpret_cast<RenderFovFn>(Detour(kRenderFov, kRenderFovPrologue, sizeof(kRenderFovPrologue),
        reinterpret_cast<const void*>(&RenderFovHook)));
}

void SetFreeCamera(bool on)
{
    g_Wanted = on;
}

bool FreeCameraOn()
{
    return g_Wanted;
}

} // namespace swrots::game
