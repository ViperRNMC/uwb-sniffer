/*
 * @file shell_cmds.c
 * @brief `sniff` shell command tree for the passive capture engine.
 *
 * Exposes the capture.h API over the Zephyr shell (RTT/UART):
 *   sniff start | stop
 *   sniff chan <5|9>       sniff preamble <code>   sniff plen <symbols>
 *   sniff sfd <0..3>       sniff sp <0..3>       sniff ccc
 *   sniff minpeak <hex>    sniff human | raw      sniff burst [N]  sniff scan
 *   sniff stskey <hex>|off sniff stsiv <hex>      sniff stats [clear]
 */

#include "capture.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/shell/shell.h>

/* ── Response styling ──────────────────────────────────────────────────────
 * A green "ok " confirmation prefix (message text that follows stays default),
 * and one colored row of the `sniff stats` table.  VT100 color auto-drops when
 * CONFIG_SHELL_VT100_COLORS=n or the terminal doesn't support it. */
static void tick(const struct shell *sh)
{
	shell_fprintf(sh, SHELL_VT100_COLOR_GREEN, "  ok ");
}

/** @brief One `sniff stats` row: name in default, count colored (dim when 0). */
static void stat_row(const struct shell *sh, const char *name, uint32_t v,
		     enum shell_vt100_color col)
{
	shell_fprintf(sh, SHELL_VT100_COLOR_DEFAULT, "  %-9s", name);
	shell_fprintf(sh, v ? col : SHELL_VT100_COLOR_DEFAULT, " %8u\n", v);
}

/** @brief A colored section header in the `sniff` help screen. */
static void sec(const struct shell *sh, const char *title)
{
	shell_fprintf(sh, SHELL_VT100_COLOR_YELLOW, "\n  %s\n", title);
}

/** @brief One help row: cyan `name+args` column, then the description. */
static void row(const struct shell *sh, const char *cmd, const char *desc)
{
	shell_fprintf(sh, SHELL_VT100_COLOR_CYAN, "    %-24s", cmd);
	shell_print(sh, "%s", desc);
}

/** @brief One `sniff raw` legend row: cyan field name, then its meaning. */
static void leg(const struct shell *sh, const char *field, const char *meaning)
{
	shell_fprintf(sh, SHELL_VT100_COLOR_CYAN, "    %-8s ", field);
	shell_print(sh, "%s", meaning);
}

/** @brief `uwb start` - arm continuous RX. */
static int cmd_start(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	int err = capture_start();

	if (err) {
		shell_error(sh, "start failed: %d", err);
		return err;
	}
	tick(sh);
	shell_print(sh, "capturing");
	return 0;
}

/** @brief `uwb stop` - force radio off. */
static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	capture_stop();
	tick(sh);
	shell_print(sh, "stopped");
	return 0;
}

/** @brief `uwb chan <5|9>`. */
static int cmd_chan(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	int err = capture_set_channel((uint8_t)atoi(argv[1]));

	if (err) {
		shell_error(sh, "invalid channel (5 or 9)");
		return err;
	}
	tick(sh);
	shell_print(sh, "chan %s", argv[1]);
	return 0;
}

/** @brief `uwb preamble <code>` - TX+RX preamble code. */
static int cmd_preamble(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	int err = capture_set_preamble_code((uint8_t)atoi(argv[1]));

	if (err) {
		shell_error(sh, "reconfigure failed: %d", err);
		return err;
	}
	tick(sh);
	shell_print(sh, "code %s", argv[1]);
	return 0;
}

/** @brief `uwb plen <symbols>` - preamble length (64/128/256/512). */
static int cmd_plen(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	int err = capture_set_preamble_len((uint16_t)atoi(argv[1]));

	if (err) {
		shell_error(sh, "invalid plen (64/128/256/512)");
		return err;
	}
	tick(sh);
	shell_print(sh, "plen %s", argv[1]);
	return 0;
}

/** @brief `uwb sfd <0..3>` - dwt_sfd_type_e (3 = IEEE 802.15.4z BPRF). */
static int cmd_sfd(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	int err = capture_set_sfd((uint8_t)atoi(argv[1]));

	if (err) {
		shell_error(sh, "invalid sfd (0..3)");
		return err;
	}
	tick(sh);
	shell_print(sh, "sfd %s", argv[1]);
	return 0;
}

/** @brief `uwb sp <0..3>` - STS packet config (0=SP0 off, 3=SP3 no-data). */
static int cmd_sp(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	int err = capture_set_sp((uint8_t)atoi(argv[1]));

	if (err) {
		shell_error(sh, "invalid sp (0..3)");
		return err;
	}
	tick(sh);
	shell_print(sh, "sp %s", argv[1]);
	return 0;
}

/** @brief `sniff ccc` - retune to the Aliro/CCC ranging PHY (Config 0000). */
static int cmd_ccc(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	int err = capture_set_ccc();

	if (err) {
		shell_error(sh, "ccc: reconfigure failed: %d", err);
		return err;
	}
	tick(sh);
	shell_print(sh, "ccc - chan9 code9 plen64 sfd4a 6.8Mb SP0 - Apple Pre-POLL");
	return 0;
}

/** @brief `sniff minpeak <hex>` - only log frames with peak >= value (0 = all). */
static int cmd_minpeak(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	uint32_t v = (uint32_t)strtoul(argv[1], NULL, 0); /* accepts 0x-prefixed hex */

	capture_set_min_peak(v);
	tick(sh);
	shell_print(sh, "minpeak 0x%08X", v);
	return 0;
}

/** @brief `sniff summary` - the 1 Hz aggregate gauge (signal bar + round rate). */
static int cmd_summary(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	capture_set_view(CAP_VIEW_SUMMARY);
	tick(sh);
	shell_print(sh, "view summary - 1 Hz gauge (signal bar + round rate)");
	return 0;
}

/** @brief `sniff human` - one plain-English line per frame. */
static int cmd_human(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	capture_set_view(CAP_VIEW_HUMAN);
	tick(sh);
	shell_print(sh, "view human - one plain-English line per frame");
	return 0;
}

/** @brief `sniff raw` - per-frame `UWBCAP key=value` machine lines (for parsing). */
static int cmd_raw(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	capture_set_view(CAP_VIEW_RAW);
	tick(sh);
	shell_print(sh, "view raw - per-frame UWBCAP lines (feed to the parser / pcap)");

	sec(sh, "legend");
	leg(sh, "idx", "frame number (monotonic)");
	leg(sh, "ev", "OK | STS_ERR | FCS_ERR | PHY_ERR | TO");
	leg(sh, "ts", "Ipatov RX timestamp, 40-bit device time (~15.65 ps/tick)");
	leg(sh, "cfo", "clock/carrier offset vs the transmitter (signed)");
	leg(sh, "cia", "CIR diagnostics fresh? 1 = yes, 0 = stale (errored frame)");
	leg(sh, "cir", "Ipatov CIR power   -> RSSI dBm (derived host-side)");
	leg(sh, "f1 f2 f3", "first-path amplitudes -> first-path dBm");
	leg(sh, "fpidx", "first-path index in the CIR, Q10.6");
	leg(sh, "acc", "preamble symbols accumulated (N in the power formula)");
	leg(sh, "peak", "Ipatov CIR peak (index | amplitude)");
	leg(sh, "len", "frame length, bytes (incl FCS for data frames)");
	leg(sh, "rng", "PHR ranging bit (0/1)");
	leg(sh, "dr", "data rate (6M8 | 850K)");
	leg(sh, "sts", "STS quality (off in SP0; > 0 = good in SP1/2/3)");
	leg(sh, "ststat", "STS status bits (CP error & co.)");
	leg(sh, "status", "raw SYS_STATUS low word");
	leg(sh, "pl", "payload bytes, hex (empty for SP3-ND / timeout)");
	return 0;
}

/** @brief `sniff raw_pretty` - one compact, colored line per frame. */
static int cmd_raw_pretty(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	capture_set_view(CAP_VIEW_RAW_PRETTY);
	tick(sh);
	shell_print(sh, "view raw_pretty - one compact colored line per frame (raw = machine)");
	return 0;
}

/** @brief `sniff burst [N]` - capture N frames to RAM at full speed, then dump. */
static int cmd_burst(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t n = (argc == 2) ? (uint32_t)strtoul(argv[1], NULL, 0) : 512u;
	int err = capture_burst(n);

	if (err == -EBUSY) {
		shell_error(sh, "burst already running");
	} else if (err) {
		shell_error(sh, "burst: invalid N (1..512)");
	} else {
		tick(sh);
		shell_print(sh, "burst - capturing %u frames to RAM, then dumping...", n);
	}
	return err;
}

/** @brief `sniff scan` - find the live preamble code (Apple negotiates it). */
static int cmd_scan(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	int err = capture_scan();

	if (err == -EBUSY) {
		shell_error(sh, "scan/burst already running");
	} else if (err) {
		shell_error(sh, "scan failed: %d", err);
	} else {
		tick(sh);
		shell_print(sh, "scanning preamble codes 9..12, locking onto the live one...");
	}
	return err;
}

/** @brief One hex nibble to its value, or -1 if not a hex digit. */
static int hexnib(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/** @brief Parse a 32-hex-char (16-byte) STS value into @p buf. @return 0 on success. */
static int parse_sts_hex(const char *s, uint8_t buf[16])
{
	if (strlen(s) != 32) {
		return -EINVAL;
	}
	for (int i = 0; i < 16; i++) {
		int hi = hexnib(s[2 * i]);
		int lo = hexnib(s[2 * i + 1]);

		if (hi < 0 || lo < 0) {
			return -EINVAL;
		}
		buf[i] = (uint8_t)((hi << 4) | lo);
	}
	return 0;
}

/** @brief `sniff stskey <32hex dURSK> | off` - load/clear the custom STS key. */
static int cmd_stskey(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	if (strcmp(argv[1], "off") == 0) {
		capture_clear_sts();
		tick(sh);
		shell_print(sh, "sts default");
		return 0;
	}

	uint8_t key[16];

	if (parse_sts_hex(argv[1], key) != 0) {
		shell_error(sh, "stskey: need 32 hex chars (16-byte dURSK) or 'off'");
		return -EINVAL;
	}
	int err = capture_set_sts_key(key);

	if (err) {
		shell_error(sh, "stskey: reconfigure failed: %d", err);
	} else {
		tick(sh);
		shell_print(sh, "stskey set");
	}
	return err;
}

/** @brief `sniff stsiv <32hex STS-V>` - load the custom STS IV / V value. */
static int cmd_stsiv(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	uint8_t iv[16];

	if (parse_sts_hex(argv[1], iv) != 0) {
		shell_error(sh, "stsiv: need 32 hex chars (16-byte STS-V)");
		return -EINVAL;
	}
	int err = capture_set_sts_iv(iv);

	if (err) {
		shell_error(sh, "stsiv: reconfigure failed: %d", err);
	} else {
		tick(sh);
		shell_print(sh, "stsiv set");
	}
	return err;
}

/** @brief `uwb stats [clear]` - per-event counts, or zero them. */
static int cmd_stats(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "clear") == 0) {
		capture_clear_stats();
		tick(sh);
		shell_print(sh, "stats cleared");
		return 0;
	}

	const struct capture_stats *s = capture_get_stats();

	shell_print(sh, "  %-9s %8s", "event", "count");
	shell_print(sh, "  ------------------");
	stat_row(sh, "OK",      s->per_event[CAP_EV_OK],      SHELL_VT100_COLOR_GREEN);
	stat_row(sh, "STS_ERR", s->per_event[CAP_EV_STS_ERR], SHELL_VT100_COLOR_YELLOW);
	stat_row(sh, "FCS_ERR", s->per_event[CAP_EV_FCS_ERR], SHELL_VT100_COLOR_RED);
	stat_row(sh, "PHY_ERR", s->per_event[CAP_EV_PHY_ERR], SHELL_VT100_COLOR_RED);
	stat_row(sh, "TO",      s->per_event[CAP_EV_TO],      SHELL_VT100_COLOR_DEFAULT);
	shell_print(sh, "  ------------------");
	stat_row(sh, "total",   s->total,                     SHELL_VT100_COLOR_CYAN);
	return 0;
}

/** @brief `sniff` (bare) and `sniff help` - the grouped, colored reference. */
static int cmd_sniff_help(const struct shell *sh, size_t argc, char **argv)
{
	/* Reached bare (`sniff`), via `sniff help`, or with an unrecognised
	 * subcommand (`sniff foo`) - in that last case argc>1: name the bad word
	 * in red, then fall through to the full reference. */
	if (argc > 1) {
		shell_fprintf(sh, SHELL_VT100_COLOR_RED,
			      "\n  unknown command: %s\n", argv[1]);
	}

	shell_fprintf(sh, SHELL_VT100_COLOR_CYAN, "\n  sniff");
	shell_print(sh, "   passive 802.15.4z HRP-UWB frame capture");

	sec(sh, "CAPTURE");
	row(sh, "start", "arm continuous receive");
	row(sh, "stop", "force the radio off");
	row(sh, "burst [N]", "grab N frames to RAM, then dump (default 512)");
	row(sh, "scan", "sweep preamble codes 9-12, lock the live one");

	sec(sh, "RADIO");
	row(sh, "chan <5|9>", "UWB channel");
	row(sh, "preamble <code>", "preamble code (9-12)");
	row(sh, "plen <64|128|256|512>", "preamble length, symbols");
	row(sh, "sfd <0-3>", "SFD:  0 IEEE-4a  1 DW-8  2 DW-16  3 IEEE-4z");
	row(sh, "sp <0-3>", "STS:  0 off  1 SP1  2 SP2  3 SP3-ND");
	row(sh, "ccc", "one-shot Apple/CCC ranging preset");

	sec(sh, "DECODE");
	row(sh, "stskey <hex|off>", "load 128-bit STS key (dURSK)");
	row(sh, "stsiv <hex>", "load STS IV (STS-V)");

	sec(sh, "OUTPUT");
	row(sh, "summary", "1 Hz gauge: signal bar + round rate");
	row(sh, "human", "one plain-English line per frame");
	row(sh, "raw_pretty", "one compact colored line per frame");
	row(sh, "raw", "per-frame UWBCAP machine lines (parser/pcap)");
	row(sh, "minpeak <hex>", "log only frames with peak >= value");
	row(sh, "stats [clear]", "RX event counters");
	row(sh, "help", "show this screen");

	shell_fprintf(sh, SHELL_VT100_COLOR_GREEN,
		      "\n  key card:  sniff ccc  ->  tap the reader  ->  sniff stats\n\n");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	uwb_sub,
	SHELL_CMD_ARG(start, NULL, "Arm continuous RX", cmd_start, 1, 0),
	SHELL_CMD_ARG(stop, NULL, "Stop RX", cmd_stop, 1, 0),
	SHELL_CMD_ARG(chan, NULL, "Set channel <5|9>", cmd_chan, 2, 0),
	SHELL_CMD_ARG(preamble, NULL, "Set preamble code <n>", cmd_preamble, 2, 0),
	SHELL_CMD_ARG(plen, NULL, "Set preamble length <64|128|256|512>", cmd_plen, 2, 0),
	SHELL_CMD_ARG(sfd, NULL, "Set SFD type <0..3>", cmd_sfd, 2, 0),
	SHELL_CMD_ARG(sp, NULL, "Set STS packet cfg <0..3>", cmd_sp, 2, 0),
	SHELL_CMD_ARG(ccc, NULL, "Retune to Aliro/CCC ranging PHY", cmd_ccc, 1, 0),
	SHELL_CMD_ARG(minpeak, NULL, "Only log peak >= <hex> (0=all)", cmd_minpeak, 2, 0),
	SHELL_CMD_ARG(summary, NULL, "1 Hz signal/round gauge", cmd_summary, 1, 0),
	SHELL_CMD_ARG(human, NULL, "Per-frame plain-English view", cmd_human, 1, 0),
	SHELL_CMD_ARG(raw_pretty, NULL, "Per-frame compact colored view", cmd_raw_pretty, 1, 0),
	SHELL_CMD_ARG(raw, NULL, "Per-frame UWBCAP machine lines", cmd_raw, 1, 0),
	SHELL_CMD_ARG(burst, NULL, "Capture N frames to RAM then dump [N]", cmd_burst, 1, 1),
	SHELL_CMD_ARG(scan, NULL, "Find the live preamble code (9..12)", cmd_scan, 1, 0),
	SHELL_CMD_ARG(stskey, NULL, "Load STS key <32hex dURSK> | off", cmd_stskey, 2, 0),
	SHELL_CMD_ARG(stsiv, NULL, "Load STS IV <32hex STS-V>", cmd_stsiv, 2, 0),
	SHELL_CMD_ARG(stats, NULL, "Show/clear event counts [clear]", cmd_stats, 1, 1),
	SHELL_CMD_ARG(help, NULL, "Show the sniff command reference", cmd_sniff_help, 1, 0),
	SHELL_SUBCMD_SET_END);

/* Bare `sniff` runs cmd_sniff_help (the pretty reference) instead of Zephyr's
 * default subcommand dump; `sniff <cmd>` still dispatches to the subcommands. */
SHELL_CMD_REGISTER(sniff, &uwb_sub,
		   "passive 802.15.4z HRP-UWB capture (type 'sniff' for help)",
		   cmd_sniff_help);
