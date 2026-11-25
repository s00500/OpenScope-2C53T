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
| 083dc65 | feat(display): `display` breadcrumb, `lcd status/reinit/sleep`, self-healing panel | EXP-76, EXP-78, `test_display_trace` | no: Makefile and usb_debug.c context sit on #41/#47 changes; small port needed | not proposed yet; candidate once EXP-78's physical-press check is done |

## How to cut an upstream PR

1. `git worktree add --no-track -b <topic> ../OpenScope-2C53T-<topic> origin/main`
2. `git cherry-pick <commit>` (resolve the context-only conflicts noted above)
3. Run what the row lists as evidence, plus `python3 scripts/test_firmware_build.py`
   and `python3 scripts/test_shell_table.py` when usb_debug.c is touched.
4. Push the topic branch to `fork`; the PR text is the user's to approve before
   it is opened (never post upstream without that).
5. When upstream merges it, rebase or merge `origin/main` into the
   integration branch; a cherry-picked commit with the same patch drops out.

## Open findings (not yet changes)

- EXP-78: root cause of the panel's hardware reset on a physical UP press
  (mechanical flex of the board / LCD FPC, or ESD) is not established; needs a
  look with the case open. The firmware recovery hides it.
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
