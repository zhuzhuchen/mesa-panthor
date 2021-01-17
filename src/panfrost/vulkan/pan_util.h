#ifndef PAN_UTIL_H
#define PAN_UTIL_H

#ifdef HAVE___BUILTIN_POPCOUNT
#define util_bitcount(i) __builtin_popcount(i)
#else
extern unsigned int
util_bitcount(unsigned int n);
#endif

#endif /* PAN_UTIL_H */
