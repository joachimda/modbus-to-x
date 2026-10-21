# Board identification

Boards from hardware revision 1.0.0 onwards carry a hardware ID on four
ESP32 pins. Firmware support is planned and not yet in this repository:
the firmware will read the ID once at boot and show it next to the
firmware version in the system stats and the web UI. The value is
informational only; no firmware behaviour will depend on it.

## Pin map

| Signal          | Module pin | GPIO              | Pin type                           | Resistors                                        |
|-----------------|------------|-------------------|------------------------------------|--------------------------------------------------|
| `HW_ID_PRESENT` | 8          | IO32              | GPIO, internal pull-up available   | R28, 2 kΩ to GND, fitted on every board from 1.0.0 |
| `HW_ID0`        | 6          | IO34              | input only, no internal pull       | R22, 10 kΩ to VDD33; R23, 10 kΩ to GND           |
| `HW_ID1`        | 7          | IO35              | input only, no internal pull       | R24, 10 kΩ to VDD33; R25, 10 kΩ to GND           |
| `HW_ID2`        | 4          | IO36 (SENSOR_VP)  | input only, no internal pull       | R26, 10 kΩ to VDD33; R27, 10 kΩ to GND           |

IO39 (SENSOR_VN, module pin 5) stays unconnected and is reserved for a
fourth bit. None of the five pins is a boot-strapping pin. IO34, IO35 and
IO36 are also ADC1 channels, and IO36 and IO39 double as the Hall sensor
inputs; the firmware uses none of these functions and must not add them on
these pins.

## Parts and placement

All seven resistors are 0603. R22 to R27 are Yageo RT0603DRE0710KL (10 kΩ,
footprint `Resistor_SMD:R_0603_1608Metric`); R28 is RND 155TC0325B2001T5H
(2 kΩ, the hand-solder pad variant of the same footprint). Tolerance plays
no role here, so any 0603 resistor of the right value is a valid rework
substitute.

On the board R22 to R27 form a column on the top side just right of the
ESP32 module, with R28 below them. Empty footprints carry no DNP mark on
the silkscreen; identify them by reference designator.

## Reading the ID

1. Configure IO32 as an input with the internal pull-up enabled and read it.
   High means the board has no ID pads: it is older than 1.0.0 and is
   reported as `<1.0.0`. Low means the pads are present. Disable the
   pull-up again afterwards; it is only needed for the read.
2. Read IO34, IO35 and IO36 as plain inputs. The code is `ID2 ID1 ID0`,
   with IO36 as the most significant bit.
3. Look the code up in the table below. A code without an entry is shown as
   the raw number.

Read the pins once, early in boot, before Wi-Fi starts and before any sleep
mode is used. IO36 and IO39 can show a short low glitch when the RTC or ADC
power domain switches (ESP32 errata); a boot-time read avoids it, and a read
at any other time should sample several times and take the majority.

The presence resistor is 2 kΩ rather than 10 kΩ because of the ESP32
internal pull-up. The ESP32 datasheet gives it as 45 kΩ typical with no
minimum; this design assumes 15 kΩ as the worst case. Against 2 kΩ the pin
sits at 0.14 V typical and 0.39 V worst case, below the 0.825 V
(0.25 × VDD) input-low limit from the module datasheet. A 10 kΩ pull-down
would reach 1.32 V in the assumed worst case. The ID bits need no such
margin: exactly one resistor of each pair is fitted, so there is no
divider.

## Code table

| Code | ID2 | ID1 | ID0 | Hardware revision          |
|------|-----|-----|-----|----------------------------|
| none | –   | –   | –   | before 1.0.0 (`HW_ID_PRESENT` reads high) |
| 0    | 0   | 0   | 0   | reserved, never assigned   |
| 1    | 0   | 0   | 1   | 1.0.0                      |
| 2–7  |     |     |     | unassigned                 |

## Fitting rule

For each ID bit exactly one of its two resistors is fitted: the pull-up for
a 1, the pull-down for a 0. The other footprint stays on the board as an
empty pad and is marked "do not populate" (DNP) in the schematic, so the
BOM lists it with the DNP flag set and position files exported with
`kicad-cli pcb export pos --exclude-dnp` leave it out. A bit with neither
resistor fitted floats, because the input-only pins have no internal pulls,
and the code then reads at random. Never leave a pair empty.

| Revision | Code | R22 (ID0 up) | R23 (ID0 down) | R24 (ID1 up) | R25 (ID1 down) | R26 (ID2 up) | R27 (ID2 down) | R28 (presence) |
|----------|------|--------------|----------------|--------------|----------------|--------------|----------------|----------------|
| 1.0.0    | 1    | fitted       | DNP            | DNP          | fitted         | DNP          | fitted         | fitted         |

## Revision marking

The revision itself comes from the `HW_REV` text variable in the KiCad
project. The schematic and PCB title blocks and the front silkscreen text
`MBX HW <rev>` all reference `${HW_REV}`.

## Changing the revision

1. Set `HW_REV` in KiCad under Schematic Setup → Project → Text Variables
   (Board Setup → Text & Graphics → Text Variables edits the same list),
   then save both the schematic and the board. Exports read the value from
   the project file; the board file also stores a copy for standalone use,
   which KiCad refreshes on the next save.
2. If the new revision keeps these pads, assign the next free code, flip the
   DNP marks on R22 to R27 to match and update the "Board ID" note on the
   schematic sheet, which spells out the fitted set.
3. Add a row to the code table and the fitting table above, and to the
   hardware/firmware compatibility table in
   [COMPATIBILITY.md](COMPATIBILITY.md).
