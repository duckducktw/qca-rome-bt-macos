/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Kext module info, equivalent to the <target>_info.c that Xcode generates
 * for "Kernel Extension" targets. libkmod.a provides _start/_stop, which
 * call _realmain/_antimain (NULL here: an IOKit-only kext needs no module
 * start routine) and run C++ static constructors.
 *
 * QCAROME_BUNDLE_ID / QCAROME_VERSION are passed with -D by the build script
 * so they stay in sync with Info.plist.
 */
#include <mach/mach_types.h>
#include <mach/kmod.h>

#ifndef QCAROME_BUNDLE_ID
#error "QCAROME_BUNDLE_ID must be defined"
#endif
#ifndef QCAROME_VERSION
#error "QCAROME_VERSION must be defined"
#endif

extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);

/* Spelled out instead of KMOD_EXPLICIT_DECL because that macro stringifies
 * its `name` token, which would not expand QCAROME_BUNDLE_ID. */
__attribute__((visibility("default")))
kmod_info_t KMOD_INFO_NAME = {
    0, KMOD_INFO_VERSION, -1U,
    { QCAROME_BUNDLE_ID }, { QCAROME_VERSION },
    -1, 0, 0, 0, 0,
    _start, _stop
};

__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
