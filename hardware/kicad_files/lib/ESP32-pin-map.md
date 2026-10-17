# ESP32 exposed-pad mapping verification

Manufacturer: Espressif Systems. Part: ESP32-WROOM-32D(M113DH2800PH3Q0), PCB antenna module.
Source: repository datasheet v2.6, pp. 8–9 (Figure 1, top view; Table 3).
URL: https://www.espressif.com/sites/default/files/documentation/esp32-wroom-32d_esp32-wroom-32u_datasheet_en.pdf

Symbol: project_symbols:ESP32-WROOM-32D_EP39. Footprint: project_footprints:ESP32-WROOM-32D_ANT_SILK_REMOVED.
The module has 38 perimeter contacts plus exposed GND pad 39. The schematic retains 47 drawn pins: nine drawings represent the same EP39. Konnect symbol queries deduplicate these to 39 electrical pins.
The unchanged footprint has 60 pad objects: 38 perimeter pads and 22 objects for EP39 (main pad, nine paste lands and twelve thermal vias). No mechanical-only pads.
Existing passive pin types are preserved; pin 32 remains no_connect. This correction concerns numbering only.

|Lead|Function / symbol name|Symbol pin / type|Footprint pad X/Y mm|View / direction|Evidence|
|---|---|---|---|---|---|
|1|GND_1|1 / passive|(-8.75, -8.25)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|2|3V3|2 / passive|(-8.75, -6.98)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|3|EN|3 / passive|(-8.75, -5.71)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|4|SENSOR_VP|4 / passive|(-8.75, -4.44)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|5|SENSOR_VN|5 / passive|(-8.75, -3.17)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|6|IO34|6 / passive|(-8.75, -1.9)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|7|IO35|7 / passive|(-8.75, -0.63)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|8|IO32|8 / passive|(-8.75, 0.64)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|9|IO33|9 / passive|(-8.75, 1.91)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|10|IO25|10 / passive|(-8.75, 3.18)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|11|IO26|11 / passive|(-8.75, 4.45)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|12|IO27|12 / passive|(-8.75, 5.72)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|13|IO14|13 / passive|(-8.75, 6.99)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|14|IO12|14 / passive|(-8.75, 8.26)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|15|GND_2|15 / passive|(-5.71, 9.51)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|16|IO13|16 / passive|(-4.44, 9.51)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|17|SHD/SD2|17 / passive|(-3.17, 9.51)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|18|SWP/SD3|18 / passive|(-1.9, 9.51)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|19|SCS/CMD|19 / passive|(-0.63, 9.51)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|20|SCK/CLK|20 / passive|(0.64, 9.51)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|21|SDO/SD0|21 / passive|(1.91, 9.51)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|22|SDI/SD1|22 / passive|(3.18, 9.51)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|23|IO15|23 / passive|(4.45, 9.51)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|24|IO2|24 / passive|(5.72, 9.51)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|25|IO0|25 / passive|(8.75, 8.26)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|26|IO4|26 / passive|(8.75, 6.99)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|27|IO16|27 / passive|(8.75, 5.72)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|28|IO17|28 / passive|(8.75, 4.45)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|29|IO5|29 / passive|(8.75, 3.18)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|30|IO18|30 / passive|(8.75, 1.91)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|31|IO19|31 / passive|(8.75, 0.64)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|32|NC|32 / no_connect|(8.75, -0.63)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|33|IO21|33 / passive|(8.75, -1.9)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|34|RXD0|34 / passive|(8.75, -3.17)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|35|TXD0|35 / passive|(8.75, -4.44)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|36|IO22|36 / passive|(8.75, -5.71)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|37|IO23|37 / passive|(8.75, -6.98)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|38|GND_3|38 / passive|(8.75, -8.25)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|
|39|GND_EP|39 / passive|(-2.9, -1.93), (-2.9, -1.23), (-2.9, -0.53), (-2.9, 0.17), (-2.9, 0.87), (-2.2, -1.93), (-2.2, -0.53), (-2.2, 0.87), (-1.525, -1.23), (-1.525, 0.17), (-1.5, -1.93), (-1.5, -0.53), (-1.5, -0.53), (-1.5, 0.87), (-0.8, -1.93), (-0.8, -0.53), (-0.8, 0.87), (-0.1, -1.93), (-0.1, -1.23), (-0.1, -0.53), (-0.1, 0.17), (-0.1, 0.87)|Top; anticlockwise from upper left, antenna up|v2.6 p.8 Figure 1; pp.8–9 Table 3|

Scratch symbol and pin readback preserve endpoints. Exported netlist preserves all original connections when the three jumper shunts are fitted (ground pins canonicalized to EP39).
Scratch ERC: unchanged 3 errors and 15 warnings. Schematic and existing footprint renders inspected.
