#include <simde/x86/avx512.h>

namespace detail
{

template <typename K, typename T> class Table
{
  static constexpr size_t chunkCapacity = 16;

  static constexpr int8_t emptyFlag = int8_t(0b11111111);
  static constexpr int8_t reservedFlag = int8_t(0b11111110);
  static constexpr int8_t bitsForDirectHit = int8_t(0b10000000);
  static constexpr int8_t directHitFlag = int8_t(0b00000000);
  static constexpr int8_t listEntryFlag = int8_t(0b10000000);

  static constexpr int8_t bits_for_distance = int8_t(0b01111111);

  static constexpr int jumpDistancesCount = 126;

  static constexpr size_t jump_distances[jumpDistancesCount]{
    0,
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    9,
    10,
    11,
    12,
    13,
    14,
    15,

    21,
    28,
    36,
    45,
    55,
    66,
    78,
    91,
    105,
    120,
    136,
    153,
    171,
    190,
    210,
    231,
    253,
    276,
    300,
    325,
    351,
    378,
    406,
    435,
    465,
    496,
    528,
    561,
    595,
    630,
    666,
    703,
    741,
    780,
    820,
    861,
    903,
    946,
    990,
    1035,
    1081,
    1128,
    1176,
    1225,
    1275,
    1326,
    1378,
    1431,
    1485,
    1540,
    1596,
    1653,
    1711,
    1770,
    1830,
    1891,
    1953,
    2016,
    2080,
    2145,
    2211,
    2278,
    2346,
    2415,
    2485,
    2556,

    3741,
    8385,
    18915,
    42486,
    95703,
    215496,
    485605,
    1091503,
    2456436,
    5529475,
    12437578,
    27986421,
    62972253,
    141700195,
    318819126,
    717314626,
    1614000520,
    3631437253,
    8170829695,
    18384318876,
    41364501751,
    93070021080,
    209407709220,
    471167588430,
    1060127437995,
    2385287281530,
    5366895564381,
    12075513791265,
    27169907873235,
    61132301007778,
    137547673121001,
    309482258302503,
    696335090510256,
    1566753939653640,
    3525196427195653,
    7931691866727775,
    17846306747368716,
    40154190394120111,
    90346928493040500,
    203280588949935750,
    457381324898247375,
    1029107980662394500,
    2315492957028380766,
    5209859150892887590,
  };

  inline static int distance(int8_t metadata)
  {
    return metadata & int8_t(0b01111111);
  }

  struct Chunk
  {
    uint8_t metadata[chunkCapacity];
    struct KeyValue
    {
      K key;
      T value;
    };

    KeyValue data[chunkCapacity];

    inline static void emptyChunk(Chunk *c)
    {
      for (size_t i = 0; i < chunkCapacity; i++)
      {
        c->metadata[i] = emptyFlag;
      }
    }
  };

  Chunk *chunks;

  inline bool find(const FindKey &key, T &out)
  {
    size_t index = hash_object(key);
    size_t num_slots_minus_one = this->num_slots_minus_one;

    Chunk *entries = this->entries;

    index = index & num_slots_minus_one;

    bool first = true;

    while (true)
    {
      size_t block_index = index / chunkCapacity;
      int index_in_block = index % chunkCapacity;

      Chunk *block = entries + block_index;

      int8_t metadata = block->control_bytes[index_in_block];

      if (first)
      {
        if ((metadata & bitsForDirectHit) != directHitFlag)
        {
          return false;
        }
        first = false;
      }

      if (block->data[index_in_block].key == key)
      {
        out = block->data[index_in_block].value;
        return true;
      }

      int8_t to_next_index = distance(metadata);

      if (to_next_index == 0)
      {
        return false;
      }

      index += jump_distances[to_next_index];
      index = index & num_slots_minus_one;
    }
  }

  inline bool emplace(Key &key, T &val)
  {
    size_t index = hash_object(key);
    size_t num_slots_minus_one = this->num_slots_minus_one;
    Chunk* entries = this->entries;
    
    index = hash_policy.index_for_hash(index, num_slots_minus_one);
    
    bool first = true;
    
    for (;;)
    {
      size_t block_index = index / chunkCapacity;
      int index_in_block = index % chunkCapacity;
      Chunk* block = entries + block_index;

      int8_t metadata = block->control_bytes[index_in_block];

      if (first)
      {
        if ((metadata & bitsForDirectHit) != directHitFlag) {
        -->  return emplace_direct_hit({index, block}, std::forward<Key>(key), std::forward<Args>(args)...);
        }

        first = false;
      }

      if (key == block->data[index_in_block].key) {
        return false;
      }

      int8_t to_next_index = distance(metadata); 

      if (to_next_index == 0) {
        return emplace_new_key({index, block}, std::forward<Key>(key), std::forward<Args>(args)...);
      }

      index += jump_distances[to_next_index];
      index = index & num_slots_minus_one;
    }
  }
};

} // namespace detail