/*
 * @file capture.c
 * @brief Passive HRP-UWB frame-capture engine — DW3110 radio layer.
 *
 * Continuous, error-tolerant receive: RX is armed with rxtimeout 0 and
 * DWT_START_RX_IMMEDIATE, and the DW3000 rxok / rxto / rxerr callbacks each
 * emit one structured log line and immediately re-arm RX, so STS/FCS/PHY faults
 * never stall the loop — a faulted frame is still captured with whatever timing
 * and diagnostics the CIA produced before the fault.
 *
 * Threading: the DW3000 driver's platform HAL (dw3000_hw.c) submits dwt_isr() —
 * and therefore these callbacks — to the system workqueue via k_work_submit(),
 * not hard-IRQ context.  That makes SPI reads (dwt_readrxdata,
 * dwt_readdiagnostics, ...) legal here, and serialises all callbacks onto one
 * thread, so the file-scope scratch buffers below need no locking.  The whole
 * dwt_isr()->emit() chain (SPI + snprintf) runs on sysworkq, so its stack is
 * sized up via CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE in prj.conf.
 *
 * Register/field names cited in comments (STS, RX diagnostics, timestamps) are
 * from the DW3000 User Manual.
 */

#include "capture.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

/* Qorvo dwt_uwb_driver + platform glue (unprefixed: the module's CMakeLists
 * adds dwt_uwb_driver/ and platform/ to the include path). */
#include <deca_device_api.h>
#include <deca_probe_interface.h>
#include <dw3000_hw.h>

LOG_MODULE_REGISTER(uwbcap, LOG_LEVEL_INF);

/** @brief Longest payload we pull off-chip per frame (max 127-B standard HRP frame). */
#define CAP_MAX_PAYLOAD 127

/**
 * @brief Expected DEV_ID (register 0x00, RIDTAG:MODEL:VER:REV) for the DW3110
 *        on the DWM3001CDK.  Reading this correctly over SPI is the cheap
 *        bring-up gate: it confirms the SPI + RESET pinmap is right before we
 *        trust anything else.  (A wrong IRQ pin still reads DEV_ID fine — that
 *        one is only proven once RX callbacks actually fire.)
 */
#define CAP_DW3110_DEVID 0xDECA0302UL

/* ── Module state (single-consumer: all mutation is on the capture path or the
 *    shell thread while RX is stopped). ─────────────────────────────────────── */

static bool g_initialised;   /**< capture_init() completed. */
static bool g_running;       /**< RX currently armed. */

static uint32_t g_index;     /**< Monotonic captured-event counter. */
static struct capture_stats g_stats;

/**
 * @brief Minimum Ipatov CIR peak (IP_DIAG_1) to *log* a frame; 0 = log all.
 *
 * A strong transmitter close to the sniffer floods the RX loop with real frames
 * plus a swarm of weak noise/partial detections.  Under CONFIG_LOG_MODE_IMMEDIATE
 * that can drown the console.  Frames whose peak is below this are still counted
 * and re-armed — just not printed — so a threshold isolates the strong frames
 * without dropping any from the tally.
 */
static uint32_t g_min_peak;

/**
 * @brief Console output view.
 *
 * true  = one human-readable summary per second (per-frame lines suppressed) —
 *         readable in a serial terminal without the DW3000 manual.
 * false = the per-frame `UWBCAP key=value` machine lines (for the host parser).
 * Default human so the console is legible out of the box; `sniff raw` restores
 * the machine format for capture/analysis.
 */
#define CAP_DEFAULT_VIEW_HUMAN true
static bool g_view_human = CAP_DEFAULT_VIEW_HUMAN;

/** @brief Strongest first-path magnitude (ipatovF1) seen in the current 1 s window. */
static uint32_t g_win_sig;

/** @brief Ranging-round cadence: a real frame after a big gap starts a new
 *  round.  Counting them lets the live view show the round rate on-device, so
 *  the common "what's the rhythm" question needs no host-side analysis. */
static uint32_t g_round_count;
static uint32_t g_last_real_ms;

/* ── Burst capture ─────────────────────────────────────────────────────────
 * Stash real frames' key fields in RAM at full radio speed (no per-frame UART),
 * then dump them all at once for offline round analysis.  Decouples capture
 * from the slow console so a ranging round's back-to-back frames aren't dropped
 * by the synchronous print path. */
#define CAP_BURST_MAX 512
struct burst_rec {
	uint64_t ts;         /**< 40-bit Ipatov RX timestamp. */
	int32_t  cfo;        /**< CIA clock offset (signed raw). */
	uint32_t f1, f2, f3; /**< first-path magnitudes. */
	uint16_t acc;        /**< accumulated preamble symbols. */
	uint8_t  ev;         /**< @ref capture_event. */
};
static struct burst_rec g_burst[CAP_BURST_MAX];
static uint32_t g_burst_target; /**< frames to capture this burst. */
static uint32_t g_burst_count;  /**< frames captured so far. */
static bool g_burst_active;     /**< capturing into g_burst. */
static bool g_burst_ready;      /**< buffer full → summary thread dumps it. */

/* ── Preamble-code scan ────────────────────────────────────────────────────
 * Apple Nearby-Interaction sessions negotiate the preamble code, so the live
 * code isn't known ahead of time.  Sweep CAP_SCAN_FIRST..CAP_SCAN_LAST (1 s
 * each, stepped by the summary thread), tally real frames per code, then lock
 * onto the busiest. */
#define CAP_SCAN_FIRST 9
#define CAP_SCAN_LAST  12
static bool g_scan_active;
static uint8_t g_scan_code;       /**< code currently under test. */
static uint8_t g_scan_best_code;  /**< busiest code so far. */
static uint8_t g_scan_saved_code; /**< restored if the sweep finds nothing. */
static uint32_t g_scan_base;      /**< real-frame count at the current dwell's start. */
static uint32_t g_scan_best_hits; /**< frames at the busiest code. */

/** @brief Off-chip scratch, reused per event (see threading note above). */
static uint8_t g_payload[CAP_MAX_PAYLOAD];
static char g_hex[CAP_MAX_PAYLOAD * 2 + 1];
static char g_line[512];

/**
 * @brief Live PHY configuration.
 *
 * Defaults track the Aliro/CCC ranging PHY an Apple Wallet key negotiates —
 * CCC v4 Table 21-1 Config 0000, the one set shared by every frame in a round:
 * Channel 9, BPRF / PRF 64 MHz, 64-symbol preamble (PAC8), preamble code 9
 * (the phone's SYNC_Code_Index, carried in the plaintext M4 `07 01 09`),
 * 6.8 Mbps, and the IEEE-8 ternary SFD (dwt_sfd_type_e DWT_SFD_IEEE_4A — the
 * 4z SFD used by generic FiRa SFD-times-out on every Apple frame).
 *
 * STS mode defaults to SP0 (STS off) so the Pre-POLL — the cleartext data frame
 * that leads each ranging round and carries poll_sts_index — is heard with no
 * key.  The keyed POLL/RESP/FINAL are SP3-ND: load the session dURSK/STS-V
 * (`sniff stskey`/`stsiv`) and switch to `sniff sp 3` to decode those.
 * `sniff ccc` restores this whole set; sfdTO is recomputed from the preamble
 * length on every apply.
 */
static dwt_config_t g_cfg = {
	.chan           = 9,
	.txPreambLength = DWT_PLEN_64,
	.rxPAC          = DWT_PAC8,
	.txCode         = 9,
	.rxCode         = 9,
	.sfdType        = DWT_SFD_IEEE_4A,
	.dataRate       = DWT_BR_6M8,
	.phrMode        = DWT_PHRMODE_STD,
	.phrRate        = DWT_PHRRATE_STD,
	.sfdTO          = (64 + 1 + 8 - 8),
	.stsMode        = DWT_STS_MODE_OFF,
	.stsLength      = DWT_STS_LEN_64,
	.pdoaMode       = DWT_PDOA_M0,
};

/* ── Custom STS key/IV (to decode a keyed session, e.g. CCC/Aliro) ───────────
 * The DW3000 default STS key never matches a real secure-ranging session, so
 * those frames always fault as STS_ERR.  When the session's STS_KEY (dURSK) and
 * STS_V are known — derived host-side from the URSK — loading them lets the STS
 * correlate so a matching frame decodes as OK.  Both are canonical big-endian
 * 16-byte arrays (byte 0 = MSB), exactly as the CCC deriver prints dURSK / STS-V.
 * dwt_configure() resets the STS registers, so apply_sts() re-loads after every
 * apply_config(). */
static bool g_sts_custom;
static uint8_t g_sts_key[16]; /* STS_KEY (dURSK), canonical big-endian. */
static uint8_t g_sts_iv[16];  /* STS_V / IV,      canonical big-endian. */

/* SYS_STATUS (low half) event bits we react to.  Enabled in SYS_ENABLE via
 * dwt_setinterrupt() so the IRQ line actually asserts on each. */
#define CAP_RX_INT_MASK                                                     \
	(DWT_INT_RXFCG_BIT_MASK | DWT_INT_RXFR_BIT_MASK |                   \
	 DWT_INT_RXFCE_BIT_MASK | DWT_INT_RXPHE_BIT_MASK |                  \
	 DWT_INT_RXFSL_BIT_MASK | DWT_INT_RXFTO_BIT_MASK |                  \
	 DWT_INT_RXPTO_BIT_MASK | DWT_INT_RXSTO_BIT_MASK |                  \
	 DWT_INT_CIAERR_BIT_MASK | DWT_INT_CPERR_BIT_MASK |                 \
	 DWT_INT_ARFE_BIT_MASK)

/** @brief Human-readable tag per @ref capture_event (index-aligned). */
static const char *const g_ev_name[CAP_EV_COUNT] = {
	"OK", "STS_ERR", "FCS_ERR", "PHY_ERR", "TO",
};

/**
 * @brief Classify a SYS_STATUS word into a @ref capture_event.
 *
 * Error bits take priority over RXFCG/RXFR so an otherwise-decoded frame that
 * failed STS (CIAERR) or CRC (RXFCE) is reported as the fault, not as OK.
 */
static enum capture_event classify(uint32_t status)
{
	/* STS quality failure: CIA error OR CP error (CPERR = STS quality
	 * warning/error, SYS_STATUS bit 0x10000000).  An SP3 frame received with
	 * a non-matching STS sets CPERR, not CIAERR — checking only CIAERR (the
	 * old behaviour) missed it and mislabelled the frame FCS_ERR. */
	if (status & (DWT_INT_CIAERR_BIT_MASK | DWT_INT_CPERR_BIT_MASK)) {
		return CAP_EV_STS_ERR;
	}
	/* RXFCE = frame CRC error.  In SP3 (STS-no-data) there is no PHR / PSDU /
	 * FCS, so the RXFCE the chip raises on an unresolved STS-only frame is
	 * spurious — ignore it there.  SP0/SP1/SP2 carry a real FCS, so it stands. */
	if ((status & DWT_INT_RXFCE_BIT_MASK) && g_cfg.stsMode != DWT_STS_MODE_ND) {
		return CAP_EV_FCS_ERR;
	}
	if (status & (DWT_INT_RXPHE_BIT_MASK | DWT_INT_RXFSL_BIT_MASK)) {
		return CAP_EV_PHY_ERR;
	}
	if (status & (DWT_INT_RXFTO_BIT_MASK | DWT_INT_RXPTO_BIT_MASK |
		      DWT_INT_RXSTO_BIT_MASK | DWT_INT_ARFE_BIT_MASK)) {
		return CAP_EV_TO;
	}
	return CAP_EV_OK; /* RXFCG and/or RXFR with no error bits. */
}

/** @brief Hex-encode @p n bytes of @p in into @p out (needs 2*n+1 bytes). */
static void to_hex(const uint8_t *in, size_t n, char *out)
{
	static const char lut[] = "0123456789ABCDEF";

	for (size_t i = 0; i < n; i++) {
		out[2 * i]     = lut[in[i] >> 4];
		out[2 * i + 1] = lut[in[i] & 0x0F];
	}
	out[2 * n] = '\0';
}

static int rearm_rx(void);

/**
 * @brief Build and emit the one-line record for one RX event, then re-arm RX.
 *
 * Fields (space-separated `key=value`, all hex/decimal, stable and greppable):
 *   idx    monotonic event index
 *   ev     OK | STS_ERR | FCS_ERR | PHY_ERR | TO
 *   ts     40-bit Ipatov RX timestamp (RX_TIME / dwt_readrxtimestamp_ipatov);
 *          survives STS failure because it comes from the preamble, not the STS
 *   cfo    carrier/clock frequency offset (CIA_TDIAG clock offset, signed raw)
 *   cir    Ipatov CIR power estimate      (IP_DIAG_12 / rxdiag.ipatovPower)
 *   f1..f3 first-path magnitudes          (IP_DIAG_2/3/4 / rxdiag.ipatovF1..F3)
 *   fpidx  first-path index, Q10.6        (IP_DIAG_8 / rxdiag.ipatovFpIndex)
 *   acc    accumulated preamble symbols   (IP_DIAG_12 / rxdiag.ipatovAccumCount)
 *   peak   Ipatov CIR peak (idx|amp)      (IP_DIAG_1 / rxdiag.ipatovPeak)
 *   len    PHR frame length (bytes, incl FCS for data frames)
 *   rng    PHR ranging bit (cb rx_flags RNG)
 *   dr     configured data rate
 *   sts    STS quality index (dwt_readstsquality; <0 => STS bad), or 'off'
 *   ststat STS status word    (STS_STS register / dwt_readstsstatus), CP error bits
 *   status raw SYS_STATUS low word
 *   pl     payload bytes, hex (empty for STS-no-data / timeout)
 *
 * @p d is the driver callback data; its `status` is the entry-time SYS_STATUS.
 */
static void emit(const dwt_cb_data_t *d)
{
	const enum capture_event ev = classify(d->status);

	g_stats.per_event[ev]++;
	g_stats.total++;

	/* Preamble-code scan: only tally (done above) and re-arm — no register
	 * reads, no printing — while the summary thread steps codes and picks the
	 * live one. */
	if (g_scan_active) {
		g_index++;
		(void)rearm_rx();
		return;
	}

	uint64_t ts = 0;
	int32_t cfo = 0;
	dwt_rxdiag_t diag = {0};
	int sts_qi = 0;
	uint16_t sts_stat = 0;
	uint16_t len = d->datalength;
	const int rng = (d->rx_flags & DWT_CB_DATA_RX_FLAG_RNG) ? 1 : 0;
	/* CIA-done flag: the DW3000 only fully refreshes the CIR diagnostic
	 * registers (cir/f1/f2/f3/fpidx/acc/peak) when the CIA completes — i.e.
	 * on a good frame.  On an errored frame CIA aborts and those registers
	 * keep stale values, so cia=0 marks the diagnostics as untrustworthy
	 * (the timestamp and clock offset are still re-latched every frame). */
	const int cia = (d->rx_flags & DWT_CB_DATA_RX_FLAG_CIA) ? 1 : 0;
	const char *dr = (g_cfg.dataRate == DWT_BR_6M8) ? "6M8" : "850K";

	/* Timing/diagnostics only exist once a frame (or at least its preamble +
	 * CIA) was seen — i.e. everything except a bare timeout. */
	if (ev != CAP_EV_TO) {
		uint8_t ts5[5] = {0};

		dwt_readrxtimestamp_ipatov(ts5); /* RX_TIME, 40-bit, LSB first */
		for (int i = 0; i < 5; i++) {
			ts |= (uint64_t)ts5[i] << (8 * i);
		}
		cfo = dwt_readclockoffset();     /* CIA clock offset, signed raw */
		dwt_readdiagnostics(&diag);
	}

	/* Track the strongest first-path magnitude this second for the human
	 * summary's signal bar (cheap integer; updated regardless of view). */
	if (ev != CAP_EV_TO && diag.ipatovF1 > g_win_sig) {
		g_win_sig = diag.ipatovF1;
	}

	/* Round cadence: a real frame preceded by a >50 ms gap starts a new
	 * ranging round (intra-round frames arrive ~ms apart, so they don't count
	 * as new rounds).  The live view reads g_round_count for the round rate. */
	if (ev != CAP_EV_TO) {
		uint32_t now = k_uptime_get_32();

		if (now - g_last_real_ms > 50u) {
			g_round_count++;
		}
		g_last_real_ms = now;
	}

	/* Burst capture takes priority: stash the frame's key fields and re-arm in
	 * microseconds — no printing — so a ranging round's back-to-back frames are
	 * not dropped by the console.  Real frames only (a timeout carries no ts). */
	if (g_burst_active) {
		if (ev != CAP_EV_TO && g_burst_count < g_burst_target) {
			struct burst_rec *r = &g_burst[g_burst_count++];

			r->ts  = ts;
			r->cfo = cfo;
			r->f1  = diag.ipatovF1;
			r->f2  = diag.ipatovF2;
			r->f3  = diag.ipatovF3;
			r->acc = (uint16_t)diag.ipatovAccumCount;
			r->ev  = (uint8_t)ev;
		}
		g_index++;
		if (g_burst_count >= g_burst_target) {
			g_burst_active = false;
			g_burst_ready  = true;
			dwt_forcetrxoff(); /* stop RX; the summary thread dumps + resumes */
			return;
		}
		(void)rearm_rx();
		return;
	}

	/* Human view: suppress the per-frame machine line — the 1 Hz summary
	 * thread renders a readable digest instead.  Frame is still counted
	 * (above) and RX re-armed (below). */
	if (g_view_human) {
		g_index++;
		(void)rearm_rx();
		return;
	}

	/* Peak/strength gate: below-threshold frames are counted (above) and
	 * re-armed (below), just not logged — so a strong-transmitter burst
	 * doesn't drown the console.  Applies only to frames that produced a
	 * peak (non-timeout); TO events fall through and still print. */
	if (g_min_peak && ev != CAP_EV_TO && diag.ipatovPeak < g_min_peak) {
		g_index++;
		(void)rearm_rx();
		return;
	}

	/* STS quality is only meaningful when an STS segment was configured. */
	int16_t qi = 0;

	if (g_cfg.stsMode != DWT_STS_MODE_OFF && ev != CAP_EV_TO) {
		/* dwt_readstsquality: return >0 good, <=0 bad; qi = raw STS accumulator
		 * quality (preambleCount) regardless of the verdict.  Log the RAW qi
		 * (not a -1 mask) so a failing STS is diagnosable: qi≈0 => no correlation
		 * (no matching STS on air); qi just under the ~0.6·len threshold =>
		 * partial correlation (timing/counter alignment).  The ev field
		 * (classify) still carries the pass/fail verdict. */
		(void)dwt_readstsquality(&qi, 0);

		sts_qi = qi;
		(void)dwt_readstsstatus(&sts_stat, 0); /* STS_STS: CP_ERR & co. */
	}

	/* Payload: only data-bearing frames carry bytes; SP3 (no data) and
	 * timeouts leave it empty.  Cap the off-chip read at the buffer size. */
	uint16_t pl_n = 0;

	if (ev != CAP_EV_TO && len > 0) {
		pl_n = (len > CAP_MAX_PAYLOAD) ? CAP_MAX_PAYLOAD : len;
		dwt_readrxdata(g_payload, pl_n, 0); /* RX_BUFFER_0 */
	}
	to_hex(g_payload, pl_n, g_hex);

	if (g_cfg.stsMode == DWT_STS_MODE_OFF) {
		snprintf(g_line, sizeof(g_line),
			 "UWBCAP idx=%06u ev=%s ts=%010llX cfo=%d cia=%d "
			 "cir=%08X f1=%08X f2=%08X f3=%08X fpidx=%04X acc=%04X peak=%08X "
			 "len=%u rng=%d dr=%s sts=off status=%08X pl=%s",
			 g_index, g_ev_name[ev], (unsigned long long)ts, cfo, cia,
			 diag.ipatovPower, diag.ipatovF1, diag.ipatovF2, diag.ipatovF3,
			 diag.ipatovFpIndex, diag.ipatovAccumCount, diag.ipatovPeak,
			 len, rng, dr, d->status, g_hex);
	} else {
		snprintf(g_line, sizeof(g_line),
			 "UWBCAP idx=%06u ev=%s ts=%010llX cfo=%d cia=%d "
			 "cir=%08X f1=%08X f2=%08X f3=%08X fpidx=%04X acc=%04X peak=%08X "
			 "len=%u rng=%d dr=%s sts=%d ststat=%04X status=%08X pl=%s",
			 g_index, g_ev_name[ev], (unsigned long long)ts, cfo, cia,
			 diag.ipatovPower, diag.ipatovF1, diag.ipatovF2, diag.ipatovF3,
			 diag.ipatovFpIndex, diag.ipatovAccumCount, diag.ipatovPeak,
			 len, rng, dr, sts_qi, sts_stat, d->status, g_hex);
	}

	g_index++;
	LOG_INF("%s", g_line);

	/* Re-arm immediately so no frame in a back-to-back burst is missed. */
	(void)rearm_rx();
}

/**
 * @brief Re-arm continuous RX.
 * @return dwt_rxenable() status.
 */
static int rearm_rx(void)
{
	return dwt_rxenable(DWT_START_RX_IMMEDIATE);
}

/** @brief rxok callback — good frame (and good STS in SP1/2/3). */
static void cb_rx_ok(const dwt_cb_data_t *d) { emit(d); }
/** @brief rxto callback — preamble/frame/SFD timeout. */
static void cb_rx_to(const dwt_cb_data_t *d) { emit(d); }
/** @brief rxerr callback — STS/CRC/PHY fault; frame still captured. */
static void cb_rx_err(const dwt_cb_data_t *d) { emit(d); }

/** @brief Preamble length in SYMBOLS from a DWT_PLEN_* register code. */
static uint16_t plen_symbols(uint16_t txPreambLength)
{
	switch (txPreambLength) {
	case DWT_PLEN_64:   return 64;
	case DWT_PLEN_128:  return 128;
	case DWT_PLEN_256:  return 256;
	case DWT_PLEN_512:  return 512;
	case DWT_PLEN_1024: return 1024;
	default:            return 64;
	}
}

/** @brief PAC size in SYMBOLS from a DWT_PAC* code (only 8/16 are used here). */
static uint16_t pac_symbols(uint8_t rxPAC)
{
	return (rxPAC == DWT_PAC16) ? 16 : 8;
}

/**
 * @brief Program the custom STS key/IV into the DW3000.
 *
 * Must run after dwt_configure() (which resets the STS registers).  Byte order
 * matches Qorvo's own loader (fira_sts): the 128-bit KEY is written by reversing
 * the whole 16-byte big-endian array, then packing four little-endian words
 * (STS_KEY0[7:0] holds the integer's *least* significant byte); the IV/V is
 * packed as four little-endian words with NO whole-array reversal.
 * dwt_configurestsloadiv() latches the IV into the STS counter for the next RX.
 */
static void apply_sts(void)
{
	if (!g_sts_custom) {
		return;
	}

	uint8_t rev[16];

	for (int i = 0; i < 16; i++) {
		rev[i] = g_sts_key[15 - i];
	}
	dwt_sts_cp_key_t key = {
		.key0 = sys_get_le32(&rev[0]),
		.key1 = sys_get_le32(&rev[4]),
		.key2 = sys_get_le32(&rev[8]),
		.key3 = sys_get_le32(&rev[12]),
	};
	dwt_configurestskey(&key);

	dwt_sts_cp_iv_t iv = {
		.iv0 = sys_get_le32(&g_sts_iv[0]),
		.iv1 = sys_get_le32(&g_sts_iv[4]),
		.iv2 = sys_get_le32(&g_sts_iv[8]),
		.iv3 = sys_get_le32(&g_sts_iv[12]),
	};
	dwt_configurestsiv(&iv);
	dwt_configurestsloadiv();
}

/**
 * @brief Re-apply @ref g_cfg to the radio (must be idle).
 */
static int apply_config(void)
{
	dwt_forcetrxoff();

	/* sfdTO = preamble length + 1 + SFD length - PAC (Qorvo formula), in
	 * preamble SYMBOLS.  txPreambLength / rxPAC are DWT_PLEN_* / DWT_PAC*
	 * REGISTER CODES (e.g. DWT_PLEN_64 == 0x07), not symbol counts — feeding the
	 * raw enum here gives sfdTO=8, which closes the SFD-detect window before the
	 * SFD arrives and turns every frame into a bogus RXSTO/TO.  Decode to symbols
	 * first.  The IEEE-4z BPRF SFD is 8 symbols. */
	g_cfg.sfdTO = plen_symbols(g_cfg.txPreambLength) + 1 + 8 - pac_symbols(g_cfg.rxPAC);

	if (dwt_configure(&g_cfg) != DWT_SUCCESS) {
		LOG_ERR("dwt_configure failed (chan=%u code=%u sp=%u)",
			g_cfg.chan, g_cfg.rxCode, g_cfg.stsMode);
		return -EIO;
	}

	/* Log the full CIA diagnostic register set.  Without this the RX
	 * diagnostic registers (CIR power, F1/F2/F3, FP index, accum count)
	 * read back as 0 — the RX timestamp still works, but RSSI/first-path
	 * would be blank.  dwt_configure() resets this, so re-apply it here. */
	dwt_configciadiag(DW_CIA_DIAG_LOG_ALL);

	/* dwt_configure() reset the STS registers to the driver default; re-load
	 * the custom key/IV if one was set (no-op otherwise). */
	apply_sts();

	return 0;
}

int capture_init(void)
{
	if (g_initialised) {
		return 0;
	}

	dw3000_hw_init();
	dw3000_hw_reset();
	k_msleep(5); /* DW3110 wake-after-reset latency (datasheet >= 2 ms). */

	if (dwt_probe((struct dwt_probe_s *)&dw3000_probe_interf) != DWT_SUCCESS) {
		LOG_ERR("dwt_probe failed — check SPI wiring (SCK/MOSI/MISO/CS) + RSTn");
		return -EIO;
	}

	/* Bring-up gate: confirm the DEV_ID before doing anything else.  This
	 * proves SPI + RESET are wired to the pins in the overlay. */
	uint32_t devid = dwt_readdevid();

	if (devid != CAP_DW3110_DEVID) {
		LOG_WRN("DEV_ID=0x%08X, expected 0x%08X — pinmap/silicon mismatch",
			(unsigned int)devid, (unsigned int)CAP_DW3110_DEVID);
	} else {
		LOG_INF("DEV_ID=0x%08X OK (DW3110) — SPI+RST pinmap confirmed",
			(unsigned int)devid);
	}

	if (dwt_initialise(DWT_DW_INIT) != DWT_SUCCESS) {
		LOG_ERR("dwt_initialise failed");
		return -EIO;
	}
	if (apply_config() != 0) {
		return -EIO;
	}

	/* Blink the DW3000 activity LEDs (D13 on the CDK) so RX is visible. */
	dwt_setleds(DWT_LEDS_ENABLE | DWT_LEDS_INIT_BLINK);

	/* Wire the DW_IRQ line into Zephyr GPIO -> dwt_isr(), then install our
	 * callbacks and unmask the RX events in SYS_ENABLE. */
	(void)dw3000_hw_init_interrupt();

	static dwt_callbacks_s cbs;

	cbs.cbRxOk  = cb_rx_ok;
	cbs.cbRxTo  = cb_rx_to;
	cbs.cbRxErr = cb_rx_err;
	dwt_setcallbacks(&cbs);
	dwt_setinterrupt(CAP_RX_INT_MASK, 0u, DWT_ENABLE_INT);

	g_initialised = true;
	LOG_INF("capture_init: DW3110 ready");
	return 0;
}

int capture_start(void)
{
	if (!g_initialised) {
		int err = capture_init();

		if (err) {
			return err;
		}
	}

	dwt_setrxtimeout(0); /* rxtimeout 0 => listen until a frame/error. */

	if (rearm_rx() != DWT_SUCCESS) {
		LOG_ERR("dwt_rxenable failed");
		return -EIO;
	}
	g_running = true;
	LOG_INF("capture: RX armed (continuous)");
	return 0;
}

int capture_stop(void)
{
	dwt_forcetrxoff();
	g_running = false;
	LOG_INF("capture: RX stopped");
	return 0;
}

bool capture_is_running(void) { return g_running; }

/** @brief Common tail for a live PHY-config setter: re-apply, re-arm if running. */
static int reconfigure(void)
{
	if (!g_initialised) {
		return 0; /* Change takes effect at capture_init(). */
	}

	int err = apply_config();

	if (err) {
		return err;
	}
	if (g_running) {
		dwt_setrxtimeout(0);
		(void)rearm_rx();
	}
	return 0;
}

int capture_set_channel(uint8_t chan)
{
	if (chan != 5 && chan != 9) {
		return -EINVAL;
	}
	g_cfg.chan = chan;
	return reconfigure();
}

int capture_set_preamble_code(uint8_t code)
{
	g_cfg.txCode = code;
	g_cfg.rxCode = code;
	return reconfigure();
}

int capture_set_preamble_len(uint16_t symbols)
{
	switch (symbols) {
	case 64:  g_cfg.txPreambLength = DWT_PLEN_64;  g_cfg.rxPAC = DWT_PAC8;  break;
	case 128: g_cfg.txPreambLength = DWT_PLEN_128; g_cfg.rxPAC = DWT_PAC8;  break;
	case 256: g_cfg.txPreambLength = DWT_PLEN_256; g_cfg.rxPAC = DWT_PAC16; break;
	case 512: g_cfg.txPreambLength = DWT_PLEN_512; g_cfg.rxPAC = DWT_PAC16; break;
	default:
		return -EINVAL;
	}
	return reconfigure();
}

int capture_set_sfd(uint8_t sfd_type)
{
	if (sfd_type > DWT_SFD_IEEE_4Z) {
		return -EINVAL;
	}
	g_cfg.sfdType = (dwt_sfd_type_e)sfd_type;
	return reconfigure();
}

int capture_set_sp(uint8_t sp)
{
	switch (sp) {
	case 0: g_cfg.stsMode = DWT_STS_MODE_OFF; break;
	case 1: g_cfg.stsMode = DWT_STS_MODE_1;   break;
	case 2: g_cfg.stsMode = DWT_STS_MODE_2;   break;
	case 3: g_cfg.stsMode = DWT_STS_MODE_ND;  break;
	default: return -EINVAL;
	}
	return reconfigure();
}
int capture_set_ccc(void)
{
	/* CCC v4 Table 21-1 Config 0000 — the PHY an Apple Wallet key uses for every
	 * frame in an Aliro/CCC ranging round.  One reconfigure; leaves any loaded
	 * STS key/IV untouched.  Preamble code 9 is the phone's usual SYNC_Code_Index
	 * (M4 `07 01 09`); if a session negotiates a different one, `sniff scan` or
	 * `sniff preamble <n>` retunes without disturbing the rest of this set. */
	g_cfg.chan           = 9;
	g_cfg.txPreambLength = DWT_PLEN_64;
	g_cfg.rxPAC          = DWT_PAC8;
	g_cfg.txCode         = 9;
	g_cfg.rxCode         = 9;
	g_cfg.sfdType        = DWT_SFD_IEEE_4A;
	g_cfg.dataRate       = DWT_BR_6M8;
	g_cfg.phrMode        = DWT_PHRMODE_STD;
	g_cfg.phrRate        = DWT_PHRRATE_STD;
	g_cfg.stsMode        = DWT_STS_MODE_OFF; /* SP0 — hear the cleartext Pre-POLL */
	g_cfg.stsLength      = DWT_STS_LEN_64;
	g_cfg.pdoaMode       = DWT_PDOA_M0;
	return reconfigure();
}

int capture_set_sts_key(const uint8_t key_be[16])
{
	if (key_be == NULL) {
		return -EINVAL;
	}
	memcpy(g_sts_key, key_be, sizeof(g_sts_key));
	g_sts_custom = true;
	return reconfigure();
}

int capture_set_sts_iv(const uint8_t iv_be[16])
{
	if (iv_be == NULL) {
		return -EINVAL;
	}
	memcpy(g_sts_iv, iv_be, sizeof(g_sts_iv));
	g_sts_custom = true;
	return reconfigure();
}

void capture_clear_sts(void)
{
	g_sts_custom = false;
	memset(g_sts_key, 0, sizeof(g_sts_key));
	memset(g_sts_iv, 0, sizeof(g_sts_iv));
	(void)reconfigure(); /* dwt_configure() restores the driver-default key. */
}

const struct capture_stats *capture_get_stats(void) { return &g_stats; }

void capture_clear_stats(void)
{
	memset(&g_stats, 0, sizeof(g_stats));
	g_index = 0;
}

void capture_set_min_peak(uint32_t min_peak)
{
	g_min_peak = min_peak;
	LOG_INF("capture: min log peak = 0x%08X (%s)", min_peak,
		min_peak ? "throttled" : "log all");
}

void capture_set_view_human(bool human)
{
	g_view_human = human;
	LOG_INF("capture: view = %s", human ? "human (1/s summary)"
					     : "raw (per-frame UWBCAP lines)");
}

int capture_burst(uint32_t n_frames)
{
	if (n_frames == 0 || n_frames > CAP_BURST_MAX) {
		return -EINVAL;
	}
	if (g_burst_active || g_scan_active) {
		return -EBUSY;
	}
	if (!g_running) {
		int err = capture_start();

		if (err) {
			return err;
		}
	}
	g_burst_count  = 0;
	g_burst_target = n_frames;
	g_burst_ready  = false;
	g_burst_active = true;
	LOG_INF("capture: burst — stashing %u frames to RAM (dumps when full)", n_frames);
	return 0;
}

int capture_scan(void)
{
	if (g_scan_active || g_burst_active) {
		return -EBUSY;
	}
	if (!g_running) {
		int err = capture_start();

		if (err) {
			return err;
		}
	}
	g_scan_saved_code = g_cfg.rxCode;
	g_scan_best_code  = CAP_SCAN_FIRST;
	g_scan_best_hits  = 0;
	g_scan_code       = CAP_SCAN_FIRST;
	g_scan_base       = g_stats.per_event[CAP_EV_OK] +
			    g_stats.per_event[CAP_EV_STS_ERR];
	(void)capture_set_preamble_code(CAP_SCAN_FIRST);
	g_scan_active = true;
	LOG_INF("capture: scanning preamble codes %u..%u (1 s each)...",
		CAP_SCAN_FIRST, CAP_SCAN_LAST);
	return 0;
}

/**
 * @brief 1 Hz human-readable summary line, for the plain-terminal view.
 *
 * In human view emit() suppresses the per-frame machine lines; this thread
 * prints one digest per second instead: presence, an eyeball signal bar (from
 * the strongest first-path magnitude that second), the frame rate, and running
 * counts.  Lowest priority, off the radio path — no dBm/float, no jargon.
 */
static void summary_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	uint32_t last_real = 0, last_rounds = 0, rr_ewma = 0;
	bool was_active = false;
	uint32_t quiet_secs = 0;

	while (true) {
		k_sleep(K_SECONDS(1));

		/* A completed burst dumps here (thread context — slow UART is fine;
		 * capture already happened): one parseable UWBCAP line per frame. */
		if (g_burst_ready) {
			g_burst_ready = false;
			LOG_INF("=== burst: %u frames — feed to parser --analyze ===",
				g_burst_count);
			for (uint32_t i = 0; i < g_burst_count; i++) {
				const struct burst_rec *r = &g_burst[i];

				LOG_INF("UWBCAP idx=%06u ev=%s ts=%010llX cfo=%d cia=1 "
					"cir=00000000 f1=%08X f2=%08X f3=%08X fpidx=0000 "
					"acc=%04X peak=00000000 len=0 rng=0 dr=6M8 sts=0 "
					"ststat=0000 status=00000000 pl=",
					i, g_ev_name[r->ev],
					(unsigned long long)r->ts, r->cfo,
					r->f1, r->f2, r->f3, r->acc);
			}
			LOG_INF("=== burst end ===");
			if (g_running) {
				dwt_setrxtimeout(0);
				(void)rearm_rx();
			}
			continue;
		}

		uint32_t real = g_stats.per_event[CAP_EV_OK] +
				g_stats.per_event[CAP_EV_STS_ERR];
		uint32_t real_rate  = real - last_real;
		uint32_t rounds     = g_round_count;
		uint32_t round_rate = rounds - last_rounds;
		uint32_t sig        = g_win_sig;

		g_win_sig   = 0;
		last_real   = real;
		last_rounds = rounds;

		/* Exponential moving average (~4 s) so the round rate reads calm
		 * instead of bouncing with each second's capture luck.  x16 fixed-point. */
		rr_ewma = (rr_ewma * 3 + round_rate * 16 + 2) / 4;

		/* Preamble-code scan: one code per tick, tally hits, lock the winner. */
		if (g_scan_active) {
			uint32_t hits = real - g_scan_base;

			LOG_INF("[scan] code %u -> %u frames", g_scan_code, hits);
			if (hits > g_scan_best_hits) {
				g_scan_best_hits = hits;
				g_scan_best_code = g_scan_code;
			}
			if (g_scan_code < CAP_SCAN_LAST) {
				g_scan_code++;
				(void)capture_set_preamble_code(g_scan_code);
				g_scan_base = real;
			} else {
				uint8_t win = g_scan_best_hits ? g_scan_best_code
							       : g_scan_saved_code;

				g_scan_active = false;
				(void)capture_set_preamble_code(win);
				LOG_INF("[scan] done — code %u wins (%u frames), locked in",
					win, g_scan_best_hits);
			}
			continue;
		}

		if (!g_view_human || !g_running) {
			continue;
		}

		/* Keep the console calm: print every second while a device is
		 * active, print once on the edge back to quiet, then only a slow
		 * heartbeat (every 15 s) while nothing is around. */
		bool active = (real_rate > 0);
		bool show   = active || was_active;

		if (!active && ++quiet_secs >= 15) {
			show = true;
			quiet_secs = 0;
		}
		if (active) {
			quiet_secs = 0;
		}
		was_active = active;

		if (!show) {
			continue;
		}

		/* Signal bar 0..10 from the strongest first-path magnitude this
		 * second.  Relative and bench-dependent — an eyeball gauge, not
		 * a calibrated dBm; RSSI/dBm stays host-side in the parser. */
		int lvl = (sig > 0x100u) ? (int)((sig - 0x100u) / 0x100u) : 0;

		if (lvl > 10) {
			lvl = 10;
		}

		char bar[11];

		for (int i = 0; i < 10; i++) {
			bar[i] = (i < lvl) ? '#' : '.';
		}
		bar[10] = '\0';

		const char *prox = (lvl >= 7) ? "~close"
				 : (lvl >= 4) ? "~near"
				 : (lvl >= 1) ? "~far" : "--";

		uint32_t rr = (rr_ewma + 8) / 16;

		LOG_INF("[uwb] %-6s signal [%s] %-6s  rounds %u/s (~%ums)  seen %u",
			real_rate ? "ACTIVE" : "quiet", bar, prox, rr,
			rr ? 1000u / rr : 0u, real);
	}
}
K_THREAD_DEFINE(uwb_summary_tid, 2048, summary_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

void capture_print_banner(void)
{
	static const char *const sp_name[] = { "SP0", "SP1", "SP2", "SP3" };
	uint8_t sp = (g_cfg.stsMode == DWT_STS_MODE_OFF) ? 0
		   : (g_cfg.stsMode == DWT_STS_MODE_1)  ? 1
		   : (g_cfg.stsMode == DWT_STS_MODE_2)  ? 2 : 3;

	LOG_INF("=== DWM3001CDK passive UWB capture ===");
	LOG_INF("chan=%u code=%u plen=%u sfd=%u %s dr=%s sfdTO=%u",
		g_cfg.chan, g_cfg.rxCode, plen_symbols(g_cfg.txPreambLength),
		g_cfg.sfdType, sp_name[sp],
		(g_cfg.dataRate == DWT_BR_6M8) ? "6M8" : "850K", g_cfg.sfdTO);
	LOG_INF("view=%s  (sniff human|raw)  start|stop chan|preamble|plen|sfd|sp|ccc|minpeak|stats",
		g_view_human ? "human" : "raw");
}
