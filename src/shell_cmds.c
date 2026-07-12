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

/** @brief `uwb start` — arm continuous RX. */
static int cmd_start(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	int err = capture_start();

	if (err) {
		shell_print(sh, "start failed: %d", err);
	} else {
		shell_print(sh, "capturing");
	}
	return err;
}

/** @brief `uwb stop` — force radio off. */
static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	capture_stop();
	shell_print(sh, "stopped");
	return 0;
}

/** @brief `uwb chan <5|9>`. */
static int cmd_chan(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	int err = capture_set_channel((uint8_t)atoi(argv[1]));

	shell_print(sh, err ? "invalid channel (5 or 9)" : "chan=%s", argv[1]);
	return err;
}

/** @brief `uwb preamble <code>` — TX+RX preamble code. */
static int cmd_preamble(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	int err = capture_set_preamble_code((uint8_t)atoi(argv[1]));

	if (err) {
		shell_print(sh, "reconfigure failed: %d", err);
	} else {
		shell_print(sh, "code=%s", argv[1]);
	}
	return err;
}

/** @brief `uwb plen <symbols>` — preamble length (64/128/256/512). */
static int cmd_plen(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	int err = capture_set_preamble_len((uint16_t)atoi(argv[1]));

	shell_print(sh, err ? "invalid plen (64/128/256/512)" : "plen=%s", argv[1]);
	return err;
}

/** @brief `uwb sfd <0..3>` — dwt_sfd_type_e (3 = IEEE 802.15.4z BPRF). */
static int cmd_sfd(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	int err = capture_set_sfd((uint8_t)atoi(argv[1]));

	shell_print(sh, err ? "invalid sfd (0..3)" : "sfd=%s", argv[1]);
	return err;
}

/** @brief `uwb sp <0..3>` — STS packet config (0=SP0 off, 3=SP3 no-data). */
static int cmd_sp(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	int err = capture_set_sp((uint8_t)atoi(argv[1]));

	shell_print(sh, err ? "invalid sp (0..3)" : "sp=%s", argv[1]);
	return err;
}

/** @brief `sniff ccc` — retune to the Aliro/CCC ranging PHY (Config 0000). */
static int cmd_ccc(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	int err = capture_set_ccc();

	shell_print(sh, err ? "ccc: reconfigure failed: %d" :
		    "ccc: chan9 code9 plen64 sfd4a 6M8 SP0 — listening for Apple Pre-POLL",
		    err);
	return err;
}

/** @brief `sniff minpeak <hex>` — only log frames with peak >= value (0 = all). */
static int cmd_minpeak(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	uint32_t v = (uint32_t)strtoul(argv[1], NULL, 0); /* accepts 0x-prefixed hex */

	capture_set_min_peak(v);
	shell_print(sh, "minpeak=0x%08X", v);
	return 0;
}

/** @brief `sniff human` — readable one-line-per-second summary view. */
static int cmd_human(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	capture_set_view_human(true);
	shell_print(sh, "view=human (1/s summary; 'sniff raw' for machine lines)");
	return 0;
}

/** @brief `sniff raw` — per-frame `UWBCAP key=value` machine lines (for parsing). */
static int cmd_raw(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	capture_set_view_human(false);
	shell_print(sh, "view=raw (per-frame UWBCAP lines)");
	return 0;
}

/** @brief `sniff burst [N]` — capture N frames to RAM at full speed, then dump. */
static int cmd_burst(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t n = (argc == 2) ? (uint32_t)strtoul(argv[1], NULL, 0) : 512u;
	int err = capture_burst(n);

	if (err == -EBUSY) {
		shell_print(sh, "burst already running");
	} else if (err) {
		shell_print(sh, "burst: invalid N (1..512)");
	} else {
		shell_print(sh, "burst: capturing %u frames to RAM, then dumping...", n);
	}
	return err;
}

/** @brief `sniff scan` — find the live preamble code (Apple negotiates it). */
static int cmd_scan(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	int err = capture_scan();

	if (err == -EBUSY) {
		shell_print(sh, "scan/burst already running");
	} else if (err) {
		shell_print(sh, "scan failed: %d", err);
	} else {
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

/** @brief `sniff stskey <32hex dURSK> | off` — load/clear the custom STS key. */
static int cmd_stskey(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	if (strcmp(argv[1], "off") == 0) {
		capture_clear_sts();
		shell_print(sh, "sts=default");
		return 0;
	}

	uint8_t key[16];

	if (parse_sts_hex(argv[1], key) != 0) {
		shell_print(sh, "stskey: need 32 hex chars (16-byte dURSK) or 'off'");
		return -EINVAL;
	}
	int err = capture_set_sts_key(key);

	if (err) {
		shell_print(sh, "stskey: reconfigure failed: %d", err);
	} else {
		shell_print(sh, "stskey set");
	}
	return err;
}

/** @brief `sniff stsiv <32hex STS-V>` — load the custom STS IV / V value. */
static int cmd_stsiv(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	uint8_t iv[16];

	if (parse_sts_hex(argv[1], iv) != 0) {
		shell_print(sh, "stsiv: need 32 hex chars (16-byte STS-V)");
		return -EINVAL;
	}
	int err = capture_set_sts_iv(iv);

	if (err) {
		shell_print(sh, "stsiv: reconfigure failed: %d", err);
	} else {
		shell_print(sh, "stsiv set");
	}
	return err;
}

/** @brief `uwb stats [clear]` — per-event counts, or zero them. */
static int cmd_stats(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "clear") == 0) {
		capture_clear_stats();
		shell_print(sh, "stats cleared");
		return 0;
	}

	const struct capture_stats *s = capture_get_stats();

	shell_print(sh, "OK=%u STS_ERR=%u FCS_ERR=%u PHY_ERR=%u TO=%u total=%u",
		    s->per_event[CAP_EV_OK], s->per_event[CAP_EV_STS_ERR],
		    s->per_event[CAP_EV_FCS_ERR], s->per_event[CAP_EV_PHY_ERR],
		    s->per_event[CAP_EV_TO], s->total);
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
	SHELL_CMD_ARG(human, NULL, "Readable 1/s summary view", cmd_human, 1, 0),
	SHELL_CMD_ARG(raw, NULL, "Per-frame UWBCAP machine lines", cmd_raw, 1, 0),
	SHELL_CMD_ARG(burst, NULL, "Capture N frames to RAM then dump [N]", cmd_burst, 1, 1),
	SHELL_CMD_ARG(scan, NULL, "Find the live preamble code (9..12)", cmd_scan, 1, 0),
	SHELL_CMD_ARG(stskey, NULL, "Load STS key <32hex dURSK> | off", cmd_stskey, 2, 0),
	SHELL_CMD_ARG(stsiv, NULL, "Load STS IV <32hex STS-V>", cmd_stsiv, 2, 0),
	SHELL_CMD_ARG(stats, NULL, "Show/clear event counts [clear]", cmd_stats, 1, 1),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(sniff, &uwb_sub, "Passive UWB frame capture", NULL);
