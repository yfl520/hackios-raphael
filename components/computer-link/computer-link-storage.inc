/* Included in the lockdownd-only shim and its host regression. Original valid
 * answers remain untouched. Only a missing/negative total is replaced, using
 * the kernel's read-only GPT-derived darwin capacity, never a model size. */
static CFTypeRef cl_storage_answer(CFStringRef key,CFDictionaryRef options) {
    CFTypeRef answer=MGCopyAnswer(key,options);
    int saved_errno=errno;
    if (!key || !CFEqual(key,CFSTR("DiskUsage")) || !answer ||
        CFGetTypeID(answer)!=CFDictionaryGetTypeID()) return answer;
    CFStringRef total_key=CFSTR("TotalDiskCapacity");
    CFStringRef *exported=dlsym(RTLD_DEFAULT,"kMGQDiskUsageTotalDiskCapacity");
    if (exported && *exported && CFGetTypeID(*exported)==CFStringGetTypeID()) total_key=*exported;
    CFTypeRef old=CFDictionaryGetValue((CFDictionaryRef)answer,total_key);
    int64_t value=0;
    if (old && (CFGetTypeID(old)!=CFNumberGetTypeID() ||
        !CFNumberGetValue((CFNumberRef)old,kCFNumberSInt64Type,&value) || value>0)) {
        errno=saved_errno;return answer;
    }
    uint64_t capacity=0;size_t length=sizeof(capacity);
    if (sysctlbyname("kern.hakios.darwin_capacity",&capacity,&length,NULL,0) ||
        length!=sizeof(capacity) || !capacity || capacity>INT64_MAX) {
        errno=saved_errno;return answer;
    }
    CFMutableDictionaryRef copy=CFDictionaryCreateMutableCopy(NULL,0,(CFDictionaryRef)answer);
    int64_t bytes=(int64_t)capacity;
    CFNumberRef number=CFNumberCreate(NULL,kCFNumberSInt64Type,&bytes);
    if (!copy || !number) {
        if(copy)CFRelease(copy);if(number)CFRelease(number);
        errno=saved_errno;return answer;
    }
    CFDictionarySetValue(copy,total_key,number);CFRelease(number);CFRelease(answer);
    errno=saved_errno;return copy;
}
