//
// glibc compatibility wrappers for GCC 13+ with older glibc
//

#include <stdlib.h>

// Wrapper for __isoc23_strtol to call the legacy strtol
long __wrap___isoc23_strtol(const char *nptr, char **endptr, int base) {
    return strtol(nptr, endptr, base);
}

// Wrapper for __isoc23_strtoll to call the legacy strtoll
long long __wrap___isoc23_strtoll(const char *nptr, char **endptr, int base) {
    return strtoll(nptr, endptr, base);
}

// Wrapper for __isoc23_strtoul to call the legacy strtoul
unsigned long __wrap___isoc23_strtoul(const char *nptr, char **endptr, int base) {
    return strtoul(nptr, endptr, base);
}

// Wrapper for __isoc23_strtoull to call the legacy strtoull
unsigned long long __wrap___isoc23_strtoull(const char *nptr, char **endptr, int base) {
    return strtoull(nptr, endptr, base);
}
