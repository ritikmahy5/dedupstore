#pragma once

// Clang thread-safety annotations (D17). With clang and -Wthread-safety, the compiler proves that
// every DDS_GUARDED_BY field is accessed with its mutex held and that DDS_REQUIRES functions are
// called with their locks. Other compilers see empty macros; CI builds once with clang.
// Reference: https://clang.llvm.org/docs/ThreadSafetyAnalysis.html
//
// DDS_CAPABILITY(name)      a class that is a lock (a capability)
// DDS_SCOPED_CAPABILITY     an RAII class that acquires in its constructor, releases in its
//                           destructor
// DDS_GUARDED_BY(mu)        a field readable and writable only with `mu` held
// DDS_PT_GUARDED_BY(mu)     a pointer field whose pointee is accessible only with `mu` held
// DDS_REQUIRES(...)         a function that must be called with these mutexes held
// DDS_EXCLUDES(...)         a function that must be called without them (it acquires them)
// DDS_ACQUIRE(...)          a function that acquires the capability (`this` if none is named)
// DDS_RELEASE(...)          a function that releases it

#if defined(__clang__)
#define DDS_CAPABILITY(name) __attribute__((capability(name)))
#define DDS_SCOPED_CAPABILITY __attribute__((scoped_lockable))
#define DDS_GUARDED_BY(mu) __attribute__((guarded_by(mu)))
#define DDS_PT_GUARDED_BY(mu) __attribute__((pt_guarded_by(mu)))
#define DDS_REQUIRES(...) __attribute__((requires_capability(__VA_ARGS__)))
#define DDS_EXCLUDES(...) __attribute__((locks_excluded(__VA_ARGS__)))
#define DDS_ACQUIRE(...) __attribute__((acquire_capability(__VA_ARGS__)))
#define DDS_RELEASE(...) __attribute__((release_capability(__VA_ARGS__)))
#else
#define DDS_CAPABILITY(name)
#define DDS_SCOPED_CAPABILITY
#define DDS_GUARDED_BY(mu)
#define DDS_PT_GUARDED_BY(mu)
#define DDS_REQUIRES(...)
#define DDS_EXCLUDES(...)
#define DDS_ACQUIRE(...)
#define DDS_RELEASE(...)
#endif
