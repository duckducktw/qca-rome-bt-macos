// SPDX-License-Identifier: GPL-2.0-or-later
//
// QCARomePatchRAM: one-shot firmware uploader for Lite-On 04ca:3016
// (QCA6174A-5 / Qualcomm Rome 3.2 USB Bluetooth).
//
// Lifecycle (modelled on acidanthera/BrcmPatchRAM3, IOUSBHostDevice API):
//   probe  : verify VID/PID, raise score. No device I/O.
//   start  : open IOUSBHostDevice, ensure configured, open interface 0,
//            copy bulk-OUT pipe 0x02, run the portable qca_rome state machine,
//            then close everything and return false so this object is
//            detached. The personality uses its own IOMatchCategory, so Apple
//            transports in other categories are never blocked; after closing
//            we re-register the device/interface to re-trigger matching.
//
// Boot args:  -qcarpoff   skip entirely
//             -qcarpprobe read-only probe (GET_VERSION + CHECK_STATUS)
#include "QCARomePatchRAM.hpp"

#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <libkern/version.h>
#include <pexpert/pexpert.h>

#include "qca_rome_firmware.h"

#define LOG(fmt, ...) IOLog("QCARomePatchRAM: " fmt "\n", ##__VA_ARGS__)

// Set on the IOUSBHostDevice after one attempt. Prevents a match loop when we
// re-register the device so Apple's transport can match. A real re-enumeration
// (replug, sleep/wake power loss) creates a new IOUSBHostDevice without it.
#define kQCAAttemptedKey "QCARomePatchRAM-Attempted"

#define super IOService
OSDefineMetaClassAndStructors(QCARomePatchRAM, IOService)

namespace {

int trCtrlIn(void *ctx, uint8_t req, void *buf, uint16_t len, uint32_t *actual, uint32_t to)
{
    return static_cast<QCARomePatchRAM *>(ctx)->xferControlIn(req, buf, len, actual, to);
}

int trCtrlOut(void *ctx, uint8_t req, const void *buf, uint16_t len, uint32_t *actual, uint32_t to)
{
    return static_cast<QCARomePatchRAM *>(ctx)->xferControlOut(req, buf, len, actual, to);
}

int trBulkOut(void *ctx, uint8_t ep, const void *buf, uint32_t len, uint32_t *actual, uint32_t to)
{
    return static_cast<QCARomePatchRAM *>(ctx)->xferBulkOut(ep, buf, len, actual, to);
}

void trSleep(void *, uint32_t ms)
{
    IOSleep(ms);
}

void trLog(void *, const char *msg, uint32_t a, uint32_t b, uint32_t c)
{
    char line[160];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
    snprintf(line, sizeof(line), msg, a, b, c);
#pragma clang diagnostic pop
    LOG("%s", line);
}

bool bootFlag(const char *name)
{
    char dummy[8];
    return PE_parse_boot_argn(name, dummy, sizeof(dummy));
}

} // namespace

IOService *QCARomePatchRAM::probe(IOService *provider, SInt32 *score)
{
    IOUSBHostDevice *dev = OSDynamicCast(IOUSBHostDevice, provider);
    if (!dev) {
        LOG("probe: provider is not IOUSBHostDevice");
        return nullptr;
    }
    const StandardUSB::DeviceDescriptor *dd = dev->getDeviceDescriptor();
    if (!dd) {
        LOG("probe: no device descriptor");
        return nullptr;
    }
    uint16_t vid = USBToHost16(dd->idVendor);
    uint16_t pid = USBToHost16(dd->idProduct);
    if (vid != QCAROME_VENDOR_ID || pid != QCAROME_PRODUCT_ID) {
        LOG("probe: unexpected device %04x:%04x, declining", vid, pid);
        return nullptr;
    }
    if (dev->getProperty(kQCAAttemptedKey)) {
        // Already handled this device instance; stay out of the way.
        return nullptr;
    }
    if (bootFlag("-qcarpoff")) {
        LOG("probe: disabled by -qcarpoff");
        return nullptr;
    }
    LOG("probe: %04x:%04x on Darwin %d.%d", vid, pid, version_major, version_minor);
    IOService *ret = super::probe(provider, score);
    if (score)
        *score = 4000;
    return ret;
}

bool QCARomePatchRAM::openDeviceAndInterface(IOUSBHostDevice *dev, uint8_t *configValueOut,
                                             bool *weConfigured)
{
    *weConfigured = false;
    if (!dev->open(this)) {
        LOG("could not open IOUSBHostDevice (already owned?)");
        return false;
    }
    fDeviceOpen = true;

    // Current configuration via standard GET_CONFIGURATION.
    uint8_t current = 0;
    {
        StandardUSB::DeviceRequest req;
        req.bmRequestType = makeDeviceRequestbmRequestType(kRequestDirectionIn, kRequestTypeStandard,
                                                           kRequestRecipientDevice);
        req.bRequest = kDeviceRequestGetConfiguration;
        req.wValue = 0;
        req.wIndex = 0;
        req.wLength = 1;
        uint32_t got = 0;
        IOReturn rc = dev->deviceRequest(this, req, &current, got,
                                         kUSBHostStandardRequestCompletionTimeout);
        if (rc != kIOReturnSuccess || got != 1) {
            LOG("GET_CONFIGURATION failed (0x%08x, %u bytes)", rc, got);
            return false;
        }
    }

    const StandardUSB::ConfigurationDescriptor *cd = dev->getConfigurationDescriptor(0);
    if (!cd) {
        LOG("no configuration descriptor at index 0");
        return false;
    }
    *configValueOut = cd->bConfigurationValue;
    LOG("current configuration %u, descriptor bConfigurationValue %u, interfaces %u",
        current, cd->bConfigurationValue, cd->bNumInterfaces);

    if (current == 0) {
        // Interfaces are only published once the device is configured with
        // interface matching enabled (same as acidanthera/BrcmPatchRAM). We
        // open interface 0 ourselves immediately after, so a competing HCI
        // driver cannot start DFU traffic on EP2 before the upload finishes.
        IOReturn rc = dev->setConfiguration(cd->bConfigurationValue, true);
        if (rc != kIOReturnSuccess) {
            LOG("setConfiguration(%u) failed (0x%08x)", cd->bConfigurationValue, rc);
            return false;
        }
        *weConfigured = true;
    }

    // Find interface 0 among the device's children (interfaces may be
    // published shortly after setConfiguration).
    for (int attempt = 0; attempt < 200 && !fInterface; attempt++) {
        OSIterator *it = dev->getChildIterator(gIOServicePlane);
        if (it) {
            while (OSObject *o = it->getNextObject()) {
                IOUSBHostInterface *intf = OSDynamicCast(IOUSBHostInterface, o);
                if (!intf)
                    continue;
                const StandardUSB::InterfaceDescriptor *id = intf->getInterfaceDescriptor();
                if (id && id->bInterfaceNumber == 0 && id->bAlternateSetting == 0) {
                    intf->retain();
                    fInterface = intf;
                    break;
                }
            }
            it->release();
        }
        if (!fInterface)
            IOSleep(10);
    }
    if (!fInterface) {
        LOG("interface 0 not found");
        return false;
    }
    if (!fInterface->open(this)) {
        LOG("could not open interface 0 (owned by another driver?)");
        return false;
    }
    fIfaceOpen = true;

    fBulkOut = fInterface->copyPipe(QCA_DFU_BULK_EP);
    if (!fBulkOut) {
        LOG("copyPipe(0x%02x) failed", QCA_DFU_BULK_EP);
        return false;
    }
    const StandardUSB::EndpointDescriptor *ed = fBulkOut->getEndpointDescriptor();
    if (!ed || StandardUSB::getEndpointType(ed) != kEndpointTypeBulk ||
        StandardUSB::getEndpointDirection(ed) != kEndpointDirectionOut) {
        LOG("endpoint 0x%02x is not bulk OUT", QCA_DFU_BULK_EP);
        return false;
    }

    fBulkBuffer = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionOut,
                                                              QCA_DFU_PACKET_LEN);
    if (!fBulkBuffer) {
        LOG("bulk buffer allocation failed");
        return false;
    }
    if (fBulkBuffer->prepare(kIODirectionOut) != kIOReturnSuccess) {
        LOG("bulk buffer prepare failed");
        OSSafeReleaseNULL(fBulkBuffer);
        return false;
    }
    return true;
}

void QCARomePatchRAM::closeAll(IOUSBHostDevice *dev)
{
    if (fBulkBuffer) {
        fBulkBuffer->complete(kIODirectionOut);
        OSSafeReleaseNULL(fBulkBuffer);
    }
    if (fBulkOut) {
        fBulkOut->abort();
        OSSafeReleaseNULL(fBulkOut);
    }
    if (fInterface) {
        if (fIfaceOpen)
            fInterface->close(this);
        fIfaceOpen = false;
    }
    if (fDeviceOpen && dev)
        dev->close(this);
    fDeviceOpen = false;
}

int QCARomePatchRAM::xferControlIn(uint8_t reqCode, void *buf, uint16_t len, uint32_t *actual,
                                   uint32_t timeoutMs)
{
    StandardUSB::DeviceRequest req;
    req.bmRequestType = makeDeviceRequestbmRequestType(kRequestDirectionIn, kRequestTypeVendor,
                                                       kRequestRecipientDevice);
    req.bRequest = reqCode;
    req.wValue = 0;
    req.wIndex = 0;
    req.wLength = len;
    uint32_t got = 0;
    IOReturn rc = fDevice->deviceRequest(this, req, buf, got, timeoutMs);
    *actual = got;
    if (rc != kIOReturnSuccess)
        LOG("vendor IN 0x%02x failed (0x%08x)", reqCode, rc);
    return (int)rc;
}

int QCARomePatchRAM::xferControlOut(uint8_t reqCode, const void *buf, uint16_t len,
                                    uint32_t *actual, uint32_t timeoutMs)
{
    uint8_t local[64];
    if (len > sizeof(local))
        return (int)kIOReturnBadArgument;
    memcpy(local, buf, len);

    StandardUSB::DeviceRequest req;
    req.bmRequestType = makeDeviceRequestbmRequestType(kRequestDirectionOut, kRequestTypeVendor,
                                                       kRequestRecipientDevice);
    req.bRequest = reqCode;
    req.wValue = 0;
    req.wIndex = 0;
    req.wLength = len;
    uint32_t got = 0;
    IOReturn rc = fDevice->deviceRequest(this, req, local, got, timeoutMs);
    *actual = got;
    if (rc != kIOReturnSuccess)
        LOG("vendor OUT 0x%02x (%u bytes) failed (0x%08x)", reqCode, len, rc);
    return (int)rc;
}

int QCARomePatchRAM::xferBulkOut(uint8_t ep, const void *buf, uint32_t len, uint32_t *actual,
                                 uint32_t timeoutMs)
{
    *actual = 0;
    if (ep != QCA_DFU_BULK_EP || !fBulkOut || !fBulkBuffer || len > QCA_DFU_PACKET_LEN)
        return (int)kIOReturnBadArgument;
    memcpy(fBulkBuffer->getBytesNoCopy(), buf, len);
    uint32_t sent = 0;
    IOReturn rc = fBulkOut->io(fBulkBuffer, len, sent, timeoutMs);
    *actual = sent;
    if (rc != kIOReturnSuccess) {
        LOG("bulk OUT %u bytes failed (0x%08x)", len, rc);
        if (rc != kIOReturnAborted && rc != kIOReturnNoDevice)
            fBulkOut->clearStall(true);
    }
    return (int)rc;
}

void QCARomePatchRAM::publishResult(IOUSBHostDevice *dev, const qca_result &r)
{
    // Diagnostic properties on the provider for `ioreg -l`.
    dev->setProperty("QCARomePatchRAM-Result", qca_strerror(r.err));
    dev->setProperty("QCARomePatchRAM-Stage", qca_stage_name(r.stage));
    dev->setProperty("QCARomePatchRAM-RomVersion", (unsigned long long)r.version_initial.rom_version, 32);
    dev->setProperty("QCARomePatchRAM-PatchVersionBefore", (unsigned long long)r.version_initial.patch_version, 32);
    dev->setProperty("QCARomePatchRAM-PatchVersionAfter", (unsigned long long)r.version_after.patch_version, 32);
    dev->setProperty("QCARomePatchRAM-StatusInitial", (unsigned long long)r.status_initial, 8);
    dev->setProperty("QCARomePatchRAM-StatusFinal", (unsigned long long)r.status_final, 8);
    dev->setProperty("QCARomePatchRAM-RampatchDownloaded", r.rampatch_downloaded != 0);
    dev->setProperty("QCARomePatchRAM-NVMDownloaded", r.nvm_downloaded != 0);
    if (r.err != QCA_OK)
        dev->setProperty("QCARomePatchRAM-TransportReturn", (unsigned long long)(uint32_t)r.transport_rc, 32);
}

bool QCARomePatchRAM::start(IOService *provider)
{
    IOUSBHostDevice *dev = OSDynamicCast(IOUSBHostDevice, provider);
    if (!dev || !super::start(provider))
        return false;

    fDevice = dev;
    fDevice->retain();
    dev->setProperty(kQCAAttemptedKey, true);

    uint64_t t0 = 0, t1 = 0, ns = 0;
    clock_get_uptime(&t0);

    uint8_t configValue = 0;
    bool weConfigured = false;
    bool opened = openDeviceAndInterface(dev, &configValue, &weConfigured);
    if (opened) {
        qca_transport t;
        t.ctx = this;
        t.control_in = trCtrlIn;
        t.control_out = trCtrlOut;
        t.bulk_out = trBulkOut;
        t.sleep_ms = trSleep;
        t.log = trLog;

        const qca_firmware rampatch = {qca_rampatch_usb_00000302, qca_rampatch_usb_00000302_len};
        const qca_firmware nvm = {qca_nvm_usb_00000302, qca_nvm_usb_00000302_len};
        qca_options opt;
        opt.probe_only = bootFlag("-qcarpprobe") ? 1 : 0;

        qca_result r;
        qca_err e = qca_rome_setup(&t, &rampatch, &nvm, &opt, &r);
        if (e == QCA_OK) {
            LOG("%s: rom 0x%08x patch 0x%x->0x%x status 0x%02x->0x%02x (rampatch=%d nvm=%d, %u bulk packets)",
                opt.probe_only ? "probe-only done" : "firmware ready",
                r.version_initial.rom_version, r.version_initial.patch_version,
                r.version_after.patch_version, r.status_initial, r.status_final,
                r.rampatch_downloaded, r.nvm_downloaded, r.bulk_packets);
        } else {
            LOG("FAILED at %s: %s (transport 0x%08x, offset %u, expected %u, actual %u)",
                qca_stage_name(r.stage), qca_strerror(e), (uint32_t)r.transport_rc,
                r.fail_offset, r.expected_len, r.actual_len);
        }
        publishResult(dev, r);
    }

    closeAll(dev);

    // Let the Apple Bluetooth stack (or BlueToolFixup-enabled path) match.
    if (fInterface) {
        fInterface->registerService(kIOServiceAsynchronous);
        OSSafeReleaseNULL(fInterface);
    }
    if (weConfigured)
        LOG("configured device with bConfigurationValue %u", configValue);
    dev->registerService(kIOServiceAsynchronous);

    clock_get_uptime(&t1);
    absolutetime_to_nanoseconds(t1 - t0, &ns);
    LOG("done in %llu ms; releasing device", ns / 1000000ULL);

    OSSafeReleaseNULL(fDevice);
    super::stop(provider);
    // Returning false detaches this object; we hold no open session.
    return false;
}

void QCARomePatchRAM::stop(IOService *provider)
{
    closeAll(fDevice);
    OSSafeReleaseNULL(fInterface);
    OSSafeReleaseNULL(fDevice);
    super::stop(provider);
}
