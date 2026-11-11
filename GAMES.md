# Games

Steam runs natively: Valve's aarch64 client (`software-patches/steam-arm64`).
Windows games run in Proton ARM64 (Wine ARM64EC, FEX translating the x86
code), Direct3D 9-11 through DXVK on PanVK (`software-patches/mesa`, feature
work in `DXVK.md` there). x86 Linux games run through FEX with an x86 guest
Mesa (`software-patches/fex-guestos-mesa`).

## What the port adds

| What | Where | Notes |
|---|---|---|
| 20:9 resolutions | `software-patches/xorg-xwayland` | Xwayland's emulated modes include 20:9 ones (800x360 ... 2160x972, 2400x1080) and work on the phone's rotated portrait panel. Proton offers every host mode, so Windows games list them. A game that still shows bars limits its own aspect ratio. |
| GPU render-pass overlap | `software-patches/mesa` 0018, `linux/patches` 0081, 0082 | PanVK runs a render pass's vertex / tiler job while the previous pass's fragment job is still running. On by default; `PANVK_JM_OVERLAP=0` turns it off. Dark Souls Remastered: about 5% more fps. Vulkan CTS: no new failures. |
| Big-core clocks under load | `linux/dts/mt6885.dtsi` (cpu-big thermal zones), `linux/system/cezanne-base` `etc/udev/rules.d/69-cezanne-thermal.rules` | The A77 thermal zones run the power_allocator governor (switch-on 55 C, control 80 C, 1.1 W sustainable). In games the big cores hold 1.75-1.93 GHz at 65-77 C. The previous step_wise trip (75 C, 15 C hysteresis) left them at 730 MHz, and the game threads ran on the A55s. |
| MangoHud | `software-patches/mangohud`, `linux/system/cezanne-base` `etc/MangoHud.conf` | Arch's MangoHud with the MediaTek temperature sensors. The system config sets the default panels and a control socket for each game process (`control=mangohud-%p`). |
| Hide / show MangoHud from a gamepad | `linux/system/cezanne-base` `usr/local/bin/cezanne-hud-pad` (user service) | Hold L3 for 0.4 s to hide or show the overlay; a short click doesn't toggle it (the game sees L3 either way). The service sends `:hud;` to every MangoHud control socket. It reads the pads without grabbing them and skips Steam's virtual pad. Keyboard: Shift_R+F12. Back paddles: assign one to L3 on the controller (they send nothing until assigned). |
| Screen stays on | `gnome-extensions/cezanne-game-awake@arch-port` | Inhibits idle while a Steam game runs or a window is fullscreen. |

## MangoHud for every Steam game

Don't set `MANGOHUD=1` for the whole session: GTK apps that use Vulkan would
load the layer too. Give it to Steam instead:

```sh
cp /usr/share/applications/steam.desktop ~/.local/share/applications/
sed -i 's|^Exec=/usr/bin/steam|Exec=env MANGOHUD=1 /usr/bin/steam|' \
    ~/.local/share/applications/steam.desktop
```

Steam's game shortcuts in `~/.local/share/applications` work the same way
(`Exec=env MANGOHUD=1 steam steam://rungameid/<id>`). Games started from a
Steam that was launched without the variable run without the overlay. For a
single game, use the launch option `MANGOHUD=1 %command%` instead.

## Per game

| Game | Runs through | Notes |
|---|---|---|
| Dark Souls Remastered | Proton ARM64, D3D11 | PanVK has no tessellation, so DXVK offers feature level 10_1 and the game refuses to start. A `dxvk.conf` in the game folder with `d3d11.maxFeatureLevel = 11_0` fixes it. Never set this globally: games that adapt to 10_1 (Risk of Rain 2) would take 11_0 paths. |
| Dark Souls II: Scholar of the First Sin | Proton ARM64, D3D11 | With online play on, the frame rate falls to single digits whenever the character moves. Block its login server in `/etc/hosts` (`0.0.0.0 frpg2-steam64-ope-login.fromsoftware-game.net`, plus a `::` line) and it starts offline. The Steam runtime container uses the host's `/etc/hosts`. Short stalls (100-250 ms every 10-25 s) remain: FEX handling the game's self-modifying code. `FEX_SMCCHECKS=full` makes it unplayable, and `FEX_MULTIBLOCK=0` only splits the stalls into more, shorter ones. 30 fps cap: `DXVK_FRAME_RATE=30 %command%`. |
| Ziggurat | Proton ARM64, D3D9 | The first game that ran. |
| Risk of Rain 2 | Proton ARM64, D3D11 | GPU bound (texture latency); runs at feature level 10_1. |
| Drag'n Wash | x86 Linux, FEX + guest Mesa | Renders, with texture artefacts at native resolution only (open). |
