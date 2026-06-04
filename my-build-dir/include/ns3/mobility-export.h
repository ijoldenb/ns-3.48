
#ifndef MOBILITY_EXPORT_H
#define MOBILITY_EXPORT_H

#ifdef MOBILITY_STATIC_DEFINE
#  define MOBILITY_EXPORT
#  define MOBILITY_NO_EXPORT
#else
#  ifndef MOBILITY_EXPORT
#    ifdef mobility_EXPORTS
        /* We are building this library */
#      define MOBILITY_EXPORT __attribute__((visibility("default")))
#    else
        /* We are using this library */
#      define MOBILITY_EXPORT __attribute__((visibility("default")))
#    endif
#  endif

#  ifndef MOBILITY_NO_EXPORT
#    define MOBILITY_NO_EXPORT __attribute__((visibility("hidden")))
#  endif
#endif

#ifndef MOBILITY_DEPRECATED
#  define MOBILITY_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef MOBILITY_DEPRECATED_EXPORT
#  define MOBILITY_DEPRECATED_EXPORT MOBILITY_EXPORT MOBILITY_DEPRECATED
#endif

#ifndef MOBILITY_DEPRECATED_NO_EXPORT
#  define MOBILITY_DEPRECATED_NO_EXPORT MOBILITY_NO_EXPORT MOBILITY_DEPRECATED
#endif

/* NOLINTNEXTLINE(readability-avoid-unconditional-preprocessor-if) */
#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef MOBILITY_NO_DEPRECATED
#    define MOBILITY_NO_DEPRECATED
#  endif
#endif

// Undefine the *_EXPORT symbols for non-Windows based builds
#ifndef NS_MSVC
#undef MOBILITY_EXPORT
#define MOBILITY_EXPORT
#undef MOBILITY_NO_EXPORT
#define MOBILITY_NO_EXPORT
#endif
#endif /* MOBILITY_EXPORT_H */
