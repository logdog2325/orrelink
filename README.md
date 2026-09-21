# OrreLink

**Pokémon XD: Gale of Darkness GBA-vs-GBA battles over the internet.** Two
players, each with their own emulated Game Boy Advance running Emerald, fight
in XD's Colosseum using the game's real link battle engine.

<p align="center">
  <img src="docs/images/battle-cave.jpg" alt="Regirock and Moltres against Gengar and Metagross in a cave arena" width="49%">
  <img src="docs/images/battle-arena.jpg" alt="Regirock and Regice against Metagross and Gengar in a colosseum" width="49%">
</p>

Runs on **Android** (including the AYN Thor handheld and its two screens; a
normal phone works too), **macOS** (Apple Silicon), **Windows** and **Linux**.
Every release comes out for all four at once, so any two players can battle
each other.

## What OrreLink does for you

- **Pick a format and XD's rules are set for you.** Nobody pages through the
  rules screen, and nobody can get it wrong.
- **Paste a team.** A Showdown export or a pokepast.es link becomes a legal
  Emerald team in your GBA. Joiners hand their team to the host from inside
  the room.
- **The host's picks reach both players:** battle timer, music (or no music),
  and battle location. Each player picks their own trainer model.
- **Your keyboard controls the GBA out of the box.**
- **Any platform against any platform.** Mac against Windows, Thor against PC,
  it all stays in sync.
- **When something goes wrong, the log knows.** One button shares it, and a
  crash leaves a report next to it.

## What you need

| | |
|---|---|
| **Pokémon XD** | A clean **USA** disc image (game ID `GXXE01`) |
| **Pokémon Emerald** | The standard clean English ROM (No-Intro "USA, Europe", `BPEE`). Both players need the exact same file. |
| **Official GBA BIOS** | `gba_bios.bin`, 16 KB. Required. The games will not link without the real Nintendo file, and free substitutes do not work. The app checks that you have the right one. |

None of these can be distributed here.

## Setup

1. Download the build for your platform from
   [Releases](https://github.com/logdog2325/orrelink/releases). Everyone in
   a match must be on the **same version**. Mismatched builds refuse to connect.
2. **Desktop:** put the XD disc image, the Emerald ROM and the BIOS in one
   folder, then open the app. The OrreLink launcher opens on startup and sets
   itself up from that folder.
   **Android:** open the app, tap **XD Netplay**, and work the checklist until
   it is green.
3. **macOS only:** allow **Input Monitoring** for OrreLink when it asks on
   first launch, or the keyboard does nothing in the game. If you denied it,
   turn it on under System Settings, Privacy & Security, Input Monitoring, then
   relaunch.

## Play a match

Two rules explain everything below.

- **The host's GBA saves are the ones that get played.** When the host presses
  Start, those saves are copied to everyone. A joiner's own team is not read
  automatically, so joiners **submit** their team instead.
- **Format, battle timer, music and location are the host's picks.** Trainer
  models are per player.
- **Hidden Power comes out as the type in your paste.** With no IVs line you
  get the same default IVs Showdown uses for that type. If your paste has an
  IVs line, those IVs are used as written.
- **Pasted teams have max PP.** Every move gets all three PP Ups. A team you
  pasted before version 1.6.2 keeps its old PP until you paste it again.

If you imported a personal Emerald save, only a rebuilt copy with your party
and trainer identity ever leaves your machine. Your boxes, items and story
stay put. A guest's team goes into a spare slot and is removed when the room
closes.

### Host

You can set your team in the Team Editor before you open the room, or paste
it in the room with **Submit Team**. Once the room is open, the guest slot in
the editor locks.

1. Pick a **Format** in the launcher (Orre Colosseum is the usual choice, see
   Formats below). Tick **Battle timer** if you want one.
2. Open the **Team Editor**. In the dropdown choose **Host: GBA port 2**, press
   **Import**, paste your Showdown team or pokepast.es link, type your in-game
   name, and press **Save**. The editor tells you if anything is banned in your
   format.
3. *Optional:* switch the dropdown to **Guest: GBA port 3** and put a team there
   too. That is the team your opponent plays with if they never send one.
   Useful for testing alone or for handing a friend a team. A guest who sends a
   team replaces it.
4. Go to **Netplay** and **Host**. Share the code. In the room, **Submit Team**
   lets you swap your team, name or trainer model, and **Music & Location**
   changes the battle music and where you fight (stadiums and lobbies). Both
   work any time before you press Start and apply to the next battle.
5. Wait for your opponent's team to arrive. A line in the room chat says so.
   If the line never appears, their team did not arrive. Ask them to send it
   again before you press Start, or you will fight the wrong team.
6. Press **Start**. In XD, go to **VS Mode**, then **GBA vs GBA**, and confirm
   through the rules. They are already set. You drive these menus with the
   GameCube keys (see Controls). Your opponent's buttons do nothing here.

A team you paste with **Submit Team** replaces the team in your **Host: GBA
port 2** slot. If that slot holds a save you imported, the change only lasts
until the room closes and your own save comes back untouched.

### Join

1. Go to **Netplay**, paste the host's code, and **Join**.
2. Press **Submit Team**. On desktop it is in the room window. On Android it is
   the bottom-right button. Type your in-game name (7 letters max), then paste
   your team, or tick **Use my save** to send the party from the Emerald save
   you imported on the **Host: GBA port 2** slot of your own Team Editor. Only
   your party and trainer identity are sent. Pick your trainer model. In a Lv
   100 format you can tick **Raise my team to Lv. 100**. Press **Send**.
3. Check the room chat for the confirmation line. If it is missing, send again
   before the host starts. A team that breaks the format is refused, and the
   line says why. You can send any time before Start, and sending again
   replaces what you sent. A name with no Gen 3 equivalent is dropped and the
   team still goes through, so check the chat line if the name matters.
4. Wait. The host presses Start and clicks through XD's menus for both of you.
   Your buttons do nothing there, and that is not a bug. When the GBA
   connection screen appears, leave it alone.

### The GBA connection screen

- **Touch nothing for about 45 seconds.** XD uploads a program to each GBA in
  turn, and nothing is drawn while it happens. That is normal, not lag.
- **Do not press B while the GBAs are linking**, on the GameCube pad or on
  either GBA. It backs you out mid-upload and XD drops both links with a
  "GBA not detected" message.
- Nothing after well over a minute? Press B, back out of VS Mode, and go in
  again.

### The team pick

About 18 seconds after both GBAs link, **your GBA screen shows your party and
the GameCube screen stops moving.** That is the team pick, and it happens on
the GBA. **A adds a Pokémon, B removes one.** The GameCube screen stays still
until **both** players have confirmed. Two things look like a freeze but are
not:

- keys only reach the game while the OrreLink game window or a GBA window is
  the active window. If Discord is in front, nothing you press counts.
- Wi-Fi dropping or switching networks freezes both screens at once. After 20
  seconds of silence the other side ends the match.

## Formats

| Format | Entry | Level | Team rules |
|---|---|---|---|
| **Orre Colosseum** | bring 6, pick 4, doubles | 100 | Restricted ("box") legendaries and Mythicals banned, Soul Dew banned |
| **Orre Unlimited** | bring 6, pick 4, doubles | 100 | everything allowed |
| **Orre Limited** | bring 6, pick 4, doubles | 50 | every legendary banned |
| **Hoenn Stadium** | bring 6, pick 3, singles | 100 | as Orre Colosseum |
| **Hoenn Unlimited** | bring 6, pick 3, singles | 100 | everything allowed |
| **Hoenn Limited** | bring 6, pick 3, singles | 50 | every legendary banned |
| **OU** | you set it on XD's rules screen | you set it | the community OU rules. OrreLink applies the community's XD OU game fixes and does not check teams. |
| **Free** | you set it on XD's rules screen | you set it | none. XD's menus are left alone. |

Species Clause, Item Clause, Sleep Clause, Freeze Clause and the Self-KO
Clause are on in every Orre and Hoenn format, enforced by the game itself.
The team rules are enforced by OrreLink: a paste that breaks them is flagged
in the editor, a host cannot open a room with an illegal team, and an illegal
submission is refused with the reason. Rooms show their format in the lobby
name (`[Orre]`, `[Hoenn-L]`, and so on).

**Battle timer.** Tick **Battle timer** under the Format and set the seconds
per turn (default 60) and minutes per game (default 20). XD's own timer is set
to those values for both players, and XD's rules screen shows them. Off by
default. Not available for OU and Free. Menus are slow over netplay, so 60
seconds per turn is the sensible minimum.

**Team Editor.** Imports assume maximum happiness unless the paste has a
`Happiness:` line. The Lv. 100 button, offered in level 100 formats, raises
Pokémon and never lowers them.

## Controls

You do not need a real GameCube controller. The host's keyboard (or a plugged
in gamepad) is the GameCube pad, and each player's keyboard is their GBA.

| GameCube (host drives XD's menus) | Key |
|---|---|
| A / B | X / Z |
| X / Y / Z | C / S / D |
| Start | Enter |
| Control stick | Arrow keys |
| D-pad | T / G / F / H |
| C-stick | I / J / K / L |
| L / R | Q / W |

| GBA (each player) | Key |
|---|---|
| D-pad | W / A / S / D |
| A / B | X / Z |
| L / R | Q / E |
| Start / Select | Enter / Backspace |

- The launcher's **GBA controls** row sets these on every GBA slot and applies
  them right away. If you used an older version, your old keys are switched
  over for you.
- **Change GBA controls...** next to it opens the mapping window if you want
  your own keys, and applies them to every slot.
- Your GBA is **GBA 1 when you join, GBA 2 when you host** (the host's
  GameCube controller takes the first slot).
- A GBA window's title says **KEYS OFF** whenever the game is not the active
  window.
- **Android:** long-press the bottom GBA screen to switch which GBA it shows.
  Solo only. In netplay it only ever shows your own.

## If you cannot connect

- **Leave the connection type on the default.** It needs no router setup and
  adds no lag. After the introduction, input goes straight between the two
  machines.
- **Direct** is plan B for when the default fails (some mobile networks and
  phone hotspots). The host opens port **2626** (UDP) on their router, or you
  use Tailscale as below.
- **Two devices in the same house usually cannot connect to each other.** Most
  routers will not loop a connection back. Test with someone elsewhere, or put
  one device on a phone hotspot. Two devices in the same room can also both
  join one phone's hotspot and use **Direct** with the **Local** address shown
  on the host's room screen.
- **Blocked networks** (campus, hotel and apartment Wi-Fi, or a hotspot trying
  to host) show **"Could not communicate with host"** on both connection
  types, even with both devices on the same Wi-Fi. The network keeps its
  devices apart, and nothing in the app can change that. The fix is
  [Tailscale](https://tailscale.com/download), which is free for personal use:
  both players join one tailnet, the host picks **Direct connection**, and the
  joiner enters the host's Tailscale `100.x.y.z` address with port **2626**.
- **Lag** comes from distance, not download speed, but still close downloads
  during a match. The input buffer sizes itself from the measured ping. Leave
  it alone, because editing it by hand turns the automatic sizing off for the
  rest of the session. A ping in the hundreds of milliseconds is the network,
  not the app.

## When something goes wrong

**Send the newest session log.** It records both GBA links once a second and
every button pressed, so it shows exactly what happened. Both players' logs
together are twice as useful.

| | |
|---|---|
| **Android** | The **Share Log** button in the app |
| **Desktop** | The **Share Log...** button at the bottom of the launcher. It saves the newest log where you choose and opens that folder. |
| Windows, by hand | `%APPDATA%\Dolphin Emulator\GBA\` (press Win+R and paste it) |
| macOS, by hand | `~/Library/Application Support/Dolphin/GBA/` (in Finder press Cmd+Shift+G) |
| Linux, by hand | `~/.local/share/dolphin-emu/GBA/` |

Logs are named `gba_detect_YYYYMMDD_HHMMSS.log`, and the last three sessions
are kept. If a battle failed to start and you relaunched, send the log from
that session, not the newest one. **If OrreLink crashed**, the same folder
holds a `crash_....txt`. Send it with the log.

Reading the logs yourself: see [docs/technical.md](docs/technical.md).

## Updating

**Check for Updates** in the app tells you when a newer release exists. Nothing
installs on its own. Everyone in a match must be on the same release.

**Android, one time only:** builds before 1.0.0 will not update in place.
Uninstall them first, and back up your teams before you do, because
uninstalling deletes them. From 1.0.0 on, updates install straight over the
top.

## PBR Online (bonus mode)

Pokémon Battle Revolution over the community Wiimmfi servers. Point the app at
a clean PBR disc of any region. The online patches are applied in memory, and
the disc is never modified. Online play needs a NAND backup **from your own
Wii** (shared or generated NANDs are banned by the server). Offline play does
not. Matchmaking happens inside the game.

## For developers

How the four builds are produced from this repository, what happens in a
mixed Mac, Windows and Android room, and how to check a match from its logs:
[docs/technical.md](docs/technical.md).

## Credits

OrreLink stands on other people's work.

- **im a blisy ._. and papajefe** made the original **XD Netplay** build, the
  Windows Dolphin bundle that first got GBA-vs-GBA Pokémon XD battles working
  over netplay. OrreLink grew out of it. The netplay-ready game configuration
  and the *XD OU Fixes* Action Replay set behind the OU format are carried
  over from that bundle.
- **Akiak** made the original **OrreLink**, whose name this project carries.
  Their working netplay setup showed what an XD link session needs to be
  configured like, the Orre and Hoenn format set is theirs, and they have
  tested every release from the first.
- **PKHeX** (kwsch and contributors) provided the Gen 3 save structure
  references the team saves are built against, and the Gen 4 code PBR Online
  ports (save container geometry, BK4 encryption, experience tables, string
  tables).
- **Dolphin Emulator Project.** OrreLink is a fork of Dolphin and stays under
  Dolphin's licence, GPL-2.0-or-later.
- **SapphireRhodonite** wrote Dolphin pull request #14745, the Android
  dual-screen GBA support the Thor's second screen is built on.
- **mGBA** (endrift and contributors) is the emulated Game Boy Advance inside
  Dolphin.
- **pret** decompilation projects (pokeruby, pokefirered, pokeemerald) for the
  Gen 3 save layout and character map.
- **Pokémon Showdown** for the team paste format the editor reads.
