# Passive HRP-UWB frame capture — DWM3001CDK

Receive-only firmware for the **Qorvo DWM3001CDK** (DW3110 UWB transceiver +
onboard nRF52833) that logs every 802.15.4z HRP-UWB frame it hears — good
frames *and* STS/FCS/PHY faults — to RTT/UART as one structured, greppable line
for host-side analysis. Built directly on the Qorvo `dwt_uwb_driver` PHY API;
**no FiRa/uwbmac MAC blob**.

## Responsible use

Receive-only research tool: it listens, never transmits, and cannot itself
recover a session's keys. Decoding a keyed session (`sniff stskey`/`stsiv`)
requires STS material you supply — which presumes you are a participant in that
session. Use it only on traffic you are authorized to monitor: your own devices,
or a system you have explicit permission to test. Complying with the radio,
wiretap, and computer-access laws of your jurisdiction is your responsibility.

## Why this shape

- **NCS/Zephyr app**, not bare-metal FreeRTOS. Zephyr already has an in-tree
  board for this hardware (`qorvo/decawave_dwm3001cdk`) and a shell + logging
  subsystem, so we get a reliable RTT/UART console and a clean SPI/GPIO driver
  binding for free while keeping *all* radio code in the portable DW3000 API
  (`src/capture.c`). The radio layer never touches Zephyr specifics beyond the
  platform HAL that ships with the driver module.
- The DW3000 driver (`dwt_uwb_driver` + a Zephyr platform HAL that routes the
  DW_IRQ line through a workqueue into `dwt_isr()`) is supplied as an external
  Zephyr module — see [Dependencies](#dependencies).

## Dependencies

- **nRF Connect SDK** — **NCS v3.3.0** (Zephyr 4.3.99), which ships the in-tree
  `qorvo/decawave_dwm3001cdk` board. Either let the bundled `west.yml` fetch a
  pinned workspace, or build against an NCS v3.3.0 install you already have —
  see [Build](#build).
- **Qorvo DW3000 "decadriver" (`dwt_uwb_driver`)** packaged as a Zephyr module,
  placed at **`external/dw3000`**. It must provide
  `dwt_uwb_driver/deca_device_api.h`, a `zephyr/module.yml` + Kconfig exposing
  `CONFIG_DW3000`, a `decawave,dw3000` devicetree binding, and a platform HAL
  exporting `dw3000_hw_init()`, `dw3000_hw_reset()`, `dw3000_hw_init_interrupt()`
  and the `dw3000_probe_interf` probe interface.

  The community [`br101/zephyr-dw3000-decadriver`][br101] port satisfies all of
  this (ISC-licensed glue bundling the Qorvo `dwt_uwb_driver`). Add it as a
  submodule:

  ```bash
  git submodule add https://github.com/br101/zephyr-dw3000-decadriver external/dw3000
  ```

  The bundled Qorvo `dwt_uwb_driver/` is under Qorvo's own license; it is **not**
  vendored into this repository (only referenced as a submodule).

[br101]: https://github.com/br101/zephyr-dw3000-decadriver

## Hardware / pinmap

Target: **DWM3001CDK**, board target `decawave_dwm3001cdk/nrf52833`. RTT/UART
via the onboard J-Link (VCOM). The DW3110 hangs off **SPI3**; pins are taken
verbatim from Qorvo's own board config (`DW3_QM33_SDK_1.1.1`,
`Projects/FreeRTOS/<proj>/DWM3001CDK/ProjectDefinition/uwb_stack_llhw.cmake`),
cross-checked against the NCS board DTS and the community svhoy port:

| Signal | nRF52833 pin | Source |
|---|---|---|
| SPI SCK | P0.03 | NCS board `&spi3` + Qorvo `CONFIG_SPI_UWB_SCK_GPIO` |
| SPI MOSI | P0.08 | ″ |
| SPI MISO | P0.29 | ″ |
| SPI CS | P1.06 | ″ |
| DW_IRQ | **P1.02** | Qorvo `CONFIG_DWT_IRQ_GPIO` |
| DW_RSTn | **P0.25** | Qorvo `CONFIG_DWT_RSTN_GPIO` |
| DW_WAKEUP | **P1.19** | Qorvo `CONFIG_DWT_WU_GPIO` |

The DW3110 node is added in `boards/decawave_dwm3001cdk.overlay` (the stock NCS
board enables `&spi3` but doesn't declare the device).

**Bring-up gate:** on boot the firmware reads DEV_ID and expects
`0xDECA0302` (DW3110). A correct read confirms the **SPI + RESET** pinmap;
`DEV_ID=0x... OK` on the console is the cheap "pins are right" signal. (A wrong
IRQ pin still reads DEV_ID fine — IRQ is only proven once RX events fire.)

## Build

Freestanding Zephyr/NCS app: it carries only the app + the DW3000 module
(`external/dw3000`) and builds against an **NCS v3.3.0** (Zephyr 4.3.99) tree.
First make sure the driver submodule is populated:

```bash
git submodule update --init          # populates external/dw3000
```

Then pick the path that matches your machine.

### A — no NCS yet: bundled, pinned workspace

The repo ships a [`west.yml`](west.yml) pinned to NCS v3.3.0. Clone it into a
**dedicated parent directory** (west places Zephyr/NCS as *siblings* of the
repo), then:

```bash
west init -l uwb-sniffer      # make this repo the workspace manifest repo
west update                   # fetch pinned NCS (Zephyr, sdk-nrf, HALs, ...)
west build -b decawave_dwm3001cdk/nrf52833 --no-sysbuild -p always uwb-sniffer
```

### B — you already have NCS v3.3.0: reuse it (no re-download)

Build the app freestanding against your existing NCS workspace; the `west.yml`
is ignored. From your NCS workspace directory:

```bash
west build -b decawave_dwm3001cdk/nrf52833 --no-sysbuild -p always \
    -d /path/to/uwb-sniffer/build /path/to/uwb-sniffer
```

`-d` keeps build output inside the app tree — nothing is written into your NCS
workspace.

Single-core nRF52833 → **no sysbuild**. The `decawave_dwm3001cdk.overlay`
auto-applies by board name.

**Compatibility:** developed against **NCS v3.3.0 / Zephyr 4.3.99** — the version
`west.yml` pins, and the one that ships the in-tree `qorvo/decawave_dwm3001cdk`
board. Other NCS versions are untested; the board target and driver APIs may
differ.

## Flash (J-Link)

The CDK enumerates as its own J-Link. Program it:

```bash
nrfutil device list          # note the CDK's J-Link serial

# Option A — west (pass the CDK serial if more than one J-Link is attached):
west flash --dev-id <CDK_SERIAL>

# Option B — nrfutil directly:
nrfutil device program --serial-number <CDK_SERIAL> \
    --firmware build/zephyr/zephyr.hex
nrfutil device reset --serial-number <CDK_SERIAL>
```

Open the console on the CDK's VCOM (`/dev/tty.usbmodem*`, 115200 8N1), or RTT:

```bash
JLinkRTTLogger -Device NRF52833_XXAA -RTTChannel 0 -if SWD -Speed 4000 capture.log
```

The firmware **auto-starts** capture on boot; drive it live via the shell.

### Terminal capture (macOS) — `capture.sh`

No GUI serial monitor needed. [`capture.sh`](capture.sh) records the CDK's VCOM
console straight from the terminal and shows a **live decoded table** (piping the
stream through the parser) while saving the raw log. Run it with no arguments for
usage:

```bash
./capture.sh rec run.log        # live decoded frames + raw log to run.log (Ctrl-C to stop)
./capture.sh --sp0 run.log      # force SP0 (STS off) first — reset to the CCC Pre-POLL default
./capture.sh --sp1 run.log      # set SP1 (frames carry a PSDU) first, then record
./capture.sh --scan run.log     # lock the live preamble code (sniff scan) first
./capture.sh --pcap run.log     # convert a saved log -> run.pcap and open it
```

It **auto-detects** the CDK (the single-VCOM SEGGER J-Link) among multiple boards;
override with `PORT=/dev/cu.usbmodemXXXX` or `CDK_SERIAL=<serial>` (same serial as
`west flash --dev-id`). It uses `/dev/cu.*` and holds one fd open across
`stty`+read — a fresh `cat`/reopen resets the tty to 9600 on macOS and reads
binary garbage.

## Record format

One line per RX event, prefixed `UWBCAP`, space-separated `key=value` (hex unless
noted). Field → DW3000 User Manual register/field:

| key | meaning | DW3000 UM source |
|---|---|---|
| `idx` | monotonic event index (decimal) | — |
| `ev` | `OK` \| `STS_ERR` \| `FCS_ERR` \| `PHY_ERR` \| `TO` | derived from SYS_STATUS |
| `ts` | 40-bit Ipatov RX timestamp | `RX_TIME` (`dwt_readrxtimestamp_ipatov`) |
| `cfo` | carrier/clock freq offset (signed raw) | CIA clock offset (`dwt_readclockoffset`) |
| `cia` | 1 = CIR diagnostics fresh this frame, 0 = stale | cb `rx_flags` CIA-done bit |
| `cir` | Ipatov CIR power estimate | `IP_DIAG_12` / `rxdiag.ipatovPower` |
| `f1`,`f2`,`f3` | first-path magnitudes | `IP_DIAG_2/3/4` / `ipatovF1..F3` |
| `fpidx` | first-path index, Q10.6 | `IP_DIAG_8` / `ipatovFpIndex` |
| `acc` | accumulated preamble symbols | `IP_DIAG_12` / `ipatovAccumCount` |
| `peak` | Ipatov CIR peak (index\|amp) | `IP_DIAG_1` / `ipatovPeak` |
| `len` | PHR frame length, bytes (incl FCS for data frames) | `RX_FINFO` / `datalength` |
| `rng` | PHR ranging bit | cb `rx_flags` RNG |
| `dr` | configured data rate (`6M8`/`850K`) | `dwt_config_t.dataRate` |
| `sts` | STS quality index (`<0` = STS bad), or `off` | `dwt_readstsquality` |
| `ststat` | STS status word (CP error bits) | `STS_STS` (`dwt_readstsstatus`) |
| `status` | raw SYS_STATUS low word | `SYS_STATUS` |
| `pl` | payload bytes, hex (empty for SP3 no-data / timeout) | `RX_BUFFER_0` (`dwt_readrxdata`) |

**Error-tolerant timing:** for `STS_ERR` / `FCS_ERR` / `PHY_ERR` the Ipatov
timestamp and clock offset are re-latched every frame, so a faulted frame is
captured with non-empty `ts`/`cfo`, not dropped. Only a bare `TO` has empty
timing.

**CIA freshness (`cia`):** the DW3000 only fully refreshes the CIR diagnostic
registers (`cir`/`f1`/`f2`/`f3`/`fpidx`/`acc`/`peak`) when the CIA runs to
completion — i.e. on a *good* frame. On an errored frame CIA aborts and those
registers keep their previous values, so `cia=0` marks the amplitude
diagnostics as stale/untrustworthy (and `cir` channel-power is not logged at
all). `ts` and `cfo` remain fresh regardless. The parser only derives
RSSI/first-path dBm when `cia=1`.

RSSI and first-path power (dBm) are **derived host-side** by the parser from
`cir`/`f1..f3`/`acc` using the UM formula (kept out of the firmware hot path to
avoid float/`log10` in the IRQ workqueue).

## Shell (RTT / UART)

```
sniff | sniff help                                   # grouped command reference
sniff start | stop
sniff summary | human | raw_pretty | raw             # output views (below)
sniff chan <5|9>   sniff preamble <code>   sniff plen <64|128|256|512>
sniff sfd <0..3>   sniff sp <0..3>         sniff ccc
sniff scan         sniff burst [N]         sniff minpeak <hex>
sniff stskey <32hex>|off   sniff stsiv <32hex>   sniff stats [clear]
```

### Output views

Every RX event is available in four renderings; switch live:

- **`summary`** (default) — a calm 1 Hz gauge: presence, a relative signal bar,
  and the ranging-round rate. Answers "is anything happening?".
- **`human`** — one plain-English line per frame (event / signal / clock / length
  as words), with a `time` column.
- **`raw_pretty`** — one compact, colored, aligned line per frame, with a bold
  header row and a `time` column; its columns map 1:1 onto `raw`.
- **`raw`** — the per-frame `UWBCAP key=value` machine lines for the parser / pcap
  (typing `sniff raw` also prints a field legend).

### Example — a live Aliro/CCC ranging session

Boot, then the default `summary` view while an iOS **Aliro** lock ran a UWB
ranging session next to the sniffer (in the terminal `UWB SNIFFER` is bold cyan,
`OK` and every `ACTIVE` + signal bar are green, `quiet` is dim):

```
  UWB SNIFFER   DWM3001CDK / DW3110  -  802.15.4z HRP  -  receive-only
  DEV_ID  0xDECA0302  OK
  radio   chan 9  code 9  plen 64  sfd 0  6.8Mb  SP0
  view    summary    type 'sniff' for commands

[00:00:00.339,324] <inf> uwbcap: capture: RX armed (continuous)
  quiet    [####### ]  close  rounds 9/s (~111ms)  seen 0
  ACTIVE   [#########]  close  rounds 9/s (~111ms)  seen 4
  ACTIVE   [#########]  close  rounds 9/s (~111ms)  seen 20
  ACTIVE   [#########]  close  rounds 8/s (~125ms)  seen 57
  ACTIVE   [#########]  close  rounds 6/s (~166ms)  seen 96
  ACTIVE   [#########]  close  rounds 8/s (~125ms)  seen 150
```

Each line prints once per second:

- **`ACTIVE` / `quiet`** — whether frames decoded cleanly (`OK` + `STS_ERR`) that
  second; PHY/CRC errors and timeouts don't count as activity.
- **`[#########] close`** — a *relative* signal bar from the first-path magnitude
  (`f1`): near-full → strong, hence `close`. A bench gauge, not a calibrated
  distance/dBm (dBm is derived host-side in the parser).
- **`rounds 9/s (~111ms)`** — ranging rounds detected, ~9 per second (one every
  ~111 ms). A "round" starts when any frame arrives after a >50 ms gap, so this
  tracks the lock's ranging cadence whether or not each frame decodes.
- **`seen N`** — running total of the cleanly-decoded frames. At the default
  **SP0** config those are the cleartext **Pre-POLL** at the head of each Aliro/CCC
  round, so a rising `seen` means the sniffer is decoding the Pre-POLLs.

So this is the Aliro lock's UWB ranging captured out of the box — ~9 rounds/s,
strong and close, with the cleartext Pre-POLL decoding at the head of each round.
The encrypted **POLL / RESP / FINAL** that follow are SP3-ND and show up as errors
here; decode them by loading the session key and switching to SP3 (`sniff
stskey`/`stsiv` + `sniff sp 3`, below). The `rounds/s` easing from 9→6→8 is the
*smoothed* rate — the session's cadence varying and/or the immediate-log path
dropping a few Pre-POLLs during faster sub-bursts (see *Known limitations*).

### STS quality without a key

The sniffer receives but does not transmit and, by default, holds no STS key (to
load one, see the next section). In an
STS-protected packet config (SP1/2/3) it still reports the DW3000's STS quality
verdict: a frame whose STS the receiver can't reproduce raises the `CPERR` bit
(STS quality error, `0x10000000`) and surfaces as `STS_ERR` with valid preamble
timing. In SP3 (STS-no-data) there is no real FCS, so the spurious `RXFCE` the
chip raises on an unresolved STS-only frame is ignored; an SP3 `STS_ERR` means
"heard the preamble, couldn't validate the STS."

### Decoding a keyed session (`sniff stskey` / `sniff stsiv`)

If you *do* have a session's STS material — e.g. you are a participant in a
secure-ranging session and derived its 128-bit STS key and IV (V) off-device —
load them to make the STS correlate, turning matching `STS_ERR` frames into
decoded `OK` (RXFCG) with a positive `sts` quality:

```
sniff stskey <32 hex>   # 128-bit STS AES key (canonical big-endian, byte 0 = MSB)
sniff stsiv  <32 hex>   # 128-bit STS IV / V   (same byte order)
sniff stskey off        # revert to the DW3000 default key
```

Byte order matches Qorvo's own loader: the key is whole-array byte-reversed then
packed as four little-endian words; the IV is packed as four little-endian words
directly (`dwt_configurestskey` / `dwt_configurestsiv` / `dwt_configurestsloadiv`
in `apply_sts()`). The STS index for dynamic-STS schemes advances the IV per
frame, so this is typically driven from a host script that reloads per index.

### Log throttling under a strong-transmitter burst (`minpeak`)

When a strong transmitter starts up right next to the sniffer, the RX loop
floods — real frames *plus* a swarm of weak noise/partial detections — and
immediate logging from the RX callback can't keep up, drowning the console.
`sniff minpeak <hex>` logs only frames whose Ipatov CIR peak is `>=` the value;
weaker frames are still counted (in `stats`) and RX is re-armed, they just
aren't printed. Default 0 = log all.

Pick the threshold from a quiet-air capture: read off the peak amplitudes of the
frames you care about vs. the junk, and set `minpeak` between them. (This gates
on peak *position/amplitude* and is specific to your bench geometry — re-check
the value if the antenna/channel changes.) The robust fix for sustained bursts
is a lock-free ring + dedicated log thread (see *Known limitations*).

### Defaults (overridable via shell or the `g_cfg` `#define`s)

Channel 9, BPRF, PRF 64 MHz, preamble code 9, 64-symbol preamble, SFD type 0
(`DWT_SFD_IEEE_4A` — the IEEE 8-symbol ternary SFD), 6.8 Mbps, **SP0** (STS
off). This is the PHY an Apple Wallet key negotiates for Aliro/CCC ranging (CCC
v4 Table 21-1 **Config 0000**), the one set shared by every frame in a round —
so the cleartext **Pre-POLL** that leads each round is heard out of the box.
`sniff ccc` restores this whole set in one command. (The generic-FiRa 4z SFD
SFD-times-out on Apple frames — that was the old default's blind spot.)

The keyed **POLL / RESP / FINAL** are SP3-ND: load the session dURSK/STS-V with
`sniff stskey`/`stsiv` and switch to `sniff sp 3` to decode those.

> **Note on mixed SP0/SP3:** the DW3000 receiver decodes one STS packet-config
> at a time — there is no true simultaneous SP0+SP3 RX. Pick the config that
> matches the frame under observation (SP0 for the Pre-POLL and unprotected
> frames, SP3 for the keyed ranging RFRAMEs) with `sniff sp`.

## Host parser

`parser/uwb_capture_parse.py` reads capture lines (from a file or stdin) into
rows `index, event, rx_stamp, rssi, fp, phr, payload_hex, sts_qual`, computing
RSSI/first-path dBm from the raw diagnostics:

```bash
python3 parser/uwb_capture_parse.py capture.log
cat /dev/cu.usbmodem* | python3 parser/uwb_capture_parse.py --live -   # stream a live table
python3 parser/uwb_capture_parse.py capture.log --json
python3 parser/uwb_capture_parse.py capture.log --analyze   # plain-language summary
```

`--live` prints one table row per frame as its line arrives (for a piped serial
capture — this is what `capture.sh rec` uses); the other modes read to EOF first.

### Wireshark (802.15.4 dissection)

`--pcap` turns the captured PSDUs into an IEEE 802.15.4 capture that Wireshark's
`wpan` dissector renders in full — frame control, addressing, FiRa/802.15.4z
fields — instead of hand-decoding hex. Device-time RX stamps are rebuilt into a
nanosecond timeline, so inter-frame and inter-round gaps are preserved.

Frames only carry a decodable MAC payload when there's data in the PPDU, so
capture with an STS mode that leaves a data segment — the default **`sniff sp 0`**
(STS off, e.g. the CCC Pre-POLL) or **`sniff sp 1`** (STS + data). **SP3** is
STS-only (no PSDU), so a pure-SP3 capture produces an empty pcap by design.

```bash
python3 parser/uwb_capture_parse.py capture.log --pcap capture.pcap
python3 parser/uwb_capture_parse.py capture.log --pcap capture.pcap --nofcs  # PSDU has no trailing FCS
open capture.pcap        # or: wireshark capture.pcap
```

## Known limitations

- **Log-drop under burst load.** Per-frame lines are emitted synchronously
  (`CONFIG_LOG_MODE_IMMEDIATE`) from the DW3000 IRQ handler, which the HAL runs
  on the **system workqueue** (stack sized via `CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE`). In a
  fast DS-TWR burst (POLL/RESP/FINAL every few ms) the UART write can stall the
  re-arm path and lines can be dropped. Fine for a first cut; the robust upgrade
  is a lock-free SPSC ring filled in the callback + a dedicated drain/log thread
  (callback does only: read regs → push record → `dwt_rxenable`).
- **No CIR/accumulator dump.** The line carries CIA summary diagnostics, not the
  raw complex CIR accumulator. Add a separate on-demand `dwt_readaccdata` dump
  if per-tap analysis is needed.

## Safety

Never sets APPROTECT / one-way fuses / lifecycle-advance bits. All radio access
stays in the portable DW3000 API (`src/capture.c`); DW3000 UM register/field
names are cited in comments touching STS, RX diagnostics, and timestamps.

## License

MIT — see [`LICENSE`](LICENSE).
The Qorvo DW3000 driver at `external/dw3000` is **not** part of this repository
and is governed by its own license.
