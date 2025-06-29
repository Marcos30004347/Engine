#pragma once
#include "ThreadLocalStorage.hpp"
#include "lib/datastructure/HazardPointer.hpp"
#include <atomic>
#include <unordered_set>
namespace lib
{

#define ALIGNED(N) __attribute__((aligned(N)));

template <typename T, typename P = size_t> class ConcurrentPriorityQueue
{
  static thread_local uint32_t seed;

  struct Node
  {

    std::atomic<Node *> parent;
    std::atomic<Node *> left;
    std::atomic<Node *> next;
    std::atomic<Node *> right;
    T value;
    P priority;
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

  HazardPointer<16> hazardAllocator;
  using HPRecord = HazardPointer<16>::Record;

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

  static inline Node *mark(volatile Node *ptr, size_t mark)
  {
    return reinterpret_cast<Node *>((reinterpret_cast<uintptr_t>(ptr) & ~uintptr_t(0x3)) | mark);
  }

  Node *allocateNode(T v, P p)
  {
    // TODO: use aligned allocator
    return new Node(v, p);
  }

  inline void readLeft(volatile Node *&parent_node, volatile Node *&child_node, volatile size_t &child_mark, size_t &operation_mark, unsigned char &parent_direction)
  {
    operation_mark = getMark(parent_node->next.load());
    auto raw = parent_node->left.load();
    child_node = address(raw);
    child_mark = getMark(raw);
    parent_direction = LEFT_DIRECTION;
  }

  inline void readRight(volatile Node *&parent_node, volatile Node *&child_node, volatile size_t &child_mark, size_t &operation_mark, unsigned char &parent_direction)
  {
    operation_mark = getMark(parent_node->next.load());
    auto raw = parent_node->right.load();
    child_node = address(raw);
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
    unsigned char expected = 1;

    bool parentDir = newNode->parentDirection.load(std::memory_order_relaxed);

    Node *cas1 = newNode->parent.load(std::memory_order_relaxed);
    Node *cas2 = newNode->left.load(std::memory_order_relaxed);

    if (parentDir == LEFT_DIRECTION && newNode->inserting.load())
    {
      if (newNode->inserting.compare_exchange_strong(expected, 0))
      {
        cas1->left.compare_exchange_strong(cas2, newNode, std::memory_order_acq_rel, std::memory_order_acquire);
      }
    }
    else if (parentDir == RIGHT_DIRECTION && newNode->inserting.load())
    {
      if (newNode->inserting.compare_exchange_strong(expected, 0))
      {
        cas1->right.compare_exchange_strong(cas2, newNode, std::memory_order_acq_rel, std::memory_order_acquire);
      }
    }
  }

  // thread_local static std::string dump;
#define LOAD_ADDR(c, b, a, r, i)                                                                                                                                                   \
  do                                                                                                                                                                               \
  {                                                                                                                                                                                \
    a = b;                                                                                                                                                                         \
    c = address(a);                                                                                                                                                                \
    r->assign(c, i);                                                                                                                                                               \
  } while (b != a);

#define LOAD(a, b, r, i)                                                                                                                                                           \
  do                                                                                                                                                                               \
  {                                                                                                                                                                                \
    a = b;                                                                                                                                                                         \
    r->assign(a, i);                                                                                                                                                               \
  } while (b != a);

  InsertSeekRecordInfo insertSearch(P priority, HPRecord *record)
  {
    // dump += "inserting " + std::to_string(priority) + "\n";
    Node *tmp;

    volatile Node *grand_parent_node = nullptr;
    volatile Node *parent_node; //, record, 0);

    LOAD(parent_node, root.load(), record, 1);

    volatile Node *child_node = address(parent_node->left.load());
    LOAD_ADDR(child_node, parent_node->left.load(), tmp, record, 2);

    size_t operation_mark = getMark(parent_node->left.load());
    volatile size_t child_mark = 0;
    unsigned char parent_direction;
    Node *marked_node = nullptr;

    while (true)
    {
      // dump += "current " + std::to_string(child_node->priority) + "\n";

      if (operation_mark == DELETE_MARK)
      {
        readRight(parent_node, child_node, child_mark, operation_mark, parent_direction);
        marked_node = const_cast<Node *>(parent_node);

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
              if (!getMark(grand_parent_node->next.load()) && grand_parent_node->left.load() == marked_node)
              {
                grand_parent_node->left.compare_exchange_strong(marked_node, const_cast<Node *>(parent_node));
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
        grand_parent_node = parent_node;
        parent_node = child_node;

        // dump += "traversal 1: " + std::to_string(priority) + " <= " + std::to_string(parent_node->priority) + "\n";

        if (priority <= parent_node->priority)
          readLeft(parent_node, child_node, child_mark, operation_mark, parent_direction);
        else
          readRight(parent_node, child_node, child_mark, operation_mark, parent_direction);
      }
      else
      {

        Node *current_next = child_node->next.load();
        Node *child_next = address(current_next);

        if (getMark(current_next))
        {
          // printf("got mark\n");
          parent_node = child_next;
          // dump += "traversal 2 right: " + std::to_string(priority) + " <= " + std::to_string(parent_node->priority) + "\n";
          readRight(parent_node, child_node, child_mark, operation_mark, parent_direction);
          continue;
        }

        while (child_next && child_next->inserting.load())
        {
          /*
          dump += "inserting " + std::to_string(priority) + " will try help insert, child next = " + std::to_string(child_next->priority) + "\n";
          // printf("child next inserting\n");
          tryHelpInsert(child_next);

          parent_node = child_next;
          dump += "traversal 3: " + std::to_string(priority) + " <= " + std::to_string(parent_node->priority) + "\n";
          if (priority <= parent_node->priority)
            readLeft(parent_node, child_node, child_mark, operation_mark, parent_direction);
          else
            readRight(parent_node, child_node, child_mark, operation_mark, parent_direction);

          continue;
          */
        }

        if (child_next && child_next->priority == priority)
        {
          InsertSeekRecordInfo ins_seek;

          // printf("child next priority equal %p %lu %lu\n", child_next, child_next->priority, priority);
          ins_seek.duplicate = DUPLICATE_DIRECTION;
          return ins_seek;
        }

        bool is_correct_leaf = (parent_direction == LEFT_DIRECTION && parent_node->left.load() == mark(child_node, LEAF_MARK)) ||
                               (parent_direction == RIGHT_DIRECTION && parent_node->right.load() == mark(child_node, LEAF_MARK));

        if (is_correct_leaf)
        {
          // printf("correct leaf %i\n", parent_direction);
          InsertSeekRecordInfo ins_seek;
          ins_seek.duplicate = 0;
          ins_seek.child = const_cast<Node *>(child_node);
          ins_seek.cast1 = const_cast<Node *>(parent_node);
          ins_seek.cast2 = mark(child_node, LEAF_MARK);
          ins_seek.next = child_next;
          ins_seek.parentDirection = parent_direction;

          // std::string out = "";
          // Node *r = root.load(std::memory_order_acquire);
          // std::unordered_set<Node *> visited;
          // printSubtree(r, 0, out, visited, "Root");

          return ins_seek;
        }

        // dump += "traversal 4: " + std::to_string(priority) + " <= " + std::to_string(parent_node->priority) + "\n";
        //  else TRAVERSE()
        if (priority < parent_node->priority)
          readLeft(parent_node, child_node, child_mark, operation_mark, parent_direction);
        else
          readRight(parent_node, child_node, child_mark, operation_mark, parent_direction);
      }
    }
  }

  void physicalDelete(Node *dummyNode)
  {
    volatile Node *grandParent = nullptr;
    volatile Node *parent = root.load(std::memory_order_acquire);
    volatile Node *child = address(parent->left.load(std::memory_order_acquire));
    unsigned char parentDirection;
    size_t opMark = 0;
    volatile size_t childMark = 0;
    Node *marked = nullptr;

    while (true)
    {
      // --- traverse logically deleted nodes ---
      if (opMark == DELETE_MARK)
      {
        readRight(parent, child, childMark, opMark, /*out*/ parentDirection);
        marked = const_cast<Node *>(parent);

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
                while (childNext->inserting.load(std::memory_order_acquire) && childNext->parent.load(std::memory_order_acquire) == parent)
                {
                }
                // tryHelpInsert(childNext);
              }
              else if (parent->right.load(std::memory_order_acquire) == mark(child, LEAF_MARK))
              {
                // “confirm edge is complete”
                if (grandParent && grandParent->priority != P(0))
                {
                  // in C they zeroed key; here we zero priority
                  grandParent->value = T(0);
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
                if (grandParent->left.compare_exchange_strong(marked, const_cast<Node *>(parent), std::memory_order_acq_rel, std::memory_order_acquire))
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
              parent->value = T(0);
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

              while (childNext->inserting.load(std::memory_order_acquire) && childNext->parent.load(std::memory_order_acquire) == parent)
              {
              }
              // tryHelpInsert(childNext);
            }
            else if (parent->left.load(std::memory_order_acquire) == mark(child, LEAF_MARK))
            {
              if (childNext->priority != P(0))
              {
                childNext->priority = 0;
                childNext->value = 0;
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
  void printSubtree(Node *node, int depth, std::string &out, std::unordered_set<Node *> &visited, std::string prefix)
  {
    if (!node || visited.count(node) > 0)
      return;
    visited.insert(node);

    // if (node->priority > 0)
    {
      for (int i = 0; i < depth; ++i)
        out += "  ";

      out += prefix + "Node(priority=" + std::to_string(node->priority) + ", value = " + std::to_string(node->value);

      if (node->inserting.load())
        out += ", inserting";

      out += ")\n";
      depth += 1;
    }

    Node *leftChild = address(node->left.load(std::memory_order_acquire));
    Node *rightChild = address(node->right.load(std::memory_order_acquire));
    Node *nextChild = address(node->next.load(std::memory_order_acquire));
    printSubtree(leftChild, depth, out, visited, "L");
    printSubtree(rightChild, depth, out, visited, "R");
    printSubtree(nextChild, depth, out, visited, "N");
    // printSubtree(next, depth + 1, out, visited);
  }

public:
  ConcurrentPriorityQueue() : previousDummy(), previousHead()
  {
    previousHead.set(nullptr);
    previousDummy.set(nullptr);

    Node *headNode = allocateNode(0, 0);
    Node *rootNode = allocateNode(0, 1);
    Node *dummyNode = allocateNode(0, 0);

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
    HPRecord *record = hazardAllocator.acquire();

    // dump = "enqueing " + std::to_string(priority) + "\n";
    assert(priority != 0);

    // HazardPointer<16>::Record *record = hazardAllocator.acquire();

    Node *newNode = allocateNode(value, priority);
    newNode->right.store(mark(newNode, LEAF_MARK), std::memory_order_relaxed);

    record->assign(newNode, 0);

    while (true)
    {
      InsertSeekRecordInfo ins = insertSearch(priority, record);

      if (ins.duplicate == DUPLICATE_DIRECTION)
      {
        // printf("duplicate\n");
        delete newNode;
        // hazardAllocator.release(record);
        // os::print("%s\n---------\n", dump.c_str());
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
      newNode->inserting.store(1, std::memory_order_release);
      unsigned char expected = 1;

      if (leaf->next.load() == nextLeaf)
      {
        if (parentDir == RIGHT_DIRECTION)
        {
          if (leaf->next.load() == nextLeaf)
          {
            if (leaf->next.compare_exchange_strong(nextLeaf, newNode, std::memory_order_acq_rel, std::memory_order_acquire))
            {
              if (newNode->inserting.load(std::memory_order_acquire))
              {
                if (cas1->right.load() == cas2)
                {
                  cas1->right.compare_exchange_strong(cas2, newNode, std::memory_order_acq_rel, std::memory_order_acquire);
                }
                if (newNode->inserting.load())
                {
                  newNode->inserting.store(0);
                }
              }
              // os::print("%s\n---------\n", dump.c_str());

              return true;
            }
          }
        }
        else if (parentDir == LEFT_DIRECTION)
        {
          if (leaf->next.load() == nextLeaf)
          {
            if (leaf->next.compare_exchange_strong(nextLeaf, newNode))
            {
              if (newNode->inserting.load(std::memory_order_acquire))
              {
                if (cas1->left.load() == cas2)
                {
                  cas1->left.compare_exchange_strong(cas2, newNode);
                }
                if (newNode->inserting.load())
                {
                  newNode->inserting.store(0);
                }
              }
              // os::print("%s---------\n", dump.c_str());

              return true;
            }
          }
        }
      }
    }
  }

  bool tryDequeue(T &outValue)
  {
    outValue = -1;
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
        continue;
      }

      // std::atomic<uintptr_t> *p = (std::atomic<uintptr_t> *)(&leafNode->next);
      std::atomic<uintptr_t> &raw = reinterpret_cast<std::atomic<uintptr_t> &>(leafNode->next);

      auto oldTagged = raw.fetch_xor(1LL, std::memory_order_acquire);
      Node *xorNode = ((Node *)(oldTagged));

      if (!getMark(xorNode))
      {
        outValue = std::move(xorNode->value);
        previousDummy.set(xorNode);

        const static size_t physicalDeleteRate = 5;

        if (true || randomGen() >= physicalDeleteRate)
        {
          return true;
        }

        if (h->next.load() == headItemNode)
        {
          if (h->next.compare_exchange_strong(headItemNode, xorNode, std::memory_order_acq_rel, std::memory_order_acquire))
          {
            // printf("entering here p = %i\n", xorNode->priority);
            previousHead.set(xorNode);

            if (xorNode->priority != P(0))
            {
              xorNode->value = T(0);
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

  void printTree(int thread)
  {
    std::string out = "";
    Node *r = root.load(std::memory_order_acquire);
    out += "Thread " + std::to_string(thread) + " Tree from root:\n";
    std::unordered_set<Node *> visited;

    printSubtree(r, 0, out, visited, "R");

    os::print("%s\n", out.c_str());
  }
};

template <typename T, typename P> thread_local uint32_t ConcurrentPriorityQueue<T, P>::seed = 0;
// template <typename T, typename P> thread_local std::string ConcurrentPriorityQueue<T, P>::dump = "";
} // namespace lib