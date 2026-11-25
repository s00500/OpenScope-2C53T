/*
 * ESP32 Communication Protocol for OpenScope 2C53T
 *
 * UART-based protocol between the GD32F307 (scope MCU) and an ESP32
 * co-processor module. Handles module downloads, firmware updates,
 * framebuffer streaming, and remote button input.
 *
 * Packet format:
 *   [0xAA] [cmd] [len_hi] [len_lo] [payload...] [checksum]
 *   checksum = XOR of all bytes from cmd through end of payload
 *
 * All communication is half-duplex: ESP32 sends command, GD32 responds.
 */

#ifndef ESP_COMM_H
#define ESP_COMM_H

#include <stdint.h>
#include <stdbool.h>

/* Packet framing */
#define ESP_SYNC_BYTE       0xAA
#define ESP_MAX_PAYLOAD     256     /* RECEIVE cap only (host->device). Responses are not
                                     * capped: a WAVEFORM_FRAME is 1048 B. Do not "fix" the
                                     * asymmetry (remote_protocol.md §2.2, §3.5). */
#define ESP_HEADER_SIZE     4       /* sync + cmd + len_hi + len_lo */
#define ESP_CHECKSUM_SIZE   1

/* Commands: ESP32 → GD32 */
#define ESP_CMD_PING            0x01    /* Ping — scope replies with version */
#define ESP_CMD_MODULE_START    0x02    /* Begin module transfer (slot + size) */
#define ESP_CMD_MODULE_DATA     0x03    /* Module data chunk (up to 256 bytes) */
#define ESP_CMD_MODULE_END      0x04    /* Finalize module install */
#define ESP_CMD_FW_UPDATE_START 0x05    /* Begin firmware update (size) */
#define ESP_CMD_FW_UPDATE_DATA  0x06    /* Firmware data chunk */
#define ESP_CMD_FW_UPDATE_COMMIT 0x07   /* Mark staged firmware, reboot */
#define ESP_CMD_STATUS          0x08    /* Request device status */
#define ESP_CMD_FRAMEBUFFER     0x09    /* Request current framebuffer */
#define ESP_CMD_BUTTON          0x0A    /* Simulate button press */
#define ESP_CMD_SIGNAL_CONFIG   0x0B    /* Set signal injection config */
#define ESP_CMD_MODULE_LIST     0x0C    /* List installed modules */
#define ESP_CMD_MODULE_DELETE   0x0D    /* Delete a module by slot */
#define ESP_CMD_GET_METER       0x21    /* remote_protocol.md §3.4: one meter reading */
#define ESP_CMD_GET_WAVEFORM    0x22    /* §3.4: u8 channel_mask -> one WAVEFORM_FRAME per channel */
#define ESP_CMD_GET_STATE       0x23    /* M6: -> STATE (every setting, TLV) */
#define ESP_CMD_SET_MODE        0x24    /* M6: u8 device_mode -> STATE after the switch */
#define ESP_CMD_SET_PARAM       0x2A    /* M6: u8 key, value -> STATE after the write */
#define ESP_CMD_ACTION          0x2B    /* M6: u8 action, u8 arg -> ACK */

/* Responses: GD32 → ESP32 */
#define ESP_RSP_ACK             0x81    /* Command accepted */
#define ESP_RSP_NAK             0x82    /* Command rejected */
#define ESP_RSP_DATA            0x83    /* Data response */
#define ESP_RSP_FRAMEBUFFER     0x84    /* Framebuffer data (multi-packet) */
#define ESP_RSP_STATUS          0x85    /* Status response */
#define ESP_RSP_MODULE_LIST     0x86    /* Module list response */
#define ESP_RSP_METER_FRAME     0x90    /* §3.5 METER_FRAME */
#define ESP_RSP_WAVEFORM_FRAME  0x91    /* §3.5 WAVEFORM_FRAME */
#define ESP_RSP_STATE           0x92    /* M6 STATE (TLV, layout below) */

/* NAK error codes */
#define ESP_ERR_UNKNOWN_CMD     0x01
#define ESP_ERR_BAD_CHECKSUM    0x02
#define ESP_ERR_BAD_LENGTH      0x03
#define ESP_ERR_FLASH_WRITE     0x04
#define ESP_ERR_FLASH_FULL      0x05
#define ESP_ERR_INVALID_SLOT    0x06
#define ESP_ERR_NOT_READY       0x07
#define ESP_ERR_TRANSFER_ACTIVE 0x08
#define ESP_ERR_UNSUPPORTED     0x09    /* command exists but is not implemented on this build */
#define ESP_ERR_RESERVED_0A     0x0A    /* reserved: never sent (a gap timeout is silent, see esp_comm_rx_poll) */
#define ESP_ERR_NO_CAPTURE_DATA 0x0B    /* remote_protocol.md §3.4 — never substitute the demo trace */
#define ESP_ERR_UNSUPPORTED_IN_MODE 0x0C
#define ESP_ERR_BAD_ARG         0x0D    /* argument out of range */
#define ESP_ERR_HW_REFUSED      0x0E    /* M6: the hardware write did not take (acq task
                                         * would not park, relay readback disagreed):
                                         * nothing changed */
#define ESP_ERR_UNKNOWN_KEY     0x0F    /* M6: key / action this firmware does not know */
#define ESP_ERR_READ_ONLY       0x10    /* M6: key is reported, not settable */
#define ESP_ERR_OUTCOME_UNKNOWN 0x11    /* M6: the write started but did not finish in
                                         * time: it MAY have taken. Read STATE. */

/* ─── Remote protocol (issue #10, docs/design/remote_protocol.md) ───
 *
 * Wire-format version reported in STATUS byte 0. Bump the major on any
 * incompatible change; the host refuses unknown majors (§3.7). */
#define ESP_PROTO_VERSION       1

/* A frame whose bytes stop arriving for this long is abandoned and the
 * receiver resyncs. Without it a truncated packet leaves the parser in
 * PAYLOAD and, on the shared CDC endpoint, it would swallow the operator's
 * shell text (and any later 0xAA) as payload. USB delivers a host write in
 * back-to-back 64-byte packets, so a real gap of this size means the host
 * gave up, not that it is slow. */
#define ESP_RX_GAP_MS           50

/* STATUS payload v1 — explicit little-endian layout, NOT a C struct dump
 * (a struct's padding and enum width depend on the compiler):
 *   [0]    u8   proto_version   (ESP_PROTO_VERSION)
 *   [1]    u8   current_mode    (device_mode_t: 0 scope 1 meter 2 siggen 3 settings)
 *   [2]    u8   battery_pct     (0..100)
 *   [3]    u8   flags           bit0 charging, bit1 capture data ready,
 *                               bit2 battery critical, bit3 battery UNKNOWN
 *                               (no sample yet: battery_pct/mv are 0 and
 *                               must not be read as a measurement)
 *   [4..5] u16  battery_mv
 *   [6..9] u32  uptime_ms
 *   [10..13] u32 usb_tx_stalls  (CDC IN waits that timed out, issue #39)
 *   [14..15] u16 usb_heals      (transport self-heal reconnects, issue #39)
 *   [16]   u8   fw_len
 *   [17..] char fw_version[fw_len]  (no NUL)
 */
#define ESP_STATUS_FIXED_LEN    17
#define ESP_FW_VERSION_MAX      48      /* bytes of fw_version sent; also sizes the STATUS buffer */
#define ESP_STATUS_FLAG_CHARGING      0x01
#define ESP_STATUS_FLAG_CAPTURE_READY 0x02
#define ESP_STATUS_FLAG_BATT_CRITICAL 0x04
#define ESP_STATUS_FLAG_BATT_UNKNOWN  0x08

/* Snapshot the firmware fills in for STATUS/PING. esp_comm itself knows
 * nothing about the device, so the host tests can inject any state. */
typedef struct {
    uint8_t     current_mode;
    uint8_t     battery_pct;
    uint8_t     flags;
    uint16_t    battery_mv;
    uint32_t    uptime_ms;
    uint32_t    usb_tx_stalls;
    uint16_t    usb_heals;
    const char *fw_version;     /* NUL-terminated; truncated to ESP_FW_VERSION_MAX on the wire */
} esp_status_snapshot_t;

typedef void (*esp_status_fn)(esp_status_snapshot_t *out);

/* METER_FRAME payload v1 (remote_protocol.md §3.5, plus the display text):
 *   [0..3]   u32  update_count   monotonic: a host sees drops and stale reads
 *   [4..7]   f32  value          as the firmware scaled it (IEEE-754 LE)
 *   [8..9]   i16  raw_bcd        the instrument's own digits, uncalibrated —
 *                                per-device cal (#28) means a log should keep
 *                                what the meter saw, not only what we concluded
 *   [10]     u8   decimal_pos
 *   [11]     u8   result_class   meter_result_class_t (NORMAL, OL, …)
 *   [12]     u8   flags          bit0 negative, bit1 ac, bit2 autorange, bit3 hold
 *   [13]     u8   submode
 *   [14]     u8   unit_variant
 *   [15]     u8   unit_len, then unit[unit_len] (ASCII, e.g. "V", "kOhm")
 *   [..]     u8   display_len, then display[display_len] (what the LCD shows)
 */
#define ESP_METER_FIXED_LEN     16
#define ESP_METER_FLAG_NEGATIVE 0x01
#define ESP_METER_FLAG_AC       0x02
#define ESP_METER_FLAG_AUTO     0x04
#define ESP_METER_FLAG_HOLD     0x08

typedef struct {
    uint32_t    update_count;
    float       value;
    int16_t     raw_bcd;
    uint8_t     decimal_pos;
    uint8_t     result_class;
    uint8_t     flags;
    uint8_t     submode;
    uint8_t     unit_variant;
    char        unit[16];       /* copied, NUL-terminated: the provider's own */
    char        display[16];    /* snapshot is gone by the time we encode */
} esp_meter_snapshot_t;

/* Fill a coherent reading. Anything but ESP_METER_OK is answered with a NAK:
 * a reading the instrument is not currently producing must never be sent as
 * if it were live (§2.3). */
typedef enum {
    ESP_METER_OK = 0,
    ESP_METER_NOT_READY,        /* in meter mode, no reading parsed yet */
    ESP_METER_WRONG_MODE,       /* not in meter mode: the last reading is frozen */
} esp_meter_result_t;
typedef esp_meter_result_t (*esp_meter_fn)(esp_meter_snapshot_t *out);

/* WAVEFORM_FRAME payload v1 (remote_protocol.md §3.5, header extended). One
 * frame per requested channel, CH1 first; all frames of one request are ONE
 * capture (same frame_id). Header, little-endian:
 *   [0..3]   u32  frame_id        acquisition generation of the copied record
 *                                 (fpga_acq_frame_generation(): even, +2 per
 *                                 committed capture — the `gen=` that `spi3
 *                                 frame` prints). Never 0: no record, no frame.
 *   [4]      u8   channel         0 = CH1, 1 = CH2
 *   [5]      u8   flags           ESP_WAVE_FLAG_* below — derived HERE from the
 *                                 provider's facts, never copied from it
 *   [6]      u8   timebase_idx    reg 0x01 code IN FORCE in the FPGA (what the
 *                                 samples were taken at, not the display's) when
 *                                 the frame was requested: a record HELD across a
 *                                 timebase change (STOP, SINGLE, NORMAL with no
 *                                 crossing) is labelled with the new code, as on
 *                                 the LCD — the code is not yet published with
 *                                 the record
 *   [7]      u8   vdiv_idx        frontend range index of this channel
 *   [8..9]   u16  sample_count    1024 today
 *   [10..11] u16  header_len      offset of samples[0] (§3.5's "reserved"):
 *                                 a host skips to it, so fields can be appended
 *                                 without a major bump
 *   [12..15] u32  sample_rate_hz  bench-measured rate of this code
 *                                 (scope_timebase.c, bench unit #1), rounded;
 *                                 0 = no trustworthy rate (never measured,
 *                                 incoherent, or display != hardware)
 *   [16..19] u32  uv_per_div      bench-measured volts/div of this channel and
 *                                 range (scope_cal.c, bench unit #1), in uV, at
 *                                 the BNC (no probe factor); 0 = the range has
 *                                 no volts meaning. A GAIN only: the zero
 *                                 point is uncalibrated, so counts->volts
 *                                 gives Vpp, not absolute volts.
 *   [20..21] u16  counts_per_div  ADC counts one division of uv_per_div spans
 *   [22..23] u16  head_skip       samples [0, head_skip) are the known record-
 *                                 head defect (scope_record.h): analyse from here
 *   [24..]   u8   samples[sample_count]  unsigned ADC counts as committed by the
 *                                 acquisition task (a coherent copy, not the
 *                                 live buffer)
 */
#define ESP_WAVE_HDR_LEN        24
#define ESP_WAVE_MAX_SAMPLES    1024    /* one FPGA record (FPGA_ADC_BUF_SIZE) */
#define ESP_WAVE_MASK_CH1       0x01
#define ESP_WAVE_MASK_CH2       0x02
#define ESP_WAVE_MASK_ALL       (ESP_WAVE_MASK_CH1 | ESP_WAVE_MASK_CH2)

#define ESP_WAVE_FLAG_CALIBRATED   0x01  /* per-unit calibration: always 0 until
                                          * that work is bench-validated (§3.5) */
#define ESP_WAVE_FLAG_TB_MEASURED  0x02  /* sample_rate_hz is tier MEASURED   */
#define ESP_WAVE_FLAG_VDIV_MEASURED 0x04 /* uv_per_div is tier MEASURED       */
#define ESP_WAVE_FLAG_SYNTHETIC    0x08  /* never set: a synthetic record is
                                          * refused, not labelled (§2.3)      */
#define ESP_WAVE_FLAG_TB_PROVISIONAL   0x10  /* rate is right order of magnitude
                                              * only ('~' on the LCD)         */
#define ESP_WAVE_FLAG_VDIV_PROVISIONAL 0x20  /* same for uv_per_div           */
#define ESP_WAVE_FLAG_TIME_ORDERED 0x40  /* record un-rotated at its seam: the
                                          * hardware trigger sits at index 512 */
#define ESP_WAVE_FLAG_TB_DISAGREES 0x80  /* the display's timebase is not the
                                          * one in force: rate withheld (0)   */

/* Confidence of a measured number — numerically identical to scope_cal_tier_t
 * and scope_tb_tier_t (usb_debug.c asserts it). */
#define ESP_TIER_NONE           0
#define ESP_TIER_PROVISIONAL    1
#define ESP_TIER_MEASURED       2

typedef struct {
    const uint8_t *samples;     /* a coherent COPY of the record, or NULL */
    uint16_t    count;          /* 1..ESP_WAVE_MAX_SAMPLES */
    uint8_t     vdiv_idx;
    uint8_t     vdiv_tier;      /* ESP_TIER_* of uv_per_div */
    uint32_t    uv_per_div;     /* 0 = no volts meaning */
} esp_wave_channel_t;

typedef struct {
    uint32_t    frame_id;       /* generation of the copy; 0 = none (refused) */
    uint8_t     timebase_idx;   /* code in force */
    uint8_t     timebase_tier;  /* ESP_TIER_* of sample_rate_hz */
    bool        timebase_disagrees;
    bool        time_ordered;
    bool        synthetic;      /* anything but a real capture: the encoder refuses */
    uint32_t    sample_rate_hz;
    uint16_t    counts_per_div;
    uint16_t    head_skip;
    esp_wave_channel_t ch[2];   /* [0] CH1, [1] CH2; only requested ones are read */
} esp_wave_snapshot_t;

/* Fill a coherent snapshot of the channels in `channel_mask`. Anything but
 * ESP_WAVE_OK is answered with a NAK — a record the instrument is not
 * currently producing must never be sent as if it were (§2.3). */
typedef enum {
    ESP_WAVE_OK = 0,
    ESP_WAVE_NO_DATA,           /* fpga_data_ready() false / no committed record */
    ESP_WAVE_WRONG_MODE,        /* not in scope mode: the buffers are not live */
    ESP_WAVE_BUSY,              /* no tear-free copy within the retry budget */
} esp_wave_result_t;
typedef esp_wave_result_t (*esp_wave_fn)(uint8_t channel_mask, esp_wave_snapshot_t *out);
/* ─── M6: remote control surface (remote_protocol.md §10) ───
 *
 * STATE payload: [0] u8 state_version (ESP_STATE_VERSION), then entries
 *   [u8 key][u8 len][value[len], little-endian]
 * in no guaranteed order. A host skips keys it does not know by `len`, so
 * keys are added without a version bump; `len` always equals the key's type
 * size below, which the host may check. A key is ABSENT when this build has
 * no such thing (e.g. `view` without FEATURE_FFT) -- never sent as a guess.
 *
 * SET_PARAM payload: [u8 key][value of exactly the key's type size]. The
 * reply to a write that took is the whole STATE read back AFTER it, so a host
 * sees what the device did (a clamp, an inert output) rather than what it
 * asked for. A write that did not take is a NAK and changed nothing.
 *
 * Writes run in the input task, the one writer of UI state, exactly where a
 * button press runs, and get the same settings-persistence tail. */
#define ESP_STATE_VERSION       1
#define ESP_STATE_MAX_LEN       320     /* ~50 keys x <= 6 B + version byte */

/* Value types (wire size in brackets). */
#define ESP_VT_U8               1       /* [1] enums and bools (0/1) */
#define ESP_VT_I16              2       /* [2] */
#define ESP_VT_U16              3       /* [2] */
#define ESP_VT_U32              4       /* [4] */
#define ESP_VT_F32              5       /* [4] IEEE-754; NaN/inf refused on write */

/* Keys. RO = reported only (SET_PARAM answers READ_ONLY). */
#define ESP_KEY_MODE            0x01    /* u8  RO device_mode_t (SET_MODE changes it) */
#define ESP_KEY_THEME           0x02    /* u8     theme_id_t 0..3 */
#define ESP_KEY_STARTUP         0x03    /* u8     startup_mode_t 0 scope 1 meter */
#define ESP_KEY_MATH            0x04    /* u8     bool */
#define ESP_KEY_MATH_OP         0x05    /* u8     math_op_t 0..4 */
#define ESP_KEY_PERSIST         0x06    /* u8     bool */
#define ESP_KEY_FRAME_GEN       0x07    /* u32 RO generation of the latest committed capture
                                         *        (= WAVEFORM_FRAME frame_id; +2 per capture,
                                         *        0 = none): poll this, fetch frames on change */

#define ESP_KEY_TIMEBASE        0x10    /* u8     display timebase code 0..20 (written through
                                         *        fpga_apply_timebase: display + reg 0x01) */
#define ESP_KEY_TIMEBASE_HW     0x11    /* u8  RO reg 0x01 code in force */
#define ESP_KEY_SAMPLE_RATE     0x12    /* u32 RO S/s of the code in force; 0 = no trustworthy rate */
#define ESP_KEY_RATE_TIER       0x13    /* u8  RO ESP_TIER_* of SAMPLE_RATE */
#define ESP_KEY_RUNNING         0x14    /* u8     bool RUN/STOP */
#define ESP_KEY_VIEW            0x15    /* u8     scope_view_t 0 time 1 fft 2 split 3 waterfall 4 xy
                                         *        (FEATURE_FFT builds only) */
#define ESP_KEY_GRATICULE       0x16    /* u8     bool: true scale (1) / autofit (0) */
#define ESP_KEY_SOFTTRIG        0x17    /* u8     bool: software display trigger */
#define ESP_KEY_COUNTS_PER_DIV  0x18    /* u16 RO ADC counts per vertical division */
#define ESP_KEY_TRIG_MODE       0x19    /* u8     trigger_mode_t 0 auto 1 normal 2 single */
#define ESP_KEY_TRIG_EDGE       0x1A    /* u8     trigger_edge_t 0 rising 1 falling */
#define ESP_KEY_TRIG_SOURCE     0x1B    /* u8     trigger_source_t 0 CH1 1 CH2 */
#define ESP_KEY_TRIG_LEVEL      0x1C    /* i16    -100..100 (written through fpga_apply_trigger_level) */
#define ESP_KEY_TRIG_X          0x1D    /* i16    trigger column 8..312 */
#define ESP_KEY_TRIG_CODE       0x1E    /* u8  RO reg 0x08 code in force */
#define ESP_KEY_ACTIVE_CH       0x1F    /* u8     0 CH1 1 CH2: what UP/DOWN adjust on the device */

/* Per channel: ESP_KEY_CH_BASE(ch) + offset, ch = 0 (CH1) or 1 (CH2). */
#define ESP_KEY_CH_BASE(ch)     (uint8_t)(0x20u + 8u * (uint8_t)(ch))
#define ESP_KEY_CH_ENABLED      0       /* u8     bool */
#define ESP_KEY_CH_VDIV         1       /* u8     frontend range 0..9 (fpga_apply_vdiv) */
#define ESP_KEY_CH_COUPLING     2       /* u8     0 DC 1 AC (2 GND is refused: no hardware path) */
#define ESP_KEY_CH_PROBE        3       /* u8     0 x1 1 x10 */
#define ESP_KEY_CH_UV_PER_DIV   4       /* u32 RO bench-measured volts/div at the BNC, uV; 0 = none */
#define ESP_KEY_CH_VDIV_TIER    5       /* u8  RO ESP_TIER_* of UV_PER_DIV */

#define ESP_KEY_METER_FUNCTION  0x30    /* u8     meter submode 0..10 */
#define ESP_KEY_METER_LAYOUT    0x31    /* u8     0 big 1 graph 2 stats 3 fuse 4 limits */
#define ESP_KEY_METER_REL       0x32    /* u8     bool relative mode */
#define ESP_KEY_METER_HOLD      0x33    /* u8     bool hold */

/* The generator is stock's (DAC2/PA5 + TMR7 + DMA2 ch4, siggen_wave.h). */
#define ESP_KEY_SG_WAVE         0x40    /* u8     siggen_waveform_t 0..12, stock order */
#define ESP_KEY_SG_FREQ         0x41    /* f32    Hz, rounded to an integer, clamped 1..50000 */
#define ESP_KEY_SG_AMP          0x42    /* f32    V, rounded to 0.1 V, clamped 0..3.0 */
#define ESP_KEY_SG_OFFSET       0x43    /* f32 RO always 0: the output spans 0..amplitude */
#define ESP_KEY_SG_DUTY         0x44    /* u8     % 0..100 (Square and Sawtooth only) */
#define ESP_KEY_SG_OUTPUT       0x45    /* u8     bool */
#define ESP_KEY_SG_FREQ_ACTUAL  0x46    /* f32 RO Hz TMR7 really produces (quantised); 0 while off */

/* Actions (ACTION payload [u8 action][u8 arg]). */
#define ESP_ACT_METER_RESET     0x01    /* clear min/max/avg; arg 0 */
#define ESP_ACT_SETTINGS_SAVE   0x02    /* commit pending settings now; arg 0 */

/* Type and writability of a key; false for a key this firmware does not know. */
bool esp_comm_key_info(uint8_t key, uint8_t *type, bool *writable);
/* Wire size of a value type, 0 for an unknown type. */
uint8_t esp_comm_vtype_size(uint8_t type);

/* Incremental STATE encoder handed to the state provider. Put functions
 * encode by the KEY's type (an integer put on an F32 key is converted, an
 * unknown key is dropped and counted) so the provider cannot put a value on
 * the wire in a shape the table does not promise. */
typedef struct {
    uint8_t  *buf;
    uint16_t  cap;
    uint16_t  len;
    uint8_t   dropped;      /* unknown keys or no room: a test asserts 0 */
} esp_state_writer_t;

void esp_state_put_int(esp_state_writer_t *w, uint8_t key, int32_t v);
void esp_state_put_f32(esp_state_writer_t *w, uint8_t key, float v);

typedef void (*esp_state_fn)(esp_state_writer_t *w);

/* A decoded SET_PARAM value: `i` for integer types, `f` for F32. */
typedef struct {
    int32_t i;
    float   f;
} esp_value_t;

/* Outcome of a write. Anything but OK is a NAK and must mean "nothing
 * changed": a provider that cannot promise that must not return early. */
typedef enum {
    ESP_APPLY_OK = 0,
    ESP_APPLY_BAD_ARG,          /* value out of range           -> NAK BAD_ARG */
    ESP_APPLY_WRONG_MODE,       /* not meaningful in this mode  -> NAK UNSUPPORTED_IN_MODE */
    ESP_APPLY_HW_REFUSED,       /* hardware write did not take  -> NAK HW_REFUSED */
    ESP_APPLY_BUSY,             /* input task did not take it   -> NAK NOT_READY */
    ESP_APPLY_UNSUPPORTED,      /* not in this build            -> NAK UNSUPPORTED */
    ESP_APPLY_UNKNOWN,          /* key/action not handled       -> NAK UNKNOWN_KEY */
    ESP_APPLY_TIMED_OUT,        /* started, never finished      -> NAK OUTCOME_UNKNOWN
                                 * (the one result that does NOT promise "unchanged") */
} esp_apply_result_t;

typedef esp_apply_result_t (*esp_mode_fn)(uint8_t mode);
typedef esp_apply_result_t (*esp_param_fn)(uint8_t key, const esp_value_t *v);
typedef esp_apply_result_t (*esp_action_fn)(uint8_t action, uint8_t arg);

/* Inject a button press (id 1..15 = button_id_t). Return false if it could
 * not be queued, so the host gets NAK instead of a false ACK. */
typedef bool (*esp_button_fn)(uint8_t button_id);
/* Block writer: the whole of `len` bytes, in order. Preferred over the
 * byte writer — the USB CDC path sends 64-byte packets, not bytes. */
typedef void (*esp_write_block_fn)(const uint8_t *data, uint16_t len);
/* Where non-protocol bytes go when routing a shared stream (§3.2). */
typedef void (*esp_passthrough_fn)(const uint8_t *data, uint16_t len, void *ctx);

/* Module slots */
#define ESP_MODULE_SLOT_COUNT   4
#define ESP_MODULE_MAX_SIZE     (1024 * 1024)   /* 1MB per slot */

/* Button IDs (matches button_id_t in ui.h) */
#define ESP_BTN_CH1     1
#define ESP_BTN_CH2     2
#define ESP_BTN_MOVE    3
#define ESP_BTN_SELECT  4
#define ESP_BTN_TRIGGER 5
#define ESP_BTN_PRM     6
#define ESP_BTN_AUTO    7
#define ESP_BTN_SAVE    8
#define ESP_BTN_MENU    9
#define ESP_BTN_UP      10
#define ESP_BTN_DOWN    11
#define ESP_BTN_LEFT    12
#define ESP_BTN_RIGHT   13
#define ESP_BTN_OK      14
#define ESP_BTN_POWER   15

/* Firmware update state */
typedef enum {
    FW_UPDATE_IDLE = 0,
    FW_UPDATE_RECEIVING,
    FW_UPDATE_STAGED,
    FW_UPDATE_FAILED,
} fw_update_state_t;

/* Module slot info */
typedef struct {
    char     name[32];
    char     version[16];
    uint32_t size;
    bool     installed;
} module_slot_info_t;

/* Device status (sent in response to STATUS command) */
typedef struct {
    char     fw_version[16];
    uint8_t  current_mode;
    uint8_t  battery_pct;
    uint8_t  num_modules;
    fw_update_state_t fw_state;
} device_status_t;

/* Received packet (parsed) */
typedef struct {
    uint8_t  cmd;
    uint16_t payload_len;
    uint8_t  payload[ESP_MAX_PAYLOAD];
    bool     valid;
} esp_packet_t;

/* ─── Protocol API ─── */

/* Initialize the ESP32 communication handler */
void esp_comm_init(void);

/* Process one byte from UART RX. Call this from UART ISR or polling loop.
 * Returns true when a complete valid packet is ready. */
bool esp_comm_receive_byte(uint8_t byte);

/* Get the last received packet (valid after esp_comm_receive_byte returns true) */
const esp_packet_t *esp_comm_get_packet(void);

/* Process the received packet and generate response.
 * This is the main command dispatcher. */
void esp_comm_process(const esp_packet_t *pkt);

/* Send a response packet over UART.
 * write_byte: function pointer to UART TX byte function */
typedef void (*esp_write_fn)(uint8_t byte);

void esp_comm_set_writer(esp_write_fn fn);

/* Send a raw response */
void esp_comm_send_response(uint8_t cmd, const uint8_t *payload, uint16_t len);

/* Send ACK */
void esp_comm_send_ack(void);

/* Send NAK with error code */
void esp_comm_send_nak(uint8_t error_code);

/* Check if a module transfer or firmware update is in progress */
bool esp_comm_transfer_active(void);

/* ─── Remote protocol bindings ─── */

void esp_comm_set_block_writer(esp_write_block_fn fn);
void esp_comm_set_status_provider(esp_status_fn fn);
void esp_comm_set_button_injector(esp_button_fn fn);
void esp_comm_set_meter_provider(esp_meter_fn fn);
void esp_comm_set_waveform_provider(esp_wave_fn fn);
void esp_comm_set_state_provider(esp_state_fn fn);
void esp_comm_set_mode_setter(esp_mode_fn fn);
void esp_comm_set_param_setter(esp_param_fn fn);
void esp_comm_set_action_handler(esp_action_fn fn);

/* True while a frame is being received (sync seen, checksum not yet). */
bool esp_comm_rx_in_frame(void);

/* Abandon a frame whose bytes stopped arriving ESP_RX_GAP_MS ago.
 * Returns true if a frame was abandoned. Nothing is sent: without request
 * ids an unsolicited reply would be mistaken for the next request's answer. */
bool esp_comm_rx_poll(uint32_t now_ms);

/* Refresh the gap clock of a frame still open (no-op otherwise). Call after
 * esp_comm_route() returns, with a fresh time: route() stamps a whole chunk
 * with its entry time, and work inside the chunk may have taken a while. */
void esp_comm_rx_touch(uint32_t now_ms);

/* Route a chunk from a stream shared with the ASCII shell (§3.2):
 * bytes belonging to a frame (starting at 0xAA) go to the parser and every
 * completed frame is dispatched; all other bytes are handed, in order and
 * in contiguous runs, to `passthrough`. Malformed frames are answered with
 * NAK (bad checksum / bad length) rather than dropped silently. */
void esp_comm_route(const uint8_t *data, uint16_t len, uint32_t now_ms,
                    esp_passthrough_fn passthrough, void *ctx);

/* Diagnostics for `usbstat`/tests. */
typedef struct {
    uint32_t frames_ok;
    uint32_t bad_checksum;
    uint32_t bad_length;
    uint32_t gap_timeouts;
} esp_rx_stats_t;
void esp_comm_get_rx_stats(esp_rx_stats_t *out);

/* Compute XOR checksum */
uint8_t esp_comm_checksum(const uint8_t *data, uint16_t len);

#endif /* ESP_COMM_H */
