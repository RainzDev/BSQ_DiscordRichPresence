# DiscordRichPresence

A Beat Saber Quest mod for displaying status on Discord

## Configuration

Navigate in-game to configure the mod:

```
Settings → Mod Settings → DRP
```

| Setting | Description | Default |
|---------|-------------|---------|
| **Private IP** | Your PC's local network IP | `192.168.x.x` |
| **Port** | Server port number | `8080` |

## Finding Your Private IP

<details>
<summary><b>🪟 Windows (CMD)</b></summary>

```bash
ipconfig
```

**Look for:**
```
IPv4 Address . . . . . . . . . . : 192.168.x.x
```
</details>

<details>
<summary><b>🍎 macOS (Terminal)</b></summary>

```bash
ifconfig
```

**Look for:**
```
inet 192.168.x.x (under active interface, usually en0)
```
</details>

<details>
<summary><b>🐧 Linux</b></summary>

```bash
ip a
```

**Look for:**
```
inet 192.168.x.x/24
```
</details>

## Building

For a full introduction to Quest modding, visit the [BSMG Wiki](https://bsmg.wiki/modding/quest/intro.html).

To just build, install [QPM](https://github.com/QuestPackageManager/QPM.CLI/releases/latest), [CMake](https://cmake.org/download/), [Ninja](https://github.com/ninja-build/ninja/releases/latest), the [Android NDK](https://developer.android.com/ndk/downloads), and [Python 3](https://www.python.org/downloads/), then run:

```sh
qpm ndk resolve
qpm restore
qpm qmod zip
```

## Credits

* [Metalit](https://github.com/Metalit), [Lauriethefish](https://github.com/Lauriethefish), [Fern](https://github.com/Fernthedev), [Bobby Shmurner](https://github.com/BobbyShmurner) for: [this template](https://github.com/Lauriethefish/quest-mod-template)
* [Metalit](https://github.com/Metalit), [Sc2ad](https://github.com/Sc2ad), [zoller27osu](https://github.com/zoller27osu), [jakibaki](https://github.com/jakibaki) for: [beatsaber-hook](https://github.com/QuestPackageManager/beatsaber-hook)
* [Fern](https://github.com/Fernthedev) for: [QPM](https://github.com/QuestPackageManager/QPM.CLI), [bs-cordl](https://github.com/QuestPackageManager/bs-cordl), [paperlog](https://github.com/Fernthedev/paperlog)
