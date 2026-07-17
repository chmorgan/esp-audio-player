#pragma once

#define ESP_RETURN_ON_FALSE(condition, error, tag, format, ...) \
    do {                                                         \
        if (!(condition)) {                                      \
            (void)(tag);                                         \
            return (error);                                      \
        }                                                        \
    } while (0)
