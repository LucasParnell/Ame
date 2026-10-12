// First-light v6 probe: writes persistent ramoops records (five zone copies)
// before anything else, dumps the inherited display configuration, attempts
// the standard paint and finishes through the timed reset ladder (entry.S).
// No silent exits: nested faults also force a fast reset.
#include "framebuffer.h"
#include "timing.h"

namespace {
constexpr unsigned kZones = 5;
#ifdef ANNE_MINIMAL_TEST
constexpr std::uint64_t kZoneBase[kZones] = {0x44060000, 0x440e0000, 0x44000000, 0x44020000, 0x44040000};
#else
constexpr std::uint64_t kZoneBase[kZones] = {0x34860000, 0x348e0000, 0x34800000, 0x34820000, 0x34840000};
#endif
constexpr std::uint64_t kZoneSize[kZones] = {0x80000, 0x20000, 0x20000, 0x20000, 0x20000};
constexpr unsigned kDmaExtra[20] = {0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x40, 0x44, 0x48, 0x4c, 0x50, 0x54, 0x58, 0x5c};
constexpr unsigned kLdiExtra[10] = {0x00, 0x04, 0x0c, 0x10, 0x18, 0x1c, 0x28, 0x2c, 0x30, 0x34};
volatile std::uint32_t* g_hdr[kZones];
volatile std::uint8_t* g_data[kZones];
unsigned g_cursor[kZones];
bool g_ready;
unsigned g_notes;

void barrier() { asm volatile("dsb sy" ::: "memory"); }

void clean_range(std::uint64_t base, unsigned bytes) {
    unsigned long ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const unsigned long line = 4ul << ((ctr >> 16) & 15);
    const std::uint64_t end = base + bytes;
    for (std::uint64_t p = base & ~(line - 1); p < end; p += line)
        asm volatile("dc cvac, %0" :: "r"(p) : "memory");
    barrier();
}

void rec_putc(char c) {
    for (unsigned i = 0; i < kZones; ++i) {
        if (!g_ready || !g_hdr[i] || g_cursor[i] >= kZoneSize[i] - 12) continue;
        g_data[i][g_cursor[i]++] = static_cast<std::uint8_t>(c);
        g_hdr[i][1] = g_cursor[i];
        g_hdr[i][2] = g_cursor[i];
    }
}
void rec_flush() {
    for (unsigned i = 0; i < kZones; ++i)
        if (g_hdr[i]) clean_range(kZoneBase[i], 12 + g_cursor[i]);
}
void rec_str(const char* text) { while (*text) rec_putc(*text++); }
void rec_dec(std::uint64_t value) {
    char digits[20];
    unsigned n = 0;
    if (!value) digits[n++] = '0';
    while (value) { digits[n++] = static_cast<char>('0' + value % 10); value /= 10; }
    while (n) rec_putc(digits[--n]);
}
void rec_hex(std::uint64_t value) {
    rec_putc('0'); rec_putc('x');
    for (int shift = 60; shift >= 0; shift -= 4) {
        const unsigned digit = static_cast<unsigned>((value >> shift) & 15);
        rec_putc(digit < 10 ? static_cast<char>('0' + digit) : static_cast<char>('a' + digit - 10));
    }
}
void rec_init() {
    for (unsigned i = 0; i < kZones; ++i) {
        g_hdr[i] = reinterpret_cast<volatile std::uint32_t*>(kZoneBase[i]);
        g_data[i] = reinterpret_cast<volatile std::uint8_t*>(kZoneBase[i] + 12);
        g_cursor[i] = 0;
        g_hdr[i][0] = 0x43474244; // persistent_ram signature (all ramoops zones)
        g_hdr[i][1] = 0;
        g_hdr[i][2] = 0;
    }
    g_ready = true;
    // The dmesg-ramoops copies need the "====" kmsg header at the data start.
    const char* kmsg = "====0.0-D\n";
    for (unsigned i = 2; i < kZones; ++i) {
        const char* p = kmsg;
        while (*p) {
            g_data[i][g_cursor[i]++] = static_cast<std::uint8_t>(*p++);
            g_hdr[i][1] = g_cursor[i];
            g_hdr[i][2] = g_cursor[i];
        }
    }
    rec_str("AMEFL6 ");
    rec_flush();
}

// --- Notification LED (PMIC register window at 0xFFF34000) -----------------
// Transport verified from hisi_pmic.c + live DTB: 8-bit access at base + (reg << 2).
#ifdef ANNE_MINIMAL_TEST
constexpr std::uint64_t kPmicBase = 0x43000000;
#else
constexpr std::uint64_t kPmicBase = 0xFFF34000ull;
#endif
std::uint8_t pmic_read(unsigned reg) {
    return *reinterpret_cast<volatile std::uint8_t*>(kPmicBase + (reg << 2));
}
void pmic_write(unsigned reg, std::uint8_t val) {
    *reinterpret_cast<volatile std::uint8_t*>(kPmicBase + (reg << 2)) = val;
}
void led_stage(char stage) {
    switch (stage) {
    case 'r': case 'g': case 'b': {
        const unsigned id = stage == 'r' ? 0u : stage == 'g' ? 1u : 2u;
        const std::uint8_t iset[3] = {0x7, 0x1, 0x7}; // red/green/blue (green capped by device tree)
        const unsigned iset_reg[3] = {0x14F, 0x151, 0x153};
        const unsigned start_reg[3] = {0x150, 0x152, 0x154};
        pmic_write(0x156, 0x33); // normalize DR345 time config1
        pmic_write(iset_reg[id], iset[id]);
        pmic_write(start_reg[id], 0x3);
        pmic_write(0x155, 0xF0); // constant on
        const std::uint8_t ctl = pmic_read(0x14D);
        pmic_write(0x14D, static_cast<std::uint8_t>((ctl & ~0x7u) | (1u << id)));
        const std::uint8_t out = pmic_read(0x14E);
        pmic_write(0x14E, static_cast<std::uint8_t>((out | 0x3Fu) & ~(3u << (id * 2))));
        break;
    }
    case 'w': { // white = fault marker: all three on
        pmic_write(0x156, 0x33);
        pmic_write(0x14F, 0x7); pmic_write(0x150, 0x3);
        pmic_write(0x151, 0x1); pmic_write(0x152, 0x3);
        pmic_write(0x153, 0x7); pmic_write(0x154, 0x3);
        pmic_write(0x155, 0xF0);
        const std::uint8_t ctl = pmic_read(0x14D);
        pmic_write(0x14D, static_cast<std::uint8_t>((ctl & ~0x7u) | 0x7u));
        const std::uint8_t out = pmic_read(0x14E);
        pmic_write(0x14E, static_cast<std::uint8_t>(out & ~0x3Fu));
        break;
    }
    case 'o': { // all off
        const std::uint8_t ctl = pmic_read(0x14D);
        pmic_write(0x14D, static_cast<std::uint8_t>(ctl & ~0x7u));
        const std::uint8_t out = pmic_read(0x14E);
        pmic_write(0x14E, static_cast<std::uint8_t>(out | 0x3Fu));
        break;
    }
    default:
        break;
    }
}

void led_hold(char stage) {
    led_stage(stage);
    rec_str("hold="); rec_putc(stage);
    rec_str(" hz="); rec_dec(anne::minimal::kCounterHz);
    rec_str(" ctl="); rec_hex(pmic_read(0x14D));
    rec_str(" out="); rec_hex(pmic_read(0x14E));
    rec_str(" begin="); rec_hex(anne::minimal::counter());
    rec_putc('\n'); rec_flush();
    const auto start = anne::minimal::counter();
    anne::minimal::wait_ticks(anne::minimal::kLedHoldTicks);
    rec_str("hold="); rec_putc(stage); rec_str(" end="); rec_hex(anne::minimal::counter());
    rec_str(" ticks="); rec_dec(anne::minimal::counter() - start);
    rec_putc('\n'); rec_flush();
}

// Mirrors anne::minimal::validate(); returns the first failing condition code.
unsigned reason(const anne::minimal::Registers& r) {
    const unsigned format = (r.control >> 3) & 31;
    const unsigned bpp = format < 5 ? 2 : 4;
    const unsigned panel_width = (r.panel_width & 0x1fff) + 1;
    const unsigned panel_height = (r.panel_height & 0x1fff) + 1;
    if (!(r.channel & 1)) return 1;                          // channel disabled
    if (r.channel & 4) return 2;                             // not linear mode
    if (!(r.ldi & 1)) return 3;                              // LDI disabled
    if (r.control & ((1u << 1) | (1u << 8) | (7u << 9) | (1u << 12) | (1u << 17))) return 4;
    if (format > 6) return 5;                                // unsupported pixel format
    if (r.stride >> 16) return 6;                            // stride overflow
    if (r.address & 15) return 7;                            // alignment
    if (r.x0 > r.x1 || r.y0 > r.y1 || r.x1 > 0xffff || r.y1 > 0xffff) return 8;
    if (panel_width != 1080 || panel_height != 2280) return 9;
    const std::uint64_t row_bytes = (std::uint64_t{r.x1} - r.x0 + 1) * 16;
    const unsigned height = r.y1 - r.y0 + 1;
    const unsigned stride = (r.stride & 0xffff) * 16;
    if (!stride) return 10;
    if (stride > 16384) return 11;
    if (row_bytes > stride) return 12;
    if (row_bytes > ((panel_width * bpp + 15) & ~15u)) return 13;
    if (height > panel_height) return 14;
    if (r.address < anne::minimal::kLogoBase) return 15;
    const std::uint64_t bytes = std::uint64_t{stride} * (height - 1) + row_bytes;
    const std::uint64_t offset = r.address - anne::minimal::kLogoBase;
    if (offset > anne::minimal::kLogoSize) return 16;
    if (bytes > anne::minimal::kLogoSize - offset) return 17;
    return 0;
}

void flush_painted(const anne::minimal::Buffer& fb) {
    const std::uint64_t end = fb.address +
        static_cast<std::uint64_t>(fb.stride) * (fb.height - 1) + fb.width * fb.bpp();
    clean_range(fb.address, static_cast<unsigned>(end - fb.address));
}
}

extern "C" void anko_fl_finish(unsigned long el); // entry.S: timed reset ladder

extern "C" void anko_fl_note(unsigned long code) {
    if (!g_ready || g_notes >= 16) return;
    ++g_notes;
    rec_str("\nnmi="); rec_hex(code);
    rec_str(" n="); rec_dec(g_notes);
    rec_flush();
}

extern "C" void anko_fl_retry_wait() {
    // Only reached if the reset pulse returned. Four on/off cycles = two seconds.
    for (unsigned i = 0; i < 4; ++i) {
        led_stage('g'); anne::minimal::wait_ticks(anne::minimal::kBlinkTicks);
        led_stage('o'); anne::minimal::wait_ticks(anne::minimal::kBlinkTicks);
    }
}

extern "C" void anko_firstlight_main(unsigned long el, unsigned long dtb, unsigned long sctlr) {
    using namespace anne::minimal;
    rec_init();
    rec_str("led0="); rec_hex(pmic_read(0x14D));
    rec_str(" led1="); rec_hex(pmic_read(0x14E));
    rec_str(" p0="); rec_hex(pmic_read(0x000));
    rec_str(" p1="); rec_hex(pmic_read(0x001));
    rec_putc('\n');
    led_stage('r');
    rec_str("lR="); rec_hex(pmic_read(0x14D));
    rec_str(" L=r\n"); rec_flush();
    led_hold('r');
    rec_str("el="); rec_dec(el);
    rec_str(" mmu="); rec_dec(sctlr & 1);
    rec_str(" sctlr="); rec_hex(sctlr);
    rec_str(" x0="); rec_hex(dtb);
    unsigned long freq;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    rec_str(" frq="); rec_hex(freq);
    unsigned long mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    rec_str(" mp="); rec_hex(mpidr);
    unsigned long t0;
    asm volatile("mrs %0, cntpct_el0" : "=r"(t0));
    rec_str(" ct="); rec_hex(t0);
    rec_putc('\n'); rec_flush();

    auto* ldi = reinterpret_cast<volatile std::uint32_t*>(kDss + kLdi);
    const unsigned ldi_ctl = ldi[0x24 / 4], ldi_w = ldi[8 / 4], ldi_h = ldi[0x14 / 4];
    rec_str("ldi="); rec_hex(ldi_ctl); rec_putc(' '); rec_hex(ldi_w); rec_putc(' '); rec_hex(ldi_h);
    rec_putc('\n');
    rec_str("ldi2:");
    for (unsigned k = 0; k < 10; ++k) { rec_putc(' '); rec_hex(ldi[kLdiExtra[k] / 4]); }
    rec_putc('\n'); rec_flush();

    Buffer chosen{};
    int chosen_index = -1;
    for (int index = 0; index < 3; ++index) {
        auto* dma = reinterpret_cast<volatile std::uint32_t*>(kDss + kChannels[index]);
        const Registers r{dma[0xd4 / 4], dma[0x1c / 4], dma[0x60 / 4], dma[0x64 / 4],
                          dma[0], dma[1], dma[2], dma[3], ldi_ctl, ldi_w, ldi_h};
        rec_putc('c'); rec_dec(static_cast<unsigned>(index)); rec_putc('=');
        rec_hex(r.channel); rec_putc(' '); rec_hex(r.control); rec_putc(' ');
        rec_hex(r.address); rec_putc(' '); rec_hex(r.stride); rec_putc(' ');
        rec_hex(r.x0); rec_putc(' '); rec_hex(r.y0); rec_putc(' ');
        rec_hex(r.x1); rec_putc(' '); rec_hex(r.y1);
        const unsigned code = reason(r);
        rec_str(" r="); rec_dec(code);
        if (!code && !chosen.address) {
            chosen = validate(r);
            chosen_index = index;
        }
        rec_putc('\n');
        rec_str("rg:");
        for (unsigned k = 0; k < 20; ++k) { rec_putc(' '); rec_hex(dma[kDmaExtra[k] / 4]); }
        rec_putc('\n'); rec_flush();
    }
    led_stage('b'); rec_str("L=b\n"); rec_flush();
    led_hold('b');
    if (chosen.address) {
        paint(chosen, el);
        flush_painted(chosen);
        rec_str("paint=ok c="); rec_dec(static_cast<unsigned>(chosen_index)); rec_putc('\n');
    } else {
        rec_str("paint=skip\n");
    }
    if ((dtb & 3) == 0 && dtb >= 0x400000 && dtb < 0x880000000ull) {
        unsigned long t1;
        asm volatile("mrs %0, cntpct_el0" : "=r"(t1));
        const std::uint32_t magic = *reinterpret_cast<volatile std::uint32_t*>(dtb);
        rec_str("dtb="); rec_hex(magic); rec_str(" t1="); rec_hex(t1); rec_putc('\n');
    }
#ifdef ANNE_MINIMAL_TEST_FAULT
    asm volatile("brk #0");
#endif
    led_stage('g'); rec_str("L=g\n"); rec_flush();
    led_hold('g');
    rec_str("finish el="); rec_dec(el);
    rec_str(" d=0");
    rec_putc('\n'); rec_flush();
    anko_fl_finish(el); // never returns
}

extern "C" void anko_fl_fault(unsigned long esr, unsigned long pc, unsigned long far) {
    if (g_ready) {
        rec_str("\n!FAULT esr="); rec_hex(esr); rec_str(" pc="); rec_hex(pc);
        rec_str(" far="); rec_hex(far);
        rec_putc('\n'); rec_flush();
        led_hold('w');
    }
    unsigned long el;
    asm volatile("mrs %0, currentel" : "=r"(el));
    anko_fl_finish(el >> 2); // never returns
}
