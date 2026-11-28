# Factory calibration — recovered MCU settings page

## Files

| File | Unit | Read | Committed |
|---|---|---|---|
| `unit1_mcu_settings_page_0x08006000.bin` | unit #1 (maintainer's bench unit) | extracted 2026-08-14 from `../factory_iap_bootloader_2C53T.bin` | yes |
| `unit4_mcu_settings_page_0x08006000.bin` | my unit (s00500) | 2026-11-24 | yes |
| `unit4_mcu_flash_0x08000000-0x08007000.bin` | my unit, the whole read the page came from | 2026-11-24 | yes |
| `dads_unit_mcu_settings_page_0x08006000.bin` | Dad's unit | 2026-11-28 | yes |
| `dads_unit_mcu_flash_0x08000000-0x08007000.bin` | Dad's unit, the whole read the page came from | 2026-11-28 | yes |
| `../w25q128_unit4_2026-11-24.bin` (+ `.sha256`) | my unit, full 16 MB W25Q | 2026-11-24 | no, local only |
| `../w25q128_dads_unit_2026-11-28.bin` (+ `.sha256`) | Dad's unit, full 16 MB W25Q | 2026-11-28 | no, local only |

The `unit_maksidze_…` and `unit_dendi_…` pages described below were never committed;
they were shared on #18. Every `.bin` here is covered by the repo's `*.bin` ignore
rule, so a new page needs `git add -f`.

## `unit1_mcu_settings_page_0x08006000.bin`

4096 bytes. MCU internal flash `0x08006000–0x08006FFF` from **bench unit #1**, stock's
saved-settings page. Extracted 2026-08-14 from `../factory_iap_bootloader_2C53T.bin`,
which is `0x7000` bytes long and therefore spans `0x08000000–0x08007000` — the settings
page is inside it. That file was committed 2026-06-12 under a name that says
"bootloader", so nobody realised it also carried the settings page.

```
sha256  6004374abb123b99aa8a2516f66b0f6465032e817ebd3fb463c2559accdf621f
crc32   59E91404
```

Verified content: **513 non-`0xFF` bytes**, signature byte `0x55` at offset 0, data
through `0x803`, the rest erased. Values are small clustered integers of calibration
shape (`1701, 1626, 1700, 1628, 3201, 3304, …`), not the large values found in the
stock image's compiled-in default table.

## ✅ VERIFIED AGAINST LIVE HARDWARE — 2026-08-14

`make guest-caldump` embeds this file and diffs it against the live page on bench
unit #1. Result, read off the device:

| Region | Offsets | Differs | Meaning |
|---|---|---|---|
| header / settings | `0x000–0x02F` | ~15 / 48 | stock rewrote its settings — expected |
| **CALIBRATION** | `0x030–0x12F` | **0 / 256** | **byte-identical to this file** |
| tail (RAM garbage) | `0x130–0x1FF` | ~124 / 208 | uninitialised stack in stock's 512 B staging buffer; differs on every write, means nothing |

Whole-page diff was 139 bytes across `0x005–0x1FB`, which looked alarming until it
was split by region. **The calibration did not move.** Unit #1 still holds its factory
values, this file is a byte-exact copy of them, and stock never overwrote them with
its compiled-in defaults during the July/August bench sessions.

Note the method: the whole-page byte count conflated a meaningless garbage tail with
the data that matters and would have supported either conclusion. The per-region split
is what made it a measurement rather than an argument.

## Why this file exists separately

**This may be the only per-device factory calibration record this project possesses,
and it was one careless cleanup away from being lost.** A tidy-up that truncated the
bootloader archive to its nominal 24 KB would have destroyed it silently — the file
would still be a valid bootloader image, and nothing would have flagged the loss.
Copying it out under a name that says what it is removes that failure mode.

**Do not delete, truncate, or "clean up" either this file or
`../factory_iap_bootloader_2C53T.bin`.** They are not regenerable: no firmware we hold
computes these values from a measurement (an exhaustive scan of the stock image found
`master_init` to be the only writer of that RAM region), so if the physical page is ever
erased there is nothing to restore it from except this copy.

## What is established, and what is not

**Established:** the page is populated; its values differ from the defaults compiled into
the stock image at `0x0802D1BE–0x0802D506`; stock falls back to those defaults when the
page's sentinel is erased or zero; and nothing in any firmware we have reconstructs the
recovered values.

**NOT established — n = 1.** "Differs from the compiled-in defaults" is proven. "Differs
*between units*" is not. These could be a per-unit trim written at manufacture, or one
constant factory table flashed to every device. Only a second unit's page decides it, and
that distinction is the whole question of whether this data is irreplaceable.

`make guest-caldump` reads this page on a live unit and prints its CRC32 — read-only, no
case opening. Two never-flashed units reporting the *same* CRC means a constant table;
different CRCs means per-device trim. See `firmware/src/util/cal_dump.h`.

## Caveat on the archive generally

`archive/` holds 13 W25Q images but they are **one observation, not thirteen** — 11 of the
13 are emulator artifacts derived from a single physical read. Sample size in this project
is usually smaller than the file count suggests.

## Layout

Not yet decoded. Current reading: the stock RAM table it feeds
(`0x20000358–0x2000044A`, 242 bytes) is **4 sub-tables of 30 `uint16`**, with the regime
change at `0x20000394` — which is the address the documented meter DAC formula already
used for "upper bounds". Decoding is required for a *re-calibration* feature; it is **not**
required to back this up or restore it byte-for-byte.

## `unit_maksidze_mcu_settings_page_0x08006000.bin` — CROSS-UNIT COMPARISON, 2026-08-16

4096 bytes, same region, from **a different physical 2C53T** (maksidze, issue #18),
dumped with his own MCU-memory reader. sha256 `682069379cf36c6e282ee0afd971348f…`.

This is the comparison `analysis_v120/factory_cal_truth_2026-08-14.md` was blocked on.
The standing caution was that every unit *we* had dumped had already been reflashed by
us, so "absent in our dumps" could not be distinguished from "absent on a pristine
unit". A second, independently dumped device settles it.

**229 of 4096 bytes differ, and they fall in exactly three regions — the same split
the live-hardware verification above established:**

| Region | Offsets | Differs | Reading |
|---|---|---|---|
| header / settings | `0x003–0x012` | 16 B | per-unit settings, expected |
| **CALIBRATION** | `0x025–0x12E` | **266 B** | **per-device values — see below** |
| garbage tail | `0x180–0x1FB` | 124 B | uninitialised staging buffer, means nothing |
| erased | `0x1FC–0xFFF` | **0 B** | 3580 bytes of `0xFF`, byte-identical |

**The calibration region is structured, not random.** It is an array of 16-bit LE
pairs, in two groups whose values are plausible 12-bit DAC codes:

| | group 1 (low, high) | group 2 (low, high) |
|---|---|---|
| unit #1 | 1598–1628, 1696–1717 | 3195–3221, 3283–3306 |
| maksidze | 1639–1667, 1657–1705 | 3239–3268, 3230–3290 |

Same layout, same clusters, different numbers — with unit #1 sitting systematically
~30 codes below maksidze's and showing about twice the spread. That is what
per-device factory calibration looks like, and it is not what a compiled-in default
table looks like (the stock image's defaults are large values of a different shape).

The pair structure matches the documented meter DAC path exactly: `CLAUDE.md` records
cal tables at RAM `0x20000394` (upper bounds) and `0x20000358` (baselines), 2 bytes per
entry, combined as `DAC = ((upper - baseline) / divisor) * (offset + 100) + baseline`
and written to `0x40007408`. Pairs of (baseline, upper) per range is precisely the
shape seen here.

**Conclusion: per-device factory calibration DOES exist on this hardware, it lives in
MCU internal flash at `0x08006000`, and it is confined to roughly `0x025–0x12E`.**
Two independent units agree on layout and disagree on values. This is also the region
our own app overwrites, so it must be preserved or archived before flashing a unit
that has never been dumped.

## THIRD UNIT + ARRAY ALIGNMENT SETTLED — 2026-08-17

`unit_dendi_mcu_settings_page_0x08006000.bin` — a third unit, dumped by its owner and relayed
by maksidze (issue #18, 2026-08-16). Diffs against the other two:

| pair | differing bytes | of which in cal 0x025-0x12E |
|---|---|---|
| unit1 vs maksidze | 229 | 130 |
| unit1 vs dendi    | 268 | 161 |
| maksidze vs dendi | 184 | 162 |

All three differ pairwise across essentially the whole calibration region while the erased
remainder (0x1FC-0xFFF) stays byte-identical `0xFF`. Per-device factory calibration is
established beyond the two-unit case.

**Alignment/endianness pinned.** The array is little-endian `uint16` starting at page offset
**0x026**. At that alignment 129/132 entries land inside 12-bit DAC range; the other three
candidate alignments give ~7/132. (An earlier writeup quoted the same cluster values from a
big-endian-at-0x025 read — coincidentally similar, but 0x026/LE is the correct parse.)

**Structure: two blocks of 30 (baseline, upper) pairs.**

| block | offsets | unit1 | maksidze | dendi |
|---|---|---|---|---|
| 1 | 0x038-0x0AE | 1598-1628 / 1696-1717 | 1639-1667 / 1657-1705 | 1509-1676 |
| 2 | 0x0B0-0x126 | 3195-3221 / 3283-3306 | 3230-3290 | 3145-3282 |

30 entries per table matches the decompiled RAM cal tables exactly — baselines at `0x20000358`,
upper bounds at `0x20000394`, delta 0x3C = 60 bytes = 30 x uint16. Block 2 sits at roughly 2x
block 1.

Dendi's page carries a handful of out-of-range outliers (62808, 50862, 27857, 27887, 31944)
scattered through otherwise well-formed entries; not yet explained, and worth keeping in mind
before treating any single unit's page as a clean reference.

## `unit4_mcu_settings_page_0x08006000.bin`, 2026-11-24

4096 bytes, MCU flash `0x08006000–0x08006FFF`, from **my unit** (s00500's; its files
carry the prefix `unit4_`), a new 2C53T dumped the day it arrived.

```
sha256  cba81a6af60723ceae1fd97ece838a865714e6be3489e899c0c502a8807a2802
crc32   1CB45339
```

**Provenance.** Shipped with stock V1.2.0 (About screen). It booted stock once as
shipped and no settings were changed. Then it went through the factory IAP (MENU + Power)
in this order:

1. v0.4.1 `caldump` (sha256 `fb432c31…`, matches the release `SHA256SUMS`). The LCD
   reported CRC32 `1CB45339`. Photos of every screen were kept.
2. v0.4.1 `coldtrace`. With no button pressed (`settings`: `writes ok: 0`),
   `scripts/dump_factory_bootloader.py` read `0x08000000–0x08007000` over the CDC shell,
   and this file is bytes `0x6000–0x6FFF` of that read. The whole read is
   `unit4_mcu_flash_0x08000000-0x08007000.bin` (28672 B, sha256 `136b1ebd…`).

Both readings give CRC32 `1CB45339`. So this is a pristine-unit page, in the same sense
as unit #3's in #28: stock had run on it, and no custom firmware had run before the
read.

**Content.** 344 non-`0xFF` bytes in `0x000–0x803`, signature byte `0x55`. Against
unit #1, by the regions above: header `0x000–0x02F` 7/48 differ, **calibration
`0x030–0x12F` 167/256 differ**, tail `0x130–0x1FF` 184/208 differ.

The rest of the same read, `0x0000–0x5FFF`, is the factory IAP bootloader, and it is
byte-identical to `../factory_iap_bootloader_2C53T.bin`.

The same unit's full W25Q is `../w25q128_unit4_2026-11-24.bin` (16 MB, sha256
`f4a6c86c…`, read once over the shell; kept locally with a `.sha256` file, not committed). Its SPI flash reports JEDEC `85 20 18`, Puya's
ID rather than Winbond's `EF 40 18`. `0xF10000` in that image already holds 256 B of
OpenScope settings records, because buttons were pressed while it was being read. A
read of `0xF00000–0xFFFFFF` taken before the first save had that region all `0xFF`.
Posted to issue #28: comment 7143340561.

## `dads_unit_mcu_settings_page_0x08006000.bin` (Dad's unit), 2026-11-28

4096 bytes, MCU flash `0x08006000–0x08006FFF`, from **Dad's unit**, a 2C53T that had
been in use on stock firmware V1.2.0 (About screen) for a few weeks before this read.

```
sha256  b8473228e9653568696dbdc0dcf06c6e92cc9f5fd4ceb01a01b9a047cbab9c64
crc32   AA1929D2
```

**Provenance.** Stock only until this session. Then, through the factory IAP (MENU + Power):

1. v0.4.1 `caldump` (sha256 `fb432c31…`, matches the release `SHA256SUMS`). The LCD
   reported CRC32 `AA1929D2`, 511 non-`0xFF` bytes in `0x000–0x803`, and against the
   embedded unit #1 page: header 19/48, calibration 129/256, tail 27/208. Photos of the
   screens were kept.
2. The fork's `coldtrace` build of 2026-11-28 12:06:41 (sha256 `d1595907…`). With no
   button pressed (`settings`: `writes ok: 0`, `changes seen: 0`),
   `scripts/dump_factory_bootloader.py` read `0x08000000–0x08007000` over the CDC shell,
   and this file is bytes `0x6000–0x6FFF` of that read. The whole read is
   `dads_unit_mcu_flash_0x08000000-0x08007000.bin` (28672 B, sha256 `63256a38…`).

Both readings give CRC32 `AA1929D2`, and the region split computed on the host from this
file matches the LCD's numbers exactly. The rest of the read, `0x0000–0x5FFF`, is the
factory IAP bootloader, byte-identical to `../factory_iap_bootloader_2C53T.bin`.

**Content.** 511 non-`0xFF` bytes in `0x000–0x803`, signature byte `0x55`, `8KTA` at
`0x800`. Read as `[channel][bank][range]` (maksidze, #28: `uint32` words, CH1 in the low
half, CH2 in the high half, baselines at `0x038–0x0AF`, upper endpoints at `0x0B0–0x127`),
the spans (upper − baseline) are **CH1 1545–1552, CH2 1535–1549**, the lowest of the 11
pages below: about 3 % under unit #1 and 6 % under my unit. Gain trim at `0x12C`
(sparsematrix's reading): 186 / 187.

The tail `0x130–0x1FF` has 206 non-`0xFF` bytes, so this page is in the "full tail" group
(unit #1, unit #3, sparsematrix, saul's SWD read). That fits the guess in my #28 comment
that the 40-byte tail is the page before stock first rewrites it: this unit ran stock
for weeks, with settings changed.

Against every page posted on #28 and #18 (calibration region `0x030–0x12F`; "cal CRC" is
CRC32 over those 256 bytes):

| page | page CRC32 | cal CRC | differs from Dad's | span CH1 | span CH2 | k `0x12C` |
|---|---|---|---|---|---|---|
| **Dad's unit** | `AA1929D2` | `FFC4623E` | – | 1545–1552 | 1535–1549 | 186/187 |
| my unit (`unit4_`) | `1CB45339` | `BFA16E55` | 169/256 | 1646–1653 | 1633–1650 | 178/181 |
| unit #1 | `59E91404` | `41BCBA9C` | 129/256 | 1587–1602 | 1581–1599 | 184/182 |
| unit #2 (Stlkv) | `DFD5381C` | `035C7AAA` | 126/256 | 1566–1577 | 1535–1602 | 185/186 |
| unit #3 (mquerostudio) | `711FB4A9` | `1B84C662` | 128/256 | 1584–1594 | 1577–1611 | 183/184 |
| saulvalenzuela23, caldump | `A9DFD19B` | `41C2BBB7` | 129/256 | ≤1611 ¹ | 1588–1607 | 182/185 |
| saulvalenzuela23, SWD | `EA6B2727` | `41C2BBB7` | 129/256 | ≤1611 ¹ | 1588–1607 | 182/185 |
| sparsematrix | `09D0CEF5` | `D93A27B5` | 158/256 | 1609–1622 | 1612–1623 | 194/178 |
| githubalf (`0x000–0x12F` only) | – | `BA603753` | 121/256 | 1614–1625 | 1593–1602 | 182/184 |
| michkovitalik (unmarked board) | `0C1DE23E` | `0C98AA61` | 129/256 | 1580–1588 | 1487–1542 | 178/181 |
| dendi (via maksidze, #18) | `6D0EA2E2` | `3F059945` | 159/256 | – ¹ | 1631–1647 | 181/179 |

¹ These pages carry the out-of-range outlier words noted above, so the CH1 range is not
meaningful. Source files: the zip attachments on #28 and #18, downloaded 2026-11-28 (not
committed here). cocus's page on #12 was posted as hex text only and is not in the table.

Restore (ROM DFU, case open, BOOT0 + pinhole reset):

```bash
dfu-util -a 0 -d 2e3c:df11 -s 0x08006000 -D archive/factory_cal/dads_unit_mcu_settings_page_0x08006000.bin
```

The same unit's full W25Q is `../w25q128_dads_unit_2026-11-28.bin` (16 MB, sha256
`be8f6bdb…`, kept locally with a `.sha256` file, not committed). It was read in
16 x 1 MB pieces over the shell, all on the first try, with `settings` still at
`writes ok: 0`. JEDEC `85 20 18`, Puya, like my unit. Used 64K blocks:
`0x000000–0x33FFFF`, `0xCB0000–0xCCFFFF`, `0xFF0000–0xFFFFFF`; `0xF10000` (OpenScope
settings) is all `0xFF`, so no OpenScope write had happened. Against my unit's W25Q,
only `0x200000–0x26FFFF` differs (apart from `0xF10000`): that is the second FAT12
volume, which here holds one stock screenshot (`Screenshot file/1.bmp`, a CH1 square
wave) and one `Screenshot simple file/1.bin` (607 B). The UI asset volume at
`0x000000` is identical.

Posted to issue #28 as "my unit 2": comment 7241721486 (zip `my-unit-2-caldump-0x08006000.zip`,
checked after upload: same sha256 as this file).
