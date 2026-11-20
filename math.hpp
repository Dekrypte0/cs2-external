#pragma once

#include <cstdint>
#include <cstring>
#include <chrono>
#include <sstream>
#include <vector>
#include <cmath>
#include <algorithm>
#include "offsets.hpp"
#include "memory.hpp"
#include "logger.hpp"

/* world-to-screen: read DIRECTLY at dwViewMatrix (the dumper's canonical
 * export). verified across this whole session — varied label capture,
 * nine entities at nine distinct pixels, 46/75 synthetic map-grid points
 * projecting in-front with 46 distinct NDC positions. the slot is
 * rewritten every frame by the renderer with the current camera's
 * world-to-clip matrix, so there is nothing to re-latch on spectate,
 * camera or map changes.
 *
 * candidate scanning was tried and removed: the pool also holds
 * orthographic shadow passes which project synthetic grids convincingly
 * but do NOT match the player's view — every scoring variant eventually
 * hijacked the session onto one of them (the "ESP positions wrong" bug).
 * if a game update ever moves the pool, refresh offsets.hpp from the
 * dumper; that is the recovery path, not runtime scanning. */
class WorldToScreen {
private:
    ViewMatrix viewMatrix{};
    int screenWidth = 1920;
    int screenHeight = 1080;

    uintptr_t matrixAddr = 0;
    std::vector<Vector3> entityPositions;

    bool ValidateBasic(const ViewMatrix& m) const {
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                if (!std::isfinite(m.m[i][j])) return false;
            }
        }
        float maxAbs = 0.0f;
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                maxAbs = (std::max)(maxAbs, std::fabs(m.m[i][j]));
            }
        }
        return maxAbs > 0.001f && maxAbs < 1e7f;
    }

    /* verified layout: clip = [x y z 1] * M (row-vector).
     * x/y from rows 0/1, perspective divide by row 3. */
    bool ProjectLayout(const ViewMatrix& m, const Vector3& worldPos, Vector2& screenPos) const {
        const float w = m.m[3][0] * worldPos.x + m.m[3][1] * worldPos.y +
                        m.m[3][2] * worldPos.z + m.m[3][3];
        if (w < 0.001f) return false;

        const float x = m.m[0][0] * worldPos.x + m.m[0][1] * worldPos.y +
                        m.m[0][2] * worldPos.z + m.m[0][3];
        const float y = m.m[1][0] * worldPos.x + m.m[1][1] * worldPos.y +
                        m.m[1][2] * worldPos.z + m.m[1][3];

        const float invW = 1.0f / w;
        const float ndcX = x * invW;
        const float ndcY = y * invW;

        if (!std::isfinite(ndcX) || !std::isfinite(ndcY)) return false;

        screenPos.x = (screenWidth * 0.5f) * (1.0f + ndcX);
        screenPos.y = (screenHeight * 0.5f) * (1.0f - ndcY);
        return true;
    }

public:
    void SetScreenSize(int width, int height) {
        screenWidth = width;
        screenHeight = height;
    }

    void SetEntityPositions(const std::vector<Vector3>& positions) {
        entityPositions = positions;
    }

    /* the slot at dwViewMatrix always holds the current camera's matrix —
     * spectate transitions and camera changes need no invalidation. */
    void Invalidate() {}

    void UpdateViewMatrix(uintptr_t clientBase, Memory* memory) {
        ViewMatrix m{};
        if (!memory->ReadBuffer(clientBase + Offsets::dwViewMatrix, &m, sizeof(ViewMatrix))) {
            return;   // read hiccup — keep last good matrix
        }
        if (!ValidateBasic(m)) {
            return;   // transient garbage (loading screen) — keep last good
        }

        viewMatrix = m;

        if (matrixAddr == 0) {
            matrixAddr = clientBase + Offsets::dwViewMatrix;
            Log("[+] world-to-screen latched at dwViewMatrix (canonical offset)");
        }
    }

    bool MatrixSane() const {
        return matrixAddr != 0 && ValidateBasic(viewMatrix);
    }

    bool Transform(const Vector3& worldPos, Vector2& screenPos) const {
        if (!MatrixSane()) return false;
        return ProjectLayout(viewMatrix, worldPos, screenPos);
    }
};
