# Fork work and how it goes back upstream

We work forward on the fork (`fork` = github.com/s00500/OpenScope-2C53T) and
re-contribute to upstream (`origin` = DavidClawson/OpenScope-2C53T) in small,
separate PRs. This file is the ledger: one row per change we carry, what it
depends on, and where it stands upstream. Keep it current with every commit
that adds or changes a row.

## Branches

| branch | where | what |
|---|---|---|
| `feat/remote-control` | worktree `../OpenScope-2C53T-remote` | the integration branch: PR #47's head (stacked on #41) + `origin/main` merged (8d9ccc3) + our commits |
| `fix/scope-freq-hang` | worktree `../OpenScope-2C53T-freqfix` | topic branch off `origin/main` for PR #108 |
| `fix/acq-railed-input` | worktree `../OpenScope-2C53T-acqgate` | topic branch off `origin/main` for the clipped-input fix (EXP-82), **PR #109** |

The Go host CLI is a separate repository (`../openscope-cli`), not part of
this repo.

## Ledger

Applies-to-main column: checked 2026-11-25 against `origin/main` 7480998 with
`git apply --cached --check` on a scratch index (`--3way` where noted).

| commit on `feat/remote-control` | change | evidence | applies to `origin/main` | upstream |
|---|---|---|---|---|
| 0f8f997 | fix(scope): frequency estimator hung the display on flat inputs | EXP-76 part 2/3, `test_scope_freq` flat fixture | yes (it is #108's commit, cherry-picked) | **PR #108 open** |
| 8ceb6b6 | feat(remote): M6 control surface (GET_STATE, SET_MODE, SET_PARAM, ACTION) | `test_remote_proto` 203 checks, 16 guard mutations; remote_protocol.md section 10 | no: stacked on #41 / #47 | blocked on #41 / #47 (mquerostudio); coordinate on issue #10 first |
| 302e268 | fix(fpga): setting change timed out on the post-edge sleep (`fpga parkwake`) | EXP-76: 3/3 bursts refused off, 3/3 accepted on | yes | not proposed yet; candidate |
| 35c7f00 | fix(scope): one vertical map for every renderer (comb, stale readout strip, true-scale jump) | EXP-77, `test_scope_vmap` | yes with `--3way` (only a neighbouring Makefile line differs) | not proposed yet; candidate |
| 083dc65 | feat(display): `display` breadcrumb, `lcd status/reinit/sleep`, self-healing panel | EXP-76, EXP-78, `test_display_trace` | no: Makefile and usb_debug.c context sit on #41/#47 changes; small port needed | the self-heal and `lcd` readback **duplicate #85's `pr/lcd-health-check`** (6119415, deOliveira-R, earlier): upstream should take theirs; offer only the `display` breadcrumb, ported onto theirs |
| 2438e09 | lcd: hold PD6 (RESX) high, write cycle 17 -> 67 ns | cherry-pick of deOliveira-R's 2c68e37 (#85); on my unit: 0 panel resets over many UP presses, 1-2 per press before | yes (it is their commit) | **theirs**: #85, branch `pr/lcd-reset-and-timing`; we support it with our evidence (comment draft) |
| 3ca73da | siggen: stock's DAC2/PA5 + TMR7 + DMA2 ch4 generator, 13 waveforms (replaces the DAC1 driver that never reached GEN) | cherry-pick of deOliveira-R's 4bf87c0 (`bench/parity`, #68); EXP-79 on my unit; `test-siggen` 61 checks | yes with `--3way` | **theirs**: offered in #68 (mquerostudio's and deOliveira-R's comments), no PR yet; we support it with EXP-79 |
| 3a9f327 | scope_trigger: mask interrupts around the DAC_CTRL read-modify-write (DAC1 and DAC2 share the register) | cherry-pick of deOliveira-R's b671235 | yes | **theirs**, goes with 3ca73da |
| 0d97f93 | remote: `sg.*` keys follow the DAC2 generator (0.1 V amplitude, integer Hz, offset read-only, 13 waves) | `test_remote_proto` 203 checks; CLI params test | no: stacked on #41 / #47 (M6) | with M6 |
| c95a483 | remote: `sg.freq.actual` (0x46, f32 RO), the frequency TMR7 really produces | `test_remote_proto` 205 checks; on my unit against the Rigol counter | no: stacked on #41 / #47 (M6) | with M6 |
| 392ff97 | chore: ignore `.scratchpad/` and `.playwright-mcp/` | none needed | yes | trivial; fold into any PR that wants it |
| d11f69c | exp(79): the generator against a Rigol DS1104Z | EXP-79 file + `reverse_engineering/captures/exp79/` | yes | evidence for #68; offer as a comment with the user's approval |
| 64dc401 | fix(siggen): bottom bar shows the live generator state; the screen stays above the bar | screenshots on my unit (output on/off, remote change repaints the bar) | not checked; status_bar.c on main is the same code, main.c context may differ | candidate, after the DAC2 generator lands (it uses its API) |
| 989f144 | fix(acq): a clipped input no longer freezes the scope (`acq_gate.h`) | EXP-82; `test_acq_gate` 12 checks (old gate fails 4); on my unit railed levels commit at the normal rate | yes with `--3way` | **PR #109** (opened 2026-11-26 with the user's approved text): branch `fix/acq-railed-input` (6e500b6, off `origin/main` 7480998) |
| 0a722fd | exp(80): GEN's absolute level after the Rigol's Self-Cal | EXP-80 + `captures/exp80` | yes | evidence for #68 |
| aa165cc | exp(81): timebase codes 0x06..0x14 are the round ladder to 50 ppm | EXP-81 + `captures/exp81` | yes | evidence for PR #51 / #84 (a second and third unit on the same ladder) |
| c42bbe9 | exp(83): CH1 gain per range (1-2-5 ladder), crosstalk, frequency badge | EXP-83 + `captures/exp83` | yes | evidence for a `scope_cal.c` revision (rows 4/8/9) |
| ef70eba | cal: timebase (0x06-0x14) and gain tables (both channels, source scale 1.0) from EXP-81/83/84, guard tests rewritten; EXP-84 (TMR13/PA6 is CH2's offset reference) | `test-scope-timebase`, `test-scope-cal`, `test-scope-cursor`; fold test; screenshots | no | needs a maintainer decision upstream: it replaces unit #1's rows and overturns guards; overlaps PR #51 / #84's rows |
| b8d9449 | emu: Renode 1.17 loads `gd32f307_2c53t.repl` (I2C model rename, SPI series); emulator build ignores the physical key scan; LCD model flushes on NOP | `make emu` + `scripts/emu_shot.py` on Renode 1.17.1 | not checked | candidate: small, emulator only |
| 0b96edd | perf(lcd): shadow position stepped, colour cache direct-mapped (EXP-85) | live frame 85.4 -> 83.9 ms on my unit; screenshot checked | not checked | candidate |
| c0611c3 | fix(scope): cursors, band labels and the readout through the live compositor; cursor keys via `DCMD_SCOPE_LIVE` and the gate's live epoch; popup sized to its text; cursor keys time view only; `display` reports draws and live-frame cost; emulator renders the live path (EXP-85) | EXP-85: cursors on 23 -> 0 full repaints per 10 s, cursor presses 0; `test-band-holes`, `test-redraw-gate` (cursor-drag mutant); all host tests, shell table, firmware build | not checked (scope_ui.c / main.c moved a lot upstream) | candidate after the user has looked at the glass |
| a8f74a1 | feat(scope): trigger panel (TRIGGER), source on reg 0x06, cursors on SELECT, CH1/CH2 select/off/on with PC1/PC2 routing, remote `trig.source`/`chN.enabled` reach the hardware (EXP-86) | EXP-86: NORMAL with CH2-only signal, source CH1/CH2 = 0/45/0/43 commits per 10 s, generator-off 0; `test-trig-menu`; panel and channel keys on my unit | no: stacked on c0611c3 (chips) | overlaps deOliveira-R's f4dfc23 (their panel; our adaptation credits it): coordinate before offering; the key remap is the user's design |
| 8c0aea2 | feat(scope): MOVE modes own both axes (Scale / Trigger / Position, plus Mask and Cursors when present), vertical position through the shared vertical map, mode tag in the top row, Trigger/Position keys via the live compositor (EXP-87) | EXP-87: 13 Trigger/Position/MOVE presses on my unit = 0 full repaints, control (V/div up/down) = 2; `test-move-mode` (mutant caught), `test-scope-vmap` (offset) | no: stacked on a8f74a1 and c0611c3 | the key layout is the user's design: offer only after they have used it |
| 230bddd | fix(scope): volts/div and time/div through the live compositor (info bar written in place, badges refreshed once per change from the first new record), trigger-point marker below the labels with a line in Trigger/Position, channel reference flags from the shared map (EXP-88) | EXP-88: Scale presses 0 full repaints on my unit, control (run/stop) 2, frame rate 84-85 per 10 s unchanged; `test-scope-vmap` (reference row) | no: stacked on 8c0aea2 | with the MOVE modes |
| 390cd0b | feat(scope): calibrated 0 V per channel and range (zero model in scope_cal), ranges 2-4 in the gain table, "1"/"2" flags at 0 V, trigger level in volts in the Trigger-mode popup, CH2 boot offset from the model (2114, was 2544), `fpga scope zero` (EXP-89) | EXP-89: shorted zeros on both channels, model within 0.29 counts; GEN cross-check through both channels (GEN 0 V +8.0 / +7.0 mV); grounded CH2 48.69 vs 48.85 predicted at the new boot code; trigger sample within 3 counts of the printed level on both channels; `test-scope-cal` (mutants caught), `test-scope-vmap`, `test-scope-cursor` | no: stacked on 230bddd (flags) | the numbers are my unit's: upstream needs a per-unit story first (the zero and gains differ between units)
| 31c0873 | feat(scope): SPI3 capture counter behind Settings > Oscilloscope > Debug Info (default off, saved in the settings record's padding byte, v3 layout kept); the Oscilloscope sub-menu scrolls (Trigger Edge was never drawn) | `test-config-persist` (old-layout record loads, round trip, mutant caught); on my unit: settings loaded unchanged after the flash, off/on/off with both toggles written; emulator: menu scrolls | partly: the setting, the record field and the menu scroll are independent; the gate sits in the top-row chip code from c0611c3 | the scroll fix stands alone and is worth offering first |
| 7e0f87f | feat(scope): a channel moved by Position may cross into the other's half and overlay it (vertical map clip separate from the band), Position ±5 divisions, H-cursor dV on the active channel's scale | `test-scope-vmap` (cross into the bottom half, control flattens on the band edge, no-op clip mutant caught); emulator walk; my unit: CH1 down 5 divisions under CH2 | no: stacked on 8c0aea2 (Position) and 230bddd (flags) | the user's call ("any normal scope allows me to overlay the channels") |

## How to cut an upstream PR

1. `git worktree add --no-track -b <topic> ../OpenScope-2C53T-<topic> origin/main`
2. `git cherry-pick <commit>` (resolve the context-only conflicts noted above)
3. Run what the row lists as evidence, plus `python3 scripts/test_firmware_build.py`
   and `python3 scripts/test_shell_table.py` when usb_debug.c is touched.
4. Push the topic branch to `fork`; the PR text is the user's to approve before
   it is opened (never post upstream without that).
5. When upstream merges it, rebase or merge `origin/main` into the
   integration branch; a cherry-picked commit with the same patch drops out.

## Errata

- EXP-76/77/78 ran on **my unit**. Commit messages 302e268, 35c7f00
  and 083dc65, the fixture header `scope_records_flat.txt` and PR #108's
  description say "bench unit #1"; that is wrong. The experiment files are
  corrected; the rest is corrected when each change is cut for upstream
  (PR #108's text only with the user's approval).

## Open findings (not yet changes)

- EXP-77: the zero offset. A grounded CH1 reads ~46 counts, not mid-scale 128,
  on every range; true scale clamps a grounded trace to its band edge.
  `fpga scope center` exists; per-range zero and whether to persist it is open.
- The display loop runs ~8-10 passes/s in scope mode; full repaints dominate.
- Screenshots of a running trace come out torn; the CLI pauses RUN for them,
  which shows STOP in the picture. A firmware-side fix would hold the display
  task for the ~0.5 s transfer instead.
- No signal larger than noise has been on the inputs in these sessions: the
  frequency badge on a real tone and NORMAL trigger are untested on the device
  with these changes.
- EXP-79: the generator's absolute level is unresolved (the Rigol needs
  Self-Cal; its DC reading moves 0.2 to 1.36 V with its bandwidth limit).
  Resolved by EXP-80 after the Rigol's Self-Cal.
- EXP-81/83 (loopback): the timebase table is 0.07-1.06 % off the measured
  ladder and misses 0x06..0x0C; the provisional gain rows 4/8/9 are 25-65 %
  off. Whether to rewrite those tables is open (their guard tests forbid
  tidying rows; the new numbers are measured, on another unit).
