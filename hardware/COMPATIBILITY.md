# Hardware compatibility

One row per hardware revision from 1.0.0 onwards, kept current as revisions
are released and validated. Spins before 1.0.0 are prototypes without a
marking or an ID code; the second table resolves their old names. The policy
behind both tables is in [RELEASING.md](RELEASING.md).

## Columns

- **Revision**: the `HW_REV` value of the design, printed on the board as
  `MBX HW <rev>` and shown in the schematic and PCB title blocks.
- **Tag**: the `hw-v<rev>` Git tag and GitHub release that holds the
  fabrication outputs. The committed record of a release lives under
  [`releases/hw-v<rev>/`](releases/).
- **ID code**: the value the firmware reads from the board-ID pads, as defined
  in [BOARD-ID.md](BOARD-ID.md).
- **Status**: `planned` (design in progress, no tag yet), `released for
  fabrication` (tagged, physical validation pending) or `validated` (tested on
  manufactured boards, with a validation record), as defined by the lifecycle
  in RELEASING.md.
- **Minimum firmware**: the oldest firmware release that supports the
  revision. Older firmware may run but is outside the compatibility claim.
- **Tested firmware**: the firmware releases exercised on a physical board of
  this revision, with a link to the validation record.
- **Notes**: design changes, known issues and anything a user of the board
  must know.

## Revisions

| Revision | Tag | ID code | Status | Minimum firmware | Tested firmware | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| 1.0.0 | `hw-v1.0.0` (not yet tagged) | 1 | planned | pending: the first firmware release whose version generation and update discovery ignore hardware tags | none | First marked revision. Adds the board-ID pads, the USB bridge and the J7 isolation header. A pre-release gerber export passed a JLCPCB DFM check on 2026-10-18; no board has been fabricated. |

## Historical spins

All spins before 1.0.0 are prototypes: unsupported, unmarked and outside the
table above. This table exists so that the old names can be resolved. The
gerber folders named here were removed from the tree when the release policy
was adopted; history keeps them, and they are present at commit `1fd1ce6`
(2026-10-21).

| Name as used | Where | Source commit | Date | Notes |
| --- | --- | --- | --- | --- |
| v0.1-alpha | tag `v0.1-alpha-hw`, `releases/v0.1-alpha` | `335340e` | 2025-07-11 | First alpha; release notes only |
| v0.2-alpha | tag `v0.2-alpha-hw`, `releases/v0.2-alpha`, formerly `kicad_files/manufactoring/rev2` | `b85d923` | 2025-10-06 | Gerbers, PDFs and BOM as release assets |
| rev3 | formerly `kicad_files/manufactoring/rev3` | `3e30e70` | 2026-01-28 export | New power supply, trace refinements; untagged; fabrication status unknown |
| v0.5 | inventory assembly IPN `MBX-carrier-board-v0.5` | n/a | 2026-01-06 | Inventory label only; does not correspond to a tag |
| 0.1 | schematic title block until 2026-10-21 | n/a | 2025-07-06 | Never updated; replaced by `${HW_REV}` with the revision-identity change |
