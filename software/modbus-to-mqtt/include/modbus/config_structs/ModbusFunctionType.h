#ifndef MBREGISTERTYPE_H
#define MBREGISTERTYPE_H

enum ModbusFunctionType {
    READ_COIL = 1,
    READ_DISCRETE = 2,
    READ_HOLDING = 3,
    READ_INPUT = 4,
    WRITE_COIL = 5,
    WRITE_HOLDING = 6,
};
#endif
