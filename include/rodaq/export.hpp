#ifndef RODAQ_EXPORT_HPP_
#define RODAQ_EXPORT_HPP_

#if defined(_WIN32)
#if defined(RODAQ_EXPORTS)
#define RODAQ_EXPORT __declspec(dllexport)
#define RODAQ_LOCAL
#else
#define RODAQ_EXPORT __declspec(dllimport)
#define RODAQ_LOCAL
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define RODAQ_EXPORT [[gnu::visibility("default")]]
#define RODAQ_LOCAL [[gnu::visibility("hidden")]]
#else
#define RODAQ_EXPORT
#define RODAQ_LOCAL
#endif

#endif  // RODAQ_EXPORT_HPP_
