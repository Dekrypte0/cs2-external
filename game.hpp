#pragma once

#include "memory.hpp"
#include "offsets.hpp"
#include "math.hpp"
#include "logger.hpp"
#include <string>
#include <sstream>
#include <chrono>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <cstdint>

class GameEntity {
public:
    uintptr_t address = 0;
    int index = 0;

    bool valid = false;
    bool isLocal = false;      // this pawn is the player at the camera; the renderer decides whether to draw it
    uintptr_t sceneNode = 0;
    int health = 0;
    int team = 0;
    int armor = 0;
    bool isScoped = false;
    float distance = 0.f;
    Vector3 position{ 0, 0, 0 };
    std::string name;

    std::string GetTeamName() const {
        if (team == Offsets::TEAM_T) return "T";
        if (team == Offsets::TEAM_CT) return "CT";
        return "?";
    }
};

class Game {
private:
    Memory memory;
    WorldToScreen worldToScreen;
    int localTeam = 0;
    Vector3 localOrigin{};
    bool haveLocalOrigin = false;
    bool isSpectating = false;
    /* workshop / surf / practice maps routinely put every player on one team.
     * the team filter is meaningless there, and enforcing it hides the whole
     * server, so discovery flags it and the draw path drops the filter. */
    bool single_team_server = false;

    uintptr_t cached_local_pawn = 0;
    /* C_CSPlayerPawn vtable, learned from the resolved local pawn and kept as
     * a client.dll-relative RVA so a game restart does not invalidate it.
     * the entity list also holds hundreds of props with a scene-node
     * back-reference; the vtable is the one field that separates them. */
    std::ptrdiff_t pawn_vtable_rva = 0;
    uintptr_t cached_local_controller = 0;
    int cached_local_team = 0;
    int cached_local_slot = 0;             // controller slot (0x950) of the local player
    std::vector<uintptr_t> cached_slots;   // shared slot table, rebuilt on discovery
    std::chrono::steady_clock::time_point last_local_refresh{};

    /* controller discovery is index-based, not sweep-based: the identity pool
     * holds one slot-indexed record per player, so the whole roster is 64
     * pointer reads. poolBase is re-anchored from the local controller every
     * discovery, which survives map changes and controller reallocation. */
    uintptr_t controller_pool_base = 0;
    uintptr_t controller_vtable = 0;

    uintptr_t local_controller_address() const {
        const uintptr_t clientBase = memory.GetClientBase();
        if (!clientBase) return 0;
        const uintptr_t c = memory.Read<uintptr_t>(clientBase + Offsets::dwLocalPlayerController);
        if (c < 0x10000 || c > 0x7FFFFFFFFFFFULL) return 0;
        return c;
    }

    /* 16384 slots (32 entity chunks of 512) covers every server size this
     * build can allocate. pawns have been seen at 2369 and up; a workshop map
     * with a large entity budget pushes them higher still. */
    static constexpr int kSlotCount = 16384;

    int ReadEntityTeam(uintptr_t e) const {
        static constexpr std::ptrdiff_t kTeamCandidates[] = {
            Offsets::m_iTeamNum, 0x3E3, 0x3EB, 0x3EF, 0x3DF, 0x3F3, 0x3FB
        };
        for (const auto off : kTeamCandidates) {
            const int t = static_cast<int>(memory.Read<uint8_t>(e + off));
            if (t == Offsets::TEAM_T || t == Offsets::TEAM_CT) return t;
        }
        return 0;
    }

    int ReadPawnTeam(uintptr_t pawn) const { return ReadEntityTeam(pawn); }

    int ReadPawnTeamFromBuf(const uint8_t* buf, size_t len) const {
        static constexpr std::ptrdiff_t kTeamCandidates[] = {
            Offsets::m_iTeamNum, 0x3E3, 0x3EB, 0x3EF, 0x3DF, 0x3F3, 0x3FB
        };
        for (const auto off : kTeamCandidates) {
            if (static_cast<size_t>(off) >= len) continue;
            const int t = static_cast<int>(buf[off]);
            if (t == Offsets::TEAM_T || t == Offsets::TEAM_CT) return t;
        }
        return 0;
    }

    int ReadPawnHealthAt(uintptr_t e) const {
        static constexpr std::ptrdiff_t kHpCandidates[] = {
            Offsets::m_iHealth, 0x344, 0x348, 0x350, 0x34C, 0x340
        };
        for (const auto off : kHpCandidates) {
            const int v = memory.Read<int>(e + off);
            if (v >= 1 && v <= 100) return v;
        }
        return memory.Read<int>(e + Offsets::m_iHealth);
    }

    uint8_t ReadPawnLifeStateAt(uintptr_t e) const {
        static constexpr std::ptrdiff_t kLifeCandidates[] = {
            Offsets::m_lifeState, 0x350, 0x358, 0x354, 0x34F, 0x353
        };
        for (const auto off : kLifeCandidates) {
            const uint8_t v = memory.Read<uint8_t>(e + off);
            if (v == Offsets::LIFE_ALIVE) return v;
        }
        return memory.Read<uint8_t>(e + Offsets::m_lifeState);
    }

    /* structural pawn test. this must be strict about life state: it is what
     * decides whether the local player is alive, and a relaxed version latches
     * onto a corpse, forcing the whole overlay into spectate mode (team filter
     * off, everyone drawn). scene+0x30 must point back at the entity, which is
     * what separates a real pawn from the hundreds of props that also own a
     * scene node on workshop maps. */
    bool LooksLikePawn(uintptr_t e) const {
        if (!e || e < 0x10000 || e > 0x7FFFFFFFFFFFULL) return false;
        const uintptr_t sn = memory.Read<uintptr_t>(e + Offsets::m_pGameSceneNode);
        if (!sn || sn < 0x10000 || sn > 0x7FFFFFFFFFFFULL) return false;
        if (memory.Read<uintptr_t>(sn + 0x30) != e) return false;
        const int hp = ReadPawnHealthAt(e);
        if (hp < 1 || hp > 100) return false;
        if (ReadPawnLifeStateAt(e) != Offsets::LIFE_ALIVE) return false;
        const int team = ReadPawnTeam(e);
        return team == Offsets::TEAM_T || team == Offsets::TEAM_CT || team == 0;
    }

    bool LooksLikePawnLoose(uintptr_t e) const {
        if (!e || e < 0x10000 || e > 0x7FFFFFFFFFFFULL) return false;
        const uintptr_t sn = memory.Read<uintptr_t>(e + Offsets::m_pGameSceneNode);
        if (!sn || sn < 0x10000 || sn > 0x7FFFFFFFFFFFULL) return false;
        if (memory.Read<uintptr_t>(sn + 0x30) != e) return false;
        return true;
    }

    std::vector<uintptr_t> BuildSlots(uintptr_t entSys = 0) const {
        std::vector<uintptr_t> slots(kSlotCount, 0);
        if (!entSys) {
            const uintptr_t clientBase = memory.GetClientBase();
            if (!clientBase) return slots;
            entSys = memory.Read<uintptr_t>(clientBase + Offsets::dwEntityList);
        }
        if (!entSys) return slots;
        /* one read per chunk instead of two per slot: a 512-entry chunk is
         * 0xE000 contiguous bytes, so a fully populated table costs a handful
         * of reads instead of thousands of syscalls. chunk pointers are
         * allocated lazily, so the null check also skips most of the range. */
        static thread_local std::vector<uint8_t> chunk(0xE000);
        for (int b = 0; b < 32; b++) {
            const uintptr_t batch = memory.Read<uintptr_t>(entSys + 8i64 * b + 16);
            if (!batch || batch < 0x10000) continue;
            if (!memory.ReadBuffer(batch, chunk.data(), chunk.size())) continue;
            for (int j = 0; j < 512; j++) {
                uintptr_t entity = 0;
                std::memcpy(&entity, chunk.data() + 0x70 * j, sizeof(entity));
                if (!entity || entity < 0x10000 || entity > 0x7FFFFFFFFFFFULL) continue;
                const int idx = (b << 9) | j;
                if (idx < kSlotCount) slots[idx] = entity;
            }
        }
        return slots;
    }

    uintptr_t PawnFromControllerHandle(uintptr_t controller, const std::vector<uintptr_t>& slots, int wantTeam = 0) const {
        if (!controller) return 0;
        for (const std::ptrdiff_t field : { Offsets::m_hPlayerPawn, Offsets::m_hPlayerPawnAlt,
                                            Offsets::m_hPawnAlt2, Offsets::m_hPawnFallback }) {
            const uint32_t handle = memory.Read<uint32_t>(controller + field);
            if (!handle || handle == 0xFFFFFFFFu) continue;
            const uint32_t decodes[7] = {
                handle & 0x3FFFu, handle & 0x1FFFu, handle & 0x7FFFu,
                handle & 0xFFFu,  handle & 0xFFFFu, handle >> 14, handle >> 15
            };
            for (const uint32_t idx : decodes) {
                if (idx <= 0 || idx >= static_cast<uint32_t>(slots.size())) continue;
                const uintptr_t cand = slots[idx];
                if (!cand) continue;
                if (!LooksLikePawn(cand) && !LooksLikePawnLoose(cand)) continue;
                if (wantTeam) {
                    const int ct = ReadPawnTeam(cand);
                    if ((ct == Offsets::TEAM_T || ct == Offsets::TEAM_CT) && ct != wantTeam) continue;
                }
                // prefer controller handle backref when available
                const uint32_t ch = memory.Read<uint32_t>(cand + Offsets::m_hController);
                if (ch && ch != 0xFFFFFFFFu) {
                    // accept; some builds vary but handle decode matched
                }
                return cand;
            }
        }
        return 0;
    }

    /* the pawn handle moved in the Nov 17 build (0x92C reads a dead handle on a
     * 10-player server), and the layout shifts every patch. probe every field
     * ever observed carrying it, over every plausible decode, and accept the
     * first candidate that is structurally a pawn. returning 0 is normal on
     * this build — LinkNames() has a fallback that does not need a handle. */
    uintptr_t PawnFromController(uintptr_t controller, const std::vector<uintptr_t>& slots, int wantTeam = 0) const {
        if (!controller) return 0;
        for (const std::ptrdiff_t field : { Offsets::m_hPlayerPawn, Offsets::m_hPlayerPawnAlt,
                                            Offsets::m_hPawnAlt2, Offsets::m_hPawnFallback }) {
            const uint32_t handle = memory.Read<uint32_t>(controller + field);
            if (!handle || handle == 0xFFFFFFFFu) continue;
            const uint32_t decodes[7] = {
                handle & 0x3FFFu, handle & 0x1FFFu, handle & 0x7FFFu,
                handle & 0xFFFu,  handle & 0xFFFFu, handle >> 14, handle >> 15
            };
            for (const uint32_t idx : decodes) {
                if (idx <= 0 || idx >= static_cast<uint32_t>(slots.size())) continue;
                const uintptr_t cand = slots[idx];
                if (!cand || !LooksLikePawn(cand)) continue;
                /* never accept a pawn from the other team: while spectating,
                 * the camera sits over the enemy we are watching, and treating
                 * that pawn as "self" silently removed one enemy from the box
                 * list. */
                if (wantTeam) {
                    const int ct = ReadPawnTeam(cand);
                    if (ct != Offsets::TEAM_T && ct != Offsets::TEAM_CT) continue;
                    if (ct != wantTeam) continue;
                }
                /* a decoded handle can land on a valid pawn that is not ours —
                 * on this build the field often is not a real handle at all.
                 * only accept it when the camera sits on top of it, which is
                 * true of the local player's own eye and nobody else's. */
                const Vector3 cam = CameraPosition();
                if (cam.x || cam.y || cam.z) {
                    const uintptr_t sn = memory.Read<uintptr_t>(cand + Offsets::m_pGameSceneNode);
                    if (!sn) continue;
                    const Vector3 o = memory.Read<Vector3>(sn + Offsets::m_vecAbsOrigin);
                    const float dx = o.x - cam.x, dy = o.y - cam.y, dz = o.z - cam.z;
                    /* 100 units: the eye sits 63.6 above your feet, so
                     * anything inside this radius really is the pawn under
                     * the camera. 200 let a teammate standing 2-3 m away
                     * win the guess and then vanish from the box list. */
                    if (dx * dx + dy * dy + dz * dz > 100.0f * 100.0f) continue;
                }
                return cand;
            }
        }
        return 0;
    }

    /* the renderer's world->camera block: three orthonormal rows plus the
     * camera position folded into the fourth column, so undoing it is a
     * transpose multiply. verified live — the result lands 64 units above the
     * local pawn (standing eye height) on every sample. */
    Vector3 CameraPosition() const {
        const uintptr_t clientBase = memory.GetClientBase();
        if (!clientBase) return Vector3{};
        float m[12]{};
        if (!memory.ReadBuffer(clientBase + Offsets::dwViewMatrixView, m, sizeof(m))) return Vector3{};
        const float tx = m[3], ty = m[7], tz = m[11];
        return Vector3(-(tx * m[0] + ty * m[4] + tz * m[8]),
                       -(tx * m[1] + ty * m[5] + tz * m[9]),
                       -(tx * m[2] + ty * m[6] + tz * m[10]));
    }

    /* the controller's pawn handle is not resolvable on this build, but the
     * camera is: it sits at eye height directly above the local pawn every
     * frame, so the pawn nearest the camera is us. three independent live
     * samples put it 63.2 / 63.6 / 63.6 units away, dead vertical. */
    uintptr_t PawnFromCamera(const std::vector<uintptr_t>& slots, int wantTeam = 0) const {
        const Vector3 cam = CameraPosition();
        if (!cam.x && !cam.y && !cam.z) return 0;
        uintptr_t best = 0;
        float bestD = 300.0f;
        for (size_t i = 0; i < slots.size(); i++) {
            const uintptr_t e = slots[i];
            if (!e || !LooksLikePawn(e)) continue;
            /* a teammate can stand closer to the camera than our own pawn sits
             * below it. without the team filter the nearest body was sometimes
             * a mate, which flipped the local slot and the team filter with it. */
            if (wantTeam) {
                const int t = memory.Read<uint8_t>(e + Offsets::m_iTeamNum);
                if (t != wantTeam) continue;
            }
            const uintptr_t sn = memory.Read<uintptr_t>(e + Offsets::m_pGameSceneNode);
            const Vector3 o = memory.Read<Vector3>(sn + Offsets::m_vecAbsOrigin);
            const float dx = o.x - cam.x, dy = o.y - cam.y, dz = o.z - cam.z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d < bestD) { bestD = d; best = e; }
        }
        return best;
    }

    void RefreshLocal(const std::vector<uintptr_t>& slots) {
        cached_local_controller = local_controller_address();
        const uintptr_t previous = cached_local_pawn;
        cached_local_pawn = 0;
        cached_local_team = 0;
        if (cached_local_controller) {
            const int t = ReadEntityTeam(cached_local_controller);
            if (t == Offsets::TEAM_T || t == Offsets::TEAM_CT) cached_local_team = t;
            const uint32_t rawSlot = memory.Read<uint32_t>(cached_local_controller + Offsets::m_playerSlotField);
            const int sl = static_cast<int>(rawSlot & 0x7FFFu);
            if (sl >= 1 && sl <= 64) cached_local_slot = sl;
        }
        /* exact slot match first. every discovered pawn carries the controller
         * slot it belongs to, so this finds the camera pawn without guessing
         * from proximity. the old order asked the controller handle first, and
         * its fallback radius could accept a teammate standing nearby. */
        if (!cached_local_pawn && cached_local_slot > 0) {
            for (const auto& kp : known_pawns) {
                if (kp.slot != cached_local_slot) continue;
                if (cached_local_team && kp.team != cached_local_team) continue;
                if (!LooksLikePawn(kp.pawn)) continue;
                cached_local_pawn = kp.pawn;
                break;
            }
        }
        if (!cached_local_pawn && cached_local_controller) {
            cached_local_pawn = PawnFromController(cached_local_controller, slots, cached_local_team);
        }
        if (!cached_local_pawn && previous && LooksLikePawn(previous)) {
            /* hysteresis: keep the pawn we already believed in while it is still
             * alive and close to the camera. without this a teammate standing
             * nearer than the eye height offset can steal the local slot for a
             * frame, flipping spectate on and off (and with it the team filter). */
            const int pt = memory.Read<uint8_t>(previous + Offsets::m_iTeamNum);
            const bool teamOk = !cached_local_team || pt == cached_local_team;
            const Vector3 cam = CameraPosition();
            const uintptr_t sn = memory.Read<uintptr_t>(previous + Offsets::m_pGameSceneNode);
            if (teamOk && sn && (cam.x || cam.y || cam.z)) {
                const Vector3 o = memory.Read<Vector3>(sn + Offsets::m_vecAbsOrigin);
                const float dx = o.x - cam.x, dy = o.y - cam.y, dz = o.z - cam.z;
                if (dx * dx + dy * dy + dz * dz < 250.0f * 250.0f) cached_local_pawn = previous;
            }
        }
        if (!cached_local_pawn) cached_local_pawn = PawnFromCamera(slots, cached_local_team);
        if (!cached_local_pawn) {
        const uintptr_t clientBase = memory.GetClientBase();
        if (clientBase) {
            const uintptr_t p = memory.Read<uintptr_t>(clientBase + Offsets::dwLocalPlayerPawn);
            if (LooksLikePawn(p) && (!cached_local_team ||
                ReadEntityTeam(p) == cached_local_team)) {
                cached_local_pawn = p;
            }
        }
        }
        /* deliberately no team write from the pawn here. the pawn can be a
         * camera fallback, and while dead the camera follows whoever we are
         * spectating, which made the filter hide that player's team instead of
         * ours. the controller is ours for the whole match. */
        if (cached_local_pawn) {
            const uintptr_t vt = memory.Read<uintptr_t>(cached_local_pawn);
            const uintptr_t cb = memory.GetClientBase();
            if (cb && vt > cb && vt < cb + 0x08000000) pawn_vtable_rva = static_cast<std::ptrdiff_t>(vt - cb);
        }
    }

    uintptr_t local_pawn_address() const { return cached_local_pawn; }

    struct cached_pawn_t {
        int idx = 0;
        uintptr_t pawn = 0;
        uintptr_t sceneNode = 0;
        std::string name;
        int health = 0;
        int team = 0;
        int armor = 0;
        int slot = -1;                                     // owning controller slot (m_hController & 0x3FFF)
        bool isScoped = false;
        uint8_t lifeState = 0;
        Vector3 position{};
        bool alive = false;
        bool have_good = false;                            // last_good holds a complete, valid snapshot
        std::chrono::steady_clock::time_point last_good{};
    };
    std::vector<cached_pawn_t> known_pawns;
    bool warned_no_entsys = false;
    std::chrono::steady_clock::time_point last_discovery{};
    std::chrono::steady_clock::time_point last_name_refresh{};

    /* the name lives INLINE at controller+0x6FC on this build â€” verified
     * live on every controller and the local player ('Patris', 'IRIS',
     * 'Sayak_ss', 'FocusBtw'). single read, no offset probing. requires a
     * NUL-terminated string, so non-controller entities fail the signature. */
    std::string ReadControllerName(uintptr_t controller) const {
        if (!controller || controller < 0x10000) return {};
        char buf[128] = {};
        if (!memory.ReadBuffer(controller + Offsets::m_iszPlayerName, buf, sizeof(buf) - 1)) return {};
        buf[sizeof(buf) - 1] = '\0';
        size_t len = 0;
        while (len < sizeof(buf) - 1 && buf[len] != '\0') {
            const unsigned char c = static_cast<unsigned char>(buf[len]);
            /* accept UTF-8: printable ASCII plus any high byte, so Cyrillic
             * names (ĞšĞ˜Ğ‘Ğ•Ğ _ĞŸĞĞŸĞ) and symbol-laden tags pass. only control
             * characters indicate this isn't a name string. */
            if (c < 0x20 || c == 0x7F) return {};
            ++len;
        }
        if (len < 1 || len > 64) return {};
        return std::string(buf, len);
    }

    /* resolve a CHandle against the slot table we already walked â€” no extra
     * reads. mask cascade covers the observed encoding (probe confirmed the
     * low 12 bits hit the slot index on every verified pair). */
    uintptr_t ResolveHandleInSlots(const std::vector<uintptr_t>& slots, uint32_t handle) const {
        if (!handle || handle == 0xFFFFFFFFu) return 0;
        for (const uint32_t mask : { 0xFFFu, 0x3FFFu, 0x7FFFu }) {
            const int idx = static_cast<int>(handle & mask);
            if (idx > 0 && idx < static_cast<int>(slots.size()) && slots[idx]) return slots[idx];
        }
        return 0;
    }

    /* resolve controller team/name by slot for pawns whose team/health bytes drifted */
    void ApplyControllerTeams() {
        if (roster.empty() || known_pawns.empty()) return;
        std::unordered_map<int, const CtrlInfo*> bySlot;
        bySlot.reserve(roster.size());
        for (const auto& c : roster) if (c.slot > 0) bySlot[c.slot] = &c;
        for (auto& kp : known_pawns) {
            if (kp.slot <= 0) continue;
            auto it = bySlot.find(kp.slot);
            if (it == bySlot.end()) continue;
            if (kp.team != Offsets::TEAM_T && kp.team != Offsets::TEAM_CT) kp.team = it->second->team;
            else if (it->second->team == Offsets::TEAM_T || it->second->team == Offsets::TEAM_CT) kp.team = it->second->team;
            if (kp.name.empty() && !it->second->name.empty()) kp.name = it->second->name;
        }
    }

    /* recover pawns missed by strict validation by matching entity->controller slot */
    void SupplementPawnsFromRoster(const std::vector<uintptr_t>& slots) {
        if (roster.empty() || slots.empty()) return;
        std::unordered_map<uintptr_t, bool> havePawn;
        for (const auto& kp : known_pawns) {
            if (kp.pawn) havePawn[kp.pawn] = true;
        }
        for (auto& ctrl : roster) {
            if (ctrl.slot <= 0) continue;

            uintptr_t e = 0;
            int idx = -1;

            // 1) controller pawn handle fields
            e = PawnFromControllerHandle(ctrl.addr, slots, ctrl.team);
            if (e) {
                for (int i = 0; i < static_cast<int>(slots.size()); i++) { if (slots[i] == e) { idx = i; break; } }
            }

            // 2) scan for pawn whose controller handle decodes to this slot
            if (!e) {
                for (int i = 0; i < static_cast<int>(slots.size()); i++) {
                    const uintptr_t cand = slots[i];
                    if (!cand || cand < 0x10000) continue;
                    if (!LooksLikePawnLoose(cand)) continue;
                    const uint32_t ch = memory.Read<uint32_t>(cand + Offsets::m_hController);
                    if (!ch || ch == 0xFFFFFFFFu) continue;
                    bool match = false;
                    const uint32_t masks[5] = { 0x3FFFu, 0x7FFFu, 0x1FFFu, 0xFFFu, 0xFFFFu };
                    for (auto m : masks) { if (static_cast<int>(ch & m) == ctrl.slot) { match = true; break; } }
                    if (!match) continue;
                    e = cand; idx = i; break;
                }
            }

            if (!e) continue;

            // enrich existing pawn entry if present
            bool exists = false;
            for (auto& kp : known_pawns) {
                if (kp.pawn == e) {
                    if (kp.slot <= 0) kp.slot = ctrl.slot;
                    if (kp.name.empty() && !ctrl.name.empty()) kp.name = ctrl.name;
                    if ((kp.team != Offsets::TEAM_T && kp.team != Offsets::TEAM_CT) &&
                        (ctrl.team == Offsets::TEAM_T || ctrl.team == Offsets::TEAM_CT)) kp.team = ctrl.team;
                    if (!kp.alive) kp.alive = true;
                    exists = true;
                    break;
                }
            }
            if (exists) { ctrl.pawn = e; continue; }
            if (havePawn[e]) { ctrl.pawn = e; continue; }

            cached_pawn_t c;
            c.idx = (idx >= 0 ? idx : 0);
            c.pawn = e;
            c.slot = ctrl.slot;
            c.name = ctrl.name;
            c.team = ctrl.team;

            const PawnRead read = RefreshPawnData(c);
            if (read == PawnRead::Dead) continue;
            if (read != PawnRead::Ok || !c.alive) {
                if (!LooksLikePawnLoose(e)) continue;
                const uintptr_t sn = memory.Read<uintptr_t>(e + Offsets::m_pGameSceneNode);
                if (!sn || sn < 0x10000) continue;
                c.sceneNode = sn;
                c.position = memory.Read<Vector3>(sn + Offsets::m_vecAbsOrigin);
                if (c.position.x == 0.0f && c.position.y == 0.0f && c.position.z == 0.0f) continue;
                int hp = ReadPawnHealthAt(e);
                if (hp < 1 || hp > 100) hp = 100;
                c.health = hp;
                c.lifeState = Offsets::LIFE_ALIVE;
                c.alive = true;
                c.have_good = true;
                c.last_good = std::chrono::steady_clock::now();
            }
            if (c.team != Offsets::TEAM_T && c.team != Offsets::TEAM_CT) c.team = ctrl.team;

            ctrl.pawn = e;
            known_pawns.push_back(c);
            havePawn[e] = true;
        }
    }

    /* three outcomes, because the drawing path must tell a fact from a miss:
     * Dead is the game saying "this pawn is gone" and the box should drop
     * instantly; Failed is a read that returned nothing and the box should
     * survive a few frames. Committing only on Ok keeps a partial read from
     * leaving half-updated garbage in the cached snapshot. */
    enum class PawnRead { Ok, Dead, Failed };

    int ReadPawnHealth(const uint8_t* buf, size_t len) const {
        static constexpr std::ptrdiff_t kHpCandidates[] = {
            Offsets::m_iHealth, 0x344, 0x348, 0x350, 0x34C, 0x340
        };
        for (const auto off : kHpCandidates) {
            if (static_cast<size_t>(off + sizeof(int)) > len) continue;
            int v = 0;
            std::memcpy(&v, buf + off, sizeof(int));
            if (v >= 1 && v <= 100) return v;
        }
        int fallback = 0;
        if (static_cast<size_t>(Offsets::m_iHealth + sizeof(int)) <= len) {
            std::memcpy(&fallback, buf + Offsets::m_iHealth, sizeof(int));
        }
        return fallback;
    }

    uint8_t ReadPawnLifeState(const uint8_t* buf, size_t len) const {
        static constexpr std::ptrdiff_t kLifeCandidates[] = {
            Offsets::m_lifeState, 0x350, 0x358, 0x354, 0x34F, 0x353
        };
        for (const auto off : kLifeCandidates) {
            if (static_cast<size_t>(off) >= len) continue;
            const uint8_t v = buf[off];
            if (v == Offsets::LIFE_ALIVE) return v;
        }
        if (static_cast<size_t>(Offsets::m_lifeState) < len) return buf[Offsets::m_lifeState];
        return 0xFF;
    }

    PawnRead RefreshPawnData(cached_pawn_t& c) {
        c.alive = false;
        if (!c.pawn) return PawnRead::Failed;

        /* one read covers scene node + health + team + life state, then a
         * second read pulls the origin off the scene node. two syscalls per
         * pawn per frame instead of nine — this runs every frame, so it is
         * the number that decides whether the boxes feel glued to the bodies
         * or a beat behind them. */
        static thread_local std::vector<uint8_t> buf(0x400);
        if (!memory.ReadBuffer(c.pawn, buf.data(), buf.size())) return PawnRead::Failed;

        uintptr_t sceneNode = 0;
        std::memcpy(&sceneNode, buf.data() + Offsets::m_pGameSceneNode, sizeof(sceneNode));
        const int health = ReadPawnHealth(buf.data(), buf.size());
        int team = ReadPawnTeamFromBuf(buf.data(), buf.size());
        const uint8_t lifeState = ReadPawnLifeState(buf.data(), buf.size());

        if (!sceneNode) return PawnRead::Failed;
        /* team/lifestate bytes can be offset-drifted; keep alive pawns by hp+scene. */

        Vector3 position{};
        if (!memory.ReadBuffer(sceneNode + Offsets::m_vecAbsOrigin, &position, sizeof(Vector3))) return PawnRead::Failed;

        if (health <= 0 || health > 100) return PawnRead::Dead;

        c.sceneNode = sceneNode;
        c.health = health;
        c.team = team;
        c.lifeState = lifeState;
        c.position = position;
        c.alive = true;
        c.last_good = std::chrono::steady_clock::now();
        c.have_good = true;
        return PawnRead::Ok;
    }

    /* a controller as the game actually stores it on this build: name string
     * inline, team byte, join slot. the pawn handle is gone, so this struct is
     * everything the linker has to work with. */
    struct CtrlInfo {
        uintptr_t addr = 0;
        int idx = 0;
        int team = 0;
        int slot = -1;
        bool local = false;
        std::string name;
        uintptr_t pawn = 0;
    };
    std::vector<CtrlInfo> roster;
    std::string link_mode = "none";

    bool IsControllerInstance(uintptr_t addr) const {
        if (addr < 0x10000 || addr > 0x7FFFFFFFFFFFULL) return false;
        if (!controller_vtable) return false;
        return memory.Read<uintptr_t>(addr) == controller_vtable;
    }

    uintptr_t IdentityOf(uintptr_t entity) const {
        if (entity < 0x10000) return 0;
        const uintptr_t id = memory.Read<uintptr_t>(entity + Offsets::m_pEntityIdentity);
        if (id < 0x10000 || id > 0x7FFFFFFFFFFFULL) return 0;
        return id;
    }

    /* slot is the primary key everywhere. 0x950 carries it directly; 0x4C4
     * mirrors it on this build and is used only to reject a mid-write record,
     * never to invent a slot the primary did not provide. */
    int ReadControllerSlot(uintptr_t e) const {
        if (!e) return -1;
        const int primary = static_cast<int>(memory.Read<uint32_t>(e + Offsets::m_playerSlotField) & 0x7FFFu);
        const int alt = memory.Read<int>(e + Offsets::m_hPawnAlt2);
        if (primary >= 1 && primary <= 64) {
            if (alt >= 1 && alt <= 64 && alt != primary) return -1;
            return primary;
        }
        /* in the main menu 0x950 reads 0x7FFF until the match assigns a real
         * slot, while 0x4C4 already holds it (verified: local controller
         * identity sat exactly on the slot-1 grid record while 0x950 was
         * 32767). the mirror keeps the pool anchorable across menu -> match. */
        if (alt >= 1 && alt <= 64) return alt;
        return -1;
    }

    /* anchors the slot-indexed identity grid from whichever controller can be
     * trusted right now. the local controller is exact; a roster survivor from
     * the previous pass keeps discovery alive while the local global is null
     * (menu, map load). */
    void RefreshControllerPool(const std::vector<CtrlInfo>& previous) {
        const uintptr_t clientBase = memory.GetClientBase();
        uintptr_t vt = 0;
        if (cached_local_controller) vt = memory.Read<uintptr_t>(cached_local_controller);
        if (vt < 0x10000 && clientBase) vt = clientBase + Offsets::dwControllerVtable;
        controller_vtable = vt;

        auto derive = [&](uintptr_t controller, int slot) -> uintptr_t {
            if (!controller || slot < 1 || slot > 64) return 0;
            const uintptr_t id = IdentityOf(controller);
            if (!id) return 0;
            const uintptr_t base = id
                - static_cast<uintptr_t>(Offsets::m_controllerIdentityHead)
                - static_cast<uintptr_t>(Offsets::m_controllerIdentityStride) * static_cast<uintptr_t>(slot);
            if (base < 0x10000 || base > 0x7FFFFFFFFFFFULL) return 0;
            if (base & 0x7u) return 0;
            return base;
        };

        if (cached_local_controller) {
            const int localSlot = ReadControllerSlot(cached_local_controller);
            const uintptr_t base = derive(cached_local_controller, localSlot);
            if (base) { controller_pool_base = base; return; }
        }
        for (const auto& c : previous) {
            if (!c.addr || c.slot < 1 || c.slot > 64) continue;
            if (!IsControllerInstance(c.addr)) continue;
            const uintptr_t base = derive(c.addr, c.slot);
            if (base) { controller_pool_base = base; return; }
        }
        /* keep the last known base; the walk below simply yields nothing if
         * the allocation moved, and the next pass re-anchors as soon as a
         * controller is readable again. */
    }

    /* builds the whole roster from the identity grid. one record per slot,
     * no pool striding, no order heuristics, and the vtable check keeps
     * non-controller entities from ever entering the roster. */
    void BuildRoster(const std::vector<uintptr_t>& slots) {
        (void)slots;
        const std::vector<CtrlInfo> previous = roster;
        roster.clear();
        if (!cached_local_controller) cached_local_controller = local_controller_address();
        RefreshControllerPool(previous);

        if (!controller_pool_base) {
            link_mode = "pool-unresolved";
            return;
        }

        const uintptr_t stride = static_cast<uintptr_t>(Offsets::m_controllerIdentityStride);
        const uintptr_t head = static_cast<uintptr_t>(Offsets::m_controllerIdentityHead);
        auto push_ctrl = [&](uintptr_t inst, int slotHint) {
            if (!IsControllerInstance(inst)) return false;
            const std::string name = ReadControllerName(inst);
            if (name.empty()) return false;
            int team = ReadEntityTeam(inst);
            int slot = ReadControllerSlot(inst);
            if (slot <= 0 || slot > 64) slot = slotHint;
            if (slot <= 0 || slot > 64) return false;

            for (const auto& ex : roster) if (ex.slot == slot) return true;

            CtrlInfo ci;
            ci.addr = inst;
            ci.idx = slot;
            ci.team = team;
            ci.slot = slot;
            ci.local = (inst == cached_local_controller) || (cached_local_slot > 0 && slot == cached_local_slot);
            ci.name = name;
            roster.push_back(std::move(ci));
            return true;
        };

        int found = 0;
        for (int slot = 1; slot <= 64; slot++) {
            const uintptr_t identity = controller_pool_base + head + stride * static_cast<uintptr_t>(slot);
            const uintptr_t inst = memory.Read<uintptr_t>(identity);
            const uintptr_t ownedSlot = ReadControllerSlot(inst);
            if (ownedSlot > 0 && ownedSlot != slot) continue;
            if (push_ctrl(inst, slot)) found++;
        }

        // fallback scan: controller pool layout can drift; recover by scanning entity slots
        if (found == 0) {
            for (size_t i = 0; i < slots.size(); i++) {
                const uintptr_t e = slots[i];
                if (!e || e < 0x10000) continue;
                if (!controller_vtable || memory.Read<uintptr_t>(e) != controller_vtable) continue;
                push_ctrl(e, static_cast<int>(i & 0x3FFFu));
                if (roster.size() >= 64) break;
            }
            link_mode = found == 0 ? "scan" : "slot+scan";
        } else {
            // augment with scan in case pool missed some controllers
            for (size_t i = 0; i < slots.size(); i++) {
                const uintptr_t e = slots[i];
                if (!e || e < 0x10000) continue;
                if (!controller_vtable || memory.Read<uintptr_t>(e) != controller_vtable) continue;
                push_ctrl(e, static_cast<int>(i & 0x3FFFu));
                if (roster.size() >= 64) break;
            }
            link_mode = "slot";
        }
    }

    /* names bind by slot only. the pawn carries its controller slot at
     * +0x14BC, so this is a dictionary lookup, not a permutation guess. a
     * pawn and controller on different teams still share a slot; the name
     * belongs to the slot, and the box's team comes from the pawn. */
    void LinkNames(const std::vector<uintptr_t>& slots) {
        (void)slots;
        for (auto& c : roster) c.pawn = 0;

        int named = 0;
        for (auto& kp : known_pawns) {
            if (kp.slot <= 0) continue;
            for (const auto& ctrl : roster) {
                if (ctrl.slot != kp.slot) continue;
                if (!ctrl.name.empty()) {
                    kp.name = ctrl.name;
                    named++;
                }
                break;
            }
        }
        for (auto& ctrl : roster) {
            if (ctrl.slot <= 0) continue;
            for (const auto& kp : known_pawns) {
                if (kp.slot != ctrl.slot) continue;
                ctrl.pawn = kp.pawn;
                break;
            }
        }

        /* one line showing every controller and the pawn index it owns (-1 when
         * unbound). this is the line that explains blank names on screen. */
        static std::string lastDetail;
        std::ostringstream oss;
        oss << "[+] link: " << link_mode << " named=" << named << "/" << known_pawns.size()
            << " controllers=" << roster.size();
        for (const auto& c : roster) {
            int pidx = -1;
            if (c.pawn) {
                for (const auto& kp : known_pawns) {
                    if (kp.pawn == c.pawn) { pidx = kp.idx; break; }
                }
            }
            oss << " [" << c.slot << "t" << c.team << "->" << pidx << "]";
        }
        if (oss.str() != lastDetail) {
            lastDetail = oss.str();
            Log(oss.str());
        }
    }

    void ResolveNames(const std::vector<uintptr_t>& slotEntities) {
        BuildRoster(slotEntities);
        LinkNames(slotEntities);

        /* apply; log only genuine transitions so respawns and repeated
         * discoveries do not spam the console. names are never guessed and a
         * real player is never labelled a bot: a slot without a readable
         * controller keeps the name proven by an earlier pass, and a slot with
         * nothing at all falls back to a neutral #<slot> tag. */
        static std::unordered_map<uintptr_t, std::string> logged;
        int named = 0;
        for (auto& c : known_pawns) {
            if (c.name.empty() && c.slot > 0) c.name = "#" + std::to_string(c.slot);
            if (c.name.empty()) continue;
            named++;
            auto lit = logged.find(c.pawn);
            if (lit == logged.end() || lit->second != c.name) {
                Log("[+] name resolved: " + c.name + " -> idx " + std::to_string(c.idx) +
                    " slot " + std::to_string(c.slot));
                logged[c.pawn] = c.name;
            }
        }
        if (logged.size() > 512) logged.clear();

        /* one line per distinct roster/link state, so the log proves which
         * slot got which player */
        static std::string lastSig;
        std::string sig = link_mode + "|";
        for (const auto& ctrl : roster) {
            sig += std::to_string(ctrl.slot) + ":" + ctrl.name + ":" + std::to_string(ctrl.team) + ">";
            if (ctrl.pawn) {
                for (const auto& kp : known_pawns) if (kp.pawn == ctrl.pawn) { sig += std::to_string(kp.idx); break; }
            }
            sig += ";";
        }
        if (sig != lastSig) {
            lastSig = sig;
            Log("[+] names linked via " + link_mode + " (" + std::to_string(named) + "/" +
                std::to_string(known_pawns.size()) + " pawns named, " + std::to_string(roster.size()) + " controllers)");
            if (link_mode == "pool-unresolved") {
                Log("[!] controller pool unresolved (waiting for local controller)");
            }
            for (const auto& ctrl : roster) {
                if (!ctrl.pawn) continue;
                for (const auto& kp : known_pawns) {
                    if (kp.pawn != ctrl.pawn) continue;
                    Log("    slot " + std::to_string(ctrl.slot) + " t" + std::to_string(ctrl.team) +
                        " '" + ctrl.name + "' -> pawn idx " + std::to_string(kp.idx));
                    break;
                }
            }
        }
    }

    void Discover() {
        /* keep the previous snapshots by value: every discovery rebuilds the
         * list from scratch, and without this a single missed read at the
         * wrong moment erased a player for a full 500 ms. names ride along
         * too, so a link that fails one pass does not blank a box. */
        std::unordered_map<uintptr_t, cached_pawn_t> previous;
        previous.reserve(known_pawns.size());
        for (const auto& kp : known_pawns) previous.emplace(kp.pawn, kp);
        known_pawns.clear();
        const auto now = std::chrono::steady_clock::now();

        const uintptr_t clientBase = memory.GetClientBase();
        const uintptr_t entSys = memory.Read<uintptr_t>(clientBase + Offsets::dwEntityList);
        if (!entSys) {
            /* silent while the game is absent (menu still renders) â€” one
             * warning per detached episode is plenty */
            if (!warned_no_entsys) {
                Log("[!] entity list unreachable (game closed or loading?)");
                warned_no_entsys = true;
            }
            return;
        }
        warned_no_entsys = false;

        cached_slots = BuildSlots(entSys);
        std::vector<uintptr_t>& slotEntities = cached_slots;

        /* the entity table aliases one pawn across several slots on workshop
         * builds (a local pawn showed up at three indices pointing at the same
         * object). without this dedupe each alias would draw its own box on
         * top of the same body. */
        std::unordered_map<uintptr_t, char> seenPawn;
        std::unordered_map<int, int> seenSlotTeam; // slot -> team kept (clone filter)
        std::vector<std::string> rejectedPlayers;
        for (int idx = 0; idx < kSlotCount; idx++) {
            const uintptr_t entity = slotEntities[idx];
            if (!entity) continue;

            cached_pawn_t c;
            c.idx = idx;
            c.pawn = entity;
            const auto prevIt = previous.find(entity);
            if (prevIt != previous.end()) {
                const cached_pawn_t& old = prevIt->second;
                if (!old.name.empty()) c.name = old.name;
                c.slot = old.slot;
                c.have_good = old.have_good;
                c.last_good = old.last_good;
            }
            const PawnRead read = RefreshPawnData(c);
            if (read == PawnRead::Failed) {
                /* unreadable this pass, but this exact pawn was fully readable
                 * moments ago: keep the last good snapshot instead of making
                 * the box blink out for half a second. */
                const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - c.last_good).count();
                if (c.have_good && age < 1200) {
                    c.alive = true;
                } else {
                    /* note player-team slots that were refused: a dead body is the
                     * expected case, anything else means the structural test is
                     * rejecting a real player and the box simply vanishes. */
                    if (rejectedPlayers.size() < 6) {
                        const int tm = ReadPawnTeam(entity);
                        if (tm == Offsets::TEAM_T || tm == Offsets::TEAM_CT) {
                            const int hp = memory.Read<int>(entity + Offsets::m_iHealth);
                            const int life = memory.Read<uint8_t>(entity + Offsets::m_lifeState);
                            rejectedPlayers.push_back(std::to_string(idx) + " t" + std::to_string(tm) +
                                                      " hp" + std::to_string(hp) + " life" + std::to_string(life));
                        }
                    }
                    continue;
                }
            } else if (read == PawnRead::Dead) {
                if (rejectedPlayers.size() < 6) {
                    const int hp = memory.Read<int>(entity + Offsets::m_iHealth);
                    const int life = memory.Read<uint8_t>(entity + Offsets::m_lifeState);
                    rejectedPlayers.push_back(std::to_string(idx) + " t" + std::to_string(c.team) +
                                              " hp" + std::to_string(hp) + " life" + std::to_string(life));
                }
                continue;
            }

            /* the pawn -> controller handle is the one link that survives on
             * this build: low 14 bits are the controller's identity index,
             * which equals the player slot. this is what names bind to. */
            const uint32_t ctrlHandle = memory.Read<uint32_t>(entity + Offsets::m_hController);
            const int pawnSlot = static_cast<int>(ctrlHandle & 0x3FFFu);
            c.slot = (pawnSlot >= 1 && pawnSlot <= 64) ? pawnSlot : -1;

            /* this build keeps 3-4 synchronised clones per player on the same
             * slot. dedupe by slot (lowest entity index wins because the walk
             * ascends) so one body never draws four boxes. if the slot value
             * collides across different teams it is unreliable (offset drift),
             * so keep the pawn rather than hiding a real player. */
            if (c.slot > 0) {
                auto sit = seenSlotTeam.find(c.slot);
                if (sit != seenSlotTeam.end()) {
                    if (sit->second == c.team) continue;
                    // slot id looks bogus; fall through and keep this pawn
                } else {
                    seenSlotTeam[c.slot] = c.team;
                }
            }
            if (seenPawn.count(c.pawn)) continue;
            seenPawn[c.pawn] = 1;
            known_pawns.push_back(std::move(c));
        }

        // resolve the local pawn now that the fresh alive list exists: the
        // exact controller-slot match is the most reliable self test, and it
        // cannot latch onto a teammate the way a camera-proximity guess can.
        RefreshLocal(slotEntities);
        last_local_refresh = std::chrono::steady_clock::now();

        // controller roster from the slot-indexed identity grid, then bind by slot
        ResolveNames(slotEntities);
        SupplementPawnsFromRoster(slotEntities);
        ApplyControllerTeams();

        /* the roster supplement can re-add a pawn the slot walk already found:
         * a controller handle decodes to a high-index alias of the same player
         * (same slot/name, hp 1-36, bogus distance). left in, those aliases draw
         * stale boxes on top of the live ones and inflate the entity count.
         * the main walk runs first and its low-index pawn is the live one. */
        {
            std::unordered_map<int, char> keepSlot;
            keepSlot.reserve(known_pawns.size());
            std::vector<cached_pawn_t> deduped;
            deduped.reserve(known_pawns.size());
            const uintptr_t cb = memory.GetClientBase();
            const uintptr_t pawnVt = (cb && pawn_vtable_rva) ? cb + pawn_vtable_rva : 0;
            for (auto& kp : known_pawns) {
                if (kp.slot > 0 && keepSlot.count(kp.slot)) continue;
                /* props that satisfy the scene-node back-reference and carry a
                 * controller-shaped field otherwise survive as ghost players.
                 * the local pawn teaches us the real pawn vtable; require it. */
                if (pawnVt && kp.pawn && memory.Read<uintptr_t>(kp.pawn) != pawnVt) continue;
                if (kp.slot > 0) keepSlot[kp.slot] = 1;
                deduped.push_back(std::move(kp));
            }
            known_pawns = std::move(deduped);
        }

        // infer local team from roster/self if controller/pawn team offsets are bad
        if (localTeam != Offsets::TEAM_T && localTeam != Offsets::TEAM_CT) {
            const uintptr_t lp = local_pawn_address();
            for (const auto& kp : known_pawns) {
                const bool localBySlot = cached_local_slot > 0 && kp.slot == cached_local_slot;
                const bool localByPawn = lp && kp.pawn == lp;
                if (!(localByPawn || (localBySlot && !lp))) continue;
                if (kp.team == Offsets::TEAM_T || kp.team == Offsets::TEAM_CT) { cached_local_team = localTeam = kp.team; break; }
            }
        }
        if (localTeam != Offsets::TEAM_T && localTeam != Offsets::TEAM_CT && cached_local_slot > 0) {
            for (const auto& c : roster) {
                if (c.slot == cached_local_slot && (c.team == Offsets::TEAM_T || c.team == Offsets::TEAM_CT)) {
                    cached_local_team = localTeam = c.team; break;
                }
            }
        }

        /* one-team detection: workshop maps, surf, aim and practice servers all
         * spawn everyone on the same team. decide from the CONTROLLER roster,
         * not the alive pawns: late in a round every living player can be on
         * one side, and reading that as a single-team server bypasses the
         * filter and boxes our own teammates. */
        {
            bool hasT = false, hasCT = false;
            for (const auto& c : roster) {
                if (c.team == Offsets::TEAM_T) hasT = true;
                else if (c.team == Offsets::TEAM_CT) hasCT = true;
            }
            if (!hasT && !hasCT) {
                for (const auto& c : known_pawns) {
                    if (c.team == Offsets::TEAM_T) hasT = true;
                    else if (c.team == Offsets::TEAM_CT) hasCT = true;
                }
            }
            const bool single = (known_pawns.size() >= 1) && !(hasT && hasCT);
            if (single != single_team_server) {
                single_team_server = single;
                Log(single ? "[+] single-team server: team filter will be bypassed"
                           : "[+] two-team server: team filter active");
            }
        }

        {
            static std::string lastDiscoverLog;
            std::ostringstream oss;
            oss << "[+] Discovered " << known_pawns.size() << " entities (local team=" << cached_local_team
                << ", local pawn=" << (local_pawn_address() ? "yes" : "no") << ")";
            if (oss.str() != lastDiscoverLog) {
                lastDiscoverLog = oss.str();
                Log(oss.str());
            }
        }

        /* the whole table at a glance, so a box that is missing on screen can be
         * matched against what memory actually holds — alive pawns plus the
         * player-team slots that were refused and why */
        {
            static std::string lastTableLog;
            std::ostringstream oss;
            oss << "[+] table: alive=" << known_pawns.size();
            for (const auto& c : known_pawns) {
                oss << " [" << c.idx << " t" << c.team << " hp" << c.health << "]";
            }
            if (!rejectedPlayers.empty()) {
                oss << " rejected:";
                for (const auto& r : rejectedPlayers) oss << " " << r;
            }
            if (oss.str() != lastTableLog) {
                lastTableLog = oss.str();
                Log(oss.str());
            }
        }
    }

public:
    bool Initialize(bool verbose = true) {
        if (!memory.Initialize(verbose)) {
            if (verbose) Log("[!] Failed to init memory");
            return false;
        }
        return true;
    }

    bool Update() {
        /* survive game restarts: validate the handle every ~2s, re-hunt and
         * re-resolve module bases when CS2 closed or relaunched */
        if (!memory.EnsureAttached()) return false;
        const uintptr_t clientBase = memory.GetClientBase();
        if (!clientBase) return false;

        haveLocalOrigin = false;
        const bool wasSpectating = isSpectating;
        isSpectating = false;

        const auto localNow = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(localNow - last_local_refresh).count() >= 500) {
            /* reuse the table discovery already paid for — rebuilding it here
             * doubled the cost of every walk for no new information */
            if (cached_slots.empty()) cached_slots = BuildSlots();
            RefreshLocal(cached_slots);
            last_local_refresh = localNow;
        }

        const uintptr_t localPawn = local_pawn_address();
        const int pawnTeam = localPawn ? ReadEntityTeam(localPawn) : 0;

        /* team comes from the controller only. a pawn-derived team is unsafe:
         * while dead the camera tracks the spectated player, and trusting that
         * pawn's team made the filter hide the watched player's side while
         * drawing our own team — teammates boxed, enemies invisible. hold the
         * last good value if a read fails so the filter never flickers. */
        if (cached_local_team == Offsets::TEAM_T || cached_local_team == Offsets::TEAM_CT) {
            localTeam = cached_local_team;
        } else if (pawnTeam == Offsets::TEAM_T || pawnTeam == Offsets::TEAM_CT) {
            localTeam = pawnTeam;
        }
        /* do not guess spectate when local team is unknown; wrong guess flips
         * filters and can hide players. */
        if (localTeam == Offsets::TEAM_T || localTeam == Offsets::TEAM_CT) {
            isSpectating = false;
        } else {
            isSpectating = !localPawn;
        }

        if (localPawn) {
            const uintptr_t node = memory.Read<uintptr_t>(localPawn + Offsets::m_pGameSceneNode);
            if (node) {
                localOrigin.x = memory.Read<float>(node + Offsets::m_vecAbsOrigin);
                localOrigin.y = memory.Read<float>(node + Offsets::m_vecAbsOrigin + 0x4);
                localOrigin.z = memory.Read<float>(node + Offsets::m_vecAbsOrigin + 0x8);
                haveLocalOrigin = true;
            }
        }
        /* dead or between rounds there is no local pawn, but the camera is
         * still exactly 64 units above where our feet would be. without this
         * fallback every distance collapsed to 0 while spectating. */
        if (!haveLocalOrigin) {
            const Vector3 cam = CameraPosition();
            if (cam.x || cam.y || cam.z) {
                localOrigin.x = cam.x;
                localOrigin.y = cam.y;
                localOrigin.z = cam.z - Offsets::kEyeHeight;
                haveLocalOrigin = true;
            }
        }

        {
            static auto lastLocalLog = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(localNow - lastLocalLog).count() >= 2000) {
                lastLocalLog = localNow;
                std::ostringstream oss;
                oss << "[+] local: name='" << ReadControllerName(cached_local_controller) << "'"
                    << " ctrl=0x" << std::hex << cached_local_controller
                    << " ctrlTeam=" << std::dec << cached_local_team
                    << " pawn=0x" << std::hex << localPawn
                    << " pawnTeam=" << std::dec << pawnTeam
                    << " used=" << localTeam
                    << " spectate=" << (isSpectating ? 1 : 0)
                    << " single=" << (single_team_server ? 1 : 0);
                Log(oss.str());
            }
        }

        // Refresh cached pawn dynamic data once per frame for smooth ESP.
        // The local pawn is refreshed too: discovery must contain every alive
        // player, and the renderer decides whether the camera pawn draws.
        std::vector<Vector3> entPositions;
        entPositions.reserve(known_pawns.size());
        for (auto& cached : known_pawns) {
            if (!cached.pawn) { cached.alive = false; continue; }
            const PawnRead read = RefreshPawnData(cached);
            if (read == PawnRead::Dead) continue;
            if (read == PawnRead::Failed) {
                /* one missed ReadProcessMemory must not blink a box away. the
                 * snapshot is untouched on a failed read, so re-arm it if it
                 * was complete recently. */
                const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(localNow - cached.last_good).count();
                if (!(cached.have_good && age < 150)) continue;
                cached.alive = true;
            }

            if (cached.position.x != 0.0f || cached.position.y != 0.0f || cached.position.z != 0.0f) {
                entPositions.push_back(cached.position);
            }
        }

        if (wasSpectating != isSpectating) {
            worldToScreen.Invalidate();
            Log(std::string("[+] Spectate state: ") + (isSpectating ? "on" : "off"));
        }

        worldToScreen.SetEntityPositions(entPositions);
        worldToScreen.UpdateViewMatrix(clientBase, &memory);
        return true;
    }

    std::vector<GameEntity> GetEntities() {
        std::vector<GameEntity> out;

        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_discovery).count() >= 500) {
            Discover();
            last_discovery = now;
        }

        const uintptr_t localPawn = local_pawn_address();
        out.reserve(known_pawns.size());

        static int lastCount = -1;
        int validCount = 0;
        int localCount = 0;
        int seenT = 0, seenCT = 0;

        for (const auto& cached : known_pawns) {
            /* prefer the exact local pawn address when known; slot identity can be
             * wrong if controller/pawn slot offsets drift, and mis-tagging hides a
             * teammate as "self". */
            const bool localBySlot = cached_local_slot > 0 && cached.slot == cached_local_slot;
            const bool localByPawn = localPawn && cached.pawn == localPawn;
            const bool isLocal = localByPawn || (localBySlot && !localPawn);

            if (!cached.alive || !cached.pawn) continue;
            if (cached.team == Offsets::TEAM_T) seenT++;
            else if (cached.team == Offsets::TEAM_CT) seenCT++;
            /* every alive player is returned, camera pawn included. counts now
             * match the server roster, names bind for self, and the renderer's
             * "Show self" option can put a box on the camera pawn if wanted. */
            if (isLocal) localCount++;

            GameEntity e;
            e.address = cached.pawn;
            e.index = cached.idx;
            e.isLocal = isLocal;
            e.sceneNode = cached.sceneNode;
            e.health = cached.health;
            e.team = cached.team;
            e.armor = cached.armor;
            e.isScoped = cached.isScoped;
            e.position = cached.position;
            e.name = cached.name;
            e.valid = true;

            if (haveLocalOrigin) {
                const float dx = e.position.x - localOrigin.x;
                const float dy = e.position.y - localOrigin.y;
                const float dz = e.position.z - localOrigin.z;
                e.distance = std::sqrt(dx * dx + dy * dy + dz * dz) / 52.49f;
                /* a failed scene-node read leaves INT_MIN in the position; such
                 * a pawn has no drawable body, so drop it before the render pass. */
                if (!std::isfinite(e.distance) || e.distance < 0.0f || e.distance > 20000.0f) continue;
            }

            out.push_back(std::move(e));
            validCount++;
        }

        if (validCount != lastCount) {
            std::ostringstream oss;
            oss << "[+] Alive players: " << validCount
                << " (T=" << seenT << " CT=" << seenCT
                << " local=" << localTeam
                << " self=" << localCount
                << " shown=all"
                << (isSpectating ? " spectate" : "") << ")";
            Log(oss.str());
            lastCount = validCount;
        }

        return out;
    }

    bool WorldToScreen(const Vector3& world, Vector2& screen) const {
        return worldToScreen.Transform(world, screen);
    }

    int GetLocalTeam() const { return localTeam; }
    bool IsSpectating() const { return isSpectating; }
    bool IsSingleTeamServer() const { return single_team_server; }
    Memory* GetMemory() { return &memory; }

    void SetScreenSize(int w, int h) {
        worldToScreen.SetScreenSize(w, h);
    }

    void Cleanup() {
        memory.Cleanup();
    }
};
