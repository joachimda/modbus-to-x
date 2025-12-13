#ifndef STRINGUTILS_H
#define STRINGUTILS_H

#include <Arduino.h>

class StringUtils {
public:
    // Create a slug from the given text: trim, lowercase, keep [a-z0-9],
    // convert spaces/_/-/ / to single underscores, and trim trailing underscores.
    static String slugify(String text);
};

#endif // STRINGUTILS_H
