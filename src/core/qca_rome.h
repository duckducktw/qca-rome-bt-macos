/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * QCA Rome (QCA6174A / Rome 3.2) USB Bluetooth firmware download state
 * machine. Pure C, no IOKit / libc dependencies beyond <stdint.h>/<stddef.h>,
 * so it can be unit-tested on a Linux host and linked into the macOS kext.
 *
 * Protocol is a line-by-line port of Linux drivers/bluetooth/btusb_main.c:
 *   btusb_setup_qca(), btusb_setup_qca_load_rampatch(),
 *   btusb_setup_qca_load_nvm(), btusb_setup_qca_download_fw().
 * Restricted on purpose to rom_version 0x00000302 (Rome 3.2).
 */
#ifndef QCA_ROME_H
#define QCA_ROME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* USB vendor requests (EP0, bmRequestType = vendor | device). */
#define QCA_REQ_DFU_DOWNLOAD        0x01u /* OUT: firmware header        */
#define QCA_REQ_CHECK_STATUS        0x05u /* IN : 1 byte status          */
#define QCA_REQ_GET_TARGET_VERSION  0x09u /* IN : 20 byte qca_version     */

#define QCA_STATUS_SYSCFG_UPDATED   0x40u /* NVM applied                 */
#define QCA_STATUS_PATCH_UPDATED    0x80u /* rampatch applied            */
#define QCA_STATUS_DONE_MASK        0xc0u

#define QCA_DFU_PACKET_LEN          4096u
#define QCA_DFU_BULK_EP             0x02u
#define QCA_DFU_BULK_TIMEOUT_MS     3000u
#define QCA_CTRL_TIMEOUT_MS         5000u /* USB_CTRL_{GET,SET}_TIMEOUT  */
#define QCA_DFU_SWITCH_DELAY_MS     20u   /* EP2 ACL -> DFU switch delay */

#define QCA_VERSION_WIRE_SIZE       20u

#define QCA_ROME32_ROM_VERSION      0x00000302u
#define QCA_ROME32_RAMPATCH_HDR     28u
#define QCA_ROME32_NVM_HDR          4u
#define QCA_ROME32_VER_OFFSET       16u
#define QCA_RAMPATCH_VER_SIZE       6u

/* Decoded struct qca_version (wire format is packed little-endian). */
typedef struct qca_version {
    uint32_t rom_version;
    uint32_t patch_version;
    uint32_t ram_version;
    uint8_t  chip_id;
    uint8_t  platform_id;
    uint16_t flag;
    uint8_t  reserved[4];
} qca_version;

/* Decoded struct qca_rampatch_version at rampatch[ver_offset]. */
typedef struct qca_rampatch_version {
    uint16_t rom_version_high;
    uint16_t rom_version_low;
    uint16_t patch_version;
} qca_rampatch_version;

typedef struct qca_device_info {
    uint32_t rom_version;
    uint8_t  rampatch_hdr;
    uint8_t  nvm_hdr;
    uint8_t  ver_offset;
} qca_device_info;

typedef enum qca_err {
    QCA_OK = 0,
    QCA_ERR_BAD_ARGUMENT,
    QCA_ERR_GET_VERSION_IO,
    QCA_ERR_GET_VERSION_SHORT,
    QCA_ERR_UNSUPPORTED_ROM,
    QCA_ERR_CHECK_STATUS_IO,
    QCA_ERR_CHECK_STATUS_SHORT,
    QCA_ERR_RAMPATCH_MISSING,
    QCA_ERR_RAMPATCH_TOO_SHORT,
    QCA_ERR_RAMPATCH_ROM_MISMATCH,
    QCA_ERR_RAMPATCH_PATCH_OLDER,
    QCA_ERR_NVM_MISSING,
    QCA_ERR_NVM_TOO_SHORT,
    QCA_ERR_HEADER_IO,
    QCA_ERR_HEADER_SHORT,
    QCA_ERR_BODY_IO,
    QCA_ERR_BODY_SHORT,
    QCA_ERR_ROM_CHANGED,
    QCA_ERR_FINAL_STATUS,
    QCA_ERR__COUNT
} qca_err;

/* Abstract USB transport. All callbacks return 0 on success and a
 * transport-specific nonzero code (e.g. IOReturn) on failure; the code is
 * preserved verbatim in qca_result.transport_rc. `actual` must be set to the
 * number of bytes really transferred. Control requests are always
 * vendor-type, device-recipient, wValue = wIndex = 0. */
typedef struct qca_transport {
    void *ctx;
    int (*control_in)(void *ctx, uint8_t bRequest, void *buf, uint16_t len,
                      uint32_t *actual, uint32_t timeout_ms);
    int (*control_out)(void *ctx, uint8_t bRequest, const void *buf,
                       uint16_t len, uint32_t *actual, uint32_t timeout_ms);
    int (*bulk_out)(void *ctx, uint8_t endpoint, const void *buf, uint32_t len,
                    uint32_t *actual, uint32_t timeout_ms);
    void (*sleep_ms)(void *ctx, uint32_t ms);
    /* Optional, may be NULL. Receives a fully formatted-free message plus
     * up to three integer arguments to keep the core printf-free. */
    void (*log)(void *ctx, const char *msg, uint32_t a, uint32_t b, uint32_t c);
} qca_transport;

typedef struct qca_firmware {
    const uint8_t *data;
    size_t size;
} qca_firmware;

typedef struct qca_options {
    /* Read-only probe: GET_VERSION + CHECK_STATUS only, never downloads. */
    int probe_only;
} qca_options;

typedef enum qca_stage {
    QCA_STAGE_NONE = 0,
    QCA_STAGE_GET_VERSION,
    QCA_STAGE_CHECK_STATUS,
    QCA_STAGE_RAMPATCH,
    QCA_STAGE_REREAD_VERSION,
    QCA_STAGE_NVM,
    QCA_STAGE_FINAL_STATUS,
    QCA_STAGE_DONE
} qca_stage;

typedef struct qca_result {
    qca_err     err;
    qca_stage   stage;           /* stage at which err occurred / DONE   */
    int         transport_rc;    /* raw transport code for *_IO errors   */
    uint32_t    fail_offset;     /* byte offset in file for body errors  */
    uint32_t    expected_len;    /* for *_SHORT errors                   */
    uint32_t    actual_len;
    qca_version version_initial;
    qca_version version_after;   /* after rampatch stage                 */
    uint8_t     status_initial;
    uint8_t     status_final;
    int         rampatch_downloaded;
    int         nvm_downloaded;
    uint32_t    bulk_packets;    /* total bulk transfers issued          */
} qca_result;

const qca_device_info *qca_lookup_device(uint32_t rom_version);
void qca_decode_version(const uint8_t wire[QCA_VERSION_WIRE_SIZE],
                        qca_version *out);
int qca_decode_rampatch_version(const qca_firmware *fw,
                                const qca_device_info *info,
                                qca_rampatch_version *out);

/* Runs the full setup. Returns result->err. */
qca_err qca_rome_setup(const qca_transport *t, const qca_firmware *rampatch,
                       const qca_firmware *nvm, const qca_options *opt,
                       qca_result *result);

const char *qca_strerror(qca_err err);
const char *qca_stage_name(qca_stage stage);

#ifdef __cplusplus
}
#endif

#endif /* QCA_ROME_H */
