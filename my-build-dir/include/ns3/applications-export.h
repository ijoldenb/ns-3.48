
#ifndef APPLICATIONS_EXPORT_H
#define APPLICATIONS_EXPORT_H

#ifdef APPLICATIONS_STATIC_DEFINE
#  define APPLICATIONS_EXPORT
#  define APPLICATIONS_NO_EXPORT
#else
#  ifndef APPLICATIONS_EXPORT
#    ifdef applications_EXPORTS
        /* We are building this library */
#      define APPLICATIONS_EXPORT __attribute__((visibility("default")))
#    else
        /* We are using this library */
#      define APPLICATIONS_EXPORT __attribute__((visibility("default")))
#    endif
#  endif

#  ifndef APPLICATIONS_NO_EXPORT
#    define APPLICATIONS_NO_EXPORT __attribute__((visibility("hidden")))
#  endif
#endif

#ifndef APPLICATIONS_DEPRECATED
#  define APPLICATIONS_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef APPLICATIONS_DEPRECATED_EXPORT
#  define APPLICATIONS_DEPRECATED_EXPORT APPLICATIONS_EXPORT APPLICATIONS_DEPRECATED
#endif

#ifndef APPLICATIONS_DEPRECATED_NO_EXPORT
#  define APPLICATIONS_DEPRECATED_NO_EXPORT APPLICATIONS_NO_EXPORT APPLICATIONS_DEPRECATED
#endif

/* NOLINTNEXTLINE(readability-avoid-unconditional-preprocessor-if) */
#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef APPLICATIONS_NO_DEPRECATED
#    define APPLICATIONS_NO_DEPRECATED
#  endif
#endif

// Undefine the *_EXPORT symbols for non-Windows based builds
#ifndef NS_MSVC
#undef APPLICATIONS_EXPORT
#define APPLICATIONS_EXPORT
#undef APPLICATIONS_NO_EXPORT
#define APPLICATIONS_NO_EXPORT
#endif
#endif /* APPLICATIONS_EXPORT_H */
