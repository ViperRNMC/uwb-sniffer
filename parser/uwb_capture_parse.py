#!/usr/bin/env python3
"""Parse DWM3001CDK passive-capture log lines into structured rows.

The firmware emits one `UWBCAP ...` line per RX event over RTT/UART.  This
reads such lines (from a file or stdin), pulls out the
fields, and derives human-readable RSSI / first-path power in dBm from the raw
DW3000 CIA diagnostic registers using the DW3000 User Manual formulae.

Usage:
    uwb_capture_parse.py capture.log
    cat /dev/tty.usbmodem* | uwb_capture_parse.py -
    uwb_capture_parse.py capture.log --json

Row fields (as requested):
    index, event, rx_stamp, rssi, fp, phr, payload_hex, sts_qual
plus the raw diagnostics kept for anyone who wants them.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import struct
import sys
from dataclasses import asdict, dataclass, field

# One capture line, e.g.:
#   UWBCAP idx=000123 ev=STS_ERR ts=00A1B2C3D4 cfo=-42 cir=0001F2A3 f1=00012345
#   f2=0000ABCD f3=00009876 fpidx=05C0 acc=0400 peak=03A1F000 len=12 rng=1
#   dr=6M8 sts=-1 ststat=0004 status=00840000 pl=6162...
_LINE_RE = re.compile(r"\bUWBCAP\b\s+(?P<kv>.*)$")
_KV_RE = re.compile(r"(\w+)=(\S+)")

# DW3000 User Manual RSSI / first-path constants for PRF 64 MHz (BPRF).
# RSSI      = 10*log10( C * 2^21 / N^2 ) - A            (UM eq. for CIR power)
# FP power  = 10*log10( (F1^2 + F2^2 + F3^2) / N^2 ) - A
# where C = CIR power (ipatovPower), N = accumulated preamble symbols
# (ipatovAccumCount), A = 121.7 dB for 64 MHz PRF (113.8 for 16 MHz).
_PRF64_A = 121.7
_TWO_POW_21 = 1 << 21


@dataclass
class Frame:
    """One decoded capture record."""

    index: int
    event: str
    rx_stamp: int  # 40-bit device-time ticks (~15.65 ps each)
    cfo_raw: int  # signed CIA clock offset (raw register units)
    cia_done: int  # 1 => CIR diagnostics fresh this frame; 0 => stale
    phr_len: int  # PHR frame length in bytes (incl FCS for data frames)
    ranging: int  # PHR ranging bit
    data_rate: str
    sts_qual: int | None  # STS quality index; <0 => STS bad; None => STS off
    sts_status: int | None  # STS_STS status word (CP error bits), None => STS off
    sys_status: int  # raw SYS_STATUS low word
    payload_hex: str
    # raw CIA diagnostics
    cir_power: int
    f1: int
    f2: int
    f3: int
    fp_index_q: int  # first-path index, Q10.6
    accum_count: int
    peak: int
    # derived
    rssi_dbm: float | None = None
    fp_dbm: float | None = None
    fp_index: float | None = None  # fp_index_q decoded to fractional samples

    # convenience view matching the requested column set
    @property
    def fp(self) -> float | None:
        return self.fp_dbm


def _to_int(v: str, base: int = 16) -> int:
    return int(v, base)


def parse_line(line: str) -> Frame | None:
    """Parse one log line into a @ref Frame, or None if it isn't a capture line."""
    m = _LINE_RE.search(line)
    if not m:
        return None
    kv = dict(_KV_RE.findall(m.group("kv")))
    if "idx" not in kv or "ev" not in kv:
        return None

    # A garbled line — e.g. immediate-mode log output interleaved mid-write, so
    # a timestamp lands in a numeric field — must not kill the whole run; skip
    # it and move on.
    try:
        sts_present = kv.get("sts", "off") != "off"
        cia_done = int(kv.get("cia", "1"), 10)  # default 1 for logs predating the flag
        cir = _to_int(kv.get("cir", "0"))
        f1 = _to_int(kv.get("f1", "0"))
        f2 = _to_int(kv.get("f2", "0"))
        f3 = _to_int(kv.get("f3", "0"))
        acc = _to_int(kv.get("acc", "0"))
        fpidx_q = _to_int(kv.get("fpidx", "0"))

        fr = Frame(
            index=int(kv["idx"], 10),
            event=kv["ev"],
            rx_stamp=_to_int(kv.get("ts", "0")),
            cfo_raw=int(kv.get("cfo", "0"), 10),
            cia_done=cia_done,
            phr_len=int(kv.get("len", "0"), 10),
            ranging=int(kv.get("rng", "0"), 10),
            data_rate=kv.get("dr", "?"),
            sts_qual=int(kv["sts"], 10) if sts_present else None,
            sts_status=_to_int(kv["ststat"]) if "ststat" in kv else None,
            sys_status=_to_int(kv.get("status", "0")),
            payload_hex=kv.get("pl", ""),
            cir_power=cir,
            f1=f1,
            f2=f2,
            f3=f3,
            fp_index_q=fpidx_q,
            accum_count=acc,
            peak=_to_int(kv.get("peak", "0")),
        )
    except ValueError:
        return None

    # Derive dBm figures only when CIA completed (diagnostics fresh) and we
    # have a valid preamble accumulation.  On errored frames (cia=0) the DW3000
    # amplitude/power registers hold stale values, so deriving dBm from them
    # would be misleading — leave rssi/fp as None.
    if cia_done and acc > 0:
        n2 = float(acc) * float(acc)
        if cir > 0:
            fr.rssi_dbm = round(10.0 * math.log10(cir * _TWO_POW_21 / n2) - _PRF64_A, 1)
        fpsum = float(f1) ** 2 + float(f2) ** 2 + float(f3) ** 2
        if fpsum > 0:
            fr.fp_dbm = round(10.0 * math.log10(fpsum / n2) - _PRF64_A, 1)
        fr.fp_index = fpidx_q / 64.0  # Q10.6 -> fractional CIR sample index
    return fr


def parse_stream(fp) -> list[Frame]:
    """Parse every capture line from an iterable of text lines."""
    out: list[Frame] = []
    for line in fp:
        fr = parse_line(line)
        if fr is not None:
            out.append(fr)
    return out


def _cluster_cfo(values: list[int], gap: int = 40) -> list[list[int]]:
    """Gap-based 1-D clustering of clock-offset values into transmitter groups."""
    if not values:
        return []
    vs = sorted(values)
    groups = [[vs[0]]]
    for v in vs[1:]:
        if v - groups[-1][-1] > gap:
            groups.append([v])
        else:
            groups[-1].append(v)
    return groups


def _fmt_gap(ns: float) -> str:
    if ns >= 1e6:
        return f"{ns / 1e6:.2f} ms"
    if ns >= 1e3:
        return f"{ns / 1e3:.1f} us"
    return f"{ns:.0f} ns"


# DW3000 device-time tick ~= 1 / (499.2 MHz * 128) = 15.65 ps.
_TICK_NS = 1.0 / 63.8976
_TS_WRAP = 1 << 40  # 40-bit RX timestamp


def _deltas_ns(stamps: list[int]) -> list[float]:
    """Inter-frame gaps (ns) from consecutive 40-bit RX timestamps (wrap-safe)."""
    return [((b - a) % _TS_WRAP) * _TICK_NS for a, b in zip(stamps, stamps[1:])]


# libpcap link-types for a bare IEEE 802.15.4 MAC frame (PSDU).
_DLT_IEEE802_15_4_WITHFCS = 195  # PSDU ends in the 2-byte FCS (our default)
_DLT_IEEE802_15_4_NOFCS = 230  # PSDU without the trailing FCS
_PCAP_NS_MAGIC = 0xA1B23C4D  # nanosecond-resolution pcap


def write_pcap(frames: list[Frame], out, withfcs: bool = True) -> int:
    """Write data-bearing frames as a nanosecond-resolution pcap for Wireshark.

    Each captured PSDU (`pl=` bytes: MHR + payload + FCS) becomes one packet
    under DLT_IEEE802_15_4, so Wireshark's 802.15.4/802.15.4z dissector renders
    the frame control, addressing and FiRa fields we can't decode by hand.

    Timestamps are a monotonic timeline rebuilt from the DW3000 device-time RX
    stamps (wrap-safe), so inter-frame and inter-round gaps are preserved to the
    nanosecond — the part that actually matters for reading round structure.

    The default SP0 (STS off, e.g. the CCC Pre-POLL) and SP1 (STS + data) carry
    a PSDU; SP3 (STS-no-data) does not, so a pure-SP3 capture lands nothing here.

    @return number of packets written.
    """
    usable = [f for f in frames if f.payload_hex and f.rx_stamp > 0]
    dlt = _DLT_IEEE802_15_4_WITHFCS if withfcs else _DLT_IEEE802_15_4_NOFCS
    # Global header: magic, ver 2.4, thiszone, sigfigs, snaplen, network.
    out.write(struct.pack("<IHHiIII", _PCAP_NS_MAGIC, 2, 4, 0, 0, 65535, dlt))

    t_ns = 0.0
    prev_stamp: int | None = None
    n = 0
    for f in usable:
        try:
            data = bytes.fromhex(f.payload_hex)
        except ValueError:
            continue  # garbled hex — skip, keep the rest of the capture
        if prev_stamp is not None:
            t_ns += ((f.rx_stamp - prev_stamp) % _TS_WRAP) * _TICK_NS
        prev_stamp = f.rx_stamp
        sec = int(t_ns // 1_000_000_000)
        nsec = int(t_ns - sec * 1_000_000_000)
        out.write(struct.pack("<IIII", sec, nsec, len(data), len(data)))
        out.write(data)
        n += 1
    return n


def _detect_rounds(stamps: list[int]) -> tuple[list[int], list[float], list[float]]:
    """Group frames into ranging rounds by the two-timescale gap structure.

    Ranging is bursty: a few frames within a round (small gaps) separated by the
    ranging interval (large gaps).  A gap below 30% of the median splits as
    intra-round; anything larger starts a new round.  Device-agnostic — this is
    the generic FiRa / 802.15.4z round shape, not iPhone-specific.

    Returns (frames_per_round, intra_round_gaps_ns, between_round_gaps_ns).
    """
    gaps = _deltas_ns(stamps)
    if not gaps:
        return [1], [], []
    thr = sorted(gaps)[len(gaps) // 2] * 0.3
    sizes = [1]
    intra: list[float] = []
    inter: list[float] = []
    for g in gaps:
        if g <= thr:
            sizes[-1] += 1
            intra.append(g)
        else:
            inter.append(g)
            sizes.append(1)
    return sizes, intra, inter


def analyze(frames: list[Frame]) -> str:
    """Plain-language 'what's going on' summary of a passive capture.

    Reconstructs the *shape* of the exchange — how many transmitters (by
    clock-offset fingerprint), the rhythm between frames, and signal strength.
    It cannot see contents (encrypted), identity, or true distance.
    """
    real = [f for f in frames if f.event in ("OK", "STS_ERR") and f.rx_stamp > 0]
    out: list[str] = [
        f"UWB activity — {len(frames)} events captured, {len(real)} real ranging frames"
    ]
    if not real:
        out.append(
            "  No ranging frames — aim at an active UWB device and use `sniff raw`."
        )
        return "\n".join(out)

    stamps = [f.rx_stamp for f in real]
    span_ns = sum(_deltas_ns(stamps)) if len(stamps) > 1 else 0.0
    if span_ns > 0:
        out.append(
            f"  spanning ~{_fmt_gap(span_ns)} of device time "
            f"(~{len(real) / (span_ns / 1e9):.0f} frames/s seen)"
        )

    # Who's talking — group by clock-offset fingerprint.  A stray frame or two
    # is drift/noise, not a radio: only groups holding a real share of frames
    # count.
    groups = _cluster_cfo([f.cfo_raw for f in real])
    min_frames = max(3, len(real) // 50)  # a "radio" needs >= ~2% of the frames
    radios = sorted((g for g in groups if len(g) >= min_frames), key=len, reverse=True)
    strays = sum(len(g) for g in groups if len(g) < min_frames)
    out.append("")
    if len(radios) <= 1:
        g = radios[0] if radios else max(groups, key=len)
        out.append(
            f"Transmitters: ONE main radio (clock offset ~{sum(g) / len(g):+.0f})."
        )
        out.append("  Likely one pair sharing a near-identical clock (phone + lock).")
    else:
        out.append(f"Transmitters: {len(radios)} radios (by clock-offset fingerprint):")
        for i, g in enumerate(radios):
            out.append(
                f"  radio {chr(65 + i)}: clock ~{sum(g) / len(g):+.0f} "
                f"({g[0]:+d}..{g[-1]:+d})  {len(g)} frames"
            )
    if strays:
        out.append(f"  (+{strays} stray frames from clock drift/noise, ignored.)")

    # Ranging rounds — the two-timescale (intra-burst vs between-round) shape.
    sizes, intra, inter = _detect_rounds(stamps)
    out.append("")
    out.append("Ranging rounds:")
    if inter:
        ivl = sorted(inter)[len(inter) // 2]
        out.append(
            f"  ~{len(sizes)} rounds, ~{_fmt_gap(ivl)} apart (~{1e9 / ivl:.0f} rounds/s)"
        )
    med_sz = sorted(sizes)[len(sizes) // 2]
    multi = sum(1 for s in sizes if s >= 2)
    if med_sz >= 2 and intra:
        rep = sorted(intra)[len(intra) // 2]
        kind = (
            "single-sided TWR (POLL->RESPONSE)"
            if med_sz == 2
            else "double-sided TWR (POLL->RESPONSE->FINAL)"
            if med_sz == 3
            else f"{med_sz} frames/round"
        )
        out.append(f"  ~{med_sz} frames/round -> {kind}; reply gap ~{_fmt_gap(rep)}")
    elif multi and intra:
        rep = sorted(intra)[len(intra) // 2]
        out.append(
            f"  mostly 1 frame/round, but {multi} rounds caught >=2 frames "
            f"(~{_fmt_gap(rep)} apart) — a glimpse of the intra-round burst."
        )
        out.append("  Most are still dropped; match the session's preamble code")
        out.append(
            "  (sweep `sniff preamble 9..12`), then `sniff burst` for the full round."
        )
    else:
        out.append(
            "  only ~1 frame/round captured — the intra-round POLL/RESPONSE/FINAL burst"
        )
        out.append("  is being dropped. Match the session's preamble code (sweep")
        out.append("  `sniff preamble 9..12`), then `sniff burst`.")

    # Signal.
    dbm = sorted(f.fp_dbm for f in real if f.fp_dbm is not None)
    if dbm:
        out.append("")
        out.append(
            f"Signal (first-path): {dbm[0]:.0f} to {dbm[-1]:.0f} dBm "
            f"(median {dbm[len(dbm) // 2]:.0f})."
        )

    out.append("")
    out.append(
        "Can't see (passive, no key): frame contents (encrypted), device identity, "
        "or true distance (needs two-way ranging)."
    )
    return "\n".join(out)


_TABLE_HDR = (
    f"{'idx':>6} {'event':<8} {'rx_stamp':>12} {'rssi':>7} {'fp':>7} "
    f"{'phr':>4} {'sts':>4} payload_hex"
)


def _table_row(f: Frame) -> str:
    rssi = "" if f.rssi_dbm is None else f"{f.rssi_dbm:.1f}"
    fpv = "" if f.fp_dbm is None else f"{f.fp_dbm:.1f}"
    sts = "off" if f.sts_qual is None else str(f.sts_qual)
    return (
        f"{f.index:>6} {f.event:<8} {f.rx_stamp:>12X} {rssi:>7} {fpv:>7} "
        f"{f.phr_len:>4} {sts:>4} {f.payload_hex}"
    )


def stream_table(fp) -> int:
    """Print the terse table live, one row per frame as its line arrives.

    Uses readline() (not `for line in fp`) so a piped serial stream is shown
    immediately instead of waiting for Python's read-ahead buffer to fill.
    """
    print(_TABLE_HDR, flush=True)
    while True:
        line = fp.readline()
        if not line:
            break
        fr = parse_line(line)
        if fr is not None:
            print(_table_row(fr), flush=True)
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("source", help="log file, or '-' for stdin")
    ap.add_argument(
        "--live",
        action="store_true",
        help="stream the terse table live, one row per frame as it arrives "
        "(for a piped serial capture); other output modes read to EOF first",
    )
    ap.add_argument("--json", action="store_true", help="emit JSON rows")
    ap.add_argument(
        "--analyze",
        action="store_true",
        help="print a plain-language 'what's going on' summary",
    )
    ap.add_argument(
        "--pcap",
        metavar="FILE",
        help="write data-bearing frames as an 802.15.4 pcap for Wireshark "
        "('-' for stdout); capture in SP0/SP1 for dissectable frames",
    )
    ap.add_argument(
        "--nofcs",
        action="store_true",
        help="with --pcap: mark the PSDU as carrying no trailing FCS (DLT 230)",
    )
    args = ap.parse_args(argv)

    if args.source == "-":
        # Serial line noise / binary bytes aren't valid UTF-8; decode leniently
        # so one bad byte can't kill a live capture (files already do this).
        try:
            sys.stdin.reconfigure(errors="replace")
        except AttributeError:
            pass
        fp = sys.stdin
    else:
        fp = open(args.source, "r", errors="replace")
    try:
        if args.live:
            return stream_table(fp)
        frames = parse_stream(fp)
    finally:
        if fp is not sys.stdin:
            fp.close()

    if args.pcap:
        if args.pcap == "-":
            n = write_pcap(frames, sys.stdout.buffer, withfcs=not args.nofcs)
        else:
            with open(args.pcap, "wb") as pf:
                n = write_pcap(frames, pf, withfcs=not args.nofcs)
        print(f"wrote {n} frames to {args.pcap}", file=sys.stderr)
        if n == 0:
            print(
                "  (no data-bearing frames — SP3 carries no PSDU; "
                "capture with the default `sniff sp 0` or `sniff sp 1`)",
                file=sys.stderr,
            )
        return 0

    if args.analyze:
        print(analyze(frames))
        return 0

    if args.json:
        json.dump([asdict(f) for f in frames], sys.stdout, indent=2)
        sys.stdout.write("\n")
        return 0

    # Terse table: the requested column set.
    print(_TABLE_HDR)
    for f in frames:
        print(_table_row(f))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except KeyboardInterrupt:
        sys.exit(130)
