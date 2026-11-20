#pragma once

#include <cstdint>

/* Globals re-derived empirically after the Nov 17 patch (the a2x dump is
 * still 2026-11-12 and reads as zero on the live build). Values below were
 * located by structural scan of client.dll's writable data and verified
 * against a live match: the entity list candidate carried 380+ members with
 * client.dll vtables and scene nodes whose m_pOwner pointed back at them,
 * the controller slot pointed at the entity flagged m_bIsLocalPlayerController. */
namespace Offsets {
    constexpr std::ptrdiff_t dwEntityList = 0x21BF9B0;            // CGameEntitySystem*
    constexpr std::ptrdiff_t dwLocalPlayerController = 0x21C6E60; // CCSPlayerController*
    constexpr std::ptrdiff_t dwLocalPlayerPawn = 0x2553D38;       // stale — pawn resolved via controller
    /* the renderer keeps the view matrix SPLIT on this build, in three
     * consecutive 64-byte slots of the same block:
     *   0x255CDD0 - world->camera, 3x4 + (0,0,0,1): three orthonormal rows
     *               (right / up / forward for a left-handed basis) and the
     *               camera position inverted into the fourth column.
     *   0x255CE10 - reversed-Z perspective (x scale 1, y 1.3333, near 6.5).
     *   0x255CE50 - the product P*V, column-vector form: row 3 is the w row.
     * Verified live three times: negating translation by the row basis lands
     * the camera exactly 64 units above the local pawn (standing eye height,
     * dz 63.2/63.6/63.6) and w(camera) = -0.24, i.e. zero at the eye. */
    constexpr std::ptrdiff_t dwViewMatrix = 0x255D3D0;            // P*V, row 3 = w row
    constexpr std::ptrdiff_t dwViewMatrixView = 0x255D350;        // world->camera 3x4
    constexpr std::ptrdiff_t dwViewMatrixProj = 0x255D390;        // projection
    constexpr std::ptrdiff_t dwViewRender = 0x255A008;
    constexpr std::ptrdiff_t dwGameRules = 0x25537E8;

    constexpr std::ptrdiff_t m_pGameSceneNode = 0x330;
    constexpr std::ptrdiff_t m_iHealth = 0x34C;
    constexpr std::ptrdiff_t m_iTeamNum = 0x3E7;      /* uint8! */
    constexpr std::ptrdiff_t m_lifeState = 0x354;     /* uint8 */
    constexpr std::ptrdiff_t m_iMaxHealth = 0x348;
    constexpr std::ptrdiff_t m_vecAbsOrigin = 0xC8;   /* in CGameSceneNode */

    constexpr std::ptrdiff_t m_ArmorValue = 0x1F00;
    constexpr std::ptrdiff_t m_bIsScoped = 0x1ED0;
    constexpr std::ptrdiff_t m_iShotsFired = 0x1EE8;
    constexpr std::ptrdiff_t m_flVelocityModifier = 0x1EF0;

    constexpr std::ptrdiff_t sceneNode_m_vecAbsOrigin = 0xC8;
    constexpr std::ptrdiff_t sceneNode_m_pParent = 0x38;

    // Name/controller resolution — name still inline at controller+0x6FC and
    // m_bIsLocalPlayerController at 0x790 (both verified live post-patch).
    // The pawn handle MOVED in the Nov 17 build: 0x92C now reads a dead handle
    // (index 464 on a 10-player server), so every candidate field is probed at
    // runtime and the first one that resolves to a pawn-shaped entity wins.
    constexpr std::ptrdiff_t m_hPlayerPawn = 0x92C;          // CCSPlayerController -> CHandle<C_CSPlayerPawn>
    constexpr std::ptrdiff_t m_hPawnFallback = 0x6BC;        // CBasePlayerController -> CHandle (fallback)
    constexpr std::ptrdiff_t m_hPlayerPawnAlt = 0xBA0;       // observed pawn-bearing field on this build
    constexpr std::ptrdiff_t m_hPawnAlt2 = 0x4C4;            // observed pawn-bearing field on this build
    constexpr std::ptrdiff_t m_iszPlayerName = 0x6FC;        // CCSPlayerController inline char[128]
    constexpr std::ptrdiff_t m_playerSlotField = 0x950;      // low 15 bits = player slot (verified live)
    constexpr std::ptrdiff_t m_controllerPoolStride = 0xA00; // controllers are allocated on this grid
    constexpr std::ptrdiff_t m_hController = 0x14BC;         // C_BasePlayerPawn -> controller handle
    constexpr std::ptrdiff_t m_bIsLocalPlayerController = 0x790;

    /* Entity identity pool — verified live on this build. Every entity owns an
     * identity record on a contiguous 0x70 grid; controllers are indexed
     * DIRECTLY by player slot:
     *     identity(slot) = poolBase + 0x08 + 0x70 * slot
     *     instance       = qword[identity + 0x00]   (the CCSPlayerController)
     *     class          = qword[identity + 0x08]
     * poolBase is recovered from the local controller:
     *     poolBase = qword[localController + 0x10] - 0x08 - 0x70 * localSlot
     * Verified against two different matches: slot 6 -> 0x...02a8, slot 40 ->
     * 0x...1188 from the same 0x...0000 base, and a full 64-slot walk named
     * every controller in 0.2s. No heap sweep needed. */
    constexpr std::ptrdiff_t m_pEntityIdentity = 0x10;       // entity -> identity record (same as m_pEntity)
    constexpr std::ptrdiff_t m_controllerIdentityHead = 0x08;    // poolBase -> first record
    constexpr std::ptrdiff_t m_controllerIdentityStride = 0x70;  // record stride
    constexpr std::ptrdiff_t dwControllerVtable = 0x1C06EC8;     // client.dll RVA, CCSPlayerController vtable
    constexpr std::ptrdiff_t m_hObserverTarget = 0x44;       // CPlayer_ObserverServices (handle)
    constexpr std::ptrdiff_t m_pObserverServices = 0x13F0;   // C_BasePlayerPawn (ptr) - may vary by build
    constexpr std::ptrdiff_t m_iObserverMode = 0x40;         // CPlayer_ObserverServices - may vary by build

    constexpr int LIFE_ALIVE = 0;
    constexpr int LIFE_DYING = 1;
    constexpr int LIFE_DEAD = 2;

    constexpr int TEAM_SPECTATOR = 1;
    constexpr int TEAM_T = 2;
    constexpr int TEAM_CT = 3;

    /* verified live: the camera sits exactly this far above the local pawn's
     * feet (world->camera translation reconstructed against m_vecAbsOrigin) */
    constexpr float kEyeHeight = 64.0f;

    constexpr std::ptrdiff_t m_pEntity = 0x10;
    constexpr std::ptrdiff_t m_index = 0x8;
}

struct ViewMatrix {
    float m[4][4];
};

struct Vector3 {
    float x, y, z;
    Vector3() : x(0), y(0), z(0) {}
    Vector3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
};

struct Vector2 {
    float x, y;
    Vector2() : x(0), y(0) {}
    Vector2(float _x, float _y) : x(_x), y(_y) {}
};
