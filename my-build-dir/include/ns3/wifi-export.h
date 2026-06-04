
#ifndef WIFI_EXPORT_H
#define WIFI_EXPORT_H

#ifdef WIFI_STATIC_DEFINE
#  define WIFI_EXPORT
#  define WIFI_NO_EXPORT
#else
#  ifndef WIFI_EXPORT
#    ifdef wifi_EXPORTS
        /* We are building this library */
#      define WIFI_EXPORT __attribute__((visibility("default")))
#    else
        /* We are using this library */
#      define WIFI_EXPORT __attribute__((visibility("default")))
#    endif
#  endif

#  ifndef WIFI_NO_EXPORT
#    define WIFI_NO_EXPORT __attribute__((visibility("hidden")))
#  endif
#endif

#ifndef WIFI_DEPRECATED
#  define WIFI_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef WIFI_DEPRECATED_EXPORT
#  define WIFI_DEPRECATED_EXPORT WIFI_EXPORT WIFI_DEPRECATED
#endif

#ifndef WIFI_DEPRECATED_NO_EXPORT
#  define WIFI_DEPRECATED_NO_EXPORT WIFI_NO_EXPORT WIFI_DEPRECATED
#endif

/* NOLINTNEXTLINE(readability-avoid-unconditional-preprocessor-if) */
#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef WIFI_NO_DEPRECATED
#    define WIFI_NO_DEPRECATED
#  endif
#endif

// Undefine the *_EXPORT symbols for non-Windows based builds
#ifndef NS_MSVC
#undef WIFI_EXPORT
#define WIFI_EXPORT
#undef WIFI_NO_EXPORT
#define WIFI_NO_EXPORT
#endif
#endif /* WIFI_EXPORT_H */
