/* R11 storage repair plus an explicit, lockdownd-only activation display test. */
#include <CoreFoundation/CoreFoundation.h>
#include <sys/sysctl.h>
#include <stdint.h>
#include <errno.h>
#include <dlfcn.h>
extern CFTypeRef MGCopyAnswer(CFStringRef key, CFDictionaryRef options);
#include "computer-link-storage.inc"
#include "computer-link-activation-display.inc"

static CFTypeRef cl_activation_answer(CFStringRef key, CFDictionaryRef options) {
    CFTypeRef answer = cl_storage_answer(key, options);
    int saved = errno;
    CFStringRef friendly = key;
    CFStringRef *alias = dlsym(RTLD_DEFAULT, "kMGQActivationState");
    if (key && alias && *alias && CFGetTypeID(*alias) == CFStringGetTypeID() &&
        CFEqual(key, *alias)) friendly = CFSTR("ActivationState");
    answer = cl_activation_display_value(friendly, answer);
    errno = saved;
    return answer;
}

static CFDataRef cl_activation_serialize(CFAllocatorRef allocator, CFPropertyListRef value,
    CFPropertyListFormat format, CFOptionFlags options, CFErrorRef *error) {
    int saved = errno;
    CFPropertyListRef copy = cl_activation_display_reply(value);
    errno = saved;
    CFDataRef data = CFPropertyListCreateData(allocator, copy, format, options, error);
    saved = errno;
    if (copy) CFRelease(copy);
    errno = saved;
    return data;
}

__attribute__((used,section("__DATA,__interpose"))) static const struct {
    const void *replacement, *original;
} activation_display_interpose[] = {
    {(const void *)cl_activation_answer, (const void *)MGCopyAnswer},
    {(const void *)cl_activation_serialize, (const void *)CFPropertyListCreateData}
};
