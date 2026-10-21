# Hardware revision and release management

Status: accepted on 2026-10-16 after review; committed on 2026-10-21. This
document defines the policy and implements no release or workflow change by
itself.
Date: 2026-10-16
Scope: Modbus-to-X hardware in this repository, from revision 1.0.0 onwards.

## Objective

Make every manufactured board traceable to its design sources, assembly
definition, fabrication outputs and validation results. Keep hardware versions
independent of firmware versions, with explicit records of their compatibility,
and let the firmware report which board it is running on.

Continue using this repository for both stacks. A separate hardware repository
or a permanent branch for each board revision is unnecessary: immutable tags
identify released designs, while working branches contain changes under review.

## Situation at adoption

This is the state the policy was written against, on 2026-10-16. It is kept
as the rationale and is not updated; [COMPATIBILITY.md](COMPATIBILITY.md)
and the release records show the current state.

- The board has been numbered four different ways. Git tags say
  `v0.1-alpha-hw` and `v0.2-alpha-hw`; the schematic title block says `0.1`
  dated 2025-07-06; the two tracked gerber folders are named `rev2` and
  `rev3`; the component inventory lists the assembly as
  `MBX-carrier-board-v0.5`. None of these agree.
- The `rev3` gerbers were exported on 2026-01-28, after the power-supply
  change and trace refinements that followed the `v0.2-alpha-hw` tag. They
  were never tagged or released, and whether boards were fabricated from them
  is not recorded.
- The PCB file has no title block and the silkscreen carries no revision
  text. Manufactured boards are unmarked.
- `hardware/releases/` holds notes for 0.1 and the PDFs and gerber archive for
  0.2. Both historical tags also exist as GitHub prereleases, each carrying a
  BOM, PDFs and a gerber archive as assets.
- Firmware releases use `v*` tags and the Release OTA workflow. Version
  generation in `software/modbus-to-mqtt/scripts/gen_version.py` uses
  unrestricted `git describe --tags`, so any tag can become a firmware version.
  Release OTA triggers on any `v*` tag, finds the previous release with
  `--match "v*"`, which also matches the historical `*-hw` tags, and lets
  GitHub pick the previous release for generated notes.
- The device's update check depends on GitHub releases directly. With
  prereleases enabled it takes the newest release of any kind from the
  releases API and derives the manifest URL from that tag. With prereleases
  disabled it uses GitHub's "Latest" release. A hardware release published
  today would therefore break update discovery for one or both groups of
  devices until the next firmware release.
- The firmware has no way to identify the board it runs on.
- Component data in the schematic was cleaned up on 2026-10-16: all 46 placed
  components now carry `MPN`, `Manufacturer` and `StockLocation` fields aligned
  with the inventory. Still open: J3 (screw terminal, no part chosen), J6 (no
  stock, no bin), and datasheet links for the generic headers J2/J7 and SW1.
  The superseded `PartNo`, `InventoryId` and `MANUFACTURER` columns remain
  until removed in KiCad's Symbol Fields Table.
- USB connectivity, the UART bridge, the J7 isolation header and library
  consolidation are among the changes awaiting commit on the current branch.

## Revision identity

Use the following conventions for future releases. Preserve existing tags and
historical release directories unchanged.

| Identity | Convention | Example |
| --- | --- | --- |
| Hardware release tag | `hw-v<major>.<minor>.<patch>` | `hw-v1.0.0` |
| Firmware release tag | Existing `v<version>` convention | `v8.1.1-beta` |
| Physical board marking | Product and complete hardware revision | `Modbus-to-X HW 1.0.0` |
| Release title | Explicit hardware prefix | `Hardware 1.0.0` |
| Source identity | Full Git commit ID | Recorded in release manifest |

The next fabricated board is `1.0.0`. Starting at 1.0.0 draws a line under
the prototype spins and ends the numbering collision above. Hardware
revisions carry no pre-release suffix: a board can carry only one marking,
and maturity is tracked separately (see the lifecycle below), so `-alpha` or
`-rc` would either become permanent on the silkscreen or create two names for
one design.

Use three numeric fields as a project convention, rather than claiming strict
software SemVer semantics:

- Major: a new hardware generation with incompatible external electrical,
  mechanical or firmware requirements.
- Minor: a board spin introducing features, substantial layout changes or
  interface changes within the current generation.
- Patch: a small corrective fabrication or assembly change within a board spin.

Any released change to copper, footprints, component population, board geometry
or printed identification receives a new revision. Compatibility alone does not
justify reusing an existing revision. Documentation-only changes can be ordinary
commits referencing the affected release.

Initially, use one revision for the PCB and its defined assembly. If multiple
population variants become necessary, add an explicit assembly variant as a
second project text variable and a separate BOM; do not silently ship different
populations under the same identity.

### Historical spins

All spins before 1.0.0 are prototypes: unsupported, unmarked and outside the
compatibility table. The table that resolves their old names, and the commit
that still contains the removed gerber folders, is in
[COMPATIBILITY.md](COMPATIBILITY.md#historical-spins).

## Development and release lifecycle

Track maturity separately from the revision number:

1. **Draft:** design changes and review are in progress. The branch and commit
   identify snapshots; a planned revision is not a fabrication release.
2. **Released for fabrication:** reviewed sources and generated outputs are
   frozen at an annotated hardware tag. Manufacturing may proceed, but physical
   validation is still pending.
3. **Validated:** testing on manufactured hardware is documented with board
   revision, assembly details, tested firmware and observed results.

Do not move a published tag or replace fabrication assets. Corrections that
change the manufactured design require a new revision and release. Add later
validation evidence as a dated record tied to the original tag and commit;
never imply that it existed at fabrication release time.

Hardware GitHub releases are always created with `--prerelease --latest=false`.
The prerelease flag states that validation is pending and may be cleared
later; the latest flag must never be set on a hardware release, because the
device's default update path reads GitHub's "Latest" release. Neither flag is
a substitute for the firmware-side filtering described under automation.

Every hardware release also carries a verbatim copy of the newest firmware
release's `modbus-to-x.manifest.json`. Devices with prereleases enabled
resolve the newest release of any kind and fetch the manifest from it; the
manifest is signed, names the device and points at the firmware release's own
assets, and the device never checks which release it came from. With the copy
in place such a device finds the same manifest it would have found without
the hardware release, so update discovery keeps working on every firmware
ever shipped. The release workflow refuses to publish without this asset. No
hardware release may change how an existing device operates; update discovery
is the only path a release can reach, and both paths are covered.

## Revision metadata

The single source of truth for the revision is a project text variable
`HW_REV` in `hardware/kicad_files/modbus-to-x.kicad_pro`. Everything else
references it:

- The schematic and PCB title block revisions use `${HW_REV}`.
- One silkscreen text carries the physical marking, built from `${HW_REV}`.
- Exports generated by `kicad-cli` resolve the variable, so PDFs, gerbers and
  the BOM show the same value.
- The release check reads the variable directly and compares it with the tag:

  ```bash
  jq -r '.text_variables.HW_REV' hardware/kicad_files/modbus-to-x.kicad_pro
  ```

No separate metadata file is kept. Development maturity lives in the release
and validation records, so later testing never changes the frozen design
identity.

## Board identification

Add a hardware ID to the 1.0.0 respin so the firmware can report the board
revision. The exported netlist of the current design confirms
that IO32, IO34, IO35, IO36 (SENSOR_VP) and IO39 (SENSOR_VN) are unconnected,
and the firmware uses none of them. None is a boot-strapping pin.

- `HW_ID_PRESENT` on IO32. This is a normal GPIO with an internal pull-up that
  the firmware enables. Boards from 1.0.0 onwards fit a pull-down resistor, so
  the pin reads low. Every earlier board leaves it open and reads high, which
  gives a deterministic "no ID" result. Without this flag the input-only pins
  below would float on a legacy board and could read as any code.
- `HW_ID0`, `HW_ID1` and `HW_ID2` on IO34, IO35 and IO36. These are input-only
  pins without internal pulls, so each gets a pull-up pad and a pull-down pad
  of which exactly one is fitted. IO39 stays spare for a fourth bit.
- The code is read as ID2 ID1 ID0. 1.0.0 is code 1: ID0 pulled up, ID1 and ID2
  pulled down. Codes 2 to 7 remain for later revisions.
- The firmware reads the pins once after boot and shows the result next to
  the firmware version in the system stats and the web UI. A board without
  the pads shows `<1.0.0`; a code without a table entry shows the raw code.
- Nothing else is built on the value: no mismatch handling, no refusal, no
  per-revision code paths. If a future revision ever has to move a
  firmware-visible pin or change a polarity, that is a new major version with
  its own build target and OTA device name, and the compatibility table
  records it.
- [COMPATIBILITY.md](COMPATIBILITY.md) records the ID code in each revision's
  row.

## Component data and BOM

The release BOM must let a person assemble the board from the inventory
without opening the schematic. Every placed component therefore carries:

- `Manufacturer` and `MPN`. The MPN is written exactly as the inventory part
  number (IPN), including for generic parts, so the two match by equality.
- `Datasheet`, as a link.
- `StockLocation`, naming the inventory bin. The MPN is the stable key; the
  bin is a convenience that can be refreshed from the inventory.

Two checks follow from this:

- A repository check parses the schematic and fails when any placed component
  lacks one of the fields above, unless the component is mechanical or
  explicitly marked do-not-populate. This can run in CI.
- An owner-run check looks each component up in the inventory by IPN = MPN
  and compares the bin field with the part's current stock location (or its
  legacy placement parameter while the inventory migration is incomplete),
  normalising whitespace. It belongs in the pre-release checklist, not CI,
  because the inventory is private.

The BOM export includes reference, value, footprint, quantity, manufacturer,
part number, datasheet and bin, plus a do-not-populate column.

## Release contents and storage

Keep editable design sources, project-local libraries, the `HW_REV` variable,
the export configuration and human-written notes in Git. Generated binaries
belong to the GitHub release, not to the tree:

- The prototype-era gerber folders and the exported `modbus-to-x.step` were
  removed from the tree when this policy was adopted. History keeps them,
  the historical spins table in COMPATIBILITY.md names a commit that
  contains them, and `.gitignore` keeps the local KiCad export folder and
  the exported STEP model out of git. AGENTS.md and README.md describe
  fabrication outputs as release assets.
- Keep `hardware/releases/v0.1-alpha` and `v0.2-alpha` unchanged for
  continuity.
- For each new release, add `hardware/releases/hw-v<rev>/` in a follow-up
  commit after the tag, holding the manifest, the release notes and later
  validation records as text. Release assets can be replaced by anyone with
  write access; a committed manifest with checksums makes that detectable.
  GitHub's immutable-releases repository setting, where available, is a
  second layer, not a replacement.
- `.gitignore` excludes all CSV and XML. The BOM is a release asset, so that
  is fine; JSON jobsets and manifests are unaffected.

Each hardware release includes:

- Gerbers and drill files, with fabrication and stackup requirements.
- Assembly BOM as described above, plus placement data with documented units,
  origin and rotation convention.
- Exported schematic and PCB PDFs for review and reference.
- DRC/ERC reports and a reviewed list of accepted exceptions.
- Release notes describing design changes, compatibility, the board ID code
  and known issues.
- A manifest recording revision, full source commit, exact KiCad and tool
  versions, export configuration and SHA-256 checksums of payload artifacts.
  Publish a checksum for the manifest separately if desired; avoid a
  self-referential checksum entry.

Produce the package from a clean checkout of the tagged commit with pinned
tools, driven by a committed KiCad jobset that is used identically on a
workstation and in CI. Record generation time, but do not promise
byte-for-byte reproducibility until timestamp and other nondeterministic
output differences have been addressed.

## Automation changes

### Firmware-side prerequisites

These keep hardware tags out of version strings and release notes and remove
the firmware's dependence on the compatibility copy described under the
lifecycle. Ship them early; they are not a gate for the first hardware
release, because the compatibility copy protects existing devices:

- Update discovery: fetch several releases from the API and take the first
  whose tag matches `^v[0-9]`; ignore every other tag. Today the device takes
  the single newest release and would build a manifest URL for a hardware tag.
- Version generation: restrict both the describe and the nearest-tag lookups
  to firmware tags. Matching `v*` alone is insufficient because the historical
  hardware tags start with `v`:

  ```bash
  git describe --tags --match 'v[0-9]*' --exclude '*-hw'
  ```

  Without this, a dev build after the hardware tag would report something like
  `hw-v1.0.0-3-gabcdef`, which the device parses as 0.0.0, so every published
  firmware would look like an upgrade. The two stray firmware tags without a
  `v` prefix are excluded by the same filter, which is intended.
- Release OTA workflow: validate pushed tags against the same shape as
  manually supplied versions; use the same filter for the previous-release
  lookup; pass `--notes-start-tag` so generated notes never start at a
  hardware release; let the manual guard ignore hardware tags on HEAD, so a
  hardware and a firmware release may identify the same commit.

Devices running older firmware with prereleases enabled resolve the newest
release of any kind. They stay functional and keep finding firmware updates
because each hardware release carries the compatibility copy; devices with
prereleases disabled use GitHub's "Latest" release, which a hardware release
never becomes. From the firmware that contains the filter above, devices no
longer depend on the copy.

Verify these changes with fixtures containing firmware tags, historical
hardware tags, new hardware tags, both tag types on one commit, untagged
commits and dirty working trees.

### Hardware pull request checks

Add a separate hardware workflow triggered by changes to hardware sources,
libraries, checks and export configuration. In a pinned KiCad 10 container it
should:

- Run `kicad-cli sch erc`, `kicad-cli pcb drc --schematic-parity` and the
  committed jobset.
- Check that the library tables resolve through `${KIPRJMOD}` only, that
  `HW_REV` is set, and that the component-data check passes.
- Publish schematic and PCB previews and the reports as review artifacts.
- Fail on new unresolved findings. Any baseline exception must identify the
  specific finding, explain acceptance and have an owner; do not suppress all
  findings merely because their total count is unchanged.

Automated checks support review; they do not establish electrical correctness
or physical readiness. Document checks that still require an interactive
editor.

### Hardware release workflow

Trigger a dedicated release workflow on `hw-v*`. Validate the tag against
`HW_REV`, run the required checks, generate and checksum the package with the
committed jobset, fetch the compatibility copy of the newest firmware manifest
(and stop if it cannot be fetched or is not a signed manifest for this
device), then publish the release with `--prerelease --latest=false`,
the title `Hardware <rev>` and notes that state its maturity and validation
limitations. Follow up with the commit that records the manifest under
`hardware/releases/hw-v<rev>/`.

## Adoption

The policy was accepted on 2026-10-16. The work that implements it (the
firmware-side prerequisites, revision identity and board ID, component-data
checks, workflows and the first release) is tracked outside this repository.
Keep those changes reviewable separately from circuit changes where
practical; [COMPATIBILITY.md](COMPATIBILITY.md) and the release records show
what has landed.

## Acceptance criteria

- A physical board's marking identifies a unique released design and assembly,
  and the firmware reports the same revision from the ID pads.
- Each release resolves to immutable sources and checksummed manufacturing
  files, with the checksums also recorded in Git.
- Hardware tags and releases cannot change reported firmware versions,
  trigger OTA releases, change how an existing device operates, or break
  update discovery on any firmware ever shipped, including firmware older
  than the discovery filter.
- A clean environment can resolve the libraries and regenerate the release
  package using recorded tools and settings.
- Every placed component carries manufacturer, part number, datasheet and bin,
  and the release BOM shows them.
- Reviewers can inspect readable design exports and verification reports.
- Compatibility claims identify what was physically tested and what remains
  unverified.

## References

- [Existing firmware release workflow](../.github/workflows/main.yml)
- [Firmware version generation](../software/modbus-to-mqtt/scripts/gen_version.py)
- [Device update discovery](../software/modbus-to-mqtt/src/services/ota/HttpOtaService.cpp)
  and its URLs in [Config.h](../software/modbus-to-mqtt/include/Config.h)
- [Existing CI workflow](../.github/workflows/ci.yml)
- [Historical hardware releases](releases/)
- [USB programming and J7 notes](USB-programming.md)
- [KiCad project documentation](https://docs.kicad.org/10.0/en/kicad/kicad.html)
