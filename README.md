# EFZ FF Netplay

Rollback netplay and practice tools for the **Fast Food Trial** edition of
*Eternal Fighter Zero*.

A small launcher, `EFZ FF Netplay.exe`, sets up the connection, starts the game
and loads the netplay module. From there you can play online, watch a match, or
train offline with hitbox display, frame stepping and savestates.

The game itself is not included. You need your own copy.

## Requirements

- The **Fast Food Trial** edition of EFZ. The launcher checks `EFZ.EXE` before
  starting and refuses any other build.
- Windows, or Wine/Proton with 32-bit Windows application support.
- For online play, UDP access for the launcher and the game. Everyone in a match,
  spectators included, must use the same release.

## Install

1. Build the project (see [Building](#building)) or take a packaged release.
2. Copy `EFZ FF Netplay.exe`, `netplay.dll`, `ddraw.dll` and `SDL3.dll` into your
   game folder, next to `EFZ.EXE`. Keep them together, and use the included
   `ddraw.dll` rather than one from another mod.
3. Run `EFZ FF Netplay.exe`, not `EFZ.EXE`.

On Linux, run the launcher in the prefix you use for the game. `launch-wine.sh`
starts it for you and `wine-preflight.sh` checks the setup. On Steam/Proton, set
the launcher as the game's executable.

## Set up your controls

Open **CONTROLS** first. Each player uses either a keyboard or a controller, and
both sets of bindings stay saved. A key can only do one job; the launcher tells
you about conflicts before you connect. Escape cancels a rebind, and
Backspace or Delete clears one.

The same page rebinds every shortcut listed under [Practice](#practice).

## Play online

Open **PLAY ONLINE**.

- **Host:** choose your port and press **HOST GAME**, then give your opponent your
  address and port. Once they connect, you pick the input delay (**Auto** applies
  the recommended value) and start the match. **Advanced connection settings**
  holds the maximum rollback.
- **Join:** enter the host as `host:port` and press **JOIN MATCH**. If you leave
  out the port, your configured one is used. IPv6 addresses are not supported.
- **Watch:** enter the host the same way and press **WATCH MATCH**.

The launcher tries a direct connection first, so a forwarded port is the most
reliable setup. If that gets no answer, it attempts NAT traversal through a public
rendezvous service.

The host's delay, rollback and game speed (60 or 64 FPS) apply to the whole
match. During a battle, you can change your own delay with Keypad + and Keypad -.
This doesn't affect your opponent.

ESC is always available online. F4 and F5 work only if both players turned on
**Allow practice shortcuts online** before connecting. All other practice tools
are offline only.

## Practice

Open **PRACTICE** and press **START OFFLINE PRACTICE**. Default keys:

| Key | Action |
| --- | --- |
| F4 | Trial training mode |
| F5 | Restore both fighters' health and meter |
| Page Up | Toggle hitboxes |
| Page Down | Toggle hurtboxes |
| Home | Toggle collision boxes |
| Space | Pause / resume |
| P | Advance one frame while paused |
| Keypad 0 | Swap controlled player |
| [ / ] | Previous / next save slot |
| G / B | Save / load the active slot |
| F6 / F7 | Quick save / quick load |

Savestates are stored in `trial_savestates/`.

## Settings

- **Player name:** the name your opponent sees.
- **Allow practice shortcuts online:** your half of the F4/F5 opt-in.
- **Prioritize game performance:** runs the game at a higher process priority.
- **Verify game files before launch:** checks the game before starting it.
- **Interface size:** scales the launcher from 75% to 200%.
- **Diagnostics > Save diagnostic logs:** off by default. Turn it on only when
  troubleshooting, then restart the launcher.

Press **Save settings** to keep your changes. Settings are locked while a
connection or game is active.

## Firewall

Allow the launcher and EFZ through your firewall if prompted. Hosts behind a
router get the most reliable connection by forwarding their UDP port.

## Troubleshooting

- **"This EFZ.EXE build is not supported":** the game copy isn't the Fast Food
  Trial edition this project targets.
- **Can't connect:** check the firewall, confirm the address and port, and have the
  host forward their UDP port.
- **Version or compatibility error:** both players need the same release. Replace
  all the included files together, not just some of them.
- **The game closes at launch:** make sure the included `ddraw.dll` is in the game
  folder. Online sessions need its frame-rate control and won't run at a different
  speed without it.
- **On Wine/Proton:** run the launcher rather than `EFZ.EXE`, keep it in the same
  prefix as the game, and don't force `ddraw=b`.
- **Reporting a problem:** turn on **Save diagnostic logs**, restart the launcher,
  reproduce the issue, and attach the logs.

## Building

You need CMake and Visual Studio with the C++ tools. Everything builds as 32-bit,
in this order:

```powershell
cmake -S ddraw_wrapper -B ddraw_wrapper/build -A Win32
cmake --build ddraw_wrapper/build --config Release

cmake -S trial_hitbox_viewer -B trial_hitbox_viewer/build -A Win32
cmake --build trial_hitbox_viewer/build --config Release

cmake -S trial_caster -B trial_caster/build -A Win32
cmake --build trial_caster/build --config Release
```

Run the tests with:

```powershell
ctest --test-dir trial_hitbox_viewer/build -C Release --output-on-failure
ctest --test-dir trial_caster/build -C Release --output-on-failure
```

`scripts/deploy_game.ps1` collects the finished files into `dist/game`.

For how it all works, see [docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md).

## Credits

- [EfzDdrawWrapper](https://github.com/Aquat1c/EfzDdrawWrapper): the base of `ddraw_wrapper/`.
- [SDL3](https://github.com/libsdl-org/SDL), [Dear ImGui](https://github.com/ocornut/imgui) and
  [MinHook](https://github.com/TsudaKageyu/minhook): bundled in `lib/` under their own licenses.
- NAT traversal uses the AutoPunch rendezvous service at `delthas.fr`.
- ITC Bolt, the launcher's typeface: fonts made from
  [Web Fonts](http://www.onlinewebfonts.com) are licensed by CC BY 4.0.
- Rollback design based on EfzRevival; CCCaster and Giuroll were used as a reference.

*Eternal Fighter Zero* belongs to its rights holders. This is an unofficial fan project.

## License

[MIT](LICENSE). You can use, change and share this project freely as long as you keep
the copyright notice. Bundled libraries and the font keep their own licenses, listed in
[Credits](#credits). The license does not cover the game.
