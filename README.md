# RemoteDesk

An AnyDesk-style remote desktop for Windows. Each computer gets a permanent
9-digit address. Type someone's address, they click **Accept**, and you see and
control their screen, even when both computers are behind NAT or firewalls.

```
 Viewer PC                    Relay server (VPS)                   Host PC
 RemoteDesk.exe  --CONN id-->  relay/server.js  <--REG id (always)--  RemoteDesk.exe
                 <=========== encrypted stream piped ===========>
```

## Features

- **9-digit ID** for each computer, kept in `%APPDATA%\RemoteDesk\settings.ini`
- **Works through NAT.** Both sides only make outbound connections to the relay.
- **One app for both roles.** It shares "This Desk" and connects to a "Remote Desk".
- **Access permissions**: without a password, the host sees who is asking and the security code, and chooses what to
  allow: *control mouse & keyboard* (off = view only), *clipboard*, *screenshots & recording*, *files*. Allow / Decline; it
  declines by itself after 60 s. The host enforces control and clipboard itself; the viewer shows a *View only* badge.
  **Change them any time**: click the session under *Active sessions* to open its Session window and switch any
  permission on or off (password sessions too). It applies at once: revoking control releases anything held down,
  revoking recording stops it (keeping what was recorded), revoking files cancels transfers, and the viewer is told.
  The **Access** page (sidebar) sets the defaults: what an approval prompt starts with, and what the unattended
  password grants. It also lists everyone connected now, each with *Manage*.
- **Pro on either computer** unlocks the session: each side sends its signed license and the other verifies the
  signature and expiry, so a licensed host can exchange files with an unlicensed viewer and vice versa.
- **RemoteDesk Pro** (license key), each also subject to the host's permission:
  - **Full screen**: F11 or Alt+Enter; the toolbar slides in at the top edge.
  - **Screenshots**: PNG in `Pictures\RemoteDesk`.
  - **Screen recording**: MP4 (H.264, Windows' built-in encoder, up to 1080p at 15 fps) in `Videos\RemoteDesk`. The
    person being recorded sees *Recording your screen* on that session.
  - **File transfer**: the viewer's *Files* panel (button or drag and drop) sends files to the other PC's
    `Downloads\RemoteDesk`; the host sends files back by clicking the session under *Active sessions*. Files go one
    at a time in 64 KB pieces over the encrypted session, each checked with SHA-256 on arrival; names are
    sanitised and nothing is overwritten. Progress shows as a percentage on both sides (the host sees
    *Sending report.pdf · 45%* on the session). **Resumable**: if the connection drops or either PC restarts, the
    receiver keeps the part in `DownloadsRemoteDesk.partial` and the sender remembers the file
    (`%APPDATA%RemoteDeskoutbox.ini`); the next session between the same two computers continues every unfinished
    file from where it stopped (re-checked with SHA-256 over the whole file). A file changed or moved since is not
    resumed; unfinished parts are forgotten after 30 days. **Pause / Resume** (1.5.1+, both sides): the pause button on a
    transfer in the Files panel stops that file (either direction) and keeps what arrived, while the next file in the
    queue goes on; resume offers it again and it continues from there (MSG_FILE_PAUSE / MSG_FILE_RESUME).
  See *Licenses* below.
- **Lock screen, sign-in screen, Ctrl+Alt+Del** (setup.exe installed *for all users*, option "Allow access to the
  lock and sign-in screens"): installs the **RemoteDesk service**. It runs as SYSTEM from boot and keeps an *agent*
  (`RemoteDesk.exe --agent`) in the console session. The agent is this PC's host: it keeps the same address, follows
  the input desktop onto the lock/sign-in screen and UAC prompts, and works before anyone signs in (password
  sessions; approval requests need someone signed in, whose RemoteDesk shows the prompt). The encrypted session ends
  inside the agent, so no ordinary program can make it click or type. The app talks to it over a local pipe
  (`\.pipeRemoteDeskAgent-<session>`, this session only, server checked to be our own exe) for the address, approvals,
  sessions, permissions, password and license. Files received while the agent hosts go to the signed-in person's
  Downloads and are read/written with *their* rights. The viewer sends Ctrl+Alt+Del with **Ctrl+Alt+End** (setup
  allows software SAS for services). Updates of an all-users install ask for administrator approval once. Not in the
  Microsoft Store version (Store apps can't install services). If no agent answers (e.g. in a Remote Desktop
  session), the app hosts by itself as before.
- **Wake-on-LAN**: connecting to a computer that is switched off or asleep wakes it up and connects once it's on
  (the app keeps trying for 3 minutes). A sleeping PC can only be woken from inside its own network, so: while
  online, each PC tells the relay its network cards and local network (`NET` line; kept in `wol.json`, 90 days);
  for a wake-up the relay asks up to 5 other RemoteDesk PCs that are online on the **same network** (same public
  address or local subnet) to broadcast the magic packet. Needs: another RemoteDesk PC on that network switched on;
  Wake-on-LAN enabled in the target's BIOS/UEFI and network card ("Wake on Magic Packet"); a network cable (Wi-Fi
  rarely wakes a PC that's off); often Windows *Fast Startup* turned off. After waking, the PC is at the sign-in
  screen, so it's reachable only with the service (setup for all users) and the unattended password.
  **Set up Wake-on-LAN** (Settings, on the PC to be woken, with the service): turns on the wired cards' driver
  wake settings where the driver has them (*Wake on Magic Packet*, Realtek *Shutdown Wake-On-LAN*, Intel *PME*),
  lets them wake the PC (`powercfg /deviceenablewake`) and turns off Fast Startup; restart once. Helpers send from
  every network card (to 255.255.255.255, each subnet broadcast and the target's last address); the relay also
  sends it to the target's public address (office -> home: needs the home router to forward UDP 9) and answers
  `INET` when that's the only route. USB network adapters usually wake a PC from Sleep but not from shut down
  (USB loses power) unless the BIOS keeps USB powered in S5. **Cancel**: while connecting, the Connect button
  reads *Cancel*.
- **Wake-on-LAN guide** (Word + PDF): `docs/RemoteDesk - Wake-on-LAN guide.docx|.pdf`; setup installs the PDF (Start menu › RemoteDesk › Wake-on-LAN guide).
- **Locked screen without the service**: the viewer shows *The other computer is locked* instead of a frozen picture.
- **Sound and voice calls** (1.5+, `src/remote/audio.*`), over the session's encrypted channel, no other app:
  - **Hear the remote PC**: the viewer's speaker button plays what the host's speakers play (WASAPI loopback,
    32 kHz stereo). Needs the permission *Hear this computer's sound* (approval prompt, Session window, Access page);
    revoking it stops the sound at once.
  - **Call**: the phone button (viewer) or *Call* in the host's Session window rings the other side, who gets an
    *Accept / Decline* window. During the call both microphones are on (16 kHz mono), with *Mute* and hang-up on both
    sides; an unanswered call gives up after 45 s. The host's Active sessions list shows *On a call*.
  - Audio is cut into 20 ms pieces and compressed 4:1 with IMA ADPCM (about 100 kbps for a call, 250 kbps for the
    PC's sound); each piece decodes on its own, so a lost one costs 20 ms. Playback buffers ~80 ms against jitter.
  - With the service, the agent captures and plays the audio; if Windows' microphone privacy settings block it, the
    host is told. The Store package declares the microphone capability.
- **Sessions stay honest**: both sides send a heartbeat every 5 s; 20 s of silence (network gone, PC asleep) ends the
  session, and a reconnect from the same computer replaces any leftover one. Connecting to a computer you're already
  viewing brings that window up instead of opening a second.
- **Unattended access** with a password (stored as a PBKDF2-SHA256 hash, 100k iterations)
- **End-to-end encryption**: ECDH P-256 key exchange, then AES-256-GCM in each direction (Windows CNG).
  The relay only sees ciphertext. Both sides show a matching *security code*; compare it to detect a man-in-the-middle relay.
- **Efficient video**: 64px tile change detection, changed regions sent as JPEG 4:4:4 (WIC),
  and an ack-based flow control that keeps latency low on slow links instead of filling buffers.
- **Multi-monitor**: the viewer opens on the remote's main screen. The menu bar has one button per screen, plus
  **All screens**, to switch at any time. Resolution and layout changes are picked up live.
- **Full input**: mouse, wheel (vertical and horizontal), and keyboard, including Win, Alt+Tab, Ctrl+Esc and Alt+F4 via a low-level hook.
  Stuck keys are released when the viewer loses focus.
- **Text clipboard sync** in both directions
- Viewer window scales the remote screen to fit, keeping the aspect ratio
- **Several sessions at once**: connect to as many computers as you like from the one app (each opens its own
  viewer window). **Active sessions** on the Home page lists every open session in both directions: click one to bring
  it to the front, or its close button to end it (incoming viewers too). The PC is kept awake during a session.
- **Recent connections**: every computer you connect to is remembered (`%APPDATA%\RemoteDesk\history.ini`). One click
  reconnects. If a password got you in, it is saved encrypted with Windows DPAPI (only your Windows account on this PC
  can read it) and reused, so you're straight in; otherwise the other side is asked to accept, as usual. A saved
  password that stops working is forgotten automatically.
- **Our server, no setup for users**: the relay and update addresses are compiled in (`src/core/server_config.h`).
  Users install and are connected; nobody types a server address. While the server is down the app waits quietly
  (red *Offline* in the sidebar, automatic retries) and connects by itself once it is back.
- **In-app updates**: when RemoteDesk opens (or reconnects to the server, or every 6 hours while it stays open) it
  asks our update server for a newer version and offers **Update now / Later**. A download is installed only if its
  SHA-256 matches and the release signature (ECDSA P-256) verifies against the key built into the app.
  See *Releasing an update* below.
- **Clean, modern UI**: sidebar navigation (Home, Settings), a gradient card with your address and the connect form,
  grouped settings cards, **Light / Dark / System** themes (title bar included). Custom-drawn and double-buffered, so
  nothing flickers. Errors appear inline only when something is wrong.
- **Distinct pointers**: in the viewer your own pointer is a small blue ring, and the remote pointer is drawn
  with a soft blue halo, so the two are never confused.
- **Light on resources**: an unreachable relay is retried with exponential back-off (2 s up to 60 s, with jitter),
  connection attempts time out after 6 s, and the waiting thread sleeps instead of polling. **Retry now** reconnects at once.
  The viewer repaints only the changed part of the picture.

No third-party libraries. Everything uses built-in Windows APIs (Winsock, GDI, GDI+, WIC, CNG, DPAPI, WinHTTP, DWM).

## 1. Run the server

**[relay/DEPLOY.md](relay/DEPLOY.md)** covers running it permanently on Ubuntu (systemd service, firewall,
publishing updates, HTTPS). Quick start on any machine the apps can reach (a VPS, or a LAN PC for testing):

```sh
cd relay
node server.js            # listens on TCP 21116 (set PORT=... to change)
node test.js              # optional self-test
```

`node viewer-test.js <id> <password>` is a headless viewer for testing a running host end-to-end
(the host needs an unattended password set).

The same process serves in-app updates over HTTP on port 21117 (`UPDATE_PORT=...` to change, `0` to turn off).
Open TCP ports 21116 (and 21117 for updates) in the firewall. Node.js 18+ is required; it has no npm dependencies.
For always-on use, run it under `pm2`, `systemd` or similar.

## 2. Build the client

First put your server's address in **`src/core/server_config.h`** (`RD_RELAY_HOST`, `RD_RELAY_PORT`; a DNS name is
best). Every app built from it connects there.

Either toolchain works:

- **Visual Studio / Build Tools** (C++ workload): open a *Developer Command Prompt* and run `build.bat`
- **MinGW-w64**: put `g++` on PATH and run `build.bat` (this produces one static exe).
  `winget install BrechtSanders.WinLibs.POSIX.UCRT` installs it. `build.bat` copes with a toolchain path that
  contains spaces.

`build.bat` compiles every `.cpp` under `src\`, so new files need no build changes.
The output is `build\RemoteDesk.exe`. Copy it to both computers.

### Installer (`setup.exe`)

Install [Inno Setup 6](https://jrsoftware.org/isinfo.php) and run `build-installer.bat`. It produces `build\setup.exe`.
The wizard:

1. shows the privacy policy (`PRIVACY.txt`), which must be accepted,
2. asks for each permission (screen sharing, remote control, clipboard sync, network, keep awake). All of them must be ticked,
3. offers the optional services: start with Windows, and a desktop shortcut.

It does not ask for a server. "Start with Windows" is the same `Run` registry value the app's toggle in
**Settings** shows and changes.

A silent install has to give consent explicitly: `setup.exe /VERYSILENT /ACCEPTPRIVACY=yes [/TASKS=autostart]`.
For testing, an administrator can point one installation at another server:
`setup.exe /RELAYHOST=test.example.com /RELAYPORT=21116` (stored in `config.ini`; re-running setup without them removes it).

### Releasing an update

The version number lives only in `src/core/version.h`; the exe's version info and `setup.exe` take it from there.

1. Once per project: `node tools/release.js keygen`. It creates `keys/update-signing-key.pem` (git-ignored; **back it
   up and keep it private**, it is the only key that can publish updates) and writes the public half into
   `src/core/update_key.h`. Rebuild so the app trusts it. This repository already has a key generated.
2. **`release.bat 1.2.1 "What's new"`** does the rest in one go: sets the version in `src/core/version.h`, builds
   the exe and `setup.exe`, signs them and writes `relay/updates/latest.json` + `relay/updates/files/`, then
   builds the Store package `build\RemoteDesk.msix` with the same version (upload it in Partner Center).
   (Step by step: `node tools/release.js bump 1.2.1`, `build.bat`, `build-installer.bat`,
   `node tools/release.js publish --notes "..."`, `.\build-msix.ps1`.) The Store package needs the Windows SDK;
   without it the update still goes out and `release.bat` warns and exits with code 2.
3. If `server.js` runs on this machine, the update is live at once. Otherwise copy `relay/updates/` to the
   server (see [relay/DEPLOY.md](relay/DEPLOY.md)).

Each app then shows **"A new version is available" — Update now / Later** the next time it opens (or when it
reconnects to the server, or within 6 hours if it stays open). *Update now* installs silently, keeps the
user's address, password and history, and reopens the app. *Later* leaves an **Update** button in the sidebar.

**Trying it with a colleague first:** run `node relay/server.js` on your PC, build with your PC's address in
`server_config.h`, `release.bat 1.2.0`, and give your colleague `build\setup.exe`. Then
`release.bat 1.2.1 "test"`: when your colleague opens RemoteDesk they are offered 1.2.1 from your PC.

### Microsoft Store (`RemoteDesk.msix`)

Run `.\build-msix.ps1` to produce `build\RemoteDesk.msix`. A Store package has no installer pages, so the app asks
for the same consent on its first start. See [STORE.md](STORE.md) for the Partner Center steps.
Store copies connect to the same compiled-in server, but are updated by the Microsoft Store (Store policy): for
them, publish a new package in Partner Center instead of `release.bat`; the in-app updater stays off.
When the exe runs without setup.exe's consent record (MSIX, or copied by hand), it shows the same first-run screen.

## 3. Use it

1. Install and start RemoteDesk on both PCs. The sidebar shows *Online* once our server is reachable.
2. On the viewer, type the host's address under **Connect to a device** and click **Connect**. Next time, just click it
   under **Recent connections**.
3. On the host, check that the security code matches, choose what to allow and click **Allow**.
   - Or set an *unattended access password* on the host. A viewer who enters it connects without the prompt, with full access.

## Licenses (RemoteDesk Pro)

Full screen, screenshots, recording and file transfer need a RemoteDesk Pro key. How it stays secure:

- A key looks like `RDPRO-XXXXX-XXXXX-XXXXX-XXXXX-XXXXX`: 125 random bits, impossible to guess.
- The server stores only the **SHA-256 hash** of each key, never the key. Wrong guesses are rate-limited.
- **Activate** in *Settings → RemoteDesk Pro* sends the key and a hashed ID of the PC. The server checks seats, expiry
  and revocation, and returns a license **signed with the server's private key** (ECDSA P-256).
- The app verifies that signature with the public key compiled into it (`src/core/license_key.h`), and checks that the
  license is for *this* PC and hasn't expired. A license can't be forged, edited or copied to another PC. Once a
  run, the app also asks the server whether the key was revoked (and keeps working offline if it can't reach it).

Managing keys (on the server machine, where `relay/licenses/` lives):

```sh
node tools/license.js init                     # once: signing key + src/core/license_key.h (done for this repo)
node tools/license.js create --note "Acme Ltd" --seats 3 --days 365   # prints the key; shown only once
node tools/license.js create --count 10 --seats 1                     # ten single-PC keys that never expire
node tools/license.js list                     # serials, seats used (which PCs), expiry, status
node tools/license.js revoke RDPRO-7K2M9       # by key or serial; apps drop it on their next check
node tools/license.js remove-device RDPRO-7K2M9 a1b2c3   # free one seat
```

Customers activate in **Settings → RemoteDesk Pro**, and can move their key with **Deactivate on this PC**.
Back up `relay/licenses/` (the signing key and the key database); without the signing key no new licenses can be issued.

## Protocol

Relay handshake (plain text lines, then raw bytes):

| from   | line          | reply                              |
|--------|---------------|------------------------------------|
| host   | `REG <id>`    | `OK` / `TAKEN` (client keeps retrying; picks a new ID only if it is still taken after 2.5 min) |
| host   | `PING`        | `PONG` (every 25 s)                |
| viewer | `CONN <id>`   | `READY` / `OFFLINE` / `TIMEOUT`    |
| relay  | `CALL <sid>`  | host dials in: `JOIN <sid>` → `READY` |

After `READY`, the stream is end-to-end encrypted. Message types are in `src/core/common.h`.

## Files

| path | purpose |
|------|---------|
| `relay/server.js` | rendezvous and relay server (also starts the update server) |
| `relay/update-server.js` | serves `latest.json` and installers for in-app updates |
| `src/core/` | shared basics: sockets and helpers (`common.h`), encrypted channel (`crypto.*`), JPEG (`codec.*`), `server_config.h` (our server addresses), `version.h`, `update_key.h`, declarations (`app.h`) |
| `src/app/` | `main.cpp` entry point, `settings.cpp`, `relay.cpp` (registration with back-off), `history.cpp` (recent connections), `updater.cpp`, `startup.cpp` (Start with Windows) |
| `src/remote/` | `host.cpp` (capture, encoding, input, accept prompt), `viewer.cpp` (remote screen window), `sessions.cpp` (active sessions) |
| `src/ui/` | `theme.cpp`, `draw.cpp`, `controls.cpp`, `art.cpp`: palettes, fonts, drawing, custom controls, pointer art |
| `src/screens/` | `main_window.cpp` (frame, sidebar), `home_page.cpp`, `settings_page.cpp`, `update_prompt.cpp`, `welcome.cpp` (first-run screen) |
| `tools/release.js`, `release.bat` | signing key, version bump, publishing updates |
| `tools/license.js`, `relay/license-*.js` | RemoteDesk Pro keys: create, list, revoke; activation API |
| `relay/DEPLOY.md`, `relay/deploy/` | running the server permanently (systemd) |
| `PRIVACY.txt`     | privacy policy (installer, first-run screen, Store listing) |
| `installer/`      | Inno Setup script for `setup.exe` |
| `packaging/`      | MSIX manifest and Store logos (`node tools/make-icon.js` regenerates them) |
| `old/`            | the original LAN-only prototype |

## Known limitations (vs. real AnyDesk)

- **UAC prompts, the lock screen and Ctrl+Alt+Del** can't be seen or controlled. That needs a Windows service running as SYSTEM.
  Input into elevated (admin) windows only works if RemoteDesk itself runs as administrator.
- **All traffic goes through the relay.** There is no direct peer-to-peer or UDP hole punching yet,
  so performance depends on the relay's bandwidth.
- **JPEG instead of a video codec** (H.264 / DeskRT). Full-screen video and scrolling use a lot of bandwidth.
- **No audio or chat.** File transfer sends whole files (no remote folder browsing); recordings have no sound.
- **The viewer's ID shown in the prompt is self-reported.** Rely on the security code, not the ID.
