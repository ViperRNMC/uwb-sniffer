// Copyright (c) 2026 asxeem
// Licensed under the MIT License - see LICENSE file in the repo root for details.
/*
 * @file capture.h
 * @brief Passive HRP-UWB frame-capture engine for the DWM3001CDK (DW3110).
 *
 * Wraps Qorvo's portable `dwt_uwb_driver` (the DW3000 driver module) in a
 * receive-only capture loop: the radio is armed for continuous, error-tolerant
 * RX and every RX event - good frame, STS/CRC/PHY error, or timeout - is logged
 * as one structured line and RX is immediately re-armed.  This builds directly
 * on the PHY driver.
 *
 * All radio access stays inside capture.c (the "radio layer"); main.c and
 * shell_cmds.c only touch this API.
 */

#ifndef UWB_CAPTURE_H_
#define UWB_CAPTURE_H_

#include <stdbool.h>
#include <stdint.h>

/** @brief Per-frame event classification, derived from SYS_STATUS bits. */
enum capture_event {
	CAP_EV_OK = 0,   /**< RXFCG: frame with good FCS (and good STS in SP1/2/3). */
	CAP_EV_STS_ERR,  /**< CIAERR/CP error: STS did not validate. */
	CAP_EV_FCS_ERR,  /**< RXFCE: frame CRC (FCS) failed. */
	CAP_EV_PHY_ERR,  /**< RXPHE/RXFSL: PHY-header or Reed-Solomon decode error. */
	CAP_EV_TO,       /**< RXFTO/RXPTO/RXSTO: preamble/frame/SFD timeout. */
	CAP_EV_COUNT,
};

/** @brief Running tally of RX events since boot / last `stats clear`. */
struct capture_stats {
	uint32_t per_event[CAP_EV_COUNT]; /**< One counter per @ref capture_event. */
	uint32_t total;                   /**< Sum of all events. */
};

/**
 * @brief Bring the DW3110 up to a configured, callback-registered state.
 *
 * Idempotent.  Runs the proven sequence: dw3000_hw_init -> dw3000_hw_reset ->
 * dwt_probe -> dwt_initialise -> dwt_configure -> dw3000_hw_init_interrupt ->
 * dwt_setcallbacks -> dwt_setinterrupt.  Does NOT arm RX (call @ref
 * capture_start for that).
 *
 * @retval 0     Radio ready.
 * @retval -EIO  SPI probe / initialise / configure failed.
 */
int capture_init(void);

/**
 * @brief Arm continuous, timeout-free RX (rxtimeout 0, START_RX_IMMEDIATE).
 * @retval 0     Listening.
 * @retval -EIO  dwt_rxenable rejected.
 */
int capture_start(void);

/** @brief Force the radio off and stop capturing. @return 0. */
int capture_stop(void);

/** @brief True while RX is armed. */
bool capture_is_running(void);

/* ── Live PHY config (applied immediately; re-arms RX if it was running) ──── */

/** @brief Set channel (5 or 9). @retval -EINVAL if not 5/9. */
int capture_set_channel(uint8_t chan);
/** @brief Set TX+RX preamble code (9 = Apple/CCC default SYNC_Code_Index; 9/11 BPRF ideals). */
int capture_set_preamble_code(uint8_t code);
/** @brief Set preamble length in symbols (e.g. 64, 128). @retval -EINVAL if unsupported. */
int capture_set_preamble_len(uint16_t symbols);
/** @brief Set SFD type (dwt_sfd_type_e: 0 IEEE-8/4a, 1 DW-8, 2 DW-16, 3 IEEE-4z). */
int capture_set_sfd(uint8_t sfd_type);
/** @brief Set STS packet config: 0=SP0(off) 1=SP1 2=SP2 3=SP3(no-data). */
int capture_set_sp(uint8_t sp);
/**
 * @brief Retune to the Aliro/CCC ranging PHY (CCC v4 Table 21-1 Config 0000).
 *
 * One reconfigure to the set an Apple Wallet key uses: chan 9, code 9, 64-sym
 * preamble (PAC8), SFD IEEE-8/4a, 6.8 Mbps, SP0 (hear the cleartext Pre-POLL).
 * Any loaded STS key/IV is left in place.  @return 0, or a reconfigure error.
 */
int capture_set_ccc(void);

/**
 * @brief Load a custom 128-bit STS key (dURSK) / IV (STS-V) to decode a keyed
 *        secure-ranging session (e.g. CCC/Aliro).
 *
 * The DW3000 default STS key never matches a real session, so such frames always
 * fault as STS_ERR.  Given the session's key material - derived host-side from
 * the URSK - loading it lets the STS correlate so a matching frame decodes as OK
 * (event RXFCG, positive `sts` quality).  Both take a canonical big-endian
 * 16-byte value (byte 0 = MSB), exactly as the CCC deriver prints dURSK (key)
 * and STS-V (iv).  Applied immediately; re-arms RX if it was running.
 *
 * @retval 0        Loaded.
 * @retval -EINVAL  NULL pointer.
 * @retval -EIO     Underlying reconfigure failed.
 */
int capture_set_sts_key(const uint8_t key_be[16]);
int capture_set_sts_iv(const uint8_t iv_be[16]);

/** @brief Revert to the DW3000 default STS key/IV. */
void capture_clear_sts(void);

/**
 * @brief Only log frames whose Ipatov CIR peak is >= @p min_peak (0 = log all).
 *
 * Throttle for a strong-transmitter burst: below-threshold frames are still
 * counted and RX re-armed, they just aren't printed.
 */
void capture_set_min_peak(uint32_t min_peak);

/**
 * @brief Console output view.  All are per-frame renderings of the same capture
 *        except SUMMARY, which is a 1 Hz aggregate gauge.
 */
enum capture_view {
	CAP_VIEW_SUMMARY = 0, /**< 1 Hz gauge: signal bar + round rate; `sniff summary`. */
	CAP_VIEW_HUMAN,       /**< Per-frame plain-English line; `sniff human`. */
	CAP_VIEW_RAW,         /**< Per-frame `UWBCAP key=value` machine lines; `sniff raw`. */
	CAP_VIEW_RAW_PRETTY,  /**< Per-frame compact colored line; `sniff raw_pretty`. */
};

/**
 * @brief Select the console output view (@ref capture_view).  Applied immediately.
 */
void capture_set_view(enum capture_view view);

/**
 * @brief Capture the next @p n_frames real frames into RAM at full radio speed,
 *        then dump them as parseable `UWBCAP` lines for offline round analysis.
 *
 * The RX callback only stashes each frame's key fields (no per-frame printing),
 * so a ranging round's back-to-back frames aren't dropped by the slow console.
 *
 * @retval 0        Burst armed.
 * @retval -EINVAL  n_frames is 0 or exceeds the RAM buffer.
 * @retval -EBUSY   A burst is already in progress.
 */
int capture_burst(uint32_t n_frames);

/**
 * @brief Sweep the candidate preamble codes (9..12) ~1 s each, count real
 *        frames per code, and lock onto the busiest.
 *
 * Apple Nearby-Interaction sessions negotiate the preamble code, so this finds
 * the live one without prior knowledge.
 *
 * @retval 0        Scan started.
 * @retval -EBUSY   A scan or burst is already in progress.
 */
int capture_scan(void);

/** @brief Read-only snapshot of the running event tally. */
const struct capture_stats *capture_get_stats(void);
/** @brief Zero the event tally and the frame index. */
void capture_clear_stats(void);

/** @brief Emit the one-time configuration banner to the log. */
void capture_print_banner(void);

#endif /* UWB_CAPTURE_H_ */
