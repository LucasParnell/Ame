#include "mochi/loan.h"
#include <cassert>
#include <utility>

int main() {
    alignas(64) unsigned char memory[64 * 4096]{};
    mochi::LoanWriter writer(memory, 64, 4096, 123);
    mochi::LoanReader reader({memory, 64, 4096, 123});
    mochi::LoanDescriptor descriptors[64]{};
    for (unsigned i = 0; i < 64; ++i) {
        mochi::WriteLoan loan;
        assert(writer.acquire(4096, loan) == MX_OK);
        assert(loan.bytes().data == memory + i * 4096);
        loan.bytes().data[4095] = i;
        assert(loan.publish(descriptors[i]) == MX_OK && !loan.valid() && !loan.bytes().size);
    }
    mochi::WriteLoan exhausted;
    assert(writer.acquire(4096, exhausted) == MX_ERR_WOULD_BLOCK);
    for (unsigned i = 0; i < 64; ++i) {
        mochi::ReadLoan loan, duplicate;
        auto invalid = descriptors[i];
        invalid.slot = 64;
        assert(reader.accept(invalid, loan) == MX_ERR_BAD_PROTOCOL);
        invalid = descriptors[i];
        ++invalid.pool;
        assert(reader.accept(invalid, loan) == MX_ERR_BAD_PROTOCOL);
        invalid = descriptors[i];
        invalid.size = 4097;
        assert(reader.accept(invalid, loan) == MX_ERR_BAD_PROTOCOL);
        assert(reader.accept(descriptors[i], loan) == MX_OK);
        assert(loan.bytes().data == memory + i * 4096 && loan.bytes().data[4095] == i);
        assert(reader.accept(descriptors[i], duplicate) == MX_ERR_BAD_PROTOCOL);
        mochi::LoanDescriptor returned{};
        assert(loan.release(returned) == MX_OK && !loan.bytes().size);
        assert(loan.release(returned) == MX_ERR_INVALID);
        invalid = returned;
        --invalid.size;
        assert(writer.reclaim(invalid) == MX_ERR_BAD_PROTOCOL);
        assert(writer.reclaim(returned) == MX_OK);
        assert(writer.reclaim(returned) == MX_ERR_BAD_PROTOCOL);
        assert(reader.accept(returned, duplicate) == MX_ERR_BAD_PROTOCOL);
    }
    assert(writer.drained() && reader.drained());
    for (unsigned cycle = 0; cycle < 10000; ++cycle) {
        mochi::WriteLoan loan, reclaimed;
        assert(writer.acquire(4096, loan) == MX_OK);
        auto moved = std::move(loan);
        assert(!loan.valid() && moved.valid());
        mochi::LoanDescriptor descriptor{};
        assert(moved.publish(descriptor) == MX_OK);
        mochi::ReadLoan read;
        assert(reader.accept(descriptor, read) == MX_OK);
        assert(read.release(descriptor) == MX_OK);
        assert(writer.reclaim(descriptor, &reclaimed) == MX_OK);
        // Re-publishing a returned loan must get a fresh generation too.
        assert(reclaimed.publish(descriptor) == MX_OK);
        assert(reader.accept(descriptor, read) == MX_OK);
        assert(read.release(descriptor) == MX_OK);
        assert(writer.reclaim(descriptor) == MX_OK);
    }
    {
        mochi::WriteLoan cancelled;
        assert(writer.acquire(1, cancelled) == MX_OK);
    }
    assert(writer.drained());
    assert(writer.acquire(4097, exhausted) == MX_ERR_INVALID);
}
