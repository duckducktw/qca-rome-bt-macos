#ifndef QCA6174_BUILD_TARGETCONDITIONALS_WRAPPER_H
#define QCA6174_BUILD_TARGETCONDITIONALS_WRAPPER_H

#include_next <TargetConditionals.h>

/* MacKernelSDK's legacy IOPCIDevice header uses this branch selector to avoid
 * importing the DriverKit class with the same name. This wrapper is used only
 * by scripts/build_kext_macos.sh. */
#undef TARGET_OS_OSX
#define TARGET_OS_OSX 0

#endif
