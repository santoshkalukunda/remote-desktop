# Running the RemoteDesk server

One small Node.js process is the whole server side. Every RemoteDesk app connects to it:

| port  | what | used for |
|-------|------|----------|
| 21116 | relay (`server.js`) | apps register their address here and sessions are piped through it (end-to-end encrypted; the server only sees ciphertext) |
| 21117 | update server (`update-server.js`, started by `server.js`) | `latest.json` and the signed installer for in-app updates |

The apps find it through `src/core/server_config.h`, compiled into every build. Users never type a server
address. Set it **before** building the release you hand out:

```c
#define RD_RELAY_HOST "relay.yourcompany.com"   // or the server's public IP
#define RD_RELAY_PORT 21116
#define RD_UPDATE_URL ""                        // empty = http://<RD_RELAY_HOST>:21117/latest.json
```

Prefer a DNS name: if the server ever moves, you update DNS instead of shipping new builds.
Private addresses (`10.x`, `192.168.x`) only work for computers inside that network.

## 1. Set up the server (Ubuntu 22.04 / 24.04)

```sh
# Node.js 18 or newer
sudo apt update && sudo apt install -y nodejs
node --version

# A service account and the files
sudo useradd --system --home /opt/remotedesk --shell /usr/sbin/nologin remotedesk
sudo mkdir -p /opt/remotedesk
sudo cp -r relay /opt/remotedesk/          # from this repository: server.js, update-server.js, deploy/
sudo mkdir -p /opt/remotedesk/relay/updates/files /opt/remotedesk/relay/data
sudo chown -R remotedesk:remotedesk /opt/remotedesk

# Run it permanently (starts on boot, restarts if it stops)
sudo cp /opt/remotedesk/relay/deploy/remotedesk-relay.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now remotedesk-relay
sudo systemctl status remotedesk-relay      # "active (running)"

# Open the ports (also in your cloud provider's firewall / security group)
sudo ufw allow 21116/tcp
sudo ufw allow 21117/tcp
```

Logs: `journalctl -u remotedesk-relay -f` (shows devices coming online, calls and update downloads).

Check from another machine:
```sh
nc -vz relay.yourcompany.com 21116
curl http://relay.yourcompany.com:21117/latest.json      # 404 "no release published yet" until step 3
```

## 2. Build the client for your users

1. Put your server in `src/core/server_config.h`.
2. `build.bat`, then `build-installer.bat` → `build\setup.exe`.
3. Give users `setup.exe` (website, email, file share). It installs, and the app connects to your server.
   Nothing to configure on their side.

## 3. Publish an update

On your build machine (where `keys/update-signing-key.pem` lives; it is created once with
`node tools/release.js keygen` and must stay private and backed up):

```sh
# bump src/core/version.h, then:
build.bat && build-installer.bat
node tools/release.js publish --notes "What's new in this version"
# copy the result to the server
scp relay/updates/latest.json            user@server:/tmp/
scp relay/updates/files/RemoteDesk-setup-*.exe user@server:/tmp/
ssh user@server 'sudo mv /tmp/latest.json /opt/remotedesk/relay/updates/ && \
                 sudo mv /tmp/RemoteDesk-setup-*.exe /opt/remotedesk/relay/updates/files/ && \
                 sudo chown -R remotedesk:remotedesk /opt/remotedesk/relay/updates'
```

No restart is needed. The next time each user opens RemoteDesk (a few seconds after start), they see
**"A new version is available"** with **Update now** or **Later**. *Update now* downloads the installer,
checks its SHA-256 and the release signature, installs it silently (keeping their address, password and
history) and reopens the app. *Later* keeps working; an **Update** button stays in the sidebar.

Upload `files/...exe` before `latest.json`, so no app sees a manifest whose file isn't there yet.

## HTTPS (recommended for updates)

The installer is verified by signature either way, but serving updates over HTTPS also hides what is
downloaded. Put the update server behind nginx or Caddy with a certificate, e.g. Caddy:

```
updates.yourcompany.com {
    reverse_proxy 127.0.0.1:21117
}
```

and set `#define RD_UPDATE_URL "https://updates.yourcompany.com/latest.json"` in `server_config.h`.
(The relay port 21116 is raw TCP and stays as it is; sessions are already end-to-end encrypted.)

## Testing one installation against another server

Without rebuilding: `setup.exe /RELAYHOST=test-relay.example.com /RELAYPORT=21116` writes an override into
`config.ini` next to the installed exe. Re-running setup without those options removes it again.

## RemoteDesk Pro licenses

The same process answers license activations at `/api/` on port 21117 (`license-server.js`). It needs the
`relay/licenses/` folder next to `server.js` (or `LICENSE_DIR=...`):

- `license-signing-key.pem` — private key that signs licenses. Created once with `node tools/license.js init`;
  the matching public key is compiled into the app. **Keep it secret and backed up.**
- `licenses.json` — the key database (SHA-256 hashes of keys, seats, expiry, which PCs use them).

Copy that folder to the server once (`scp -r relay/licenses user@server:/opt/remotedesk/relay/`, then
`chown -R remotedesk:remotedesk`), and run `node tools/license.js create/list/revoke` **on the server**
(with `LICENSE_DIR=/opt/remotedesk/relay/licenses`) so new keys land in the database the server reads.
The systemd unit marks `/opt/remotedesk` read-only: give the license folder write access with
`ReadWritePaths=/opt/remotedesk/relay/licenses` in the `[Service]` section.
