#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <utility>

namespace lib
{

template <typename K, typename V, typename Hash = std::hash<K>, size_t NUM_STRIPES = 64> class FlatMap
{
  enum class SlotState : uint8_t
  {
    Empty = 0,
    Occupied = 1,
    Tombstone = 2,
  };

  struct Buffer
  {
    size_t capacity = 0;
    SlotState *states = nullptr;
    size_t *hashes = nullptr;
    K *keys = nullptr;
    V *values = nullptr;

    explicit Buffer(size_t cap) : capacity(cap)
    {
      states = new SlotState[cap]();
      hashes = new size_t[cap]();
      keys = static_cast<K *>(::operator new(cap * sizeof(K)));
      values = static_cast<V *>(::operator new(cap * sizeof(V)));
    }

    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;

    ~Buffer()
    {
      delete[] states;
      delete[] hashes;
      ::operator delete(keys);
      ::operator delete(values);
    }
  };

  struct alignas(64) Stripe
  {
    std::mutex m;
  };

  static constexpr size_t kInitialCapacity = 16;
  static constexpr float kMaxLoadFactor = 0.70f;

  std::atomic<Buffer *> buf_;
  std::atomic<size_t> count_{0};

  std::atomic<uint32_t> activeReaders_{0};
  std::atomic<bool> resizeBlockReads_{false};
  std::mutex resizeMutex_;

  Stripe stripes_[NUM_STRIPES];

  Hash hasher_;

  static size_t probeFind(const Buffer *b, size_t h, const K &key) noexcept
  {
    const size_t cap = b->capacity;
    size_t idx = h % cap;

    for (size_t dist = 0; dist < cap; ++dist)
    {
      const SlotState s = b->states[idx];
      if (s == SlotState::Empty)
        return cap;
      if (s == SlotState::Occupied && b->hashes[idx] == h && b->keys[idx] == key)
        return idx;
      idx = (idx + 1 == cap) ? 0 : idx + 1;
    }
    return cap;
  }

  static size_t probeInsert(const Buffer *b, size_t h) noexcept
  {
    const size_t cap = b->capacity;
    size_t idx = h % cap;

    for (size_t dist = 0; dist < cap; ++dist)
    {
      const SlotState s = b->states[idx];
      if (s == SlotState::Empty || s == SlotState::Tombstone)
        return idx;
      idx = (idx + 1 == cap) ? 0 : idx + 1;
    }
    return cap;
  }

  struct ReaderGuard
  {
    FlatMap &self;

    explicit ReaderGuard(FlatMap &fm) : self(fm)
    {
      while (true)
      {
        while (self.resizeBlockReads_.load(std::memory_order_acquire))
          std::this_thread::yield();

        self.activeReaders_.fetch_add(1, std::memory_order_acq_rel);

        if (!self.resizeBlockReads_.load(std::memory_order_acquire))
          return;

        self.activeReaders_.fetch_sub(1, std::memory_order_acq_rel);
      }
    }

    ~ReaderGuard()
    {
      self.activeReaders_.fetch_sub(1, std::memory_order_acq_rel);
    }
  };

  bool needsResize(const Buffer *b) const noexcept
  {
    if (!b)
      return false;
    return static_cast<float>(count_.load(std::memory_order_relaxed) + 1) / static_cast<float>(b->capacity) >= kMaxLoadFactor;
  }

  void doResize()
  {
    std::unique_lock<std::mutex> resizeLock(resizeMutex_, std::try_to_lock);
    if (!resizeLock.owns_lock())
      return;

    Buffer *b = buf_.load(std::memory_order_acquire);
    if (!needsResize(b))
      return;

    const size_t newCap = b->capacity * 2;

    for (size_t i = 0; i < NUM_STRIPES; ++i)
      stripes_[i].m.lock();

    Buffer *newBuf = nullptr;
    try
    {
      newBuf = new Buffer(newCap);
      for (size_t i = 0; i < b->capacity; ++i)
      {
        if (b->states[i] == SlotState::Occupied)
        {
          const size_t h = b->hashes[i];
          const size_t idx = probeInsert(newBuf, h);
          newBuf->states[idx] = SlotState::Occupied;
          newBuf->hashes[idx] = h;
          new (newBuf->keys + idx) K(std::move(b->keys[i]));
          new (newBuf->values + idx) V(std::move(b->values[i]));
        }
      }
    }
    catch (...)
    {
      delete newBuf;
      for (size_t i = 0; i < NUM_STRIPES; ++i)
        stripes_[i].m.unlock();
      throw;
    }

    resizeBlockReads_.store(true, std::memory_order_seq_cst);

    while (activeReaders_.load(std::memory_order_acquire) != 0)
      std::this_thread::yield();

    Buffer *oldBuf = buf_.exchange(newBuf, std::memory_order_release);

    resizeBlockReads_.store(false, std::memory_order_release);
    for (size_t i = 0; i < NUM_STRIPES; ++i)
      stripes_[i].m.unlock();

    for (size_t i = 0; i < oldBuf->capacity; ++i)
    {
      if (oldBuf->states[i] == SlotState::Occupied)
      {
        oldBuf->keys[i].~K();
        oldBuf->values[i].~V();
      }
    }
    delete oldBuf;
  }

  template <typename VFwd> bool doInsert(const K &key, VFwd &&val)
  {
    if (needsResize(buf_.load(std::memory_order_acquire)))
      doResize();

    const size_t h = hasher_(key);
    const size_t si = h % NUM_STRIPES;

    std::unique_lock<std::mutex> lock(stripes_[si].m);
    Buffer *b = buf_.load(std::memory_order_acquire);
    size_t idx = probeFind(b, h, key);

    if (idx != b->capacity)
    {
      b->values[idx] = std::forward<VFwd>(val);
      return false;
    }

    if (needsResize(b))
    {
      lock.unlock();
      doResize();
      lock.lock();
      b = buf_.load(std::memory_order_acquire);
    }

    const size_t ins = probeInsert(b, h);
    b->states[ins] = SlotState::Occupied;
    b->hashes[ins] = h;
    new (b->keys + ins) K(key);
    new (b->values + ins) V(std::forward<VFwd>(val));
    count_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

public:
  explicit FlatMap(size_t initialCapacity = kInitialCapacity, const Hash &h = Hash()) : hasher_(h)
  {
    const size_t cap = initialCapacity < kInitialCapacity ? kInitialCapacity : initialCapacity;
    buf_.store(new Buffer(cap), std::memory_order_relaxed);
  }

  ~FlatMap()
  {
    Buffer *b = buf_.load(std::memory_order_relaxed);
    if (!b)
      return;
    for (size_t i = 0; i < b->capacity; ++i)
    {
      if (b->states[i] == SlotState::Occupied)
      {
        b->keys[i].~K();
        b->values[i].~V();
      }
    }
    delete b;
  }

  FlatMap(const FlatMap &) = delete;
  FlatMap &operator=(const FlatMap &) = delete;

  std::optional<V> find(const K &key) const
  {
    ReaderGuard g(const_cast<FlatMap &>(*this));
    const Buffer *b = buf_.load(std::memory_order_acquire);
    const size_t h = hasher_(key);
    const size_t idx = probeFind(b, h, key);
    if (idx == b->capacity)
      return std::nullopt;
    return b->values[idx];
  }

  bool contains(const K &key) const
  {
    ReaderGuard g(const_cast<FlatMap &>(*this));
    const Buffer *b = buf_.load(std::memory_order_acquire);
    const size_t h = hasher_(key);
    return probeFind(b, h, key) != b->capacity;
  }

  template <typename Fn> bool visit(const K &key, Fn &&fn)
  {
    const size_t h = hasher_(key);
    const size_t si = h % NUM_STRIPES;
    std::lock_guard<std::mutex> lock(stripes_[si].m);
    Buffer *b = buf_.load(std::memory_order_acquire);
    const size_t idx = probeFind(b, h, key);
    if (idx == b->capacity)
      return false;
    fn(b->values[idx]);
    return true;
  }

  template <typename Fn> bool visit(const K &key, Fn &&fn) const
  {
    const size_t h = hasher_(key);
    const size_t si = h % NUM_STRIPES;
    std::lock_guard<std::mutex> lock(stripes_[si].m);
    const Buffer *b = buf_.load(std::memory_order_acquire);
    const size_t idx = probeFind(b, h, key);
    if (idx == b->capacity)
      return false;
    fn(b->values[idx]);
    return true;
  }

  template <typename Fn> void forEach(Fn &&fn)
  {
    ReaderGuard g(*this);
    Buffer *b = buf_.load(std::memory_order_acquire);
    for (size_t i = 0; i < b->capacity; ++i)
      if (b->states[i] == SlotState::Occupied)
        fn(b->keys[i], b->values[i]);
  }

  template <typename Fn> void forEach(Fn &&fn) const
  {
    ReaderGuard g(const_cast<FlatMap &>(*this));
    const Buffer *b = buf_.load(std::memory_order_acquire);
    for (size_t i = 0; i < b->capacity; ++i)
      if (b->states[i] == SlotState::Occupied)
        fn(b->keys[i], b->values[i]);
  }

  bool insert(const K &key, const V &value)
  {
    return doInsert(key, value);
  }
  bool insert(const K &key, V &&value)
  {
    return doInsert(key, std::move(value));
  }

  template <typename... Args> bool emplace(const K &key, Args &&...args)
  {
    return doInsert(key, V(std::forward<Args>(args)...));
  }

  bool erase(const K &key)
  {
    const size_t h = hasher_(key);
    const size_t si = h % NUM_STRIPES;
    std::lock_guard<std::mutex> lock(stripes_[si].m);

    Buffer *b = buf_.load(std::memory_order_acquire);
    const size_t idx = probeFind(b, h, key);
    if (idx == b->capacity)
      return false;

    b->keys[idx].~K();
    b->values[idx].~V();
    b->states[idx] = SlotState::Tombstone;
    count_.fetch_sub(1, std::memory_order_relaxed);
    return true;
  }

  template <typename FnUpdate, typename FnCreate> void upsert(const K &key, FnUpdate &&fn_update, FnCreate &&fn_create)
  {
    if (needsResize(buf_.load(std::memory_order_acquire)))
      doResize();

    const size_t h = hasher_(key);
    const size_t si = h % NUM_STRIPES;

    std::unique_lock<std::mutex> lock(stripes_[si].m);
    Buffer *b = buf_.load(std::memory_order_acquire);
    const size_t idx = probeFind(b, h, key);

    if (idx != b->capacity)
    {
      fn_update(b->values[idx]);
      return;
    }

    if (needsResize(b))
    {
      lock.unlock();
      doResize();
      lock.lock();
      b = buf_.load(std::memory_order_acquire);
    }

    const size_t ins = probeInsert(b, h);
    b->states[ins] = SlotState::Occupied;
    b->hashes[ins] = h;
    new (b->keys + ins) K(key);
    new (b->values + ins) V(fn_create());
    count_.fetch_add(1, std::memory_order_relaxed);
  }

  size_t size() const noexcept
  {
    return count_.load(std::memory_order_relaxed);
  }
  bool empty() const noexcept
  {
    return size() == 0;
  }

  void clear()
  {
    for (size_t i = 0; i < NUM_STRIPES; ++i)
      stripes_[i].m.lock();

    resizeBlockReads_.store(true, std::memory_order_seq_cst);
    while (activeReaders_.load(std::memory_order_acquire) != 0)
      std::this_thread::yield();

    Buffer *b = buf_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < b->capacity; ++i)
    {
      if (b->states[i] == SlotState::Occupied)
      {
        b->keys[i].~K();
        b->values[i].~V();
      }
      b->states[i] = SlotState::Empty;
    }
    count_.store(0, std::memory_order_relaxed);

    resizeBlockReads_.store(false, std::memory_order_release);
    for (size_t i = 0; i < NUM_STRIPES; ++i)
      stripes_[i].m.unlock();
  }
};

} // namespace lib
