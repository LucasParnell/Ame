#include "../kernel/power/anne/mailbox.h"
#include <cassert>
#include <vector>
#include <utility>
using anne::dvfs::Mailbox;
using anne::dvfs::Result;
struct Io {
    unsigned words[1024]{}, indexes[2]{}, sends = 0;
    bool fail = false;
    std::vector<std::pair<unsigned, unsigned>> writes;
    Io() {
        words[(Mailbox::kChannel + 0x10) / 4] = 0x10;
    }
    bool read32(unsigned offset, unsigned* out) {
        if (fail)
            return false;
        *out = words[offset / 4];
        return true;
    }
    bool write32(unsigned offset, unsigned value) {
        if (fail)
            return false;
        assert(offset == 0xa00 || (offset >= Mailbox::kChannel && offset < Mailbox::kChannel + 64));
        writes.push_back({offset, value});
        if (offset == 0xa00)
            words[offset / 4] = 0;
        else if (offset == Mailbox::kChannel) {
            words[offset / 4] = value;
            auto& mode = words[(Mailbox::kChannel + 0x10) / 4];
            mode = (mode & 0x80) ? 0x10 : 0x20;
        } else if (offset == Mailbox::kChannel + 0x10)
            words[offset / 4] = 0x20 | value;
        else {
            words[offset / 4] = value;
            if (offset == Mailbox::kChannel + 0x1c) {
                ++sends;
                words[(Mailbox::kChannel + 0x10) / 4] = 0x41;
            }
        }
        return true;
    }
    bool frequency_index(unsigned cluster, unsigned& value) {
        value = indexes[cluster];
        return !fail;
    }
    void ack(unsigned cluster, unsigned index) {
        indexes[cluster] = index;
        words[(Mailbox::kChannel + 0x10) / 4] = 0x81;
    }
};
int main() {
    Mailbox driver;
    Io io;
    assert(driver.request(io, 0, 1709000, 3, 100) == Result::Pending);
    assert(io.sends == 1 && io.words[(Mailbox::kChannel + 0x20) / 4] == 0x1030a);
    assert(io.words[(Mailbox::kChannel + 0x24) / 4] == 1709);
    assert(io.words[(Mailbox::kChannel + 0x14) / 4] == ~8u);
    const auto writes = io.writes.size();
    assert(driver.request(io, 1, 2362000, 4, 200) == Result::Pending && io.writes.size() == writes);
    io.ack(0, 3);
    assert(driver.request(io, 0, 1709000, 3, 300) == Result::Done && driver.completed[0] == 1);
    assert(driver.request(io, 0, 1709000, 3, 400) == Result::Done && io.sends == 1);
    assert(driver.request(io, 1, 2362000, 4, 500) == Result::Pending && io.sends == 2);
    assert(io.words[(Mailbox::kChannel + 0x20) / 4] == 0x2030a);
    io.ack(1, 4);
    assert(driver.request(io, 1, 2362000, 4, 600) == Result::Done && driver.completed[1] == 1);
    assert(driver.request(io, 0, 480000, 0, 700) == Result::Pending);
    auto before_timeout = io.writes.size();
    assert(driver.request(io, 0, 480000, 0, 50000700) == Result::Fault);
    assert(driver.failed &&
           io.writes.size() == before_timeout); // Never force-release a live remote message.
    assert(driver.request(io, 1, 1402000, 0, 60000000) == Result::Fault &&
           io.writes.size() == before_timeout);
    Mailbox denied;
    Io foreign;
    foreign.words[(Mailbox::kChannel + 0x10) / 4] = 0x80;
    foreign.words[Mailbox::kChannel / 4] = 2;
    assert(denied.request(foreign, 0, 1709000, 3, 0) == Result::Pending && foreign.writes.empty());
    Mailbox delayed;
    Io lag;
    assert(delayed.request(lag, 0, 1709000, 3, 100) == Result::Pending);
    lag.ack(0, 2);
    assert(delayed.request(lag, 0, 1709000, 3, 200) ==
           Result::Pending); // ACK is not frequency confirmation.
    lag.indexes[0] = 3;
    assert(delayed.request(lag, 0, 1709000, 3, 300) == Result::Done);
    Mailbox fault;
    Io broken;
    broken.fail = true;
    assert(fault.request(broken, 0, 1709000, 3, 100) == Result::Fault && fault.failed);
}
