/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * QCA Rome 3.2 USB firmware download state machine.
 * See qca_rome.h for provenance (Linux btusb_main.c QCA USB path).
 *
 * Freestanding: no libc calls. Safe to compile with -mkernel.
 */
#include "qca_rome.h"

static const qca_device_info k_devices[] = {
    /* rom_version, rampatch hdr, nvm hdr, rampatch version offset */
    { QCA_ROME32_ROM_VERSION, QCA_ROME32_RAMPATCH_HDR, QCA_ROME32_NVM_HDR,
      QCA_ROME32_VER_OFFSET }, /* Rome 3.2 */
};

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

const qca_device_info *qca_lookup_device(uint32_t rom_version)
{
    size_t i;
    for (i = 0; i < sizeof(k_devices) / sizeof(k_devices[0]); i++) {
        if (k_devices[i].rom_version == rom_version)
            return &k_devices[i];
    }
    return NULL;
}

void qca_decode_version(const uint8_t wire[QCA_VERSION_WIRE_SIZE],
                        qca_version *out)
{
    out->rom_version   = rd_le32(wire + 0);
    out->patch_version = rd_le32(wire + 4);
    out->ram_version   = rd_le32(wire + 8);
    out->chip_id       = wire[12];
    out->platform_id   = wire[13];
    out->flag          = rd_le16(wire + 14);
    out->reserved[0]   = wire[16];
    out->reserved[1]   = wire[17];
    out->reserved[2]   = wire[18];
    out->reserved[3]   = wire[19];
}

int qca_decode_rampatch_version(const qca_firmware *fw,
                                const qca_device_info *info,
                                qca_rampatch_version *out)
{
    const uint8_t *p;
    if (!fw || !fw->data || !info || !out)
        return -1;
    if (fw->size < (size_t)info->ver_offset + QCA_RAMPATCH_VER_SIZE)
        return -1;
    p = fw->data + info->ver_offset;
    out->rom_version_high = rd_le16(p + 0);
    out->rom_version_low  = rd_le16(p + 2);
    out->patch_version    = rd_le16(p + 4);
    return 0;
}

static void zero_bytes(void *dst, size_t n)
{
    volatile uint8_t *d = (volatile uint8_t *)dst;
    while (n--)
        *d++ = 0;
}

static void qlog(const qca_transport *t, const char *msg, uint32_t a,
                 uint32_t b, uint32_t c)
{
    if (t->log)
        t->log(t->ctx, msg, a, b, c);
}

static qca_err fail(qca_result *r, qca_stage stage, qca_err err)
{
    r->stage = stage;
    r->err = err;
    return err;
}

static qca_err get_version(const qca_transport *t, qca_result *r,
                           qca_stage stage, qca_version *out)
{
    uint8_t wire[QCA_VERSION_WIRE_SIZE];
    uint32_t actual = 0;
    int rc;

    zero_bytes(wire, sizeof(wire));
    rc = t->control_in(t->ctx, QCA_REQ_GET_TARGET_VERSION, wire,
                       (uint16_t)sizeof(wire), &actual, QCA_CTRL_TIMEOUT_MS);
    if (rc != 0) {
        r->transport_rc = rc;
        return fail(r, stage, QCA_ERR_GET_VERSION_IO);
    }
    if (actual != sizeof(wire)) {
        r->expected_len = (uint32_t)sizeof(wire);
        r->actual_len = actual;
        return fail(r, stage, QCA_ERR_GET_VERSION_SHORT);
    }
    qca_decode_version(wire, out);
    return QCA_OK;
}

static qca_err check_status(const qca_transport *t, qca_result *r,
                            qca_stage stage, uint8_t *out)
{
    uint8_t status = 0;
    uint32_t actual = 0;
    int rc;

    rc = t->control_in(t->ctx, QCA_REQ_CHECK_STATUS, &status, 1, &actual,
                       QCA_CTRL_TIMEOUT_MS);
    if (rc != 0) {
        r->transport_rc = rc;
        return fail(r, stage, QCA_ERR_CHECK_STATUS_IO);
    }
    if (actual != 1) {
        r->expected_len = 1;
        r->actual_len = actual;
        return fail(r, stage, QCA_ERR_CHECK_STATUS_SHORT);
    }
    *out = status;
    return QCA_OK;
}

/* btusb_setup_qca_download_fw(): header over EP0, 20 ms, body over bulk EP2
 * in <= 4096-byte chunks. Every transfer must be complete. */
static qca_err download_fw(const qca_transport *t, qca_result *r,
                           qca_stage stage, const qca_firmware *fw,
                           size_t hdr_size)
{
    size_t sent, count;
    uint32_t actual = 0;
    int rc;

    rc = t->control_out(t->ctx, QCA_REQ_DFU_DOWNLOAD, fw->data,
                        (uint16_t)hdr_size, &actual, QCA_CTRL_TIMEOUT_MS);
    if (rc != 0) {
        r->transport_rc = rc;
        r->fail_offset = 0;
        return fail(r, stage, QCA_ERR_HEADER_IO);
    }
    if (actual != hdr_size) {
        r->expected_len = (uint32_t)hdr_size;
        r->actual_len = actual;
        r->fail_offset = 0;
        return fail(r, stage, QCA_ERR_HEADER_SHORT);
    }

    sent = hdr_size;
    count = fw->size - hdr_size;

    /* EP2 needs time to switch from ACL function to DFU function. */
    t->sleep_ms(t->ctx, QCA_DFU_SWITCH_DELAY_MS);

    while (count) {
        uint32_t size = (uint32_t)(count < QCA_DFU_PACKET_LEN
                                       ? count : QCA_DFU_PACKET_LEN);
        actual = 0;
        rc = t->bulk_out(t->ctx, QCA_DFU_BULK_EP, fw->data + sent, size,
                         &actual, QCA_DFU_BULK_TIMEOUT_MS);
        r->bulk_packets++;
        if (rc != 0) {
            r->transport_rc = rc;
            r->fail_offset = (uint32_t)sent;
            return fail(r, stage, QCA_ERR_BODY_IO);
        }
        if (actual != size) {
            r->expected_len = size;
            r->actual_len = actual;
            r->fail_offset = (uint32_t)sent;
            return fail(r, stage, QCA_ERR_BODY_SHORT);
        }
        sent += size;
        count -= size;
    }
    return QCA_OK;
}

static qca_err load_rampatch(const qca_transport *t, qca_result *r,
                             const qca_version *ver,
                             const qca_device_info *info,
                             const qca_firmware *fw)
{
    qca_rampatch_version rver;
    uint32_t rver_rom;

    if (!fw || !fw->data || fw->size == 0)
        return fail(r, QCA_STAGE_RAMPATCH, QCA_ERR_RAMPATCH_MISSING);
    /* Need the version struct and at least one body byte past the header. */
    if (fw->size <= info->rampatch_hdr ||
        qca_decode_rampatch_version(fw, info, &rver) != 0) {
        r->expected_len = (uint32_t)info->rampatch_hdr + 1u;
        r->actual_len = (uint32_t)fw->size;
        return fail(r, QCA_STAGE_RAMPATCH, QCA_ERR_RAMPATCH_TOO_SHORT);
    }

    if (ver->rom_version & ~0xffffu)
        rver_rom = ((uint32_t)rver.rom_version_high << 16) |
                   rver.rom_version_low;
    else
        rver_rom = rver.rom_version_low;

    qlog(t, "rampatch rome 0x%x build 0x%x, controller patch 0x%x", rver_rom,
         rver.patch_version, ver->patch_version);

    if (rver_rom != ver->rom_version) {
        r->expected_len = ver->rom_version;
        r->actual_len = rver_rom;
        return fail(r, QCA_STAGE_RAMPATCH, QCA_ERR_RAMPATCH_ROM_MISMATCH);
    }
    /* Linux allows equal versions (recovery after an aborted download). */
    if ((uint32_t)rver.patch_version < ver->patch_version) {
        r->expected_len = ver->patch_version;
        r->actual_len = rver.patch_version;
        return fail(r, QCA_STAGE_RAMPATCH, QCA_ERR_RAMPATCH_PATCH_OLDER);
    }

    return download_fw(t, r, QCA_STAGE_RAMPATCH, fw, info->rampatch_hdr);
}

static qca_err load_nvm(const qca_transport *t, qca_result *r,
                        const qca_device_info *info, const qca_firmware *fw)
{
    if (!fw || !fw->data || fw->size == 0)
        return fail(r, QCA_STAGE_NVM, QCA_ERR_NVM_MISSING);
    if (fw->size <= info->nvm_hdr) {
        r->expected_len = (uint32_t)info->nvm_hdr + 1u;
        r->actual_len = (uint32_t)fw->size;
        return fail(r, QCA_STAGE_NVM, QCA_ERR_NVM_TOO_SHORT);
    }
    return download_fw(t, r, QCA_STAGE_NVM, fw, info->nvm_hdr);
}

qca_err qca_rome_setup(const qca_transport *t, const qca_firmware *rampatch,
                       const qca_firmware *nvm, const qca_options *opt,
                       qca_result *r)
{
    const qca_device_info *info;
    qca_version ver;
    uint8_t status = 0;
    qca_err err;

    if (!r)
        return QCA_ERR_BAD_ARGUMENT;
    zero_bytes(r, sizeof(*r));
    if (!t || !t->control_in || !t->control_out || !t->bulk_out ||
        !t->sleep_ms)
        return fail(r, QCA_STAGE_NONE, QCA_ERR_BAD_ARGUMENT);

    /* 1. GET_TARGET_VERSION */
    err = get_version(t, r, QCA_STAGE_GET_VERSION, &ver);
    if (err)
        return err;
    r->version_initial = ver;
    qlog(t, "rom 0x%08x patch 0x%08x ram 0x%08x", ver.rom_version,
         ver.patch_version, ver.ram_version);

    info = qca_lookup_device(ver.rom_version);
    if (!info) {
        /* Deliberately stricter than Linux: we never "assume no patch
         * needed" for unknown high ROM versions; we only drive Rome 3.2. */
        r->actual_len = ver.rom_version;
        return fail(r, QCA_STAGE_GET_VERSION, QCA_ERR_UNSUPPORTED_ROM);
    }

    /* 2. CHECK_STATUS */
    err = check_status(t, r, QCA_STAGE_CHECK_STATUS, &status);
    if (err)
        return err;
    r->status_initial = status;
    qlog(t, "initial status 0x%02x", status, 0, 0);

    if (opt && opt->probe_only) {
        r->version_after = ver;
        r->status_final = status;
        r->stage = QCA_STAGE_DONE;
        r->err = QCA_OK;
        return QCA_OK;
    }

    /* 3. rampatch if PATCH_UPDATED is not set */
    if (!(status & QCA_STATUS_PATCH_UPDATED)) {
        err = load_rampatch(t, r, &ver, info, rampatch);
        if (err)
            return err;
        r->rampatch_downloaded = 1;
        qlog(t, "rampatch sent (%u bytes)", (uint32_t)rampatch->size, 0, 0);
    }

    /* 4. Re-read version */
    err = get_version(t, r, QCA_STAGE_REREAD_VERSION, &ver);
    if (err)
        return err;
    r->version_after = ver;
    if (ver.rom_version != info->rom_version) {
        r->expected_len = info->rom_version;
        r->actual_len = ver.rom_version;
        return fail(r, QCA_STAGE_REREAD_VERSION, QCA_ERR_ROM_CHANGED);
    }

    /* 5. NVM if SYSCFG_UPDATED was not set in the original status */
    if (!(status & QCA_STATUS_SYSCFG_UPDATED)) {
        err = load_nvm(t, r, info, nvm);
        if (err)
            return err;
        r->nvm_downloaded = 1;
        qlog(t, "NVM sent (%u bytes)", (uint32_t)nvm->size, 0, 0);
        /* Rome 3.2 (< 0x00130201) has no post-NVM reset wait in Linux. */
    }

    /* 6. Final status must show both rampatch and NVM applied. */
    err = check_status(t, r, QCA_STAGE_FINAL_STATUS, &status);
    if (err)
        return err;
    r->status_final = status;
    if ((status & QCA_STATUS_DONE_MASK) != QCA_STATUS_DONE_MASK) {
        r->expected_len = QCA_STATUS_DONE_MASK;
        r->actual_len = status;
        return fail(r, QCA_STAGE_FINAL_STATUS, QCA_ERR_FINAL_STATUS);
    }

    r->stage = QCA_STAGE_DONE;
    r->err = QCA_OK;
    return QCA_OK;
}

const char *qca_strerror(qca_err err)
{
    switch (err) {
    case QCA_OK:                       return "ok";
    case QCA_ERR_BAD_ARGUMENT:         return "bad argument";
    case QCA_ERR_GET_VERSION_IO:       return "GET_TARGET_VERSION transfer failed";
    case QCA_ERR_GET_VERSION_SHORT:    return "GET_TARGET_VERSION short read";
    case QCA_ERR_UNSUPPORTED_ROM:      return "unsupported rom_version (only 0x00000302)";
    case QCA_ERR_CHECK_STATUS_IO:      return "CHECK_STATUS transfer failed";
    case QCA_ERR_CHECK_STATUS_SHORT:   return "CHECK_STATUS short read";
    case QCA_ERR_RAMPATCH_MISSING:     return "rampatch needed but not provided";
    case QCA_ERR_RAMPATCH_TOO_SHORT:   return "rampatch file too short";
    case QCA_ERR_RAMPATCH_ROM_MISMATCH: return "rampatch rom version does not match controller";
    case QCA_ERR_RAMPATCH_PATCH_OLDER: return "rampatch patch version older than controller";
    case QCA_ERR_NVM_MISSING:          return "NVM needed but not provided";
    case QCA_ERR_NVM_TOO_SHORT:        return "NVM file too short";
    case QCA_ERR_HEADER_IO:            return "DFU header control transfer failed";
    case QCA_ERR_HEADER_SHORT:         return "DFU header short write";
    case QCA_ERR_BODY_IO:              return "DFU body bulk transfer failed";
    case QCA_ERR_BODY_SHORT:           return "DFU body short write";
    case QCA_ERR_ROM_CHANGED:          return "rom_version changed after rampatch";
    case QCA_ERR_FINAL_STATUS:         return "final status lacks 0xc0";
    case QCA_ERR__COUNT:               break;
    }
    return "unknown error";
}

const char *qca_stage_name(qca_stage stage)
{
    switch (stage) {
    case QCA_STAGE_NONE:           return "none";
    case QCA_STAGE_GET_VERSION:    return "get-version";
    case QCA_STAGE_CHECK_STATUS:   return "check-status";
    case QCA_STAGE_RAMPATCH:       return "rampatch";
    case QCA_STAGE_REREAD_VERSION: return "reread-version";
    case QCA_STAGE_NVM:            return "nvm";
    case QCA_STAGE_FINAL_STATUS:   return "final-status";
    case QCA_STAGE_DONE:           return "done";
    }
    return "unknown";
}
