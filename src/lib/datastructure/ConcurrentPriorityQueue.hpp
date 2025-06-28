#pragma once

#include "ThreadLocalStorage.hpp"
#include "lib/datastructure/HazardPointer.hpp"
#include <atomic>
namespace lib
{

#define ALIGNED(N) __attribute__((aligned(N)));
#define CACHE_LINE_SIZE 64

template <typename T, typename P = size_t> class ConcurrentPriorityQueue
{
  static thread_local uint32_t seed;

  struct Node
  {
    T value;
    P priority;

    std::atomic<Node *> parent;
    std::atomic<Node *> left;
    std::atomic<Node *> next;
    std::atomic<Node *> right;

    std::atomic<unsigned char> inserting;
    std::atomic<unsigned char> parentDirection;
    // TODO: add padding

    Node(T value, P priority) : parent(nullptr), left(nullptr), next(nullptr), right(nullptr), inserting(false), parentDirection(false)
    {
      this->value = value;
      this->priority = priority;
    }
  };

  struct InsertSeekRecordInfo
  {
    Node *child;
    Node *next;
    Node *cast1;
    Node *cast2;
    unsigned char duplicate;
    unsigned char parentDirection;
    // TODO: add padding
  };

  std::atomic<Node *> head;
  std::atomic<Node *> root;

  ThreadLocalStorage<Node *> previousDummy;
  ThreadLocalStorage<Node *> previousHead;

  HazardPointer<8> hazardAllocator;

  // TODO: padding
  static constexpr size_t LEFT_DIRECTION = 1;
  static constexpr size_t RIGHT_DIRECTION = 2;
  static constexpr size_t DUPLICATE_DIRECTION = 3;
  static constexpr size_t NOT_MARKED = 0;
  static constexpr size_t DELETE_MARK = 1;
  static constexpr size_t INSERT_MARK = 2;
  static constexpr size_t LEAF_MARK = 3;

  static inline Node *address(Node *ptr)
  {
    return reinterpret_cast<Node *>(reinterpret_cast<uintptr_t>(ptr) & ~uintptr_t(0x3));
  }

  static inline uint64_t getMark(Node *ptr)
  {
    return reinterpret_cast<uintptr_t>(ptr) & 0x3;
  }

  static inline Node *mark(Node *ptr, size_t mark)
  {
    return reinterpret_cast<Node *>((reinterpret_cast<uintptr_t>(ptr) & ~uintptr_t(0x3)) | mark);
  }

  Node *allocateNode(T v = T(), P p = P())
  {
    // TODO: use aligned allocator
    return new Node(v, p);
  }

  inline void readLeft(Node *&parent_node, Node *&child_node, size_t &child_mark, size_t &operation_mark, unsigned char &parent_direction)
  {
    // printf("READING LEFT\n");
    operation_mark = getMark(parent_node->next.load());
    auto raw = parent_node->left.load();
    child_node = address(raw);
    child_mark = getMark(raw);
    parent_direction = LEFT_DIRECTION;
  }

  inline void readRight(Node *&parent_node, Node *&child_node, size_t &child_mark, size_t &operation_mark, unsigned char &parent_direction)
  {
    // printf("READING RIGHT\n");

    operation_mark = getMark(parent_node->next.load());
    auto raw = parent_node->right.load();
    // printf("child_node %p, %p\n", child_node, address(raw));
    child_node = address(raw);
    // printf("child_node %p, %p\n", child_node, address(raw));
    child_mark = getMark(raw);
    parent_direction = RIGHT_DIRECTION;
  }

  uint32_t randomGen()
  {
    if (seed == 0)
    {
      seed = os::Thread::getCurrentThreadId();
    }

    seed = seed * 1103515245 + 12345;
    return seed % 20; // deleteScale;
  }

  inline void tryHelpInsert(Node *newNode)
  {
    // HazardPointer<5>::Record *record = hazardAllocator.acquire();

    bool parentDir = newNode->parentDirection.load(std::memory_order_relaxed);

    Node *cas1 = newNode->parent.load(std::memory_order_relaxed), *cas2 = newNode->left.load(std::memory_order_relaxed);

    if (newNode->inserting.load(std::memory_order_acquire))
    {
      if (parentDir == LEFT_DIRECTION)
      {
        cas1->left.compare_exchange_strong(cas2, newNode, std::memory_order_acq_rel, std::memory_order_acquire);
      }
      else
      {
        cas1->right.compare_exchange_strong(cas2, newNode, std::memory_order_acq_rel, std::memory_order_acquire);
      }

      if (newNode->inserting.load())
      {
        newNode->inserting.store(false, std::memory_order_release);
      }
    }
  }

#define LOAD(a, b, r, i)                                                                                                                                                           \
  a = nullptr;                                                                                                                                                                     \
  do                                                                                                                                                                               \
  {                                                                                                                                                                                \
    a = b.load();                                                                                                                                                                  \
    r->assign(a, i);                                                                                                                                                               \
  } while (b.load() != a);

#define LOAD_LEFT_ADDR(a, b, r, i)                                                                                                                                                 \
  a = nullptr;                                                                                                                                                                     \
  do                                                                                                                                                                               \
  {                                                                                                                                                                                \
    a = address(b->left.load());                                                                                                                                                   \
    r->assign(a, i);                                                                                                                                                               \
  } while (address(b->left.load()) != a);
  
#define LOAD_RIGHT_ADDR(a, b, r, i)                                                                                                                                                \
  a = nullptr;                                                                                                                                                                     \
  do                                                                                                                                                                               \
  {                                                                                                                                                                                \
    a = address(b->right.load());                                                                                                                                                  \
    r->assign(a, i);                                                                                                                                                               \
  } while (address(b->right.load()) != a);

  InsertSeekRecordInfo insertSearch(P priority, HazardPointer<8>::Record *record)
  {
    Node *grand_parent_node = nullptr;

    Node *LOAD(parent_node, root, record, 0);
    Node *LOAD_LEFT_ADDR(parent_left, parent_node, record, 1);

    Node *child_node = address(parent_left);

    size_t operation_mark = getMark(child_node);
    size_t child_mark = 0;
    unsigned char parent_direction = LEFT_DIRECTION;
    Node *marked_node = nullptr;

    while (true)
    {
      // printf("current = %p, dir = %i\n", child_node, parent_direction);
      if (operation_mark == DELETE_MARK)
      {
        readRight(parent_node, child_node, child_mark, operation_mark, parent_direction);
        marked_node = parent_node;

        while (true)
        {
          if (operation_mark == DELETE_MARK)
          {
            if (child_mark != LEAF_MARK)
            {
              parent_node = child_node;
              readRight(parent_node, child_node, child_mark, operation_mark, parent_direction);
              continue;
            }
            else
            {
              parent_node = address(child_node->next.load());
              readRight(parent_node, child_node, child_mark, operation_mark, parent_direction);
              break;
            }
          }
          else
          {
            const static uint32_t insertCleanRate = 50;

            // occasional cleanup
            if (randomGen() < insertCleanRate)
            {
              if ((getMark(grand_parent_node->next.load()) == 0) && grand_parent_node->left.load() == marked_node)
              {
                grand_parent_node->left.compare_exchange_strong(marked_node, parent_node);
              }
            }
            // TRAVERSE()
            if (priority <= parent_node->priority)
              readLeft(parent_node, child_node, child_mark, operation_mark, parent_direction);
            else
              readRight(parent_node, child_node, child_mark, operation_mark, parent_direction);
            break;
          }
        }
        continue;
      }

      if (child_mark != LEAF_MARK)
      {
        /*
        printf(
            "unmarked curr %p, left = %u / %p, right = %u / %p\n",
            child_node,
            child_node->left.load() ? child_node->left.load()->priority : -1,
            child_node->left.load(),
            child_node->right.load() ? child_node->right.load()->priority : -1,
            child_node->right.load());
*/
        grand_parent_node = parent_node;
        parent_node = child_node;

        if (priority <= parent_node->priority)
          readLeft(parent_node, child_node, child_mark, operation_mark, parent_direction);
        else
          readRight(parent_node, child_node, child_mark, operation_mark, parent_direction);
      }
      else
      {
        /*
        printf(
            "marked curr %p, left = %u / %p, right = %u / %p\n",
            child_node,
            child_node->left.load() ? child_node->left.load()->priority : -1,
            child_node->left.load(),
            child_node->right.load() ? child_node->right.load()->priority : -1,
            child_node->right.load());
            */
        Node *child_next = address(child_node->next.load());

        if (getMark(child_node->next.load()))
        {
          // printf("got mark\n");
          parent_node = child_next;
          readRight(parent_node, child_node, child_mark, operation_mark, parent_direction);
          continue;
        }

        if (child_next && child_next->inserting.load())
        {
          // printf("child next inserting\n");

          tryHelpInsert(child_next);
          parent_node = child_next;
          if (priority <= parent_node->priority)
            readLeft(parent_node, child_node, child_mark, operation_mark, parent_direction);
          else
            readRight(parent_node, child_node, child_mark, operation_mark, parent_direction);
          continue;
        }
        /*
        if (child_next && child_next->priority == priority)
        {
          //printf("child next priority equal %p %lu %lu\n", child_next, child_next->priority, priority);
          ins_seek.duplicate.store(DUPLICATE_DIRECTION);
          return;
        }
        */
        bool is_correct_leaf = (parent_direction == LEFT_DIRECTION && parent_node->left.load() == mark(child_node, LEAF_MARK)) ||
                               (parent_direction == RIGHT_DIRECTION && parent_node->right.load() == mark(child_node, LEAF_MARK));

        if (is_correct_leaf)
        {
          // printf("correct leaf %i\n", parent_direction);
          InsertSeekRecordInfo ins_seek;
          ins_seek.duplicate = 0;
          ins_seek.child = child_node;
          ins_seek.cast1 = parent_node;
          ins_seek.cast2 = mark(child_node, LEAF_MARK);
          ins_seek.next = child_next;
          ins_seek.parentDirection = parent_direction;
          return ins_seek;
        }

        // else TRAVERSE()
        if (priority < parent_node->priority)
          readLeft(parent_node, child_node, child_mark, operation_mark, parent_direction);
        else
          readRight(parent_node, child_node, child_mark, operation_mark, parent_direction);
      }
    }
  }

  void physicalDelete(Node *dummyNode)
  {
    Node *grandParent = nullptr;
    Node *parent = root.load(std::memory_order_acquire);
    Node *child = address(parent->left.load(std::memory_order_acquire));
    unsigned char parentDirection;
    size_t opMark = 0;
    size_t childMark = 0;
    Node *marked = nullptr;

    while (true)
    {
      // --- traverse logically deleted nodes ---
      if (opMark == DELETE_MARK)
      {
        readRight(parent, child, childMark, opMark, /*out*/ parentDirection);
        marked = parent;

        while (true)
        {
          if (opMark == DELETE_MARK)
          {
            if (childMark != LEAF_MARK)
            {
              parent = child;
              readRight(parent, child, childMark, opMark, parentDirection);
              continue;
            }
            else
            {
              Node *childNext = address(child->next.load(std::memory_order_acquire));
              if (childNext->inserting.load(std::memory_order_acquire) && childNext->parent.load(std::memory_order_acquire) == parent)
              {
                tryHelpInsert(childNext);
              }
              else if (parent->right.load(std::memory_order_acquire) == mark(child, LEAF_MARK))
              {
                // “confirm edge is complete”
                if (grandParent && grandParent->priority != P(0))
                {
                  // in C they zeroed key; here we zero priority
                  grandParent->priority = 0;
                }
                goto FINISH;
              }
              readRight(parent, child, childMark, opMark, parentDirection);
              continue;
            }
          }
          else
          {
            // try cleanup
            // if (grandParent)
            //{
            if (!getMark(grandParent->next.load(std::memory_order_acquire)))
            {
              if (grandParent->left.load(std::memory_order_acquire) == marked)
              {
                if (grandParent->left.compare_exchange_strong(marked, parent, std::memory_order_acq_rel, std::memory_order_acquire))
                {
                  readLeft(grandParent, child, childMark, opMark, parentDirection);
                  break;
                }
              }
              // }

              parent = grandParent;
              readLeft(parent, child, childMark, opMark, parentDirection);
              break;
            }
            goto FINISH;
          }
        }
      }
      else
      {
        // --- traverse active nodes ---
        if (childMark != LEAF_MARK)
        {
          if (parent->priority == P(0) || parent == dummyNode)
          {
            if (parent->priority != P(0))
            {
              parent->priority = P(0);
            }
            goto FINISH;
          }
          grandParent = parent;
          parent = child;
          readLeft(parent, child, childMark, opMark, parentDirection);
          continue;
        }
        else
        {
          Node *currentNext = child->next.load(std::memory_order_acquire);
          Node *childNext = address(currentNext);
          if (getMark(currentNext))
          {
            if (childNext->inserting.load(std::memory_order_acquire) && childNext->parent.load(std::memory_order_acquire) == parent)
            {
              tryHelpInsert(childNext);
            }
            else if (parent->left.load(std::memory_order_acquire) == mark(child, LEAF_MARK))
            {
              if (childNext->priority != P(0))
              {
                childNext->priority = 0;
              }
              goto FINISH;
            }
            readLeft(parent, child, childMark, opMark, parentDirection);
            continue;
          }
        }
      }

    FINISH:
      break;
    }
  }

public:
  ConcurrentPriorityQueue() : previousDummy(), previousHead()
  {
    previousHead.set(nullptr);
    previousDummy.set(nullptr);

    Node *headNode = allocateNode();
    Node *rootNode = allocateNode();
    Node *dummyNode = allocateNode();

    // 2) set up dummyNode exactly as in C
    dummyNode->priority = 0;
    dummyNode->value = T(0);
    dummyNode->left.store(headNode, std::memory_order_relaxed);
    dummyNode->right.store(mark(dummyNode, LEAF_MARK), std::memory_order_relaxed);
    dummyNode->parent.store(rootNode, std::memory_order_relaxed);
    dummyNode->next.store(nullptr, std::memory_order_relaxed);

    // 3) initialize headNode
    headNode->left.store(nullptr, std::memory_order_relaxed);
    headNode->right.store(nullptr, std::memory_order_relaxed);
    headNode->next.store(dummyNode, std::memory_order_relaxed);
    headNode->priority = 0;

    // 4) initialize rootNode
    rootNode->left.store(dummyNode, std::memory_order_relaxed);
    rootNode->right.store(nullptr, std::memory_order_relaxed);
    rootNode->parent.store(nullptr, std::memory_order_relaxed);
    rootNode->priority = 1;

    // 5) publish into our atomics
    head.store(headNode, std::memory_order_relaxed);
    root.store(rootNode, std::memory_order_relaxed);
  }

  bool enqueue(const T &value, P priority)
  {
    assert(priority != 0);

    HazardPointer<8>::Record *record = hazardAllocator.acquire();

    Node *newNode = allocateNode(value, priority);
    newNode->right.store(mark(newNode, LEAF_MARK), std::memory_order_relaxed);

    while (true)
    {
      InsertSeekRecordInfo ins = insertSearch(priority, record);

      if (ins.duplicate == DUPLICATE_DIRECTION)
      {
        // printf("duplicate\n");
        delete newNode;
        hazardAllocator.release(record);

        return false;
      }

      Node *leaf = ins.child;
      if (!leaf)
      {
        continue;
      }

      // printf("insert point %i %p %p\n", ins.child.load()->priority, ins.child.load(), head.load());
      //  grab all CAS pointers and direction
      Node *cas1 = ins.cast1;
      Node *cas2 = ins.cast2;
      Node *nextLeaf = ins.next;

      unsigned char parentDir = ins.parentDirection;

      newNode->left.store(mark(leaf, LEAF_MARK), std::memory_order_relaxed);
      newNode->parentDirection.store(parentDir, std::memory_order_relaxed);
      newNode->parent.store(cas1, std::memory_order_relaxed);
      newNode->next.store(nextLeaf, std::memory_order_relaxed);
      newNode->inserting.store(true, std::memory_order_release);

      if (leaf->next.load(std::memory_order_acquire) == nextLeaf)
      {
        if (leaf->next.compare_exchange_strong(nextLeaf, newNode, std::memory_order_acq_rel, std::memory_order_acquire))
        {
          if (newNode->inserting.load(std::memory_order_acquire))
          {
            if (parentDir == RIGHT_DIRECTION)
            {
              if (!cas1->right.compare_exchange_strong(cas2, newNode, std::memory_order_acq_rel, std::memory_order_acquire))
              {
                // os::print("inserting %lu right at %p, current = %p, cmp = %p\n", newNode->priority, cas1, cas1->right.load(), cas2);
                /*
                os::print(
                    "cmpxchg failure: current = %p (%lu), expected = %p (%lu)\n",
                    cas1->right.load(),
                    reinterpret_cast<uintptr_t>(cas1->right.load()),
                    cas2,
                    reinterpret_cast<uintptr_t>(cas2));
                    */
                // continue;
                // assert(false);
              }
            }
            if (parentDir == LEFT_DIRECTION)
            {
              if (!cas1->left.compare_exchange_strong(cas2, newNode, std::memory_order_acq_rel, std::memory_order_acquire))
              {
                // os::print("inserting %lu left at %p, current = %p, cmp = %p\n", newNode->priority, cas1, cas1->left.load(), cas2);
                /*
                os::print(
                    "cmpxchg failure: current = %p (%lu), expected = %p (%lu)\n",
                    cas1->left.load(),
                    reinterpret_cast<uintptr_t>(cas1->left.load()),
                    cas2,
                    reinterpret_cast<uintptr_t>(cas2));
                */
                //                continue;

                // assert(false);
              }
            }

            if (newNode->inserting.load())
            {
              newNode->inserting.store(false, std::memory_order_release);
            }
          }

          hazardAllocator.release(record);
          return true;
        }
      }
    }
  }

  bool tryDequeue(T &outValue, int debug = 0)
  {
    Node *h = head.load(std::memory_order_acquire);
    Node *leafNode = h->next.load(); // address(h->next.load(std::memory_order_acquire));
    Node *headItemNode = leafNode;

    Node *ph = nullptr;

    previousHead.get(ph);

    if (ph == leafNode)
    {
      previousDummy.get(leafNode);
    }
    else
    {
      previousHead.set(headItemNode);
    }
    // int k = 0;
    while (true)
    {

      Node *currentNext = leafNode->next.load(std::memory_order_acquire);
      Node *nextLeaf = address(currentNext);

      if (!nextLeaf)
      {
        previousDummy.set(leafNode);
        return false;
      }

      if (getMark(currentNext))
      {
        // skip marked
        leafNode = nextLeaf;
        /*
        if (k++ > 2000)
        {
          os::print("Thread %i iter mark %p %u\n", debug, leafNode, getMark(currentNext));
        }
        */
        continue;
      }

      std::atomic<uintptr_t> *p = (std::atomic<uintptr_t> *)(&leafNode->next);

      auto oldTagged = reinterpret_cast<uintptr_t>(p->fetch_or(1LL, std::memory_order_acq_rel));
      Node *xorNode = address(reinterpret_cast<Node *>(oldTagged));

      if (!getMark(xorNode))
      {
        outValue = xorNode->value;
        previousDummy.set(xorNode);

        /*
                const static size_t physicalDeleteRate = 55;

                if (randomGen() >= physicalDeleteRate)
                {
                  return true;
                }
        */
        if (h->next.load() == headItemNode)
        {
          if (h->next.compare_exchange_strong(headItemNode, xorNode, std::memory_order_acq_rel, std::memory_order_acquire))
          {
            // printf("entering here p = %i\n", xorNode->priority);
            previousHead.set(xorNode);

            if (xorNode->priority != P(0))
            {
              xorNode->priority = P(0);
            }

            physicalDelete(xorNode);

            Node *cur = headItemNode;
            while (cur != xorNode)
            {
              Node *tmp = address(cur->next.load(std::memory_order_acquire));
#ifdef ENABLE_GC
              delete cur;
#endif
              cur = tmp;
            }
          }
        }
        return true;
      }
      /*
        if (k++ > 2000)
        {
          os::print("Thread %i iter nothing\n", debug);
        }
          */
      leafNode = address(xorNode);
    }
  }

  bool tryPeek(P &outPriority) const
  {
    Node *h = head.load(std::memory_order_acquire);
    Node *leaf = address(h->next.load(std::memory_order_acquire));

    while (true)
    {
      uintptr_t rawNext = reinterpret_cast<uintptr_t>(leaf->next.load(std::memory_order_acquire));
      Node *nextLeaf = address(reinterpret_cast<Node *>(rawNext));

      if (!nextLeaf)
      {
        return false;
      }

      if (getMark(reinterpret_cast<Node *>(rawNext)) != NOT_MARKED)
      {
        leaf = nextLeaf;
        continue;
      }

      outPriority = nextLeaf->priority;
      return true;
    }
  }
};

template <typename T, typename P> thread_local uint32_t ConcurrentPriorityQueue<T, P>::seed = 0;

} // namespace lib