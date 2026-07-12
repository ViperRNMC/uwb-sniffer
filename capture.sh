#!/usr/bin/env bash
# capture.sh — record / decode the DWM3001CDK VCOM console (macOS).
# Run with no arguments for usage.
set -euo pipefail

BAUD=115200
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
	cat <<EOF
capture.sh — record / decode the DWM3001CDK console (macOS)

Usage:
  ./capture.sh rec [out.log]        show decoded frames live in the terminal while saving the
                                    raw log to out.log (default capture.log); Ctrl-C stops
  ./capture.sh --sp0 [out.log]      force SP0 (STS off) first — hear the cleartext CCC
                                    Pre-POLL; resets the board if left in another SP mode
  ./capture.sh --sp1 [out.log]      same, but set SP1 first so frames carry a PSDU
                                    (needed before --pcap can dissect them)
  ./capture.sh --scan [out.log]     lock onto the transmitter's live preamble code
                                    first (FiRa/Apple negotiate it); combine with --sp1
  ./capture.sh --pcap <in.log>      convert a recorded log to <in>.pcap and open it in Wireshark
  ./capture.sh -h | --help          show this help

Port is auto-detected — the CDK is the single-VCOM SEGGER J-Link. Override with:
  PORT=/dev/cu.usbmodemXXX ./capture.sh ...     exact device path
  CDK_SERIAL=<serial> ./capture.sh ...          same serial as \`west flash --dev-id\`
EOF
}

resolve_port() {
	python3 - <<'PY'
import os, sys, plistlib, subprocess
from collections import Counter

def jlink_ports():
    xml = subprocess.run(
        ["ioreg", "-a", "-r", "-c", "IOUSBHostDevice", "-l", "-w0"],
        capture_output=True).stdout
    data = plistlib.loads(xml) if xml else []
    ports = []
    def walk(n, prod=None, vend=None, ser=None):
        prod = n.get("USB Product Name", prod)
        vend = n.get("USB Vendor Name", vend)
        ser = n.get("USB Serial Number", ser)
        bsd = n.get("IOCalloutDevice")
        if bsd and "usbmodem" in str(bsd) and (
                (vend and "SEGGER" in vend) or (prod and "J-Link" in prod)):
            ports.append((str(bsd), ser or ""))
        for c in n.get("IORegistryEntryChildren", []):
            walk(c, prod, vend, ser)
    for r in (data if isinstance(data, list) else [data]):
        walk(r)
    return ports

if os.environ.get("PORT"):
    print(os.environ["PORT"]); sys.exit(0)

ports = jlink_ports()
if not ports:
    sys.exit("no SEGGER J-Link VCOM found — is the CDK plugged in? "
             "(or set PORT=/dev/cu.usbmodemXXX)")

cdk = os.environ.get("CDK_SERIAL")
if cdk:
    hits = [b for b, s in ports if cdk in s]
    if len(hits) == 1:
        print(hits[0]); sys.exit(0)
    sys.exit(f"CDK_SERIAL={cdk} matched {len(hits)} ports; set PORT= instead")

# Heuristic: the CDK's J-Link shows a single VCOM; a dual-VCOM board shows two.
per_serial = Counter(s for _, s in ports)
singles = [b for b, s in ports if per_serial[s] == 1]
if len(singles) == 1:
    print(singles[0]); sys.exit(0)

sys.stderr.write("can't pick a port automatically — J-Link VCOMs found:\n")
for b, s in ports:
    sys.stderr.write(f"  {b}  (J-Link serial {s})\n")
sys.stderr.write("choose one:  CDK_SERIAL=<serial> ./capture.sh ...   "
                 "or  PORT=<dev> ./capture.sh ...\n")
sys.exit(2)
PY
}

# No arguments (or -h/--help): show usage instead of dumping the raw console.
case "${1:-}" in
	-h|--help|"") usage; exit 0 ;;
esac

# --pcap: offline convert, no serial needed.
if [ "${1:-}" = "--pcap" ]; then
	log="${2:?usage: ./capture.sh --pcap <logfile>}"
	pcap="${log%.*}.pcap"
	python3 "$HERE/parser/uwb_capture_parse.py" "$log" --pcap "$pcap"
	command -v open >/dev/null 2>&1 && open "$pcap" 2>/dev/null || true
	exit 0
fi

[ "${1:-}" = "rec" ] && shift   # `rec` is optional sugar for "record"

drive=0
sp0=0
scan=0
while [ $# -gt 0 ]; do
	case "$1" in
		--sp0)  sp0=1; shift ;;
		--sp1)  drive=1; shift ;;
		--scan) scan=1; shift ;;
		--*)    echo "unknown option: $1" >&2; usage; exit 2 ;;
		*)      break ;;   # first non-flag arg is the log filename
	esac
done
out="${1:-capture.log}"

port="$(resolve_port)"
echo "recording $port @ ${BAUD} -> $out   (decoded live; Ctrl-C to stop)" >&2

# Hold the port open on fd 3 for the whole session. On macOS every fresh open of
# a tty resets its termios to the 9600 default, so configuring with `stty` and
# then reopening via `cat` would read at the wrong baud (binary garbage). One
# persistent fd means the stty settings — and our writes — apply to that session.
exec 3<>"$port"
stty -f "$port" "$BAUD" cs8 -parenb -cstopb -crtscts raw -echo

# Put the board in machine-line mode so the parser can decode it; SP1 on request.
printf 'sniff raw\r' >&3
[ "$sp0" -eq 1 ] && printf 'sniff sp 0\r' >&3
[ "$drive" -eq 1 ] && printf 'sniff sp 1\r' >&3
# Lock onto the transmitter's live preamble code (FiRa/Apple negotiate it).
[ "$scan" -eq 1 ] && printf 'sniff scan\r' >&3

# tee keeps the raw log; the parser turns the same stream into a live table.
cat <&3 | tee "$out" | python3 -u "$HERE/parser/uwb_capture_parse.py" --live -
