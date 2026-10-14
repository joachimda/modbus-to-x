#ifndef MBREGISTERTYPE_H
#define MBREGISTERTYPE_H

enum ModbusFunctionType {
    READ_COIL = 1,
    READ_DISCRETE = 2,
    READ_HOLDING = 3,
    READ_INPUT = 4,
    WRITE_COIL = 5,
    WRITE_HOLDING = 6,
    WRITE_MULTIPLE_HOLDING = 16,
    // Wavin AHC 9000: registers addressed by category/index/page.
    WAVIN_READ = 0x43,
    WAVIN_WRITE = 0x44,
};
#endif
