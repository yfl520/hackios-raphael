/* Bridge the real firmware USB mux state into the IOKit notifications used by
 * the unchanged native lockdownd. This service does not own the USB hardware
 * and offers no user client, trust, pairing-record or activation overrides. */
#include <IOKit/IOService.h>
#include <libkern/c++/OSBoolean.h>
class IOUSBDeviceInterface : public IOService {
    OSDeclareDefaultStructors(IOUSBDeviceInterface)
};
OSDefineMetaClassAndStructors(IOUSBDeviceInterface, IOService)
class AppleUSBDeviceMux : public IOUSBDeviceInterface {
    OSDeclareDefaultStructors(AppleUSBDeviceMux)
};
OSDefineMetaClassAndStructors(AppleUSBDeviceMux, IOUSBDeviceInterface)
extern "C" void loader_log_line(const char *line);
extern "C" void computer_link_usb_state_update(int attached) {
    static AppleUSBDeviceMux *service;
    static bool previous;
    if (!service) {
        IOService *parent = IOService::getServiceRoot();
        if (!parent) return;
        AppleUSBDeviceMux *candidate = new AppleUSBDeviceMux;
        if (!candidate) return;
        if (!candidate->init()) { candidate->release(); return; }
        candidate->setName("Raphael USB mux");
        candidate->setProperty("USBDeviceFunction", "AppleUSBMux");
        candidate->setProperty("IsActive", kOSBooleanFalse);
        candidate->setProperty("TransportOwner", "Qualcomm UEFI");
        if (!candidate->attach(parent)) { candidate->release(); return; }
        service=candidate; service->registerService();
        loader_log_line("computer-link: physical USB state notification service published\n");
    }
    bool active=attached!=0;
    if (active==previous) return;
    previous=active; service->setProperty("IsActive",active ? kOSBooleanTrue:kOSBooleanFalse);
    /* Native lockdownd subscribes to this IOGeneralInterest ordinal. */
    service->messageClients(0xe3ff8201u,(void *)(uintptr_t)(active ? 1:0));
    loader_log_line(active ? "computer-link: negotiated physical USB host attached\n":
        "computer-link: physical USB host detached\n");
}
