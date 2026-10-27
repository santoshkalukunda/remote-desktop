# Publishing RemoteDesk on the Microsoft Store

The Store signs MSIX packages itself, so Store users don't hit the Smart App
Control / SmartScreen block, and you don't need a code-signing certificate.
`setup.exe` remains available for direct downloads, but it needs its own signature.

## Before you start

1. **A public relay server.** Store users, and Microsoft's testers, connect over the internet.
   A private address such as `10.13.174.83` only works inside your network.
   Run `relay/server.js` on a VPS (see README) and use its public IP or domain name.
2. **A public privacy policy URL.** Partner Center requires one for apps that use the network.
   Publish `PRIVACY.txt` on a web page (GitHub Pages or your website), and fill in the contact line at the end first.
3. **A Partner Center developer account:** <https://partner.microsoft.com/dashboard/registration>.
   Microsoft has made registration free or low-cost for individuals; check the current terms when you sign up.

## 1. Reserve the name and get the package identity

1. In Partner Center: **Apps and games → New product → MSIX or PWA app**, and reserve a name.
   "RemoteDesk" may already be taken or trademarked; pick another one if needed.
2. Open the product → **Product management → Product identity**. Copy these three values:
   - `Package/Identity/Name`
   - `Package/Identity/Publisher` (looks like `CN=XXXXXXXX-XXXX-...`)
   - `Package/Properties/PublisherDisplayName`

## 2. Build the Store package

```powershell
build.bat                        # from a Developer Command Prompt
.\build-msix.ps1 -Name "<Identity/Name>" -Publisher "<Identity/Publisher>" `
                 -PublisherName "<PublisherDisplayName>" -Version 1.0.0.0 `
                 -Relay "relay.yourdomain.com:21116"
```

This produces `build\RemoteDesk.msix`. Don't sign it; the Store signs it.
For every update, raise `-Version` (1.0.1.0, 1.1.0.0, ...). The last number must stay 0.

Optional local check (run as administrator):

```powershell
& "${env:ProgramFiles(x86)}\Windows Kits\10\App Certification Kit\appcert.exe" test `
  -appxpackagepath build\RemoteDesk.msix -reportoutputpath build\appcert.xml
```

## 3. Fill in the submission

| Section | What to enter |
|---|---|
| Pricing and availability | Free (or your price), markets |
| Properties | Category: *Utilities & tools*. Privacy policy URL. Support contact |
| Age ratings | Complete the questionnaire (no user-generated content, no purchases) |
| Packages | Upload `build\RemoteDesk.msix` |
| Store listing | Description, at least one screenshot (main window + viewer), search terms |
| Submission options → Notes for certification | See below |

**Restricted capability `runFullTrust`.** Partner Center asks why you need it. Suggested answer:

> RemoteDesk is a remote desktop application written in Win32 C++. It needs full trust to capture the
> screen (GDI), inject the remote user's mouse and keyboard input (SendInput) and install a low-level
> keyboard hook so shortcuts like Alt+Tab reach the remote computer. Every incoming session must be
> accepted by the local user, or authorised with a password they set.

**Notes for certification.** Give testers a way to try a session:

> 1. On first start, read and accept the privacy policy and permissions.
> 2. Our test host is online 24/7 at address `<9-digit ID>`, unattended password `<password>`.
>    Enter it under "Remote Desk", type the password, and click Connect.
> 3. To test incoming sessions, start RemoteDesk on a second PC and connect to the first.

Keep a PC (or VM) running RemoteDesk with that unattended password while the submission is reviewed.

## What's different in the Store version

- **No installer pages.** On first start, the app shows the privacy policy and the five permissions
  (all must be ticked), the relay server (pre-filled from the package's `config.ini`) and an optional
  **Start with Windows** checkbox (`src/welcome.cpp`).
- **Start with Windows** uses the MSIX startup task in `packaging/AppxManifest.xml`. Users can switch it
  off in **Settings → Apps → Startup**. When Windows starts the app this way, it opens minimized.
- **Uninstalling** removes all settings automatically.
- Changing `POLICY_VERSION` in `src/app.h` (and `PolicyDate` in `installer/RemoteDesk.iss`) makes
  every user accept the privacy policy again.

## Testing the MSIX on your own PC

Either:

- turn on **Developer Mode** (Settings → System → For developers), then run `.\build-msix.ps1 -Register`; or
- run `.\build-msix.ps1 -Sign`, import `build\RemoteDesk-test.cer` into
  *Local Machine → Trusted People* (as administrator), and double-click `build\RemoteDesk.msix`.

Remove the test install with `Get-AppxPackage RemoteDesk.Dev | Remove-AppxPackage`.
