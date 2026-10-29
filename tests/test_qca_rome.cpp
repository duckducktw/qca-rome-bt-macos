// SPDX-License-Identifier: GPL-2.0-or-later
//
// Host unit tests for the QCA Rome 3.2 download state machine.
// Build: make test   (clang++/g++ with -fsanitize=address,undefined)
#include "qca_rome.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(QCA_HAVE_FIRMWARE)
#include "qca_rome_firmware.h"
#endif

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__,   \
                         #cond);                                             \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        ++g_checks;                                                          \
        auto va_ = (a);                                                      \
        auto vb_ = (b);                                                      \
        if (!(va_ == vb_)) {                                                 \
            ++g_failures;                                                    \
            std::fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n",  \
                         __FILE__, __LINE__, #a, #b, (long long)va_,         \
                         (long long)vb_);                                    \
        }                                                                    \
    } while (0)

enum class Op { CtrlIn, CtrlOut, Bulk, Sleep };

struct Call {
    Op op;
    uint8_t req_or_ep;
    uint32_t len;
    uint32_t timeout;
    std::vector<uint8_t> data; // OUT payload
};

// Scripted fake device modelling the Rome 3.2 DFU behaviour.
struct FakeDevice {
    uint32_t rom = QCA_ROME32_ROM_VERSION;
    uint32_t patch = 0x0000;
    uint32_t patch_after_rampatch = 0x03e8;
    uint8_t status = 0x00;
    bool apply_on_complete = true; // set status bits when body fully sent

    // Fault injection.
    int fail_ctrl_in_req = -1;     // bRequest that returns an error
    int fail_ctrl_in_nth = 1;      // ...on the nth occurrence
    uint32_t ctrl_in_short = 0;    // if nonzero, return this many bytes for GET_VERSION
    bool header_short = false;
    int header_rc = 0;
    int bulk_fail_at = -1;         // packet index (0-based) to fail
    int bulk_fail_rc = 0;          // rc for failure, 0 => short write
    int rom_after_rampatch = -1;   // change rom_version after rampatch
    int override_final_status = -1;

    // DFU tracking.
    enum { Idle, Rampatch, Nvm } dfu = Idle;
    uint32_t dfu_expected = 0, dfu_received = 0;
    std::vector<uint8_t> rampatch_blob, nvm_blob;
    int ctrl_in_seen[256] = {};

    std::vector<Call> calls;
    uint32_t slept_ms = 0;

    void fill_version(uint8_t *b) const {
        std::memset(b, 0, QCA_VERSION_WIRE_SIZE);
        auto w32 = [&](int off, uint32_t v) {
            b[off] = uint8_t(v); b[off + 1] = uint8_t(v >> 8);
            b[off + 2] = uint8_t(v >> 16); b[off + 3] = uint8_t(v >> 24);
        };
        w32(0, rom);
        w32(4, patch);
        w32(8, 0x00000302);
        b[12] = 0x44; b[13] = 0x01; b[14] = 0x00; b[15] = 0x00;
    }

    static int ctrl_in(void *ctx, uint8_t req, void *buf, uint16_t len,
                       uint32_t *actual, uint32_t timeout) {
        auto *d = static_cast<FakeDevice *>(ctx);
        d->calls.push_back({Op::CtrlIn, req, len, timeout, {}});
        int n = ++d->ctrl_in_seen[req];
        *actual = 0;
        if (req == d->fail_ctrl_in_req && n == d->fail_ctrl_in_nth)
            return 0xe00002ed; // kIOReturnNotResponding-ish
        if (req == QCA_REQ_GET_TARGET_VERSION) {
            if (len != QCA_VERSION_WIRE_SIZE) return -1;
            d->fill_version(static_cast<uint8_t *>(buf));
            *actual = d->ctrl_in_short ? d->ctrl_in_short : len;
            return 0;
        }
        if (req == QCA_REQ_CHECK_STATUS) {
            if (len != 1) return -1;
            uint8_t s = d->status;
            if (d->override_final_status >= 0 && n >= 2)
                s = uint8_t(d->override_final_status);
            *static_cast<uint8_t *>(buf) = s;
            *actual = 1;
            return 0;
        }
        return -2;
    }

    static int ctrl_out(void *ctx, uint8_t req, const void *buf, uint16_t len,
                        uint32_t *actual, uint32_t timeout) {
        auto *d = static_cast<FakeDevice *>(ctx);
        const auto *p = static_cast<const uint8_t *>(buf);
        d->calls.push_back({Op::CtrlOut, req, len, timeout,
                            std::vector<uint8_t>(p, p + len)});
        *actual = 0;
        if (req != QCA_REQ_DFU_DOWNLOAD) return -3;
        if (d->header_rc) return d->header_rc;
        *actual = d->header_short ? uint32_t(len - 1) : len;
        // Header length identifies which file is being downloaded.
        if (len == QCA_ROME32_RAMPATCH_HDR) {
            d->dfu = Rampatch;
            d->rampatch_blob.assign(p, p + len);
        } else if (len == QCA_ROME32_NVM_HDR) {
            d->dfu = Nvm;
            d->nvm_blob.assign(p, p + len);
        }
        d->dfu_received = len;
        return 0;
    }

    static int bulk(void *ctx, uint8_t ep, const void *buf, uint32_t len,
                    uint32_t *actual, uint32_t timeout) {
        auto *d = static_cast<FakeDevice *>(ctx);
        const auto *p = static_cast<const uint8_t *>(buf);
        int idx = 0;
        for (auto &c : d->calls) if (c.op == Op::Bulk) ++idx;
        d->calls.push_back({Op::Bulk, ep, len, timeout,
                            std::vector<uint8_t>(p, p + len)});
        *actual = 0;
        if (idx == d->bulk_fail_at) {
            if (d->bulk_fail_rc) return d->bulk_fail_rc;
            *actual = len / 2;
            return 0;
        }
        *actual = len;
        auto &blob = d->dfu == Rampatch ? d->rampatch_blob : d->nvm_blob;
        blob.insert(blob.end(), p, p + len);
        d->dfu_received += len;
        if (d->apply_on_complete && d->dfu_received == d->dfu_expected) {
            if (d->dfu == Rampatch) {
                d->status |= QCA_STATUS_PATCH_UPDATED;
                d->patch = d->patch_after_rampatch;
                if (d->rom_after_rampatch >= 0) d->rom = uint32_t(d->rom_after_rampatch);
            } else if (d->dfu == Nvm) {
                d->status |= QCA_STATUS_SYSCFG_UPDATED;
            }
        }
        return 0;
    }

    static void sleep(void *ctx, uint32_t ms) {
        auto *d = static_cast<FakeDevice *>(ctx);
        d->calls.push_back({Op::Sleep, 0, ms, 0, {}});
        d->slept_ms += ms;
    }

    qca_transport transport() {
        qca_transport t{};
        t.ctx = this;
        t.control_in = ctrl_in;
        t.control_out = ctrl_out;
        t.bulk_out = bulk;
        t.sleep_ms = sleep;
        t.log = nullptr;
        return t;
    }

    size_t count(Op op) const {
        size_t n = 0;
        for (auto &c : calls) if (c.op == op) ++n;
        return n;
    }
};

// Synthetic rampatch: header 28 bytes, version struct at 16.
std::vector<uint8_t> make_rampatch(size_t size, uint16_t rom_hi, uint16_t rom_lo,
                                   uint16_t patch) {
    std::vector<uint8_t> v(size);
    for (size_t i = 0; i < size; ++i) v[i] = uint8_t(i * 7 + 3);
    v[16] = uint8_t(rom_hi); v[17] = uint8_t(rom_hi >> 8);
    v[18] = uint8_t(rom_lo); v[19] = uint8_t(rom_lo >> 8);
    v[20] = uint8_t(patch); v[21] = uint8_t(patch >> 8);
    return v;
}

std::vector<uint8_t> make_nvm(size_t size) {
    std::vector<uint8_t> v(size);
    for (size_t i = 0; i < size; ++i) v[i] = uint8_t(0xa5 ^ i);
    return v;
}

struct Fixture {
    std::vector<uint8_t> rp = make_rampatch(68644, 0x0008, 0x0302, 0x03e8);
    std::vector<uint8_t> nvm = make_nvm(1998);
    qca_firmware rpf{rp.data(), rp.size()};
    qca_firmware nvf{nvm.data(), nvm.size()};
};

// Wraps ctrl_out to set the expected DFU total from the file sizes, so the
// fake only flips status bits when the complete file was received.
struct SizedDevice : FakeDevice {
    size_t rp_size = 0, nvm_size = 0;
    static int ctrl_out2(void *ctx, uint8_t req, const void *buf, uint16_t len,
                         uint32_t *actual, uint32_t timeout) {
        auto *d = static_cast<SizedDevice *>(ctx);
        int rc = FakeDevice::ctrl_out(ctx, req, buf, len, actual, timeout);
        d->dfu_expected = uint32_t(d->dfu == Rampatch ? d->rp_size : d->nvm_size);
        return rc;
    }
    qca_transport transport() {
        qca_transport t = FakeDevice::transport();
        t.control_out = ctrl_out2;
        return t;
    }
};

void test_normal_flow() {
    std::puts("[normal cold-boot flow: status 0x00 -> rampatch + NVM]");
    Fixture f;
    SizedDevice d; d.rp_size = f.rp.size(); d.nvm_size = f.nvm.size();
    qca_transport t = d.transport();
    qca_result r;
    qca_err e = qca_rome_setup(&t, &f.rpf, &f.nvf, nullptr, &r);
    CHECK_EQ(e, QCA_OK);
    CHECK_EQ(r.stage, QCA_STAGE_DONE);
    CHECK_EQ(r.rampatch_downloaded, 1);
    CHECK_EQ(r.nvm_downloaded, 1);
    CHECK_EQ(r.status_initial, 0x00);
    CHECK_EQ(r.status_final, 0xc0);
    CHECK_EQ(r.version_initial.rom_version, 0x302u);
    CHECK_EQ(r.version_initial.chip_id, 0x44);
    CHECK_EQ(r.version_after.patch_version, 0x3e8u);
    // Byte-exact reconstruction of both files on the device side.
    CHECK(d.rampatch_blob == f.rp);
    CHECK(d.nvm_blob == f.nvm);
    // Packetisation: rampatch body 68616 = 16*4096 + 3080; NVM body 1994.
    CHECK_EQ(d.count(Op::Bulk), 18u);
    CHECK_EQ(r.bulk_packets, 18u);
    CHECK_EQ(d.count(Op::CtrlOut), 2u);
    CHECK_EQ(d.slept_ms, 40u);
    // Exact call order.
    std::vector<std::pair<Op, uint8_t>> want = {
        {Op::CtrlIn, 0x09}, {Op::CtrlIn, 0x05}, {Op::CtrlOut, 0x01},
        {Op::Sleep, 0}};
    for (int i = 0; i < 17; ++i) want.push_back({Op::Bulk, 0x02});
    want.push_back({Op::CtrlIn, 0x09});
    want.push_back({Op::CtrlOut, 0x01});
    want.push_back({Op::Sleep, 0});
    want.push_back({Op::Bulk, 0x02});
    want.push_back({Op::CtrlIn, 0x05});
    CHECK_EQ(d.calls.size(), want.size());
    for (size_t i = 0; i < want.size() && i < d.calls.size(); ++i) {
        CHECK(d.calls[i].op == want[i].first);
        CHECK_EQ(d.calls[i].req_or_ep, want[i].second);
    }
}

void test_header_and_packet_sizes() {
    std::puts("[header lengths, packet sizes, timeouts]");
    Fixture f;
    SizedDevice d; d.rp_size = f.rp.size(); d.nvm_size = f.nvm.size();
    qca_transport t = d.transport();
    qca_result r;
    CHECK_EQ(qca_rome_setup(&t, &f.rpf, &f.nvf, nullptr, &r), QCA_OK);
    std::vector<uint32_t> outs, bulks;
    for (auto &c : d.calls) {
        if (c.op == Op::CtrlOut) {
            outs.push_back(c.len);
            CHECK_EQ(c.timeout, QCA_CTRL_TIMEOUT_MS);
        }
        if (c.op == Op::Bulk) {
            bulks.push_back(c.len);
            CHECK(c.len <= QCA_DFU_PACKET_LEN);
            CHECK(c.len > 0);
            CHECK_EQ(c.timeout, QCA_DFU_BULK_TIMEOUT_MS);
            CHECK_EQ(c.req_or_ep, QCA_DFU_BULK_EP);
        }
        if (c.op == Op::CtrlIn) {
            CHECK(c.len == 20 || c.len == 1);
        }
        if (c.op == Op::Sleep) CHECK_EQ(c.len, QCA_DFU_SWITCH_DELAY_MS);
    }
    CHECK_EQ(outs.size(), 2u);
    if (outs.size() == 2) {
        CHECK_EQ(outs[0], 28u);
        CHECK_EQ(outs[1], 4u);
    }
    CHECK_EQ(bulks.size(), 18u);
    if (bulks.size() == 18) {
        for (int i = 0; i < 16; ++i) CHECK_EQ(bulks[size_t(i)], 4096u);
        CHECK_EQ(bulks[16], 3080u);
        CHECK_EQ(bulks[17], 1994u);
    }
    // Header bytes are the first bytes of each file.
    CHECK(std::memcmp(d.calls[2].data.data(), f.rp.data(), 28) == 0);

    // Body exactly a multiple of 4096 => no zero-length trailing packet.
    std::vector<uint8_t> rp2 = make_rampatch(28 + 2 * 4096, 0x0008, 0x0302, 1);
    qca_firmware rpf2{rp2.data(), rp2.size()};
    SizedDevice d2; d2.rp_size = rp2.size(); d2.nvm_size = f.nvm.size();
    d2.status = QCA_STATUS_SYSCFG_UPDATED;
    qca_transport t2 = d2.transport();
    CHECK_EQ(qca_rome_setup(&t2, &rpf2, &f.nvf, nullptr, &r), QCA_OK);
    CHECK_EQ(d2.count(Op::Bulk), 2u);
    CHECK_EQ(r.nvm_downloaded, 0);
}

void test_already_done() {
    std::puts("[status already 0xc0 -> zero downloads]");
    Fixture f;
    SizedDevice d; d.status = 0xc0; d.patch = 0x3e8;
    qca_transport t = d.transport();
    qca_result r;
    CHECK_EQ(qca_rome_setup(&t, &f.rpf, &f.nvf, nullptr, &r), QCA_OK);
    CHECK_EQ(r.rampatch_downloaded, 0);
    CHECK_EQ(r.nvm_downloaded, 0);
    CHECK_EQ(d.count(Op::CtrlOut), 0u);
    CHECK_EQ(d.count(Op::Bulk), 0u);
    CHECK_EQ(d.count(Op::Sleep), 0u);
    CHECK_EQ(d.count(Op::CtrlIn), 4u); // ver, status, ver, status
    // Also works with no firmware at all when nothing is needed.
    SizedDevice d2; d2.status = 0xc0;
    qca_transport t2 = d2.transport();
    CHECK_EQ(qca_rome_setup(&t2, nullptr, nullptr, nullptr, &r), QCA_OK);
}

void test_only_nvm() {
    std::puts("[status 0x80 -> only NVM]");
    Fixture f;
    SizedDevice d; d.status = 0x80; d.patch = 0x3e8;
    d.rp_size = f.rp.size(); d.nvm_size = f.nvm.size();
    qca_transport t = d.transport();
    qca_result r;
    CHECK_EQ(qca_rome_setup(&t, &f.rpf, &f.nvf, nullptr, &r), QCA_OK);
    CHECK_EQ(r.rampatch_downloaded, 0);
    CHECK_EQ(r.nvm_downloaded, 1);
    CHECK_EQ(d.count(Op::CtrlOut), 1u);
    CHECK_EQ(d.count(Op::Bulk), 1u);
    CHECK(d.nvm_blob == f.nvm);
    CHECK(d.rampatch_blob.empty());
    // Only NVM needed: missing rampatch must not matter.
    SizedDevice d2; d2.status = 0x80; d2.nvm_size = f.nvm.size();
    qca_transport t2 = d2.transport();
    CHECK_EQ(qca_rome_setup(&t2, nullptr, &f.nvf, nullptr, &r), QCA_OK);
}

void test_only_rampatch() {
    std::puts("[status 0x40 -> only rampatch]");
    Fixture f;
    SizedDevice d; d.status = 0x40;
    d.rp_size = f.rp.size(); d.nvm_size = f.nvm.size();
    qca_transport t = d.transport();
    qca_result r;
    CHECK_EQ(qca_rome_setup(&t, &f.rpf, nullptr, nullptr, &r), QCA_OK);
    CHECK_EQ(r.rampatch_downloaded, 1);
    CHECK_EQ(r.nvm_downloaded, 0);
    CHECK_EQ(d.count(Op::CtrlOut), 1u);
}

void test_unsupported_rom() {
    std::puts("[unsupported rom_version rejected before any write]");
    Fixture f;
    const uint32_t roms[] = {0x00000300, 0x00000201, 0x00130201, 0x00000000,
                             0xffffffff, 0x00010302};
    for (uint32_t rom : roms) {
        SizedDevice d; d.rom = rom;
        qca_transport t = d.transport();
        qca_result r;
        CHECK_EQ(qca_rome_setup(&t, &f.rpf, &f.nvf, nullptr, &r),
                 QCA_ERR_UNSUPPORTED_ROM);
        CHECK_EQ(r.stage, QCA_STAGE_GET_VERSION);
        CHECK_EQ(r.actual_len, rom);
        CHECK_EQ(d.count(Op::CtrlOut), 0u);
        CHECK_EQ(d.count(Op::Bulk), 0u);
        CHECK_EQ(d.calls.size(), 1u);
    }
}

void test_rampatch_version_mismatch() {
    std::puts("[rampatch header version checks]");
    Fixture f;
    // ROM in file != controller ROM.
    {
        auto rp = make_rampatch(4000, 0x0008, 0x0300, 0x03e8);
        qca_firmware fw{rp.data(), rp.size()};
        SizedDevice d;
        qca_transport t = d.transport();
        qca_result r;
        CHECK_EQ(qca_rome_setup(&t, &fw, &f.nvf, nullptr, &r),
                 QCA_ERR_RAMPATCH_ROM_MISMATCH);
        CHECK_EQ(r.stage, QCA_STAGE_RAMPATCH);
        CHECK_EQ(r.actual_len, 0x300u);
        CHECK_EQ(d.count(Op::CtrlOut), 0u);
        CHECK_EQ(d.count(Op::Bulk), 0u);
    }
    // For ROM < 0x10000 the high half is ignored (Linux behaviour).
    {
        auto rp = make_rampatch(4000, 0xbeef, 0x0302, 0x03e8);
        qca_firmware fw{rp.data(), rp.size()};
        SizedDevice d; d.rp_size = rp.size(); d.status = 0x40;
        qca_transport t = d.transport();
        qca_result r;
        CHECK_EQ(qca_rome_setup(&t, &fw, nullptr, nullptr, &r), QCA_OK);
    }
    // Patch older than the controller's current patch.
    {
        auto rp = make_rampatch(4000, 0x0008, 0x0302, 0x0100);
        qca_firmware fw{rp.data(), rp.size()};
        SizedDevice d; d.patch = 0x0101;
        qca_transport t = d.transport();
        qca_result r;
        CHECK_EQ(qca_rome_setup(&t, &fw, &f.nvf, nullptr, &r),
                 QCA_ERR_RAMPATCH_PATCH_OLDER);
        CHECK_EQ(d.count(Op::CtrlOut), 0u);
    }
    // Equal patch version is allowed (recovery after aborted download).
    {
        auto rp = make_rampatch(4000, 0x0008, 0x0302, 0x0100);
        qca_firmware fw{rp.data(), rp.size()};
        SizedDevice d; d.patch = 0x0100; d.patch_after_rampatch = 0x0100;
        d.rp_size = rp.size(); d.nvm_size = f.nvm.size();
        qca_transport t = d.transport();
        qca_result r;
        CHECK_EQ(qca_rome_setup(&t, &fw, &f.nvf, nullptr, &r), QCA_OK);
    }
    // Truncated files.
    {
        auto rp = make_rampatch(28, 0x0008, 0x0302, 0x0100);
        qca_firmware fw{rp.data(), rp.size()};
        SizedDevice d;
        qca_transport t = d.transport();
        qca_result r;
        CHECK_EQ(qca_rome_setup(&t, &fw, &f.nvf, nullptr, &r),
                 QCA_ERR_RAMPATCH_TOO_SHORT);
        uint8_t tiny[10] = {};
        qca_firmware fw2{tiny, sizeof(tiny)};
        SizedDevice d2;
        qca_transport t2 = d2.transport();
        CHECK_EQ(qca_rome_setup(&t2, &fw2, &f.nvf, nullptr, &r),
                 QCA_ERR_RAMPATCH_TOO_SHORT);
        uint8_t n4[4] = {};
        qca_firmware nf{n4, sizeof(n4)};
        SizedDevice d3; d3.status = 0x80;
        qca_transport t3 = d3.transport();
        CHECK_EQ(qca_rome_setup(&t3, nullptr, &nf, nullptr, &r),
                 QCA_ERR_NVM_TOO_SHORT);
        CHECK_EQ(d3.count(Op::CtrlOut), 0u);
    }
    // Missing files.
    {
        SizedDevice d;
        qca_transport t = d.transport();
        qca_result r;
        CHECK_EQ(qca_rome_setup(&t, nullptr, &f.nvf, nullptr, &r),
                 QCA_ERR_RAMPATCH_MISSING);
        SizedDevice d2; d2.status = 0x80;
        qca_transport t2 = d2.transport();
        CHECK_EQ(qca_rome_setup(&t2, &f.rpf, nullptr, nullptr, &r),
                 QCA_ERR_NVM_MISSING);
    }
}

void test_short_writes_and_errors() {
    std::puts("[short writes / transport errors abort immediately]");
    Fixture f;
    {
        SizedDevice d; d.header_short = true;
        d.rp_size = f.rp.size();
        qca_transport t = d.transport();
        qca_result r;
        CHECK_EQ(qca_rome_setup(&t, &f.rpf, &f.nvf, nullptr, &r),
                 QCA_ERR_HEADER_SHORT);
        CHECK_EQ(r.expected_len, 28u);
        CHECK_EQ(r.actual_len, 27u);
        CHECK_EQ(d.count(Op::Bulk), 0u);
        CHECK_EQ(d.count(Op::Sleep), 0u);
    }
    {
        SizedDevice d; d.header_rc = 0x1234;
        qca_transport t = d.transport();
        qca_result r;
        CHECK_EQ(qca_rome_setup(&t, &f.rpf, &f.nvf, nullptr, &r),
                 QCA_ERR_HEADER_IO);
        CHECK_EQ(r.transport_rc, 0x1234);
    }
    {
        SizedDevice d; d.bulk_fail_at = 5; // short write in 6th packet
        d.rp_size = f.rp.size();
        qca_transport t = d.transport();
        qca_result r;
        CHECK_EQ(qca_rome_setup(&t, &f.rpf, &f.nvf, nullptr, &r),
                 QCA_ERR_BODY_SHORT);
        CHECK_EQ(r.stage, QCA_STAGE_RAMPATCH);
        CHECK_EQ(r.fail_offset, 28u + 5u * 4096u);
        CHECK_EQ(r.expected_len, 4096u);
        CHECK_EQ(r.actual_len, 2048u);
        CHECK_EQ(d.count(Op::Bulk), 6u); // stopped right there
        CHECK_EQ(r.status_final, 0);
    }
    {
        SizedDevice d; d.bulk_fail_at = 0; d.bulk_fail_rc = 0xe00002d6; // timeout
        d.status = 0x80; d.nvm_size = f.nvm.size();
        qca_transport t = d.transport();
        qca_result r;
        CHECK_EQ(qca_rome_setup(&t, &f.rpf, &f.nvf, nullptr, &r),
                 QCA_ERR_BODY_IO);
        CHECK_EQ(r.stage, QCA_STAGE_NVM);
        CHECK_EQ((uint32_t)r.transport_rc, 0xe00002d6u);
        CHECK_EQ(r.fail_offset, 4u);
    }
    {
        SizedDevice d; d.fail_ctrl_in_req = QCA_REQ_GET_TARGET_VERSION;
        qca_transport t = d.transport();
        qca_result r;
        CHECK_EQ(qca_rome_setup(&t, &f.rpf, &f.nvf, nullptr, &r),
                 QCA_ERR_GET_VERSION_IO);
        CHECK_EQ(d.calls.size(), 1u);
    }
    {
        SizedDevice d; d.ctrl_in_short = 19;
        qca_transport t = d.transport();
        qca_result r;
        CHECK_EQ(qca_rome_setup(&t, &f.rpf, &f.nvf, nullptr, &r),
                 QCA_ERR_GET_VERSION_SHORT);
        CHECK_EQ(r.actual_len, 19u);
    }
    {
        SizedDevice d; d.fail_ctrl_in_req = QCA_REQ_CHECK_STATUS;
        qca_transport t = d.transport();
        qca_result r;
        CHECK_EQ(qca_rome_setup(&t, &f.rpf, &f.nvf, nullptr, &r),
                 QCA_ERR_CHECK_STATUS_IO);
        CHECK_EQ(r.stage, QCA_STAGE_CHECK_STATUS);
        CHECK_EQ(d.count(Op::CtrlOut), 0u);
    }
    {
        // Re-read of version after rampatch fails.
        SizedDevice d; d.fail_ctrl_in_req = QCA_REQ_GET_TARGET_VERSION;
        d.fail_ctrl_in_nth = 2; d.rp_size = f.rp.size(); d.nvm_size = f.nvm.size();
        qca_transport t = d.transport();
        qca_result r;
        CHECK_EQ(qca_rome_setup(&t, &f.rpf, &f.nvf, nullptr, &r),
                 QCA_ERR_GET_VERSION_IO);
        CHECK_EQ(r.stage, QCA_STAGE_REREAD_VERSION);
        CHECK_EQ(r.nvm_downloaded, 0);
    }
    {
        // ROM changes identity after rampatch -> refuse NVM.
        SizedDevice d; d.rom_after_rampatch = 0x300;
        d.rp_size = f.rp.size(); d.nvm_size = f.nvm.size();
        qca_transport t = d.transport();
        qca_result r;
        CHECK_EQ(qca_rome_setup(&t, &f.rpf, &f.nvf, nullptr, &r),
                 QCA_ERR_ROM_CHANGED);
        CHECK_EQ(d.count(Op::CtrlOut), 1u);
    }
}

void test_final_status_mismatch() {
    std::puts("[final status must be (s & 0xc0) == 0xc0]");
    Fixture f;
    const int finals[] = {0x00, 0x80, 0x40, 0x3f};
    for (int fs : finals) {
        SizedDevice d; d.override_final_status = fs;
        d.rp_size = f.rp.size(); d.nvm_size = f.nvm.size();
        qca_transport t = d.transport();
        qca_result r;
        CHECK_EQ(qca_rome_setup(&t, &f.rpf, &f.nvf, nullptr, &r),
                 QCA_ERR_FINAL_STATUS);
        CHECK_EQ(r.stage, QCA_STAGE_FINAL_STATUS);
        CHECK_EQ(r.status_final, fs);
        CHECK_EQ(r.rampatch_downloaded, 1);
        CHECK_EQ(r.nvm_downloaded, 1);
    }
    // Extra low bits set alongside 0xc0 are fine.
    SizedDevice d; d.override_final_status = 0xc3;
    d.rp_size = f.rp.size(); d.nvm_size = f.nvm.size();
    qca_transport t = d.transport();
    qca_result r;
    CHECK_EQ(qca_rome_setup(&t, &f.rpf, &f.nvf, nullptr, &r), QCA_OK);
    // Device that never applies firmware.
    SizedDevice d2; d2.apply_on_complete = false;
    d2.rp_size = f.rp.size(); d2.nvm_size = f.nvm.size();
    qca_transport t2 = d2.transport();
    CHECK_EQ(qca_rome_setup(&t2, &f.rpf, &f.nvf, nullptr, &r),
             QCA_ERR_FINAL_STATUS);
    CHECK_EQ(r.status_final, 0);
}

void test_probe_only_and_args() {
    std::puts("[probe-only mode and argument validation]");
    Fixture f;
    SizedDevice d;
    qca_transport t = d.transport();
    qca_options o{1};
    qca_result r;
    CHECK_EQ(qca_rome_setup(&t, &f.rpf, &f.nvf, &o, &r), QCA_OK);
    CHECK_EQ(d.count(Op::CtrlOut), 0u);
    CHECK_EQ(d.count(Op::Bulk), 0u);
    CHECK_EQ(d.calls.size(), 2u);

    CHECK_EQ(qca_rome_setup(nullptr, &f.rpf, &f.nvf, nullptr, &r),
             QCA_ERR_BAD_ARGUMENT);
    qca_transport bad = t; bad.bulk_out = nullptr;
    CHECK_EQ(qca_rome_setup(&bad, &f.rpf, &f.nvf, nullptr, &r),
             QCA_ERR_BAD_ARGUMENT);
    CHECK_EQ(qca_rome_setup(&t, &f.rpf, &f.nvf, nullptr, nullptr),
             QCA_ERR_BAD_ARGUMENT);
}

void test_decode_and_strings() {
    std::puts("[wire decoding and error strings]");
    const uint8_t w[20] = {0x02, 0x03, 0, 0, 0xe8, 0x03, 0, 0, 0x11, 0x22,
                           0x33, 0x44, 0x0a, 0x01, 0x34, 0x80, 1, 2, 3, 4};
    qca_version v;
    qca_decode_version(w, &v);
    CHECK_EQ(v.rom_version, 0x302u);
    CHECK_EQ(v.patch_version, 0x3e8u);
    CHECK_EQ(v.ram_version, 0x44332211u);
    CHECK_EQ(v.chip_id, 0x0a);
    CHECK_EQ(v.platform_id, 0x01);
    CHECK_EQ(v.flag, 0x8034);
    CHECK_EQ(v.reserved[3], 4);
    CHECK(qca_lookup_device(0x302) != nullptr);
    CHECK(qca_lookup_device(0x300) == nullptr);
    const qca_device_info *i = qca_lookup_device(0x302);
    CHECK_EQ(i->rampatch_hdr, 28);
    CHECK_EQ(i->nvm_hdr, 4);
    CHECK_EQ(i->ver_offset, 16);
    for (int e = 0; e < QCA_ERR__COUNT; ++e) {
        const char *s = qca_strerror(qca_err(e));
        CHECK(s && std::strcmp(s, "unknown error") != 0);
    }
    for (int s = QCA_STAGE_NONE; s <= QCA_STAGE_DONE; ++s)
        CHECK(std::strcmp(qca_stage_name(qca_stage(s)), "unknown") != 0);
}

#if defined(QCA_HAVE_FIRMWARE)
void test_real_firmware() {
    std::puts("[embedded linux-firmware blobs: sizes, version struct, full run]");
    CHECK_EQ(qca_rampatch_usb_00000302_len, 68644u);
    CHECK_EQ(qca_nvm_usb_00000302_len, 1998u);
    qca_firmware rp{qca_rampatch_usb_00000302, qca_rampatch_usb_00000302_len};
    qca_firmware nv{qca_nvm_usb_00000302, qca_nvm_usb_00000302_len};
    qca_rampatch_version rv;
    CHECK_EQ(qca_decode_rampatch_version(&rp, qca_lookup_device(0x302), &rv), 0);
    CHECK_EQ(rv.rom_version_low, 0x0302);
    std::printf("  rampatch version struct: rom_hi=0x%04x rom_lo=0x%04x patch=0x%04x\n",
                rv.rom_version_high, rv.rom_version_low, rv.patch_version);
    SizedDevice d; d.rp_size = rp.size; d.nvm_size = nv.size;
    d.patch_after_rampatch = rv.patch_version;
    qca_transport t = d.transport();
    qca_result r;
    CHECK_EQ(qca_rome_setup(&t, &rp, &nv, nullptr, &r), QCA_OK);
    CHECK_EQ(r.bulk_packets, 18u);
    CHECK(d.rampatch_blob.size() == rp.size &&
          std::memcmp(d.rampatch_blob.data(), rp.data, rp.size) == 0);
    CHECK(d.nvm_blob.size() == nv.size &&
          std::memcmp(d.nvm_blob.data(), nv.data, nv.size) == 0);
}
#endif

} // namespace

int main() {
    test_normal_flow();
    test_header_and_packet_sizes();
    test_already_done();
    test_only_nvm();
    test_only_rampatch();
    test_unsupported_rom();
    test_rampatch_version_mismatch();
    test_short_writes_and_errors();
    test_final_status_mismatch();
    test_probe_only_and_args();
    test_decode_and_strings();
#if defined(QCA_HAVE_FIRMWARE)
    test_real_firmware();
#else
    std::puts("[embedded firmware test SKIPPED: build without generated header]");
#endif
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
