#include "modbus/ModbusMqttWriteCore.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>

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

DecimalMagnitude maximumEngineeringMagnitude(const double scale) {
    int exponent2 = 0;
    const double fraction = std::frexp(std::fabs(scale), &exponent2);
    const int significandBits = std::numeric_limits<double>::digits;
    const uint64_t significand = static_cast<uint64_t>(std::ldexp(fraction, significandBits));

    DecimalMagnitude result;
    result.zero = false;
    result.digits = std::to_string(significand);
    multiplyDecimalDigits(result.digits, 65535U);

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

bool isOutsideRawRange(const DecimalMagnitude &value,
                       const DecimalMagnitude &maximumMagnitude,
                       const double scale) {
    if (value.zero) return false;
    if (value.negative != std::signbit(scale)) return true;
    return compareMagnitude(value, maximumMagnitude) > 0;
}

ConversionResult reject(const RejectionReason reason) {
    ConversionResult result;
    result.rejection = reason;
    return result;
}

ConversionResult convertPayloadWithBoundary(const ModbusFunctionType function,
                                            const std::string &payload,
                                            const double scale,
                                            const DecimalMagnitude *maximumMagnitude) {
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

    DecimalMagnitude parsedDecimal;
    if (!isWritableHoldingFunction(function) || !parseDecimal(value, parsedDecimal)) {
        return reject(RejectionReason::InvalidSyntax);
    }

    char *end = nullptr;
    const double engineering = std::strtod(value.c_str(), &end);
    if (end == nullptr || end != value.c_str() + value.size()) {
        return reject(RejectionReason::InvalidSyntax);
    }
    if (!std::isfinite(engineering)) return reject(RejectionReason::NonFinite);
    if (!isValidWritableScale(scale)) return reject(RejectionReason::InvalidScale);

    DecimalMagnitude computedMaximum;
    if (maximumMagnitude == nullptr) {
        computedMaximum = maximumEngineeringMagnitude(scale);
        maximumMagnitude = &computedMaximum;
    }
    if (isOutsideRawRange(parsedDecimal, *maximumMagnitude, scale)) {
        return reject(RejectionReason::OutOfRange);
    }

    const double rawExact = engineering / scale;
    if (!std::isfinite(rawExact)) return reject(RejectionReason::NonFinite);
    if (rawExact < 0.0 || rawExact > 65535.0) return reject(RejectionReason::OutOfRange);

    ConversionResult result;
    result.rawValue = static_cast<uint16_t>(std::floor(rawExact + 0.5));
    return result;
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
                                const double scale) {
    return convertPayloadWithBoundary(function, payload, scale, nullptr);
}

NumberDiscoveryBounds numberDiscoveryBounds(const double scale) {
    NumberDiscoveryBounds result;
    if (!isValidWritableScale(scale)) {
        result.rejection = RejectionReason::InvalidScale;
        return result;
    }

    const double scaledMaximum = 65535.0 * scale;
    if (!std::isfinite(scaledMaximum)) {
        result.rejection = RejectionReason::NonFinite;
        return result;
    }
    result.minimum = std::min(0.0, scaledMaximum);
    result.maximum = std::max(0.0, scaledMaximum);
    result.step = std::fabs(scale);
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
        case RejectionReason::None: break;
    }
    return "none";
}

MessageHandler buildMessageHandler(const CommandContext &context,
                                   SubmitSink submit,
                                   WarningSink warning) {
    const SubmitSink submitSink = std::move(submit);
    const WarningSink warningSink = std::move(warning);
    const bool hasMaximumMagnitude = isWritableHoldingFunction(context.function)
                                     && isValidWritableScale(context.scale);
    const DecimalMagnitude maximumMagnitude = hasMaximumMagnitude
                                              ? maximumEngineeringMagnitude(context.scale)
                                              : DecimalMagnitude{};
    return [context, submitSink, warningSink, hasMaximumMagnitude, maximumMagnitude]
        (const std::string &topic, const std::string &payload) {
            const ConversionResult conversion = convertPayloadWithBoundary(
                context.function,
                payload,
                context.scale,
                hasMaximumMagnitude ? &maximumMagnitude : nullptr);
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
            command.registerCount = context.function == WRITE_MULTIPLE_HOLDING
                                      ? 1U
                                      : context.registerCount;
            command.rawValue = conversion.rawValue;
            command.modbusGeneration = context.modbusGeneration;
            submitSink(command);
        };
}

}  // namespace ModbusMqttWriteCore
