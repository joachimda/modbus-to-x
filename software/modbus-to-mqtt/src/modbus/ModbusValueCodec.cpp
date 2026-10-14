#include "modbus/ModbusValueCodec.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace ModbusValueCodec {
namespace {

uint64_t packWords(const uint16_t *words, const uint8_t width, const ModbusWordOrder order) {
    uint64_t bits = 0U;
    for (uint8_t i = 0U; i < width; ++i) {
        const uint8_t index = order == ModbusWordOrder::Big
                                  ? i
                                  : static_cast<uint8_t>(width - 1U - i);
        bits = (bits << 16U) | words[index];
    }
    return bits;
}

void splitWords(const uint64_t bits, const uint8_t width, const ModbusWordOrder order,
                EncodedWords &out) {
    out.count = width;
    out.words.fill(0U);
    for (uint8_t i = 0U; i < width; ++i) {
        const unsigned shift = 16U * static_cast<unsigned>(width - 1U - i);
        const auto word = static_cast<uint16_t>((bits >> shift) & 0xFFFFU);
        const uint8_t index = order == ModbusWordOrder::Big
                                  ? i
                                  : static_cast<uint8_t>(width - 1U - i);
        out.words[index] = word;
    }
}

Formatted copyText(const char *text, char *buffer, const size_t capacity) {
    Formatted result;
    std::snprintf(buffer, capacity, "%s", text);
    result.length = std::strlen(buffer);
    result.finite = false;
    return result;
}

// "-0", "-0.0", "-0.000" -> drop the sign; rounding to the scale's decimals
// can otherwise leave a negative zero for tiny negative values.
void stripNegativeZero(char *buffer) {
    if (buffer[0] != '-') return;
    for (const char *cursor = buffer + 1; *cursor != '\0'; ++cursor) {
        if (*cursor != '0' && *cursor != '.') return;
    }
    std::memmove(buffer, buffer + 1, std::strlen(buffer));
}

// The shortest of 7, 8 or 9 significant digits that reproduces the float32
// value: 0.1f prints as "0.1", 16777215.0f as "16777215" (where "%.7g"
// gives 1.677722e+07) and 1234567.75f as "1234567.75".
int formatShortestFloat(const double value, char *buffer, const size_t capacity) {
    if (std::fabs(value) > static_cast<double>(std::numeric_limits<float>::max())) {
        // raw * scale overflowed float32; print the double as it is.
        return std::snprintf(buffer, capacity, "%.9g", value);
    }
    const auto narrowed = static_cast<float>(value);
    int written = -1;
    for (int precision = 7; precision <= 9; ++precision) {
        written = std::snprintf(buffer, capacity, "%.*g", precision, static_cast<double>(narrowed));
        if (written < 0 || static_cast<size_t>(written) >= capacity) return written;
        if (std::strtof(buffer, nullptr) == narrowed) break;
    }
    return written;
}

// Integer types with an integral scale: raw * scale in 64-bit arithmetic so
// int64/uint64 values keep every digit (a double carries 53 bits). Returns
// false when the scale is fractional or the product does not fit 64 bits;
// the caller then rounds through a double.
bool formatExactInteger(const DecodedValue &decoded, const ModbusDataType type,
                        const float scale, char *buffer, const size_t capacity,
                        Formatted &out) {
    const bool signedType = isSignedInteger(type);
    if (!signedType && !isUnsignedInteger(type)) return false;
    const auto scaleValue = static_cast<double>(scale);
    if (!std::isfinite(scaleValue) || scaleValue == 0.0 || std::floor(scaleValue) != scaleValue
        || std::fabs(scaleValue) >= 18446744073709551616.0) {
        return false;
    }
    const auto factor = static_cast<uint64_t>(std::fabs(scaleValue));
    bool negative = scaleValue < 0.0;
    uint64_t magnitude = decoded.rawUnsigned;
    if (signedType) {
        const int64_t raw = decoded.rawSigned;
        if (raw < 0) {
            negative = !negative;
            magnitude = static_cast<uint64_t>(-(raw + 1)) + 1U;
        } else {
            magnitude = static_cast<uint64_t>(raw);
        }
    }
    if (magnitude > std::numeric_limits<uint64_t>::max() / factor) return false;
    const uint64_t product = magnitude * factor;
    const int written = std::snprintf(buffer, capacity, "%s%" PRIu64,
                                      negative && product != 0U ? "-" : "", product);
    if (written < 0 || static_cast<size_t>(written) >= capacity) return false;
    out.length = static_cast<size_t>(written);
    out.finite = true;
    return true;
}

}  // namespace

uint8_t wordWidth(const ModbusDataType type) {
    switch (type) {
        case INT16:
        case UINT16:
            return 1U;
        case INT32:
        case UINT32:
        case FLOAT32:
            return 2U;
        case INT64:
        case UINT64:
            return 4U;
        case TEXT:
            break;
    }
    return 0U;
}

bool isSignedInteger(const ModbusDataType type) {
    return type == INT16 || type == INT32 || type == INT64;
}

bool isUnsignedInteger(const ModbusDataType type) {
    return type == UINT16 || type == UINT32 || type == UINT64;
}

bool isNumeric(const ModbusDataType type) {
    return wordWidth(type) != 0U;
}

const char *dataTypeName(const ModbusDataType type) {
    switch (type) {
        case TEXT: return "TEXT";
        case INT16: return "int16";
        case INT32: return "int32";
        case INT64: return "int64";
        case UINT16: return "uint16";
        case UINT32: return "uint32";
        case UINT64: return "uint64";
        case FLOAT32: return "float32";
    }
    return "unknown";
}

uint16_t sliceRegister(const uint16_t word, const RegisterSlice slice) {
    switch (slice) {
        case RegisterSlice::LowByte: return static_cast<uint16_t>(word & 0x00FFU);
        case RegisterSlice::HighByte: return static_cast<uint16_t>((word >> 8U) & 0x00FFU);
        case RegisterSlice::Full: return word;
    }
    return word;
}

DecodedValue decode(const uint16_t *words, const size_t count, const ModbusDataType type,
                    const RegisterSlice slice, const ModbusWordOrder order) {
    DecodedValue result;
    const uint8_t width = wordWidth(type);
    if (words == nullptr || width == 0U || count < width) return result;

    if (width == 1U) {
        const uint16_t sliced = sliceRegister(words[0], slice);
        if (type == INT16) {
            const int64_t half = slice == RegisterSlice::Full ? 0x8000 : 0x80;
            const int64_t value = sliced >= half ? static_cast<int64_t>(sliced) - 2 * half
                                                 : static_cast<int64_t>(sliced);
            result.rawSigned = value;
            result.raw = static_cast<double>(value);
        } else {
            result.rawUnsigned = sliced;
            result.raw = static_cast<double>(sliced);
        }
        result.ok = true;
        return result;
    }

    const uint64_t bits = packWords(words, width, order);
    switch (type) {
        case INT32: {
            const auto unsignedBits = static_cast<uint32_t>(bits);
            int32_t value = 0;
            std::memcpy(&value, &unsignedBits, sizeof value);
            result.rawSigned = value;
            result.raw = static_cast<double>(value);
            break;
        }
        case UINT32: {
            const auto value = static_cast<uint32_t>(bits);
            result.rawUnsigned = value;
            result.raw = static_cast<double>(value);
            break;
        }
        case FLOAT32: {
            const auto unsignedBits = static_cast<uint32_t>(bits);
            float value = 0.0F;
            std::memcpy(&value, &unsignedBits, sizeof value);
            result.rawFloat = value;
            result.raw = static_cast<double>(value);
            break;
        }
        case INT64: {
            int64_t value = 0;
            std::memcpy(&value, &bits, sizeof value);
            result.rawSigned = value;
            result.raw = static_cast<double>(value);
            break;
        }
        case UINT64:
            result.rawUnsigned = bits;
            result.raw = static_cast<double>(bits);
            break;
        case INT16:
        case UINT16:
        case TEXT:
            return result;
    }
    result.ok = true;
    return result;
}

double engineeringValue(const DecodedValue &decoded, const float scale) {
    double value = decoded.raw * static_cast<double>(scale);
    if (value == 0.0) value = 0.0;  // folds -0.0 into +0.0
    return value;
}

bool encodeSigned(const int64_t value, const ModbusDataType type, const ModbusWordOrder order,
                  EncodedWords &out) {
    if (!isSignedInteger(type)) return false;
    if (type == INT16 && (value < -32768 || value > 32767)) return false;
    if (type == INT32 && (value < std::numeric_limits<int32_t>::min()
                          || value > std::numeric_limits<int32_t>::max())) {
        return false;
    }
    uint64_t bits = 0U;
    std::memcpy(&bits, &value, sizeof bits);
    splitWords(bits, wordWidth(type), order, out);
    return true;
}

bool encodeUnsigned(const uint64_t value, const ModbusDataType type, const ModbusWordOrder order,
                    EncodedWords &out) {
    if (!isUnsignedInteger(type)) return false;
    if (type == UINT16 && value > 0xFFFFU) return false;
    if (type == UINT32 && value > 0xFFFFFFFFU) return false;
    splitWords(value, wordWidth(type), order, out);
    return true;
}

bool encodeFloat(const float value, const ModbusWordOrder order, EncodedWords &out) {
    uint32_t bits = 0U;
    std::memcpy(&bits, &value, sizeof bits);
    splitWords(bits, wordWidth(FLOAT32), order, out);
    return true;
}

int decimalsForScale(const float scale) {
    if (!std::isfinite(scale) || scale == 0.0F) return 0;
    // Float precision (7 significant digits) strips the binary noise of the
    // stored scale: 0.01f prints as "0.01", not "0.009999999776".
    char text[32];
    std::snprintf(text, sizeof text, "%.7g", static_cast<double>(std::fabs(scale)));

    int fractionDigits = 0;
    int exponent = 0;
    bool inFraction = false;
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor == '.') {
            inFraction = true;
        } else if (*cursor == 'e' || *cursor == 'E') {
            exponent = std::atoi(cursor + 1);
            break;
        } else if (inFraction && *cursor >= '0' && *cursor <= '9') {
            ++fractionDigits;
        }
    }
    int decimals = fractionDigits - exponent;
    if (decimals < 0) decimals = 0;
    if (decimals > kMaxScaleDecimals) decimals = kMaxScaleDecimals;
    return decimals;
}

Formatted formatEngineering(double value, const ModbusDataType type, const float scale,
                            char *buffer, const size_t capacity) {
    Formatted result;
    if (buffer == nullptr || capacity == 0U) return result;
    buffer[0] = '\0';
    if (std::isnan(value)) return copyText("nan", buffer, capacity);
    if (std::isinf(value)) return copyText(value < 0.0 ? "-inf" : "inf", buffer, capacity);
    if (value == 0.0) value = 0.0;

    int written = type == FLOAT32
                      ? formatShortestFloat(value, buffer, capacity)
                      : std::snprintf(buffer, capacity, "%.*f", decimalsForScale(scale), value);
    if (written < 0 || static_cast<size_t>(written) >= capacity) {
        written = std::snprintf(buffer, capacity, "%.9g", value);
        if (written < 0 || static_cast<size_t>(written) >= capacity) {
            buffer[0] = '\0';
            return result;
        }
    }
    stripNegativeZero(buffer);
    result.length = std::strlen(buffer);
    result.finite = true;
    return result;
}

Formatted formatDecoded(const DecodedValue &decoded, const ModbusDataType type, const float scale,
                        char *buffer, const size_t capacity) {
    Formatted result;
    if (buffer == nullptr || capacity == 0U) return result;
    if (formatExactInteger(decoded, type, scale, buffer, capacity, result)) return result;
    return formatEngineering(engineeringValue(decoded, scale), type, scale, buffer, capacity);
}

ReadingText formatReading(const uint16_t *words, const size_t count, const bool bitFunction,
                          const ModbusDataType type, const RegisterSlice slice,
                          const ModbusWordOrder order, const float scale, char *buffer,
                          const size_t capacity) {
    ReadingText text;
    if (buffer != nullptr && capacity != 0U) buffer[0] = '\0';
    // Coils and discrete inputs arrive as packed bits in one word whatever
    // the configured data type.
    const ModbusDataType effective = bitFunction ? UINT16 : type;
    text.width = wordWidth(effective);
    const DecodedValue decoded = decode(words, count, effective, slice, order);
    if (!decoded.ok) return text;
    const Formatted formatted = formatDecoded(decoded, effective, scale, buffer, capacity);
    text.ok = formatted.length != 0U;
    text.finite = formatted.finite;
    text.length = formatted.length;
    return text;
}

}  // namespace ModbusValueCodec
