/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QCAROMEPATCHRAM_HPP
#define QCAROMEPATCHRAM_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostPipe.h>

#include "qca_rome.h"

#define QCAROME_VENDOR_ID   0x04ca
#define QCAROME_PRODUCT_ID  0x3016

class QCARomePatchRAM : public IOService {
    OSDeclareDefaultStructors(QCARomePatchRAM)

public:
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

    /* qca_transport callbacks (ctx == this). Public only so the C-linkage
     * trampolines in the .cpp can reach them. */
    int  xferControlIn(uint8_t req, void *buf, uint16_t len, uint32_t *actual, uint32_t timeoutMs);
    int  xferControlOut(uint8_t req, const void *buf, uint16_t len, uint32_t *actual, uint32_t timeoutMs);
    int  xferBulkOut(uint8_t ep, const void *buf, uint32_t len, uint32_t *actual, uint32_t timeoutMs);

private:
    bool openDeviceAndInterface(IOUSBHostDevice *dev, uint8_t *configValueOut, bool *weConfigured);
    void closeAll(IOUSBHostDevice *dev);
    void publishResult(IOUSBHostDevice *dev, const qca_result &r);

    IOUSBHostDevice          *fDevice     = nullptr;
    IOUSBHostInterface       *fInterface  = nullptr;
    IOUSBHostPipe            *fBulkOut    = nullptr;
    IOBufferMemoryDescriptor *fBulkBuffer = nullptr;
    bool                      fDeviceOpen = false;
    bool                      fIfaceOpen  = false;
};

#endif
