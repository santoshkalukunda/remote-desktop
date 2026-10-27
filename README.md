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
- **Accept / Decline prompt** on the host. The prompt shows the caller's ID and a security code.
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
- Host shows who is connected and has a **Disconnect all** button. The PC is kept awake during a session.

No third-party libraries. Everything uses built-in Windows APIs (Winsock, GDI, WIC, CNG).

## 1. Run the relay server

Run it on any machine both computers can reach (a $5 VPS, or a LAN PC for testing):

```sh
cd relay
node server.js            # listens on TCP 21116 (set PORT=... to change)
node test.js              # optional self-test
```

`node viewer-test.js <id> <password>` is a headless viewer for testing a running host end-to-end
(the host needs an unattended password set).

Open TCP port 21116 in the firewall. Node.js 18+ is required; it has no npm dependencies.
For always-on use, run it under `pm2`, `systemd` or similar.

## 2. Build the client

Either toolchain works:

- **Visual Studio / Build Tools** (C++ workload): open a *Developer Command Prompt* and run `build.bat`
- **MinGW-w64**: put `g++` on PATH and run `build.bat` (this produces one static exe)

The output is `build\RemoteDesk.exe`. Copy it to both computers.

### Installer (`setup.exe`)

Install [Inno Setup 6](https://jrsoftware.org/isinfo.php) and run `build-installer.bat`. It produces `build\setup.exe`.
The wizard:

1. shows the privacy policy (`PRIVACY.txt`), which must be accepted,
2. asks for each permission (screen sharing, remote control, clipboard sync, relay network, keep awake). All of them must be ticked,
3. asks for the relay server and saves it to `config.ini` next to the exe,
4. offers the optional services: start with Windows, and a desktop shortcut.

A silent install has to give consent explicitly:
`setup.exe /VERYSILENT /ACCEPTPRIVACY=yes /RELAY=host:21116 [/TASKS=autostart]`

### Microsoft Store (`RemoteDesk.msix`)

Run `.\build-msix.ps1` to produce `build\RemoteDesk.msix`. A Store package has no installer pages, so the app asks
for the same consent on its first start. See [STORE.md](STORE.md) for the Partner Center steps.
When the exe runs without setup.exe's consent record (MSIX, or copied by hand), it shows the same first-run screen.

## 3. Use it

1. Start `RemoteDesk.exe` on both PCs. Set **Relay server** to `your-vps-ip:21116` on both and click **Save**.
   The status changes to *Online*.
2. On the viewer, type the host's address into **Remote Desk** and click **Connect**.
3. On the host, check that the security code matches and click **Yes**.
   - Or set an *unattended access password* on the host. A viewer who enters it connects without the prompt.

## Protocol

Relay handshake (plain text lines, then raw bytes):

| from   | line          | reply                              |
|--------|---------------|------------------------------------|
| host   | `REG <id>`    | `OK` / `TAKEN` (client picks new ID) |
| host   | `PING`        | `PONG` (every 20 s)                |
| viewer | `CONN <id>`   | `READY` / `OFFLINE` / `TIMEOUT`    |
| relay  | `CALL <sid>`  | host dials in: `JOIN <sid>` → `READY` |

After `READY`, the stream is end-to-end encrypted. Message types are in `src/common.h`.

## Files

| file | purpose |
|------|---------|
| `relay/server.js` | rendezvous and relay server |
| `src/main.cpp`    | main window, settings, relay registration |
| `src/host.cpp`    | screen capture, region encoding, input injection, accept prompt |
| `src/viewer.cpp`  | remote screen window, input forwarding |
| `src/crypto.*`    | encrypted channel, password hashing |
| `src/codec.*`     | JPEG encode/decode via WIC |
| `src/welcome.cpp` | first-run privacy policy and permissions screen |
| `src/startup.cpp` | Microsoft Store (MSIX) detection and Start with Windows task |
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
- **No file transfer, audio or chat.**
- **The viewer's ID shown in the prompt is self-reported.** Rely on the security code, not the ID.
