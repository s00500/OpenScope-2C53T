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
| 392ff97 | chore: ignore `.scratchpad/` and `.playwright-mcp/` | none needed | yes | trivial; fold into any PR that wants it |
| d11f69c | exp(79): the generator against a Rigol DS1104Z | EXP-79 file + `reverse_engineering/captures/exp79/` | yes | evidence for #68; offer as a comment with the user's approval |

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
  GEN into the 2C53T's own inputs (loopback) has not been run yet.
