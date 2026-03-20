# steamfixbyAlley

CS:GO SourceMod Extension — open source implementation of the archived build fix.

Made by **[Alley](https://github.com/Alleyv2)**, based on the research by [eonexdev](https://github.com/eonexdev/csgo-sv-fix-engine) (closed-source binary). This is a fully open source reimplementation.

---

## The problem

Valve archived CS:GO and made it available as a separate Steam app (appid `4465480`). Clients using the archived build cannot connect to community servers — the server rejects them:

```
S3: Client connected with ticket for the wrong game
RejectConnection: STEAM validation rejected
```

## Root cause

The engine (`engine.so`) has a jump table dispatch that handles appid validation. Case 4 (archived build) hits a rejection path instead of the default OK path.

## How it works

The extension scans `engine.so` in process memory for the jump dispatch instruction:

```
FF 24 85 ?? ?? ?? ??   jmp ds:jpt[eax*4]
8D B4 26 ?? ?? ?? ??   nop padding
31 F6                  xor esi, esi
```

The jump table address is encoded directly in bytes `[3..6]` of the `FF 24 85` instruction. Once found:

```
jt[0]  →  OK path   (connection allowed)
jt[4]  →  rejection path  ← archived build lands here

Patch: jt[4] = jt[0]
```

On extension unload the original value is restored automatically.

## Console output on load

```
[steamfixbyAlley] ============================================
[steamfixbyAlley]   steamfixbyAlley  by Alley
[steamfixbyAlley]   https://github.com/Alleyv2
[steamfixbyAlley]   based on eonexdev/csgo-sv-fix-engine
[steamfixbyAlley] ============================================
[steamfixbyAlley] loading..
[steamfixbyAlley] engine base=0xF3AE5000 size=0x2FFBE00
[steamfixbyAlley] sig hit rva=0x18A2BA
[steamfixbyAlley] jt=0xF3FE6B64  jt[0]=0xF3C6F498  jt[4]=0xF3C6F3D8
[steamfixbyAlley] jt[4]: 0xF3C6F3D8 -> 0xF3C6F498
[steamfixbyAlley] engine patched! archived clients can now connect
```

## Install

Drop these two files on your server:

```
csgo/addons/sourcemod/extensions/csgo_steamfix.ext.so
csgo/addons/sourcemod/extensions/csgo_steamfix.autoload   ← empty file
```

Restart the server. Done.

## Build from source

You need these SDKs cloned next to the source file:

| SDK | Repository |
|-----|-----------|
| SourceMod | https://github.com/alliedmodders/sourcemod → `sourcemod-master/` |
| SourcePawn | https://github.com/alliedmodders/sourcepawn → `sourcepawn-master/` |
| AMTL | https://github.com/alliedmodders/amtl → `amtl-master/` |

```bash
g++ -m32 -shared -fPIC -O2 -std=c++14    \
    -I./sourcemod-master/public           \
    -I./amtl-master                       \
    -I./amtl-master/amtl                  \
    -I./sourcepawn-master/include         \
    -o csgo_steamfix.ext.so csgo_steamfix.ext.cpp -ldl
```

Requires `gcc-multilib` / `g++-multilib` for 32-bit compilation.

## Requirements

- Linux CS:GO server (32-bit)
- SourceMod 1.10 or 1.11
