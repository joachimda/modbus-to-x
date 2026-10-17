# USB and PROG selection

Use either the USB cable at J6 or the external programmer at J4. Disconnect
both before changing J7, then connect only the selected programmer.

J7 is a 2×3 header on a 2.54 mm pitch. It uses three removable shunts:

| Connection | J7 setting |
| --- | --- |
| USB at J6 | Fit all three shunts: 1–2, 3–4 and 5–6 |
| External programmer at J4 | Remove all three shunts |

The shunts connect the CH340C's TXD, RTS and DTR outputs to RX_0, RTS and
DTR respectively. Removing them isolates these outputs from the external
programmer. A power-only jumper would leave signal paths that can back-power
the bridge, so J7 switches the three output signals instead. The bridge's
RXD input remains connected to the ESP32's TX_0 signal.

The board still needs its normal external supply. USB VBUS is unconnected;
this change does not add USB power or VBUS sensing.

PCB synchronization also required correcting the existing ESP32 symbol's
exposed-pad numbering. All exposed-ground drawings now map to pad 39;
perimeter pins and their connections are unchanged. See the
[pin-map verification](kicad_files/lib/ESP32-pin-map.md).

Verification on 2026-10-16: the exported schematic netlist isolates all three
bridge outputs with J7 open and restores the original connectivity with all
three shunts fitted. Schematic checks found no shorts or unconnected pins in
the changed circuit. ERC retains the existing 3 errors and 15 warnings.
After routing and refilling zones, PCB DRC reports no violations, no
unconnected items and no schematic/PCB mismatches. Operation has not been
tested on a physical board.
