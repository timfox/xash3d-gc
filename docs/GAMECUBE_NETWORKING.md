# GameCube networking (LAN + internet)

Build with `XASH_GAMECUBE_LINK_BBA=1` (default in `scripts/build-gamecube.sh`) so libogc2's
network stack is linked. libogc2 drives the retail Broadband Adapter and the modern
SP1 Ethernet adapters (eth2sp1 W5500/ENC28J60 class); classic libogc supports the BBA only.

| Need | How |
| --- | --- |
| DHCP (default) | Plug in, start a multiplayer game; `GC_NetEnsureReady()` brings the link up lazily. |
| Static IP | `-netstatic [-gcnetip a.b.c.d -gcnetmask m -gcnetgw g]` (default 10.0.0.2/24, gw 10.0.0.1, validated by `net_config.h`). |
| LAN browse | Uses UDP broadcast (`255.255.255.255`) on the engine's normal ports. |
| Internet | Hostnames (master servers, `connect host`) resolve through `net_dns.h` (UDP DNS A lookup, 1.5 s timeout per server). Servers tried in order: `-gcdns a.b.c.d`, the DHCP/static gateway, 1.1.1.1, 8.8.8.8. Successful lookups are cached (8 entries). |
| Disable | `-nonet` / `-noip`. |

Behind NAT, a client can join any public server and play. To host over the internet,
forward the server UDP port (default 27015) to the console's IP.

## Verification status

Host unit tests (`python3 -m unittest tests.test_gamecube_net_dns`) cover DNS query
building and response parsing.

Dolphin BuiltIn-BBA runs (2026-11-12, lean DoD disc, `-net`, `connect <host>`; logs and
evidence in `dod-gc/out/net-proof/`):

- DHCP bring-up: `net ready ip=192.168.1.244 mask=255.255.255.0 gw=192.168.1.1`,
  `gc_net_status = "connected 192.168.1.244"`.
- Static bring-up (`netstatic 10.0.0.7 255.255.255.0 10.0.0.1`): `net ready ip=10.0.0.7`,
  `gc_net_status = "connected 10.0.0.7"`.
- DNS: `connect example.com:27015` logs `dns example.com resolved` over both DHCP and static.
- LAN: with `connect <host-lan-ip>:27015` a desktop UDP responder received the guest's
  `getchallenge`, answered, and received the full `connect 49 <challenge> "\\gc_dod\\1..."`
  packet on every retry.

Not yet verified: the Options > Network page itself (the retail menu needs more MEM1 than is
free on current master, see below), and a two-console / console + desktop-server map join.

## MEM1 budget with the BBA linked

The stock libogc2 lwIP pools are sized for a desktop (`PBUF_POOL_SIZE` 128 x 1600 B plus a
192 KiB `MEM_SIZE`), about 420 KiB of `.bss` (`memp_memory`, `ram`). For a UDP-only game that
is most of the MEM1 the engine needs at boot: with the stock library the BBA-linked DoD build
reaches `unclaimed=0` at server init and `Cmd_AddCommand`/`BaseCmd_Insert` OOM-skip `connect`,
`map`, `userinfo`, ... Apply `scripts/patches/libogc2-lwip-lean.patch` to the libogc2 sources
(`PBUF_POOL_SIZE` 24, `MEM_SIZE` 64 KiB), rebuild libogc2 and re-link. That frees about
300 KiB (`.bss` 16.18 MB -> 15.88 MB), `unclaimed` after `Con_Init` goes from 300 KiB to
592 KiB, and no OOM lines remain at boot.

## In-game configuration

Options > Network (mainui `menus/Network.cpp`) edits archived cvars: `gc_net_dhcp` (1),
`gc_net_ip`, `gc_net_mask`, `gc_net_gw`, `gc_net_dns`; `gc_net_status` is read-only and shows the
link state. They are read when the network first comes up (first multiplayer use). libogc2
cannot re-init after a failed bring-up, so a reboot is needed to retry. `-netstatic`, `-gcnetip`,
`-gcnetmask`, `-gcnetgw` and `-gcdns` on the command line still override the cvars. The mainui
submodule pointer must be a commit that has both the Network page and the GameCube port
(`XASH_GAMECUBE` platform detection, static `libmenu`); upstream mainui master alone does not
build for GameCube.

## Find servers: LAN and WAN (DoD GC)

**LAN tab** runs `localservers`: `CL_LocalServers_f` sends `info 49` as a
255.255.255.255 broadcast to UDP 27015..27024 from the client socket
(`NS_CLIENT`, ephemeral port, bound to the BBA/eth2sp1 address). Hosts answer
from `SV_Info` (ignored when `maxclients == 1`) back to that port;
`CL_ParseStatusMessage` validates the reply and `UI_AddServerToList` shows it.
Replies now carry `gc_dod=1` and the client drops replies without it, so only
DoD GC servers are listed. The IPv6 multicast leg is skipped on GameCube
(IPv4-only stack).

**Internet tab** runs `internetservers`, which sends a UDP scan request to every
entry of the master list (`NET_MasterQuery`). The stock list is empty on
GameCube: the HTTP static master (`xash.su/server-list`) is deliberately skipped
(no TLS/DNS heap) and `xashcomm.lst` is empty. Master hostnames resolve through
the UDP resolver in `net_gamecube.c` (`gc_net_dns`, then gateway, then
1.1.1.1/8.8.8.8, 1.5 s timeout each, 8-entry cache). It runs on the main thread,
so a dead DNS server stalls a frame; an IP literal avoids DNS entirely.

**WAN master.** `dodgc_master "host[:27010]"` (archived cvar, or
`-dodgcmaster host`) adds one UDP master. While it is set, listen servers with
`maxclients > 1` send heartbeats without needing `public 1` (a Cube has no
console), and the scan request carries `gamedir` and `gc_dod`. Run the master
on any reachable host:

```bash
scripts/dodgc_master.py --port 27010        # UDP 27010
python3 scripts/test_dodgc_master.py        # protocol tests
```

Hosts behind a home router must forward UDP 27015 to the Cube; the master
registers the heartbeat's source address. Servers expire after 11 minutes
without a heartbeat (engine interval: 5 min, 1 min with `sv_nat`).
