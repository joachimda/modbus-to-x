#include "utils/StringUtils.h"

String StringUtils::slugify(String text) {
    String out;
    out.reserve(text.length());
    bool lastUnderscore = false;
    for (size_t i = 0; i < text.length(); ++i) {
        const auto raw = static_cast<unsigned char>(text[i]);
        if (std::isalnum(raw)) {
            const char lower = static_cast<char>(std::tolower(raw));
            out += lower;
            lastUnderscore = false;
        } else if (!lastUnderscore && out.length() > 0U) {
            out += '_';
            lastUnderscore = true;
        }
    }
    while (out.length() > 0U && out[out.length() - 1] == '_') {
        out.remove(out.length() - 1);
    }
    if (out.length() == 0U) {
        return {"device"};
    }
    return out;
}
