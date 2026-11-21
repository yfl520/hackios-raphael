/* Removable display experiment in lockdownd only. No activation records,
 * Activate replies, permissions, pairing identities or certificates are changed. */
static CFTypeRef cl_activation_display_value(CFStringRef key, CFTypeRef answer) {
    if (!key || !CFEqual(key, CFSTR("ActivationState")) || !answer ||
        CFGetTypeID(answer) != CFStringGetTypeID() ||
        CFEqual(answer, CFSTR("Activated"))) return answer;
    CFRelease(answer);
    return CFRetain(CFSTR("Activated"));
}

static CFPropertyListRef cl_activation_display_reply(CFPropertyListRef input) {
    if (!input || CFGetTypeID(input) != CFDictionaryGetTypeID())
        return input ? CFRetain(input) : NULL;
    CFDictionaryRef envelope = (CFDictionaryRef)input;
    CFTypeRef request = CFDictionaryGetValue(envelope, CFSTR("Request"));
    CFTypeRef error = CFDictionaryGetValue(envelope, CFSTR("Error"));
    CFTypeRef value = CFDictionaryGetValue(envelope, CFSTR("Value"));
    if (!request || !CFEqual(request, CFSTR("GetValue")) || error || !value ||
        CFGetTypeID(value) != CFDictionaryGetTypeID()) return CFRetain(input);
    CFDictionaryRef fields = (CFDictionaryRef)value;
    CFTypeRef product = CFDictionaryGetValue(fields, CFSTR("ProductType"));
    CFTypeRef version = CFDictionaryGetValue(fields, CFSTR("ProductVersion"));
    CFTypeRef state = CFDictionaryGetValue(fields, CFSTR("ActivationState"));
    if (!product || !CFEqual(product, CFSTR("iPhone10,3")) || !version ||
        CFGetTypeID(version) != CFStringGetTypeID() || !state ||
        CFGetTypeID(state) != CFStringGetTypeID()) return CFRetain(input);
    CFMutableDictionaryRef result = CFDictionaryCreateMutableCopy(NULL, 0, envelope);
    CFMutableDictionaryRef copy = CFDictionaryCreateMutableCopy(NULL, 0, fields);
    if (!result || !copy) {
        if (result) CFRelease(result);
        if (copy) CFRelease(copy);
        return CFRetain(input);
    }
    CFDictionarySetValue(copy, CFSTR("ActivationState"), CFSTR("Activated"));
    CFDictionarySetValue(result, CFSTR("Value"), copy);
    CFRelease(copy);
    return result;
}
