# Board identification

Boards from hardware revision 1.0.0 onwards carry a hardware ID on four
ESP32 pins. The firmware reads it once at boot and shows it next to the
firmware version in the system stats and the web UI. The value is
informational only: no firmware behaviour depends on it.

## Pin map

| Signal          | Module pin | GPIO              | Pin type                           | Resistors                                        |
|-----------------|------------|-------------------|------------------------------------|--------------------------------------------------|
| `HW_ID_PRESENT` | 8          | IO32              | GPIO, internal pull-up available   | R28, 2 kΩ to GND, fitted on every board from 1.0.0 |
| `HW_ID0`        | 6          | IO34              | input only, no internal pull       | R22, 10 kΩ to VDD33; R23, 10 kΩ to GND           |
| `HW_ID1`        | 7          | IO35              | input only, no internal pull       | R24, 10 kΩ to VDD33; R25, 10 kΩ to GND           |
| `HW_ID2`        | 4          | IO36 (SENSOR_VP)  | input only, no internal pull       | R26, 10 kΩ to VDD33; R27, 10 kΩ to GND           |

IO39 (SENSOR_VN, module pin 5) stays unconnected and is reserved for a
fourth bit. None of the five pins is a boot-strapping pin.

## Reading the ID

1. Configure IO32 as an input with the internal pull-up enabled and read it.
   High means the board has no ID pads: it is older than 1.0.0 and is
   reported as `<1.0.0`. Low means the pads are present.
2. Read IO34, IO35 and IO36 as plain inputs. The code is `ID2 ID1 ID0`,
   with IO36 as the most significant bit.
3. Look the code up in the table below. A code without an entry is shown as
   the raw number.

The presence resistor is 2 kΩ rather than 10 kΩ because the ESP32 internal
pull-up is only nominally 45 kΩ. Against a 2 kΩ pull-down the pin stays
below 0.4 V even for a 15 kΩ pull-up, well under the 0.825 V input-low
limit. The ID bits need no such margin: exactly one resistor of each pair
is fitted, so there is no divider.

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
BOM lists it with the DNP flag set.

| Revision | Code | R22 (ID0 up) | R23 (ID0 down) | R24 (ID1 up) | R25 (ID1 down) | R26 (ID2 up) | R27 (ID2 down) | R28 (presence) |
|----------|------|--------------|----------------|--------------|----------------|--------------|----------------|----------------|
| 1.0.0    | 1    | fitted       | DNP            | DNP          | fitted         | DNP          | fitted         | fitted         |

A later revision that keeps these pads takes the next free code, changes
only the DNP marks, and adds a row here and in `COMPATIBILITY.md`.

## Revision marking

The revision itself comes from the `HW_REV` text variable in the KiCad
project file. The schematic and PCB title blocks and the front silkscreen
text `MBX HW <rev>` all reference `${HW_REV}`, so a new revision is a
single edit in the project settings.
