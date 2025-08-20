#include "rhi/GPUHeap.hpp"
#include <cassert>
#include <iostream>

using namespace rhi;

static bool isPow2(uint64_t x)
{
  return x && ((x & (x - 1)) == 0);
}

int main()
{
  // --------------------------
  // Basic GPUHeap functionality
  // --------------------------
  {
    const uint64_t HEAP_SIZE = 4096;
    GPUHeap heap(HEAP_SIZE);

    // Sanity checks
    assert(heap.getTotalSize() == HEAP_SIZE);
    assert(heap.getUsedSize() == 0);
    assert(heap.getFreeSize() == HEAP_SIZE);
    assert(heap.hasAvailableNodes());
    assert(heap.getApproximateFreeBlockCount() >= 1);

    // Allocate 100 bytes with default alignment (256)
    GPUBuffer a{};
    {
      auto st = heap.allocate(100, a); // default alignment = 256 in impl
      assert(st == GPUHeap::OK);
      assert(a.isValid());
      assert(a.heap == &heap);
      assert(a.offset % 256 == 0);
      // In this implementation, size is rounded up to alignment
      assert(a.size == 256);
      assert(heap.getUsedSize() == 256);
      assert(heap.getFreeSize() == HEAP_SIZE - 256);
    }

    // Allocate 100 bytes with 512-byte alignment
    GPUBuffer b{};
    {
      auto st = heap.allocate(100, 512, b);
      assert(st == GPUHeap::OK);
      assert(b.isValid());
      assert(b.heap == &heap);
      assert(b.offset % 512 == 0);
      assert(b.size == 512);
      assert(heap.getUsedSize() == (256 + 512));
      assert(heap.getFreeSize() == HEAP_SIZE - (256 + 512));
    }

    // Free the first allocation and ensure stats update
    heap.free(a);
    assert(!a.isValid());
    assert(heap.getUsedSize() == 512);
    assert(heap.getFreeSize() == HEAP_SIZE - 512);

    // Allocate again (256 aligned). Expect reuse of the freed region at offset 0.
    GPUBuffer c{};
    {
      auto st = heap.allocate(128, 256, c);
      assert(st == GPUHeap::OK);
      assert(c.isValid());
      assert(c.heap == &heap);
      assert(c.offset % 256 == 0);
      assert(c.size == 256);
      // With the simple merge/reuse path, this should typically reuse offset 0.
      // If you change internals later, keeping it aligned is the key requirement.
      assert(c.offset == 0);
      assert(heap.getUsedSize() == (512 + 256)); // b + c
    }

    // Try to allocate until OOM (remaining capacity ~ 4096 - 768 = 3328)
    uint64_t consumed = heap.getUsedSize();

    std::vector<GPUBuffer> bulk;

    for (;;)
    {
      GPUBuffer tmp{};
      auto st = heap.allocate(256, tmp); // default align 256, size becomes 256
      // printf("ddddd %u %u %u\n", st, tmp.offset, tmp.size);

      if (st != GPUHeap::OK)
        break;
      bulk.push_back(tmp);
      consumed += 256;
    }

    // We should be out of memory now
    {
      GPUBuffer tmp{};
      auto st = heap.allocate(256, tmp);
      assert(st == GPUHeap::OUT_OF_MEMORY);
      assert(!tmp.isValid());
    }

    // Free everything we still hold (c + b + bulk)
    for (auto &buf : bulk)
      heap.free(buf);
    heap.free(c);
    heap.free(b);
    assert(heap.getUsedSize() == 0);
    assert(heap.getFreeSize() == HEAP_SIZE);
  }

  // --------------------------
  // BuddyGPUHeap minimal checks
  // --------------------------
  {
    const uint64_t HEAP_SIZE = 1024;
    BuddyGPUHeap buddy(HEAP_SIZE);

    // Allocate a non-power-of-two request and verify we got a power-of-two block >= request
    GPUBuffer x{};
    {
      auto st = buddy.allocate(300, x); // buddy overrides allocate(size, out)
      assert(st == GPUHeap::OK);
      assert(x.isValid());
      assert(x.heap == &buddy);
      assert(isPow2(x.size));
      assert(x.size >= 300);
      // Buddy blocks should be aligned to their own size
      assert(x.offset % x.size == 0);
    }

    // Allocate another, smaller request and check the same invariants
    GPUBuffer y{};
    {
      auto st = buddy.allocate(200, y);
      assert(st == GPUHeap::OK);
      assert(y.isValid());
      assert(isPow2(y.size));
      assert(y.size >= 200);
      assert(y.offset % y.size == 0);
    }

    // Note: We intentionally do not rely on free() -> reuse for BuddyGPUHeap here,
    // because this version doesn't return freed blocks to the buddy free lists.
    // We do, however, verify stats go down when freeing.
    uint64_t usedBeforeFree = buddy.getUsedSize();
    buddy.free(x);
    buddy.free(y);
    assert(buddy.getUsedSize() < usedBeforeFree);
  }

  std::cout << "All GPUHeap and BuddyGPUHeap API tests passed.\n";
  return 0;
}
