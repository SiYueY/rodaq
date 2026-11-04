#ifndef RODAQ_RUNTIME_EXPORT_HPP_
#define RODAQ_RUNTIME_EXPORT_HPP_

#if defined(_WIN32)
#if defined(RODAQ_RUNTIME_EXPORTS)
#define RODAQ_RUNTIME_EXPORT __declspec(dllexport)
#else
#define RODAQ_RUNTIME_EXPORT __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define RODAQ_RUNTIME_EXPORT __attribute__((visibility("default")))
#else
#define RODAQ_RUNTIME_EXPORT
#endif

#endif  // RODAQ_RUNTIME_EXPORT_HPP_
