# CS2 External ESP - CTF Project

> **Educational Purpose Only** - School CTF Project on Rented Servers

## Project Structure

```
cs2-external/
├── main.cpp       - Main loop and game logic
├── memory.hpp     - Memory reading wrapper (WinAPI)
├── entity.hpp     - Entity class for player data
├── offsets.hpp    - Game offsets (UPDATE FROM DUMPER!)
└── README.md      - This file
```

## What You Need To Do

### 1. Update Offsets from Your Dumper

Open `offsets.hpp` and replace ALL placeholder values with ones from your dumper.

Key offsets you need:
- **entity_list** - Find in client.dll
- **local_player** - LocalPlayerController
- **pawn_health** - `m_iHealth` in CCSPlayerPawn
- **pawn_team** - `m_iTeamNum` in CCSPlayerPawn
- **pawn_origin** - `m_vOldOrigin` or similar position vector

### 2. Compilation (Visual Studio)

```cmd
# Create a new C++ Console Project
# Add the .hpp and .cpp files
# Set to Release mode, x64 build
# Compile
```

Or with CLI (if you have MSVC):
```cmd
cl /EHsc /std:c++17 main.cpp /link user32.lib
```

### 3. How It Works

```
┌─────────────────────────────────────────────────────┐
│  1. OpenProcess() → Get handle to cs2.exe           │
│  2. Find module bases (client.dll, engine2.dll)     │
│  3. ReadProcessMemory() → Read entity list          │
│  4. Iterate entities, check:                         │
│     - Is alive? (lifeState == 0)                    │
│     - Health > 0 && <= 100                          │
│     - Team != local_team                            │
│  5. Display enemy info                              │
└─────────────────────────────────────────────────────┘
```

## Controls

- **END** - Exit the cheat

## Key Concepts for Your CTF Write-up

| Concept | Explanation |
|---------|-------------|
| **External vs Internal** | External = Separate process reading memory. Internal = DLL injected into game |
| **ReadProcessMemory** | WinAPI function to read another process's memory |
| **Offsets** | Memory addresses change with updates, use dumper to find current ones |
| **Entity List** | Game stores all players/objects in a linked list |
| **Pointer Chaining** | Following pointers through nested structures |

## Security Concepts Learned

- Process memory isolation and how it's bypassed
- Anti-cheat detection methods
- Game data structures and reverse engineering
- Windows API for process manipulation

## Next Steps (After Basic ESP Works)

- [ ] Add visual overlay (directx/ImGui)
- [ ] Implement glow via game's glow system
- [ ] Add bone detection for skeleton ESP
- [ ] Add radar implementation

## Troubleshooting

| Issue | Solution |
|-------|----------|
| "Failed to initialize" | CS2 not running, run as admin |
| All entities invalid | Wrong offsets, check dumper |
| Crash | Wrong address calculation in entity list |

## Resources

- [CS2 Dumper](https://github.com/a2x/cs2-dumper) - For latest offsets
- [GuidedHacking](https://guidedhacking.com/) - Tutorials
- [MSDN WinAPI](https://docs.microsoft.com/en-us/windows/win32/api/) - Documentation
