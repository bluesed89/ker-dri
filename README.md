# Albion Online — Mono Offset Dumper

Injects into the Albion Online process, walks the Mono runtime, and dumps every class/field/method with their offsets to a text file.

## What It Dumps

For every loaded assembly (Assembly-CSharp, etc.):
- **Classes** — namespace, name, parent class, instance size, enum/struct/class kind
- **Fields** — offset from object base (hex), type name, static/const flags
- **Properties** — type, getter/setter availability
- **Methods** — JIT-compiled native address, return type, parameter types, metadata token, flags

## Output Format

```
[Assembly] Assembly-CSharp

  class GameWorld.LocalPlayerCharacter : PlayerCharacter  [Size: 0x120]
  {
    // Fields
        0x0010  System.Single health
        0x0014  System.Single maxHealth
    [S] 0x0000  static GameWorld.LocalPlayerCharacter instance
        0x0018  System.Int32 silver

    // Properties
    [P] System.Single HealthPercent { get; }

    // Methods
    [M] 0x7FF8A1234560  [T:0x6000A12]  void Update()
    [M] 0x7FF8A1234700  [T:0x6000A13]  void TakeDamage(System.Single, DamageType)
  }
```

## Build

Requires **CMake 3.20+** and **MSVC** (Visual Studio 2019/2022).

```powershell
# Configure
cmake -B build -G "Visual Studio 17 2022" -A x64

# Build (Release for smaller binaries)
cmake --build build --config Release
```

Output: `build/bin/injector.exe` and `build/bin/dumper.dll`

## Usage

1. **Launch Albion Online** and wait until you're past the login screen (game fully loaded)
2. **Run as Administrator:**
   ```powershell
   cd build/bin
   .\injector.exe
   ```
3. The injector finds the game process, injects `dumper.dll`
4. A **MessageBox** appears in the game window when the dump is done
5. Check `%USERPROFILE%\Desktop\albion_dump.txt`

### Custom DLL Path

```powershell
.\injector.exe "C:\path\to\dumper.dll"
```

## Project Structure

```
├── CMakeLists.txt          # Build configuration
├── include/
│   └── mono.h              # Mono runtime type defs + API resolver
├── src/
│   ├── dumper.cpp           # Injected DLL — walks Mono, dumps offsets
│   └── injector.cpp         # Finds game process, injects DLL
└── README.md
```

## Notes

- **Anti-cheat**: Albion Online uses EasyAntiCheat. The basic `CreateRemoteThread` injection in this tool will likely get flagged if EAC is active. For bypassing EAC, you'd need manual mapping or a kernel driver — that's a separate concern from the dumper logic itself.
- **Mono DLL detection**: Automatically tries `mono-2.0-bdwgc.dll`, `mono-2.0-sgen.dll`, and `mono.dll`. Should work across Unity versions.
- **JIT compilation**: The dumper calls `mono_compile_method` to get native addresses. Some methods may fail to JIT (abstract, extern, etc.) — these show as `<no-jit>`.
- **Static fields**: Marked with `[S]` — their offset is relative to the static field data area, not the object instance.
