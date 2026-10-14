#ifndef MODBUS_TO_MQTT_MODBUS_VALUE_CODEC_H
#define MODBUS_TO_MQTT_MODBUS_VALUE_CODEC_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "modbus/config_structs/ModbusDataType.h"
#include "modbus/config_structs/ModbusWordOrder.h"
#include "modbus/config_structs/RegisterSlice.h"

// Conversion between Modbus register words and typed engineering values.
// The codec has no Arduino dependencies so the MQTT poll path, the HTTP
// execute path and the write path share one natively tested implementation.
namespace ModbusValueCodec {

constexpr size_t kMaxWords = 4U;
constexpr int kMaxScaleDecimals = 6;

// Number of 16-bit registers a data type occupies: TEXT 0 (free length),
// INT16/UINT16 1, INT32/UINT32/FLOAT32 2, INT64/UINT64 4.
uint8_t wordWidth(ModbusDataType type);

bool isSignedInteger(ModbusDataType type);

bool isUnsignedInteger(ModbusDataType type);

bool isNumeric(ModbusDataType type);

// The type's name as written in the configuration ("int16", "float32", "TEXT").
const char *dataTypeName(ModbusDataType type);

uint16_t sliceRegister(uint16_t word, RegisterSlice slice);

struct DecodedValue {
    bool ok{false};
    // The raw register value before scaling, as a double for every type.
    // Integers above 2^53 lose precision here; use rawSigned/rawUnsigned.
    double raw{0.0};
    int64_t rawSigned{0};        // valid for INT16/INT32/INT64
    uint64_t rawUnsigned{0U};    // valid for UINT16/UINT32/UINT64
    float rawFloat{0.0F};        // valid for FLOAT32
};

// Decodes the first wordWidth(type) words. Fails (ok=false) for TEXT or when
// fewer words than the type needs are supplied. The slice applies to the
// 16-bit types only; INT16 with a byte slice decodes that byte as a signed
// 8-bit value.
DecodedValue decode(const uint16_t *words, size_t count, ModbusDataType type,
                    RegisterSlice slice, ModbusWordOrder order);

// raw * scale with negative zero normalised to zero.
double engineeringValue(const DecodedValue &decoded, float scale);

struct EncodedWords {
    uint8_t count{0U};
    std::array<uint16_t, kMaxWords> words{};
};

// Encoders return false when the type does not match or the value does not
// fit the type's range. Words are produced in the requested word order.
bool encodeSigned(int64_t value, ModbusDataType type, ModbusWordOrder order, EncodedWords &out);

bool encodeUnsigned(uint64_t value, ModbusDataType type, ModbusWordOrder order, EncodedWords &out);

bool encodeFloat(float value, ModbusWordOrder order, EncodedWords &out);

// Number of decimals the scale has when printed with float precision
// (0.01 -> 2, 1 -> 0, 0.5 -> 1, 1e-5 -> 5), clamped to kMaxScaleDecimals.
int decimalsForScale(float scale);

struct Formatted {
    size_t length{0U};
    bool finite{false};
};

// Formats an engineering value as published on MQTT and returned by the HTTP
// execute route: integer types print decimalsForScale(scale) decimals,
// FLOAT32 prints the shortest of 7, 8 or 9 significant digits that
// reproduces the float32 value. Non-finite values print "nan", "inf" or
// "-inf" with finite=false.
Formatted formatEngineering(double value, ModbusDataType type, float scale,
                            char *buffer, size_t capacity);

// formatEngineering for a decoded value. Integer types with an integral
// scale (1, -1, 10, ...) are multiplied in 64-bit arithmetic so int64 and
// uint64 values keep every digit; a fractional scale, or a product that
// does not fit 64 bits, rounds through a double.
Formatted formatDecoded(const DecodedValue &decoded, ModbusDataType type, float scale,
                        char *buffer, size_t capacity);

struct ReadingText {
    bool ok{false};        // false: fewer than `width` words were supplied, or TEXT
    bool finite{false};
    size_t length{0U};
    uint8_t width{0U};     // registers the decoded type needs
};

// Decodes and formats a reading in one step: the sequence the MQTT poll loop
// and the HTTP execute route share so both produce identical text. Bit
// functions (FC1/FC2) carry packed bits and decode as UINT16 whatever the
// configured data type.
ReadingText formatReading(const uint16_t *words, size_t count, bool bitFunction,
                          ModbusDataType type, RegisterSlice slice, ModbusWordOrder order,
                          float scale, char *buffer, size_t capacity);

}  // namespace ModbusValueCodec

#endif
