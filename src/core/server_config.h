#pragma once
// Your RemoteDesk servers. Every copy of the app built from this source connects here:
// users never see or type these addresses. Change them before building a release.
//
//   RD_RELAY_HOST / RD_RELAY_PORT  where relay/server.js runs (the address users connect through)
//   RD_UPDATE_URL                  latest.json served by relay/update-server.js; leave empty to use
//                                  http://<RD_RELAY_HOST>:21117/latest.json (server.js serves it there)
//
// Use a domain name (e.g. relay.yourcompany.com) rather than an IP if you can: you can then move
// the server without shipping a new build. Private addresses (10.x, 192.168.x) only work inside
// that network.
//
// Administrators can still override the relay for one installation (testing, private networks)
// with setup.exe /RELAYHOST=... /RELAYPORT=..., which writes config.ini next to the exe.
#define RD_RELAY_HOST "10.13.174.12"
#define RD_RELAY_PORT 21116
#define RD_UPDATE_URL ""

// License activation API (relay/license-server.js, served by server.js next to the updates).
// Empty = http://<RD_RELAY_HOST>:21117/api
#define RD_LICENSE_URL ""
