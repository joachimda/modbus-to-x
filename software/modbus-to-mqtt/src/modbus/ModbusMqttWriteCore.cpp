#include "modbus/ModbusMqttWriteCore.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>
#include <vector>

namespace ModbusMqttWriteCore {
namespace {

bool isAsciiWhitespace(const unsigned char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n'
           || value == '\f' || value == '\v';
}

std::string trim(const std::string &value) {
    size_t begin = 0U;
    while (begin < value.size() && isAsciiWhitespace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin && isAsciiWhitespace(static_cast<unsigned char>(value[end - 1U]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

char asciiLower(const char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

bool equalsIgnoreAsciiCase(const std::string &value, const char *expected) {
    size_t index = 0U;
    while (expected[index] != '\0') {
        if (index >= value.size() || asciiLower(value[index]) != expected[index]) return false;
        ++index;
    }
    return index == value.size();
}

constexpr int64_t DECIMAL_EXPONENT_LIMIT = std::numeric_limits<int64_t>::max() / 4;
constexpr double MINIMUM_DISCOVERY_STEP = 0.001;  // Home Assistant's smallest number step

struct DecimalMagnitude {
    bool negative{false};
    bool zero{true};
    std::string digits;
    int64_t exponent10{0};
};

int64_t saturatingAdd(const int64_t left, const int64_t right) {
    if (right > 0 && left > DECIMAL_EXPONENT_LIMIT - right) {
        return DECIMAL_EXPONENT_LIMIT;
    }
    if (right < 0 && left < -DECIMAL_EXPONENT_LIMIT - right) {
        return -DECIMAL_EXPONENT_LIMIT;
    }
    return left + right;
}

int64_t sizeAsExponent(const size_t value) {
    const auto limit = static_cast<uint64_t>(DECIMAL_EXPONENT_LIMIT);
    return value > limit ? DECIMAL_EXPONENT_LIMIT : static_cast<int64_t>(value);
}

bool parseDecimal(const std::string &value, DecimalMagnitude &result) {
    if (value.empty()) return false;

    size_t index = 0U;
    result.negative = value[index] == '-';
    if (value[index] == '+' || value[index] == '-') ++index;

    result.digits.clear();
    result.digits.reserve(value.size());

    bool integerDigits = false;
    while (index < value.size() && value[index] >= '0' && value[index] <= '9') {
        integerDigits = true;
        result.digits.push_back(value[index]);
        ++index;
    }

    bool fractionDigits = false;
    size_t fractionDigitCount = 0U;
    if (index < value.size() && value[index] == '.') {
        ++index;
        while (index < value.size() && value[index] >= '0' && value[index] <= '9') {
            fractionDigits = true;
            result.digits.push_back(value[index]);
            ++fractionDigitCount;
            ++index;
        }
    }
    if (!integerDigits && !fractionDigits) return false;

    int exponentSign = 1;
    int64_t exponent = 0;
    if (index < value.size() && (value[index] == 'e' || value[index] == 'E')) {
        ++index;
        if (index < value.size() && (value[index] == '+' || value[index] == '-')) {
            exponentSign = value[index] == '-' ? -1 : 1;
            ++index;
        }
        const size_t exponentStart = index;
        while (index < value.size() && value[index] >= '0' && value[index] <= '9') {
            const int digit = value[index] - '0';
            if (exponent <= (DECIMAL_EXPONENT_LIMIT - digit) / 10) {
                exponent = exponent * 10 + digit;
            } else {
                exponent = DECIMAL_EXPONENT_LIMIT;
            }
            ++index;
        }
        if (index == exponentStart) return false;
    }
    if (index != value.size()) return false;

    const size_t firstNonZero = result.digits.find_first_not_of('0');
    if (firstNonZero == std::string::npos) {
        result.zero = true;
        result.digits = "0";
        result.exponent10 = 0;
        return true;
    }

    result.zero = false;
    result.digits.erase(0U, firstNonZero);
    result.exponent10 = exponentSign > 0 ? exponent : -exponent;
    result.exponent10 = saturatingAdd(result.exponent10, -sizeAsExponent(fractionDigitCount));
    return true;
}

void multiplyDecimalDigits(std::string &digits, const uint32_t factor) {
    uint32_t carry = 0U;
    for (size_t index = digits.size(); index > 0U; --index) {
        const uint32_t product = static_cast<uint32_t>(digits[index - 1U] - '0') * factor
                                 + carry;
        digits[index - 1U] = static_cast<char>('0' + (product % 10U));
        carry = product / 10U;
    }

    std::string prefix;
    while (carry > 0U) {
        prefix.push_back(static_cast<char>('0' + (carry % 10U)));
        carry /= 10U;
    }
    std::reverse(prefix.begin(), prefix.end());
    digits.insert(0U, prefix);
}

// Schoolbook multiplication of two non-negative decimal digit strings.
std::string multiplyDecimalStrings(const std::string &left, const std::string &right) {
    std::vector<uint32_t> product(left.size() + right.size(), 0U);
    for (size_t leftIndex = left.size(); leftIndex > 0U; --leftIndex) {
        const auto leftDigit = static_cast<uint32_t>(left[leftIndex - 1U] - '0');
        uint32_t carry = 0U;
        for (size_t rightIndex = right.size(); rightIndex > 0U; --rightIndex) {
            const auto rightDigit = static_cast<uint32_t>(right[rightIndex - 1U] - '0');
            const size_t position = leftIndex + rightIndex - 1U;
            const uint32_t sum = product[position] + leftDigit * rightDigit + carry;
            product[position] = sum % 10U;
            carry = sum / 10U;
        }
        size_t position = leftIndex - 1U;
        while (carry > 0U) {
            const uint32_t sum = product[position] + carry;
            product[position] = sum % 10U;
            carry = sum / 10U;
            if (position == 0U) break;
            --position;
        }
    }

    std::string digits;
    digits.reserve(product.size());
    for (const uint32_t digit : product) {
        if (digits.empty() && digit == 0U) continue;
        digits.push_back(static_cast<char>('0' + digit));
    }
    if (digits.empty()) digits = "0";
    return digits;
}

// Exact decimal value of |scale| * rawMagnitude, where scale is a double.
DecimalMagnitude scaledMagnitude(const double scale, const uint64_t rawMagnitude) {
    DecimalMagnitude result;
    if (rawMagnitude == 0U) {
        result.digits = "0";
        return result;
    }

    int exponent2 = 0;
    const double fraction = std::frexp(std::fabs(scale), &exponent2);
    const int significandBits = std::numeric_limits<double>::digits;
    const auto significand = static_cast<uint64_t>(std::ldexp(fraction, significandBits));

    result.zero = false;
    result.digits = multiplyDecimalStrings(std::to_string(significand), std::to_string(rawMagnitude));

    const int binaryExponent = exponent2 - significandBits;
    if (binaryExponent >= 0) {
        for (int index = 0; index < binaryExponent; ++index) {
            multiplyDecimalDigits(result.digits, 2U);
        }
    } else {
        for (int index = 0; index < -binaryExponent; ++index) {
            multiplyDecimalDigits(result.digits, 5U);
        }
        result.exponent10 = binaryExponent;
    }

    while (result.digits.size() > 1U && result.digits.back() == '0') {
        result.digits.pop_back();
        ++result.exponent10;
    }
    return result;
}

int compareMagnitude(const DecimalMagnitude &left, const DecimalMagnitude &right) {
    const int64_t leftOrder = saturatingAdd(sizeAsExponent(left.digits.size()), left.exponent10);
    const int64_t rightOrder = saturatingAdd(sizeAsExponent(right.digits.size()), right.exponent10);
    if (leftOrder != rightOrder) return leftOrder < rightOrder ? -1 : 1;

    const size_t comparisonLength = std::max(left.digits.size(), right.digits.size());
    for (size_t index = 0U; index < comparisonLength; ++index) {
        const char leftDigit = index < left.digits.size() ? left.digits[index] : '0';
        const char rightDigit = index < right.digits.size() ? right.digits[index] : '0';
        if (leftDigit != rightDigit) return leftDigit < rightDigit ? -1 : 1;
    }
    return 0;
}

struct RawRange {
    int64_t minimum{0};
    uint64_t maximum{0U};
};

bool rawRangeForType(const ModbusDataType type, RawRange &range) {
    switch (type) {
        case INT16: range = {-32768, 32767U}; return true;
        case UINT16: range = {0, 65535U}; return true;
        case INT32:
            range = {std::numeric_limits<int32_t>::min(),
                     static_cast<uint64_t>(std::numeric_limits<int32_t>::max())};
            return true;
        case UINT32: range = {0, std::numeric_limits<uint32_t>::max()}; return true;
        case INT64:
            range = {std::numeric_limits<int64_t>::min(),
                     static_cast<uint64_t>(std::numeric_limits<int64_t>::max())};
            return true;
        case UINT64: range = {0, std::numeric_limits<uint64_t>::max()}; return true;
        case FLOAT32:
        case TEXT:
            break;
    }
    return false;
}

// Largest engineering magnitudes an integer type accepts, by the sign of the
// raw value: positive is maximum * |scale|, negative is |minimum| * |scale|.
struct RawLimits {
    bool valid{false};
    DecimalMagnitude positive;
    DecimalMagnitude negative;
};

RawLimits rawLimitsForType(const ModbusDataType type, const double scale) {
    RawLimits limits;
    RawRange range;
    if (!rawRangeForType(type, range) || !isValidWritableScale(scale)) return limits;
    limits.valid = true;
    limits.positive = scaledMagnitude(scale, range.maximum);
    const uint64_t negativeMagnitude = range.minimum < 0
                                           ? static_cast<uint64_t>(-(range.minimum + 1)) + 1U
                                           : 0U;
    limits.negative = scaledMagnitude(scale, negativeMagnitude);
    return limits;
}

bool isOutsideRawRange(const DecimalMagnitude &value,
                       const RawLimits &limits,
                       const double scale) {
    if (value.zero) return false;
    const bool rawNegative = value.negative != std::signbit(scale);
    const DecimalMagnitude &limit = rawNegative ? limits.negative : limits.positive;
    if (limit.zero) return true;
    return compareMagnitude(value, limit) > 0;
}

ConversionResult reject(const RejectionReason reason) {
    ConversionResult result;
    result.rejection = reason;
    return result;
}

ConversionResult acceptWords(const ModbusValueCodec::EncodedWords &encoded) {
    ConversionResult result;
    result.wordCount = encoded.count;
    result.words = encoded.words;
    result.rawValue = encoded.words[0];
    return result;
}

ConversionResult convertPayloadWithBoundary(const ModbusFunctionType function,
                                            const std::string &payload,
                                            const double scale,
                                            const ModbusDataType dataType,
                                            const ModbusWordOrder wordOrder,
                                            const RawLimits *precomputedLimits) {
    const std::string value = trim(payload);
    if (value.empty()) return reject(RejectionReason::Empty);

    if (function == WRITE_COIL) {
        ConversionResult result;
        if (value == "1" || equalsIgnoreAsciiCase(value, "true")) {
            result.rawValue = 1U;
            return result;
        }
        if (value == "0" || equalsIgnoreAsciiCase(value, "false")) return result;
        return reject(RejectionReason::InvalidSyntax);
    }

    if (!isWritableHoldingFunction(function)) return reject(RejectionReason::InvalidSyntax);
    if (!ModbusValueCodec::isNumeric(dataType)
        || (function == WRITE_HOLDING && ModbusValueCodec::wordWidth(dataType) != 1U)) {
        return reject(RejectionReason::UnsupportedDataType);
    }

    DecimalMagnitude parsedDecimal;
    if (!parseDecimal(value, parsedDecimal)) return reject(RejectionReason::InvalidSyntax);

    char *end = nullptr;
    const double engineering = std::strtod(value.c_str(), &end);
    if (end == nullptr || end != value.c_str() + value.size()) {
        return reject(RejectionReason::InvalidSyntax);
    }
    if (!std::isfinite(engineering)) return reject(RejectionReason::NonFinite);
    if (!isValidWritableScale(scale)) return reject(RejectionReason::InvalidScale);

    const double rawExact = engineering / scale;
    if (!std::isfinite(rawExact)) return reject(RejectionReason::NonFinite);

    ModbusValueCodec::EncodedWords encoded;
    if (dataType == FLOAT32) {
        if (std::fabs(rawExact) > static_cast<double>(std::numeric_limits<float>::max())) {
            return reject(RejectionReason::OutOfRange);
        }
        ModbusValueCodec::encodeFloat(static_cast<float>(rawExact), wordOrder, encoded);
        return acceptWords(encoded);
    }

    RawLimits computedLimits;
    const RawLimits *limits = precomputedLimits;
    if (limits == nullptr || !limits->valid) {
        computedLimits = rawLimitsForType(dataType, scale);
        limits = &computedLimits;
    }
    if (!limits->valid) return reject(RejectionReason::UnsupportedDataType);
    if (isOutsideRawRange(parsedDecimal, *limits, scale)) {
        return reject(RejectionReason::OutOfRange);
    }

    // Nearest integer with exact halves rounded upward. The exact decimal
    // check above guarantees the true value is in range; the double path only
    // loses precision above 2^53, so the conversion saturates instead of
    // overflowing.
    const double rounded = std::floor(rawExact + 0.5);
    bool encodedOk = false;
    if (ModbusValueCodec::isSignedInteger(dataType)) {
        int64_t raw = 0;
        if (rounded >= 9223372036854775808.0) raw = std::numeric_limits<int64_t>::max();
        else if (rounded < -9223372036854775808.0) raw = std::numeric_limits<int64_t>::min();
        else raw = static_cast<int64_t>(rounded);
        encodedOk = ModbusValueCodec::encodeSigned(raw, dataType, wordOrder, encoded);
    } else {
        uint64_t raw = 0U;
        if (rounded >= 18446744073709551616.0) raw = std::numeric_limits<uint64_t>::max();
        else if (rounded <= 0.0) raw = 0U;
        else raw = static_cast<uint64_t>(rounded);
        encodedOk = ModbusValueCodec::encodeUnsigned(raw, dataType, wordOrder, encoded);
    }
    if (!encodedOk) return reject(RejectionReason::OutOfRange);
    return acceptWords(encoded);
}

}  // namespace

bool isWritableHoldingFunction(const ModbusFunctionType function) {
    return function == WRITE_HOLDING || function == WRITE_MULTIPLE_HOLDING;
}

bool isValidWritableScale(const double scale) {
    return std::isfinite(scale) && scale != 0.0;
}

ConversionResult convertPayload(const ModbusFunctionType function,
                                const std::string &payload,
                                const double scale,
                                const ModbusDataType dataType,
                                const ModbusWordOrder wordOrder) {
    return convertPayloadWithBoundary(function, payload, scale, dataType, wordOrder, nullptr);
}

NumberDiscoveryBounds numberDiscoveryBounds(const double scale, const ModbusDataType dataType) {
    NumberDiscoveryBounds result;
    if (!isValidWritableScale(scale)) {
        result.rejection = RejectionReason::InvalidScale;
        return result;
    }

    double low = 0.0;
    double high = 0.0;
    if (dataType == FLOAT32) {
        const double magnitude = static_cast<double>(std::numeric_limits<float>::max())
                                 * std::fabs(scale);
        low = -magnitude;
        high = magnitude;
        result.step = MINIMUM_DISCOVERY_STEP;
    } else {
        RawRange range;
        if (!rawRangeForType(dataType, range)) {
            result.rejection = RejectionReason::UnsupportedDataType;
            return result;
        }
        const double scaledMinimum = static_cast<double>(range.minimum) * scale;
        const double scaledMaximum = static_cast<double>(range.maximum) * scale;
        low = std::min(scaledMinimum, scaledMaximum);
        high = std::max(scaledMinimum, scaledMaximum);
        result.step = std::fabs(scale);
    }
    result.minimum = low;
    result.maximum = high;
    if (result.step < MINIMUM_DISCOVERY_STEP) result.step = MINIMUM_DISCOVERY_STEP;
    if (!std::isfinite(result.minimum) || !std::isfinite(result.maximum)
        || !std::isfinite(result.step) || result.step <= 0.0) {
        result.rejection = RejectionReason::NonFinite;
    }
    return result;
}

const char *rejectionReasonToString(const RejectionReason reason) {
    switch (reason) {
        case RejectionReason::Empty: return "empty";
        case RejectionReason::InvalidSyntax: return "invalid_syntax";
        case RejectionReason::NonFinite: return "non_finite";
        case RejectionReason::InvalidScale: return "invalid_scale";
        case RejectionReason::OutOfRange: return "out_of_range";
        case RejectionReason::UnsupportedDataType: return "unsupported_data_type";
        case RejectionReason::None: break;
    }
    return "none";
}

MessageHandler buildMessageHandler(const CommandContext &context,
                                   SubmitSink submit,
                                   WarningSink warning) {
    const SubmitSink submitSink = std::move(submit);
    const WarningSink warningSink = std::move(warning);
    const bool integerHolding = isWritableHoldingFunction(context.function)
                                && (ModbusValueCodec::isSignedInteger(context.dataType)
                                    || ModbusValueCodec::isUnsignedInteger(context.dataType));
    const RawLimits limits = integerHolding ? rawLimitsForType(context.dataType, context.scale)
                                            : RawLimits{};
    return [context, submitSink, warningSink, limits]
        (const std::string &topic, const std::string &payload) {
            const ConversionResult conversion = convertPayloadWithBoundary(
                context.function,
                payload,
                context.scale,
                context.dataType,
                context.wordOrder,
                limits.valid ? &limits : nullptr);
            if (!conversion.accepted()) {
                if (warningSink) {
                    warningSink(std::string("[Modbus][MQTT] Rejected command: topic=") + topic
                                + ", function=" + std::to_string(static_cast<int>(context.function))
                                + ", address=" + std::to_string(context.address)
                                + ", reason=" + rejectionReasonToString(conversion.rejection));
                }
                return;
            }
            if (!submitSink) return;

            WriteCommand command;
            command.sourceTopic = topic;
            command.slaveId = context.slaveId;
            command.function = context.function;
            command.address = context.address;
            command.registerCount = context.function == WRITE_COIL
                                        ? context.registerCount
                                        : conversion.wordCount;
            command.coilValue = conversion.rawValue != 0U;
            command.wordCount = conversion.wordCount;
            command.words = conversion.words;
            command.modbusGeneration = context.modbusGeneration;
            submitSink(command);
        };
}

}  // namespace ModbusMqttWriteCore
