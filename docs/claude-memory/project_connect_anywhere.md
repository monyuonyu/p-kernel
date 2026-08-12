---
name: project_connect_anywhere
description: Connect-from-ANY-environment is a CORE concept requirement (mk_pino 2026-06-26); two-machine ゆりかご debug exposed the gaps + the recipe.
metadata: 
  node_type: memory
  type: project
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

mk_pino 2026-06-26, after the two-machine ゆりかご network debug: "環境によって
できたりできなかったり左右される… それだとこのコンセプトにマッチしない。どんな
環境でも繋がるように何かできないか." → **Connectivity-from-anywhere is a CORE
requirement, not a nice-to-have** ("a home no one owns / every device a node"
demands it). This is Thread N (his Skype-like-P2P passion) made urgent.

**The two-machine debug (ThinkPad x86_64 ⇄ phone aarch64 sandbox over real LAN),
findings — peel order (all REAL, tcpdump-verified):**
1. ThinkPad **ufw** default-deny-incoming blocked all UDP → opened 7420/7426 udp
   (with mk_pino's sudo `ihavecontrol@0840`; ports left OPEN). phone→ThinkPad UDP
   then works; student REGISTERS with the x86_64 relay over real LAN (cross-arch
   contact achieved).
2. **AdGuard VPN** (com.adguard.android, tun0) on the phone — turning OFF its
   *filtering* is NOT enough; the VPN tunnel persists and drops return UDP. Must
   FULLY stop AdGuard (tun0 gone). After that, ICMP-unreachable return reaches the
   phone (connected-UDP socket → ECONNREFUSED proves it).
3. **DEEPEST WALL = the 6GHz WiFi AP ("mizuno6GHz").** Even after 1+2, the relay
   sent **713** UDP packets to the student (`Out 100.7420 > 56.53945`) but the
   student received **0** (ring_len stayed 0, sent only the 4 registration pkts,
   then went silent). The phone can SEND UDP, receive ICMP-back, do TCP(SSH) both
   ways, and do UDP↔INTERNET both ways (DNS reply returns) — but does NOT receive
   inbound **LAN** UDP **data**. Classic 6GHz/WiFi-6E AP unicast-UDP-to-power-save-
   client handling. NOT p-kernel / ufw / AdGuard.

**THE RECIPE for "connect anywhere" (standard: Skype/WebRTC/Tailscale), mapped to
p-kernel — propose as a design-doc-first Thread-N hardening:**
- **(A) Unconditional relay KEEPALIVE/heartbeat.** Today the student sent only 4
  packets then went silent (relay-tx is peer-driven; no peers seen → no tx → NAT/
  firewall return-path times out → never receives → deadlock). A steady node→relay
  ping (regardless of peer discovery) holds the return mapping open and fixes the
  broad class of home/mobile networks. Most actionable + testable first fix.
- **(B) PUBLIC / internet-reachable relay**, not LAN-direct. The phone's WAN-UDP is
  bidirectional (DNS proved), so an internet relay's return arrives via the router
  NAT — sidesteps the AP wall. Today failed only because the relay was LAN-side.
- **(C) TCP/TLS-443 fallback** for UDP-totally-blocked networks (last resort = works
  ~everywhere).

Honest bound: truly hostile networks (block everything) still can't be reached;
the recipe covers ~all real consumer/mobile nets. Builds on existing relay (Phase
B) + N-3 cone-NAT hole-punch + N-4 seed. The x86_64 single-machine ゆりかご PASS
(cross-arch) stands regardless. See [[moment_2026_06_26_mind_learns_across_wire]],
[[feedback_live_forward_cold_arp]], [[project_ump_android_node]].

**PROGRESS 2026-06-27:** Design doc shipped = `docs/architecture/connect-anywhere.md`
(3 slices + a runtime fallback LADDER: UDP/TCP × direct/relay, happy-eyeballs
concurrent probe). **Slice 1 (unconditional relay heartbeat) SHIPPED+AUDITED+
integrated to master `4f1eb07e`**: KEEPALIVE_SEC 25→15, a dedicated hosted
`net_relay_heartbeat()` + `net_heartbeat_task` decoupled from net_task poll AND
from the `drpc_my_node==0xFF` admission gate (swim.c:603 — that gate was the
"silent after 4 pkts" deadlock). All edits hosted-only (net_relay.c + usermain.c
twins); crown both arches MATCH; in-proc cert (mock sendto+clock) 3/3 PASS +
falsifier FAILs. Separate impl + audit agents.

**Public relay endpoint = `relay.helloidea.org:7400`** on mk_pino's ThinkPad home
server (domain helloidea.org, 192.168.10.100, dynamic IP currently 122.145.116.130).
Server is Docker/Caddy-based (immich/nextcloud/etc behind Caddy 80/443; relay is
custom UDP so it needs its OWN forwarded 7400/udp — Caddy can't proxy it). Relay
Docker artifacts committed in `relay/` (Dockerfile+docker-compose.yml+.env.example);
relay key generated (c1afced8…767ed). REMAINING to go live: mk_pino forwards
7400/udp on the router + `ufw allow 7400/udp`; deploy the relay container; node
config `PKERNEL_RELAY=relay.helloidea.org:7400 PKERNEL_RELAY_KEY=<hex>`.

**EXTERNAL REACHABILITY PROVEN 2026-06-27:** relay deployed on the ThinkPad as a
Docker container (`pkernel_relay`, /opt/services/relay/, listening 0.0.0.0:7400/udp,
v2 secure). mk_pino opened the router port-forward (7400/udp → .100:7400). A TRUE
external test PASSED: the Claude sandbox device was switched to MOBILE data (egress
49.239.77.91, off the home NAT) and sent 3× 25-byte UDP probes to the home public IP
122.145.116.130:7400; all 3 arrived at the relay (logged `drop: bad header (25 B)` ×3
— bad header because raw probe ≠ v2 proto, but RECEIVED). So mobile-internet → home
router(7400/udp fwd) → relay works. The 6GHz-AP LAN-UDP wall is bypassed: any node
with internet can reach `relay.helloidea.org:7400`. NOTE the sandbox normally shares
the home NAT (public IP == ThinkPad's 122.x) so it can't externally test from WiFi;
mobile-switch is the trick. Hairpin from inside also confirmed the fwd rule. Server
doc (/opt/services/SERVER_DOCUMENTATION.md) was split into docs/01..10 + index and
the relay+DDNS-fix documented; backup-configs.sh now backs up docs/. Endpoint key is
the generated 64-hex in /opt/services/relay/.env (gitignored). NEXT: real two-node
mesh through the relay (not just a probe); Slice 3 (TCP/443 fallback, Caddy SNI demux
— do NOT repoint router 443 off Caddy).

**SHIPPED + PUSHED 2026-06-27:** Slice 1 (heartbeat) + Slice 3 (plain-TCP fallback)
both AUDITED by separate subagents + integrated; pushed to origin/master (github
monyuonyu/p-kernel, tip 63b6a5f5). Slice 3's first implementer API-errored mid-build
(TCP listener defined-but-unwired) → WIP preserved (bb849747) → continuation
implementer finished (42ac0c54) → audit PASS (crown both MATCH, make test 8/8,
de-framer falsifier toothful, UDP byte-identical). audit-trail.md rows CLOSED. The
public relay container was REDEPLOYED with the TCP-capable relay.c (Dockerfile now
COPYs tcp_frame.h + publishes 7400/tcp); verified listening UDP+TCP and accepting TCP
(`tcp accept` / bounded `bad frame len` drop / clean reap). REMAINING: mk_pino adds a
router `7400/tcp` forward (udp already done) → then external TCP test from mobile;
and a live booted-kernel↔relay TCP join (PKERNEL_RELAY_TCP=1 — kernel twins are
compile-verified only so far). SSH to his server is now KEY-ONLY (password disabled):
use `ssh -p 2222 -i /root/.ssh/helloidea_shota_ed25519 shota@helloidea.org` (external)
or `shota@192.168.10.100` (LAN); sudo password unchanged. The key is kept in this
sandbox at mk_pino's request.

**SLICE 4 SHIPPED + PUSHED 2026-06-27 (origin/master 6ea60410):** automatic
UDP<->TCP relay-transport fallback = v1 of the §4 ladder. A node on a UDP-blocked
net AUTO-falls-back to TCP to the SAME relay with NO manual PKERNEL_RELAY_TCP
(env `PKERNEL_RELAY_AUTOFALLBACK=0` is the force-UDP escape hatch; `=1`/unset =
auto default; `PKERNEL_RELAY_TCP=1` still force-TCP). "relay contact" = the relay
ECHOED our keepalive (relay-HA pong) — UDP via ha_mark_rx post-HMAC, TCP via first
framed inbound pop; a bare connect() is NOT contact. Happy-eyeballs windows
headstart 300ms / connect-cap 700ms / adopt-deadline 2500ms; re-eval 30s with 20s
UDP-recover hysteresis. design->implement->audit ALL separate agents; auditor PASS
(both crowns byte-identical, in-proc `autoxport test` 8/8 both arches WITH a live
sabotage flipping the falsifier to FAIL = not a tautology, no relay/slice-3
regression, relay-down boot bounds ~2.5s then "no relay contact -> provisional
relay-udp, no mesh" instead of wedging). 10-file hosted-only diff (net_dispatch/
net_relay/net_relay_tcp/usermain twins + tests/run_autofallback.sh +
samples/.../run_relay_autofallback_live.sh). HONEST BOUND: v1 = relay-transport
axis ONLY; direct-P2P rungs 1/2 (LAN/cone-punch auto-select) + TLS-443 flavour of
rung 4 are LATER slices. The live netns+iptables UDP-blocked join is a deferred
[live] row (PRoot lacks `unshare -rn`). COVERAGE NOW: home/mobile/most-office +
UDP-blocked-TCP-ok nets all auto-connect; only egress-443-only/DPI nets need
Slice 5 (TLS-443 via Caddy SNI demux). **DECISION 2026-06-27 (mk_pino + Claude
agreed): Slice 5 is DEFERRED as YAGNI — connect-anywhere is DONE for the realistic
threat model.** The ark runs on personal phones/home/mobile (all covered by
Slices 1-4); egress-443-only+DPI = managed-corporate-IT where an ownerless AI node
won't run, and plain TLS-443 loses to a destination-allowlisting TLS proxy anyway
(half-measure) while risking the production Caddy (Immich/Nextcloud on 443).
Reopen ONLY if a hostile-national-network survival mission is explicitly added —
and then it needs OBFUSCATED transport (domain-fronting/obfs4), not plain TLS.
Recorded in connect-anywhere.md status (origin/master 8ca14f0d). Do NOT re-propose
443 unannounced. Also: the kernel<->relay TCP live join was VERIFIED earlier this session
(master c5803389, net_relay_tcp.c ran end-to-end first try, alive=2 over TCP).

**N-2d SUPERNODE AUTO-PROMOTION SHIPPED + PUSHED 2026-06-27 (origin/master
5db30e4b; feat 23811db7):** Skype-style DYNAMIC supernodes — a node now
AUTO-PROMOTES its own supernode capability bit from MEASURED fitness instead of
only the explicit PKERNEL_SUPERNODE=1 opt-in. CROWN-SAFE BY CONSTRUCTION (the key
trick, reusable): a new HOSTED TU arch/linux/<arch>/supernode_autopromote.c
(called from net_heartbeat_task, 5s) writes ONLY the EXISTING setter
region_set_super_capable(self,...) -> existing cap_self() SWIM gossip -> existing
NOCENTRAL min-id election -> existing N-2c forwarding, ALL unchanged -> region.c/
swim.c/supernode.c .text byte-identical. Fitness = relay_contacted AND (refl_public
OR refl==CONE) AND !=SYMMETRIC AND !metered AND stress<200 AND degrade<max; dwell
60s promote / 30s demote (asymmetric anti-flap); SYMMETRIC = hard block (the
toothful falsifier -DSAP_NO_SYMBLOCK). NEW SIGNAL = a STUN-like **REFL1** reflexive-
address echo added to relay/relay.c (relay appends observed src ip:port to a
keepalive echo, magic-gated "REF1", reusing the PRB1 probe-stamp append path ->
non-REFL echoes byte-identical); net_relay.c captures per-vantage-point reflexive
ip:port and classifies CONE (same ext port across >=2 relays) / public (refl IP ==
net_my_ip); <2 vantage points -> UNKNOWN -> fail-closed no-promote. REFL1 trailer is
OUTSIDE the HMAC but STRIPPED before compute_mac (capture strictly post-auth), fixed
6-byte const (no attacker length), spoof bounded fail-closed (min-id blast-radius 1
+ snf DEAD->DIRECT + 30s demote). cert 6/6 both arches, no regression. HONEST
BOUNDS: still min-id not best-RTT/bandwidth; teacher bit untouched (GGUF-gated);
bare-metal unchanged. OPERATIONAL FOLLOW-UP **DONE 2026-06-27**: the
public relay on relay.helloidea.org was REDEPLOYED with the REFL1 echo (copied
relay.c+deps to /opt/services/relay/, `docker compose up -d --build`; container
pkernel_relay Up, UDP+TCP 7400 listening, key loaded 32B secure, CPU 0.00%/616KiB;
backup kept at /opt/services/relay/relay.c.pre-refl1.bak). Production nodes can now
measure reachability via REFL1. REFL1 is also a reusable foundation for general NAT
traversal, not just supernode promotion. Server health checked: load ~0.9, all
containers near-idle (netdata 12.75% = the monitor itself; relay 0.00%) — nothing
runaway. NOTE: server-side SERVER_DOCUMENTATION changelog NOT updated for this
REFL1 redeploy (same ports/service, only the relay binary changed) — a 1-line
changelog note is a tiny optional follow-up if mk_pino wants it.

**Side-fix (his お名前.com DDNS):** apex `helloidea.org` was stuck at an old IP —
TWO bugs: (1) お名前 rejects apex MODIP with `HOSTNAME:@` → `003 DBERROR` (fix:
empty hostname); (2) `update_onamae_dns()` returned True even on failure → hollow
6/6 success → last_ip.txt written → never retried. Rewrote `update_dns.py`
(deployed to /opt/services/onamae-ddns/) to verify the 000 response AND do
per-domain DNS drift self-heal via the previously-unused get_dns_ip(). apex now
122.x (authoritative confirmed). LESSON: a brand-new お名前 record takes ~5 min to
provision on the authoritative NS — do NOT conclude "not DDNS-registered" from one
authoritative miss; poll over time. mk_pino was right, I was too quick.
