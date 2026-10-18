#pragma once

// Export macro for ftcore's non-inline free functions and public member functions. Mark the
// members, not the class: exporting a whole class that holds std:: members makes MSVC warn
// C4251 in the Unreal DLL build.
// Inside Unreal, UBT defines FORGOTTENCORE_API for the ForgottenCore module as DLLEXPORT there and
// DLLIMPORT in modules that depend on it (empty in monolithic builds). Those two come from UE's
// platform headers, which Core never includes, so define them here with UE's exact text; an
// identical redefinition is legal if a UE header follows. In the standalone CMake build it is empty.
#if defined(FORGOTTENCORE_API)
#if !defined(DLLEXPORT)
#if defined(_MSC_VER)
#define DLLEXPORT __declspec(dllexport)
#define DLLIMPORT __declspec(dllimport)
#else
#define DLLEXPORT __attribute__((visibility("default")))
#define DLLIMPORT __attribute__((visibility("default")))
#endif
#endif
#define FTCORE_API FORGOTTENCORE_API
#else
#define FTCORE_API
#endif
