#pragma once

#include "datastructure/ConcurrentEpochGarbageCollector.hpp"
#include <atomic>
#include <unordered_set>
namespace lib
{

template <typename T, typename P = size_t> class ConcurrentPriorityQueue
{
  static thread_local uint32_t seed;

  struct Node
  {
    // uint64_t freedBy;
    // uint64_t freedGarbageTimestamp;
    // uint64_t freedThreadTimestamp;
    std::atomic<Node *> parent;
    std::atomic<Node *> left;
    std::atomic<Node *> next;
    std::atomic<Node *> right;
    T value;
    P priority;
    std::atomic<unsigned char> inserting;
    std::atomic<unsigned char> parentDirection;
    // TODO: add padding

    Node(T value, P priority) : parent(nullptr), left(nullptr), next(nullptr), right(nullptr), inserting(false), parentDirection(false), value(value), priority(priority)
    {
      // this->freedBy = UINT64_MAX;
      // this->freedGarbageTimestamp = UINT64_MAX;
      // this->freedThreadTimestamp = UINT64_MAX;
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

  // ThreadLocalStorage<Node *> previousDummy;
  // ThreadLocalStorage<Node *> previousHead;

  // using HazardPointerManager = HazardPointer<16, Node *, memory::allocator::SystemAllocator<Node>>;
  // using HazardPointerRecord = typename HazardPointerManager::Record;

  // HazardPointerManager hazardAllocator;

  ConcurrentEpochGarbageCollector<Node> garbageCollector;

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

#define LOAD_ADDR(c, b, a)                                                                                                                                                         \
  do                                                                                                                                                                               \
  {                                                                                                                                                                                \
    a = b;                                                                                                                                                                         \
    c = address(a);                                                                                                                                                                \
  } while (b != a);

#define LOAD(a, b)                                                                                                                                                                 \
  do                                                                                                                                                                               \
  {                                                                                                                                                                                \
    a = b;                                                                                                                                                                         \
  } while (b != a);

  inline void readLeft(volatile Node *&parent_node, volatile Node *&child_node, volatile size_t &child_mark, size_t &operation_mark, unsigned char &parent_direction)
  {
    Node *parent, *parent_addr;

    // if (parent_node->freedBy != UINT64_MAX || parent_node->freedGarbageTimestamp != UINT64_MAX || parent_node->freedThreadTimestamp != UINT64_MAX)
    // {
    //   ThreadRecord trecord = {};

    //   assert(garbageCollector.activeThreads.get(os::Thread::getCurrentThreadId(), trecord));

    //   os::print(
    //       "Thread %u at %u trying to access %p, freed by %u at threadTS = %u, garbageTs = %u\n",
    //       trecord.threadId,
    //       trecord.timestamp,
    //       parent_node,
    //       parent_node->freedBy,
    //       parent_node->freedThreadTimestamp,
    //       parent_node->freedGarbageTimestamp);
    //   printTree();
    //   assert(false);
    // }

    LOAD_ADDR(parent_addr, parent_node->next.load(), parent);

    operation_mark = getMark(parent);

    Node *raw;
    LOAD_ADDR(child_node, parent_node->left.load(), raw);

    child_mark = getMark(raw);
    parent_direction = LEFT_DIRECTION;
  }

  inline void readRight(volatile Node *&parent_node, volatile Node *&child_node, volatile size_t &child_mark, size_t &operation_mark, unsigned char &parent_direction)
  {
    // if (parent_node->freedBy != UINT64_MAX || parent_node->freedGarbageTimestamp != UINT64_MAX || parent_node->freedThreadTimestamp != UINT64_MAX)
    // {
    //   ThreadRecord trecord = {};

    //   assert(garbageCollector.activeThreads.get(os::Thread::getCurrentThreadId(), trecord));

    //   os::print(
    //       "Thread %u at %u trying to access %p, freed by %u at threadTS = %u, garbageTs = %u\n",
    //       trecord.threadId,
    //       trecord.timestamp,
    //       parent_node,
    //       parent_node->freedBy,
    //       parent_node->freedThreadTimestamp,
    //       parent_node->freedGarbageTimestamp);
    //   printTree();
    //   assert(false);
    // }

    Node *parent, *parent_addr;
    LOAD_ADDR(parent_addr, parent_node->next.load(), parent);

    operation_mark = getMark(parent);
    Node *raw;
    LOAD_ADDR(child_node, parent_node->right.load(), raw);
    child_mark = getMark(raw);
    parent_direction = RIGHT_DIRECTION;
  }

  uint32_t xorshift32()
  {
    if (seed == 0)
    {
      seed = static_cast<uint32_t>(os::Thread::getCurrentThreadId());
    }
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
  }

  uint32_t randomGen(size_t randomScale = 100)
  {
    return xorshift32() % randomScale;
  }
  /*
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
  */
  // thread_local static std::string dump;

  InsertSeekRecordInfo insertSearch(P priority)
  {
    // dump += "inserting " + std::to_string(priority) + "\n";
    Node *parent_node_left, *parent_original, *grand_parent_node_next, *grand_parent_node_left_addr, *grand_parent_node_left, *grand_parent_node_next_addr, *child_node_next_addr,
        *child_node_next;

    volatile Node *grand_parent_node = nullptr;

    volatile Node *parent_node;
    LOAD(parent_node, root.load());

    volatile Node *child_node; // = address(parent_node->left.load());
    LOAD_ADDR(child_node, parent_node->left.load(), parent_node_left);
    size_t operation_mark = getMark(parent_node_left);

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
              LOAD_ADDR(parent_node, child_node->next.load(), parent_original);
              readRight(parent_node, child_node, child_mark, operation_mark, parent_direction);
              break;
            }
          }
          else
          {
            const static uint32_t insertCleanRate = 50;

            // occasional cleanup
            if (randomGen(100) < insertCleanRate)
            {
              LOAD_ADDR(grand_parent_node_next_addr, grand_parent_node->next.load(), grand_parent_node_next);
              LOAD_ADDR(grand_parent_node_left_addr, grand_parent_node->left.load(), grand_parent_node_left);

              if (!getMark(grand_parent_node_next) && grand_parent_node_left == marked_node)
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

        Node *current_next; // = child_node->next.load();
        Node *child_next;   // = address(current_next);

        LOAD_ADDR(child_next, child_node->next.load(), current_next);

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

        Node *parent_left, *parent_left_addr;
        Node *parent_right, *parent_right_addr;

        LOAD_ADDR(parent_left_addr, parent_node->left.load(), parent_left);
        LOAD_ADDR(parent_right_addr, parent_node->right.load(), parent_right);

        bool is_correct_leaf = (parent_direction == LEFT_DIRECTION && parent_left == mark(child_node, LEAF_MARK)) ||
                               (parent_direction == RIGHT_DIRECTION && parent_right == mark(child_node, LEAF_MARK));

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

  void physicalDelete(Node *root, Node *dummyNode)
  {
    volatile Node *grandParent = nullptr;
    volatile Node *parent; // = root.load(std::memory_order_acquire);
    volatile Node *child;  // = address(parent->left.load(std::memory_order_acquire));
    Node *child_ptr;

    LOAD(parent, root);
    LOAD_ADDR(child, parent->left.load(), child_ptr);

    unsigned char parentDirection;
    size_t opMark = getMark(child_ptr);

    volatile size_t childMark = 0;
    Node *marked = nullptr;

    while (true)
    {
      if (opMark == DELETE_MARK)
      {
        readRight(parent, child, childMark, opMark, parentDirection);
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
              Node *childNext, *childNextPtr;
              LOAD_ADDR(childNext, child->next.load(std::memory_order_acquire), childNextPtr);
              Node *parentRightPtr, *parentRight;

              // LOAD_ADDR(parentRight, parent->right.load(std::memory_order_acquire), parentRightPtr, record, 10);

              if (childNext->inserting.load(std::memory_order_acquire) && childNext->parent.load(std::memory_order_acquire) == parent)
              {
                while (childNext->inserting.load(std::memory_order_acquire) && childNext->parent.load(std::memory_order_acquire) == parent)
                {
                }
              }
              else if (parent->right.load(std::memory_order_acquire) == mark(child, LEAF_MARK))
              {
                if (grandParent && grandParent->priority != P(0))
                {
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
            if (grandParent)
            {
              Node *grandParentNextPtr; // = child->next.load(std::memory_order_acquire);
              Node *grandParentNext;    // = address(currentNext);

              LOAD_ADDR(grandParentNext, grandParent->next.load(std::memory_order_acquire), grandParentNextPtr);

              if (!getMark(grandParentNextPtr))
              {
                if (grandParent->left.load(std::memory_order_acquire) == marked)
                {
                  if (grandParent->left.compare_exchange_strong(marked, const_cast<Node *>(parent), std::memory_order_acq_rel, std::memory_order_acquire))
                  {

                    readLeft(grandParent, child, childMark, opMark, parentDirection);
                    break;
                  }
                }
              }

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
          Node *currentNext; // = child->next.load(std::memory_order_acquire);
          Node *childNext;   // = address(currentNext);

          LOAD_ADDR(childNext, child->next.load(std::memory_order_acquire), currentNext);

          if (getMark(currentNext))
          {
            if (childNext->inserting.load(std::memory_order_acquire) && childNext->parent.load(std::memory_order_acquire) == parent)
            {

              while (childNext->inserting.load(std::memory_order_acquire) && childNext->parent.load(std::memory_order_acquire) == parent)
              {
              }
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

  void printSubtree(Node *root, std::string &out)
  {
    if (!root)
      return;

    std::unordered_set<Node *> visited;

    struct StackItem
    {
      Node *node;
      int depth;
      std::string prefix;
    };

    std::stack<StackItem> stack;
    stack.push({root, 0, ""});

    while (!stack.empty())
    {
      auto [node, depth, prefix] = stack.top();
      stack.pop();

      if (!node || visited.count(node) > 0)
        continue;
      visited.insert(node);

      // Print current node
      for (int i = 0; i < depth; ++i)
        out += "  ";
      const void *addressPtr = static_cast<const void *>(node);
      std::stringstream ss;
      ss << addressPtr;
      std::string name = ss.str();
      out += prefix + "Node(" + name + ", priority=" + std::to_string(node->priority) + ", value = " + std::to_string(node->value) +
             ", freed at = " + std::to_string(node->freedGarbageTimestamp);

      if (node->inserting.load())
        out += ", inserting";

      out += ")\n";

      // Push children in reverse order to maintain L -> R -> N order
      Node *leftChild = address(node->left.load(std::memory_order_acquire));
      Node *rightChild = address(node->right.load(std::memory_order_acquire));
      Node *nextChild = address(node->next.load(std::memory_order_acquire));

      if (nextChild)
        stack.push({nextChild, depth + 1, "N"});
      if (rightChild)
        stack.push({rightChild, depth + 1, "R"});
      if (leftChild)
        stack.push({leftChild, depth + 1, "L"});
    }
  }

public:
  ConcurrentPriorityQueue() : garbageCollector()
  {
    // previousHead.set(nullptr);
    // previousDummy.set(nullptr);
    auto scope = garbageCollector.openEpochGuard();

    Node *headNode = garbageCollector.template allocate<T, P>(scope, 0, 0);
    Node *rootNode = garbageCollector.template allocate<T, P>(scope, 0, 1);
    Node *dummyNode = garbageCollector.template allocate<T, P>(scope, 0, 0);

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

  ~ConcurrentPriorityQueue()
  {
    auto scope = garbageCollector.openEpochGuard();

    while (head.load())
    {
      Node *tmp = head.load();
      head.store(address(tmp->next));
      scope.retire(tmp);
    }

    // root.store(garbageCollector.allocate());
  }

  bool enqueue(const T &value, P priority)
  {
    auto scope = garbageCollector.openEpochGuard();

    // dump = "enqueing " + std::to_string(priority) + "\n";
    assert(priority != 0);

    // HazardPointer<16>::Record *record = hazardAllocator.acquire();

    Node *newNode = garbageCollector.allocate(scope, value, priority);
    newNode->right.store(mark(newNode, LEAF_MARK), std::memory_order_relaxed);

    while (true)
    {
      InsertSeekRecordInfo ins = insertSearch(priority);

      if (ins.duplicate == DUPLICATE_DIRECTION)
      {
        return false;
      }

      Node *leaf = ins.child;
      if (!leaf)
      {
        continue;
      }

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

      Node *leaf_next_addr, *leaf_next;
      LOAD_ADDR(leaf_next_addr, leaf->next.load(), leaf_next);

      if (leaf_next == nextLeaf)
      {
        if (parentDir == RIGHT_DIRECTION)
        {
          if (leaf_next == nextLeaf)
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

              return true;
            }
          }
        }
        else if (parentDir == LEFT_DIRECTION)
        {
          if (leaf_next == nextLeaf)
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

  // static void nodeFreeOverload(Node *n, uint64_t tts, uint64_t gts)
  // {
  //   n->freedThreadTimestamp = tts;
  //   n->freedGarbageTimestamp = gts;
  //   n->freedBy = os::Thread::getCurrentThreadId();
  // }

  bool dequeue(T &out)
  {
    auto scope = garbageCollector.openEpochGuard();

    Node *hNode = head.load()->next;

    while (true)
    {
      if (hNode == nullptr)
      {
        return false;
      }

      if (getMark(hNode) == DELETE_MARK)
      {
        hNode = address(hNode->next);
        continue;
      }

      Node *xorNode = (Node *)reinterpret_cast<std::atomic<uintptr_t> &>(hNode->next).fetch_or(1LL, std::memory_order_acquire);

      if (!(getMark(xorNode) == DELETE_MARK))
      {
        out = xorNode->value;

        xorNode->priority = 0;

        // if (randomGen(100) < 50)
        // {
        //   return true;
        // }

        if (head.load()->next.compare_exchange_strong(hNode, xorNode))
        {
          physicalDelete(xorNode, hNode);

          Node *nextLeaf = hNode;
          while (nextLeaf != xorNode)
          {
            Node *curr = nextLeaf;
            nextLeaf = address(nextLeaf->next.load());
            scope.retire(curr);
          }
        }

        return true;
      }

      hNode = address(xorNode);
    }
  }

  // bool tryDequeue(T &outValue, P &priority)
  // {
  //   auto scope = garbageCollector.openEpochGuard();

  //   // outValue = -1;
  //   Node *h; // = head.load(std::memory_order_acquire);
  //   LOAD(h, head.load());

  //   Node *leafNode, *leafAddr; // = h->next.load(); // address(h->next.load(std::memory_order_acquire));
  //   LOAD_ADDR(leafAddr, h->next.load(), leafNode);

  //   Node *headItemNode;
  //   LOAD(headItemNode, leafNode);
  //   Node *ph = nullptr;

  //   Node *r = root.load();

  //   // previousHead.get(ph);

  //   // if (ph == leafNode)
  //   // {
  //   //   previousDummy.get(leafNode);
  //   // }
  //   // else
  //   // {
  //   //   previousHead.set(headItemNode);
  //   // }

  //   while (true)
  //   {

  //     Node *currentNext, *nextLeaf;
  //     LOAD_ADDR(nextLeaf, leafNode->next.load(std::memory_order_acquire), currentNext);

  //     if (!nextLeaf)
  //     {
  //       // previousDummy.set(leafNode);
  //       return false;
  //     }

  //     if (getMark(currentNext))
  //     {
  //       leafNode = nextLeaf;
  //       continue;
  //     }

  //     if (currentNext->priority == P(0))
  //     {
  //       leafNode = nextLeaf;
  //       continue;
  //     }

  //     // std::atomic<uintptr_t> *p = (std::atomic<uintptr_t> *)(&leafNode->next);
  //     // Node* leafNext, *leadNextAddr;
  //     // LOAD_ADDR(leadNextAddr, leafNode->next.load(std::memory_order_acquire), leafNext, record, 3);

  //     Node *curr = address(leafNode->next.load());
  //     std::atomic<uintptr_t> &raw = reinterpret_cast<std::atomic<uintptr_t> &>(leafNode->next);
  //     Node *xorNode = (Node *)raw.fetch_or(1LL, std::memory_order_acquire);

  //     if (!getMark(xorNode))
  //     {
  //       // os::print("Thread %u moving %u\n", i, xorNode->value);
  //       outValue = std::move(xorNode->value);
  //       priority = std::move(xorNode->priority);

  //       // os::print("Thread %u moved %u, %u\n", i, xorNode->value, outValue);

  //       // previousDummy.set(xorNode);

  //       // const static size_t physicalDeleteRate = 5;

  //       // if (randomGen() >= 50)
  //       // {
  //       //   // {
  //       //   //   hazardAllocator.release(record);
  //       //   //   garbageCollector.closeThreadContext();

  //       //   return true;
  //       // }

  //       if (head.load()->next.compare_exchange_strong(headItemNode, xorNode, std::memory_order_release, std::memory_order_acquire))
  //       {
  //         // previousHead.set(xorNode);

  //         if (xorNode->priority != P(0))
  //         {
  //           xorNode->priority = P(0);
  //         }

  //         physicalDelete(xorNode, r);

  //         nextLeaf = headItemNode;
  //         size_t at = 0;

  //         while (nextLeaf != xorNode)
  //         {
  //           // if (getMark(nextLeaf) == DELETE_MARK)
  //           // {
  //           // buff[at++] = nextLeaf;
  //           scope.retire(nextLeaf);

  //           // }

  //           nextLeaf = address(nextLeaf->next.load(std::memory_order_acquire));
  //         }
  //       }

  //       return true;
  //     }

  //     // Node *tmp;
  //     // LOAD_ADDR(leafNode, xorNode, tmp);
  //     leafNode = address(xorNode);
  //   }
  // }

  bool peek(P &outPriority) const
  {
    garbageCollector.openThreadContext();

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

  // void printTree(int thread = os::Thread::getCurrentThreadId())
  // {
  //   // garbageCollector.openThreadContext();

  //   std::string out = "";
  //   Node *r = root.load(std::memory_order_acquire);
  //   out += "Thread " + std::to_string(thread) + " Tree from root:\n";
  //   std::unordered_set<Node *> visited;

  //   printSubtree(r, out);

  //   os::print("%s\n", out.c_str());
  // }
};

template <typename T, typename P> thread_local uint32_t ConcurrentPriorityQueue<T, P>::seed = 0;
// template <typename T, typename P> thread_local std::string ConcurrentPriorityQueue<T, P>::dump = "";
} // namespace lib