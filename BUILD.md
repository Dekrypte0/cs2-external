# CS2 External ESP - Build Instructions

## Requirements

- **Windows 11** (or Windows 10)
- **Visual Studio 2022** (or 2019) with C++ support
- **CMake** (optional, for command line build)
- **Git** (already used to clone ImGui)

## Method 1: Visual Studio (Recommended)

### Step 1: Install ImGui dependencies

ImGui is already cloned in `external/imgui/`. You need the backends:

```bash
cd external/imgui
mkdir -p backends
```

Then copy the backend files from your ImGui installation:
- `backends/imgui_impl_win32.cpp`
- `backends/imgui_impl_win32.h`
- `backends/imgui_impl_dx11.cpp`
- `backends/imgui_impl_dx11.h`

These should already exist in the cloned ImGui repo in `backends/`.

### Step 2: Generate Visual Studio project

```bash
cd C:/Projects/cs2-external
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
```

### Step 3: Build

Open `cs2_external.sln` in Visual Studio and build in **Release x64** mode.

Or from command line:
```bash
cmake --build . --config Release
```

## Method 2: Manual Visual Studio Project

### Step 1: Create new project

1. Open Visual Studio
2. Create new "Empty Project" (C++)
3. Set to x64, Release mode

### Step 2: Add source files

Add these files to your project:
- `main.cpp`
- `overlay.cpp`
- `renderer.cpp`
- All ImGui .cpp files from `external/imgui/`

### Step 3: Configure include directories

Project Properties > C/C++ > General > Additional Include Directories:
```
C:/Projects/cs2-external
C:/Projects/cs2-external/external/imgui
C:/Projects/cs2-external/external/imgui/backends
```

### Step 4: Link libraries

Project Properties > Linker > Input > Additional Dependencies:
```
d3d11.lib
dwmapi.lib
user32.lib
gdi32.lib
```

### Step 5: Build and run

Build in Release x64 mode.

## Running the ESP

1. Start CS2
2. Run `cs2_external.exe` (from build/Release folder)
3. The overlay should appear on top of CS2

## Controls

| Key | Action |
|-----|--------|
| **INSERT** | Toggle click-through (enable/disable mouse interaction) |
| **F1** | Toggle debug window |
| **F2** | Toggle ESP rendering |
| **END** | Exit the program |

## Troubleshooting

### "Failed to initialize memory"
- Make sure CS2 is running before starting the ESP
- Run as Administrator

### "Failed to create overlay window"
- Another overlay may be running
- Restart the application

### ESP boxes in wrong position
- The window size detection may be wrong
- Check the Debug Window for "Client.dll" address

### Compilation errors
- Make sure you're using C++17 or later
- Verify ImGui backend files are present
- Check that all include directories are set correctly

## Project Structure

```
cs2-external/
├── main.cpp           - Main entry point, ImGui UI
├── overlay.cpp/hpp    - Transparent window management
├── renderer.cpp/hpp   - DirectX 11 rendering with ImGui
├── memory.hpp         - Process memory reading
├── math.hpp           - World-to-screen math
├── game.hpp           - Entity management
├── offsets.hpp        - Game offsets (from dumper)
├── external/
│   └── imgui/        - ImGui library
└── build/            - Build output directory
```

## Security Notes for CTF Write-up

### Techniques Used

1. **External Memory Reading**: Using `ReadProcessMemory` to read game memory without injection
2. **Overlay Rendering**: Transparent layered window with DirectX 11
3. **World-to-Screen**: Matrix transformation to convert 3D world coords to 2D screen coords
4. **Entity Iteration**: Traversing CS2's entity list structure

### Detection Vectors

This external can be detected by:
- Process scanning (looking for known cheat processes)
- Window pattern detection (overlay windows)
- Behavior analysis (reading game process memory)
- Integrity checks on game memory

### Educational Value

This project teaches:
- Windows API programming
- DirectX 11 graphics
- 3D mathematics (matrix transformations)
- Reverse engineering game memory structures
- Anti-cheat evasion concepts
