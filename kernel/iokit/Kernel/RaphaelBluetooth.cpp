/* Expose only the real initialized QCA HCI transport to iOS discovery.
 * bluetoothd 16.5 FindLocalMacAddress looks up a service named bluetooth,
 * then local-mac-address in IODeviceTree. The transport itself stays H4/POSIX.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>

extern "C" void hakios_tl(const char *format, ...) __printflike(1, 2);

extern "C" bool
raphael_bt_publish(const uint8_t *address, unsigned length)
{
    if (address == NULL || length != 6) return false;
    IOService *parent = IOService::getResourceService();
    IORegistryEntry *armio = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
    IOService *service = OSTypeAlloc(IOService);
    if (parent == NULL || armio == NULL || service == NULL) {
        OSSafeReleaseNULL(armio);
        OSSafeReleaseNULL(service);
        return false;
    }
    if (!service->init() || !service->attach(parent)) {
        armio->release();service->release();return false;
    }
    service->setName("bluetooth");
    OSData *mac = OSData::withBytes(address, length);
    if (mac == NULL || !service->setProperty("local-mac-address", mac)) {
        OSSafeReleaseNULL(mac);
        service->detach(parent);armio->release();service->release();return false;
    }
    mac->release();
    service->setProperty("compatible", "qcom,wcn3990-bt");
    service->setProperty("HCITransport", "com.raphael.bt.uart");
    if (!service->attachToParent(armio, gIODTPlane)) {
        service->detach(parent);armio->release();service->release();return false;
    }
    armio->release();
    if (!service->start(parent)) {
        service->detachAll(gIODTPlane);service->detach(parent);service->release();return false;
    }
    service->registerService();
    hakios_tl("raphael-bt: IOService bluetooth registered with controller address; H4/POSIX transport\n");
    service->release();
    return true;
}
