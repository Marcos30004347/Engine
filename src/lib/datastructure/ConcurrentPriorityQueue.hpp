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
  memory::allocator::SystemAllocator<Node> allocator;

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
    Node *node = allocator.allocate(1);
    return new (node) Node(v, p);
  }

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

  inline void readLeft(
      volatile Node *&parent_node,
      volatile Node *&child_node,
      volatile size_t &child_mark,
      size_t &operation_mark,
      unsigned char &parent_direction,
      HPRecord *record,
      size_t index,
      size_t index2)
  {
    Node *parent, *parent_addr;
    LOAD_ADDR(parent_addr, parent_node->next.load(), parent, record, index2);

    operation_mark = getMark(parent);

    Node *raw;
    LOAD_ADDR(child_node, parent_node->left.load(), raw, record, index);

    child_mark = getMark(raw);
    parent_direction = LEFT_DIRECTION;
  }

  inline void readRight(
      volatile Node *&parent_node,
      volatile Node *&child_node,
      volatile size_t &child_mark,
      size_t &operation_mark,
      unsigned char &parent_direction,
      HPRecord *record,
      size_t index,
      size_t index2)
  {
    Node *parent, *parent_addr;
    LOAD_ADDR(parent_addr, parent_node->next.load(), parent, record, index2);

    operation_mark = getMark(parent);
    Node *raw;
    LOAD_ADDR(child_node, parent_node->right.load(), raw, record, index);
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

  InsertSeekRecordInfo insertSearch(P priority, HPRecord *record)
  {
    // dump += "inserting " + std::to_string(priority) + "\n";
    Node *parent_node_left, *parent_original, *grand_parent_node_next, *grand_parent_node_left_addr, *grand_parent_node_left, *grand_parent_node_next_addr, *child_node_next_addr,
        *child_node_next;

    volatile Node *grand_parent_node = nullptr;

    volatile Node *parent_node;
    LOAD(parent_node, root.load(), record, 1);

    volatile Node *child_node; // = address(parent_node->left.load());
    LOAD_ADDR(child_node, parent_node->left.load(), parent_node_left, record, 2);
    size_t operation_mark = getMark(parent_node_left);

    volatile size_t child_mark = 0;
    unsigned char parent_direction;

    Node *marked_node = nullptr;

    while (true)
    {
      // dump += "current " + std::to_string(child_node->priority) + "\n";

      if (operation_mark == DELETE_MARK)
      {
        readRight(parent_node, child_node, child_mark, operation_mark, parent_direction, record, 2, 3);
        marked_node = const_cast<Node *>(parent_node);

        while (true)
        {
          if (operation_mark == DELETE_MARK)
          {
            if (child_mark != LEAF_MARK)
            {
              record->assign(child_node, 1);
              parent_node = child_node;

              readRight(parent_node, child_node, child_mark, operation_mark, parent_direction, record, 2, 3);
              continue;
            }
            else
            {
              LOAD_ADDR(parent_node, child_node->next.load(), parent_original, record, 1);
              readRight(parent_node, child_node, child_mark, operation_mark, parent_direction, record, 2, 3);
              break;
            }
          }
          else
          {
            const static uint32_t insertCleanRate = 50;

            // occasional cleanup
            if (randomGen() < insertCleanRate)
            {
              LOAD_ADDR(grand_parent_node_next_addr, grand_parent_node->next.load(), grand_parent_node_next, record, 4);
              LOAD_ADDR(grand_parent_node_left_addr, grand_parent_node->left.load(), grand_parent_node_left, record, 5);

              if (!getMark(grand_parent_node_next) && grand_parent_node_left == marked_node)
              {
                grand_parent_node->left.compare_exchange_strong(marked_node, const_cast<Node *>(parent_node));
              }
            }

            // TRAVERSE()
            if (priority <= parent_node->priority)
              readLeft(parent_node, child_node, child_mark, operation_mark, parent_direction, record, 2, 3);
            else
              readRight(parent_node, child_node, child_mark, operation_mark, parent_direction, record, 2, 3);
            break;
          }
        }
        continue;
      }

      if (child_mark != LEAF_MARK)
      {
        record->assign(parent_node, 9);
        record->assign(child_node, 1);

        grand_parent_node = parent_node;
        parent_node = child_node;

        // dump += "traversal 1: " + std::to_string(priority) + " <= " + std::to_string(parent_node->priority) + "\n";

        if (priority <= parent_node->priority)
          readLeft(parent_node, child_node, child_mark, operation_mark, parent_direction, record, 2, 3);
        else
          readRight(parent_node, child_node, child_mark, operation_mark, parent_direction, record, 2, 3);
      }
      else
      {

        Node *current_next; // = child_node->next.load();
        Node *child_next;   // = address(current_next);

        LOAD_ADDR(child_next, child_node->next.load(), current_next, record, 6);

        if (getMark(current_next))
        {
          // printf("got mark\n");
          parent_node = child_next;
          // dump += "traversal 2 right: " + std::to_string(priority) + " <= " + std::to_string(parent_node->priority) + "\n";
          readRight(parent_node, child_node, child_mark, operation_mark, parent_direction, record, 2, 3);
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

        LOAD_ADDR(parent_left_addr, parent_node->left.load(), parent_left, record, 7);
        LOAD_ADDR(parent_right_addr, parent_node->right.load(), parent_right, record, 8);

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
          readLeft(parent_node, child_node, child_mark, operation_mark, parent_direction, record, 2, 3);
        else
          readRight(parent_node, child_node, child_mark, operation_mark, parent_direction, record, 2, 3);
      }
    }
  }

  void physicalDelete(Node *dummyNode, HPRecord *record)
  {
    volatile Node *grandParent = nullptr;
    volatile Node *parent; // = root.load(std::memory_order_acquire);
    volatile Node *child;  // = address(parent->left.load(std::memory_order_acquire));
    Node *child_ptr;

    LOAD(parent, root.load(), record, 6);
    LOAD_ADDR(child, parent->left.load(), child_ptr, record, 7);

    unsigned char parentDirection;
    size_t opMark = 0;
    volatile size_t childMark = 0;
    Node *marked = nullptr;

    while (true)
    {
      if (opMark == DELETE_MARK)
      {
        readRight(parent, child, childMark, opMark, parentDirection, record, 7, 8);
        marked = const_cast<Node *>(parent);
        record->assign(marked, 12);

        while (true)
        {
          if (opMark == DELETE_MARK)
          {
            if (childMark != LEAF_MARK)
            {
              record->assign(child, 6);
              parent = child;

              readRight(parent, child, childMark, opMark, parentDirection, record, 7, 8);
              continue;
            }
            else
            {
              Node *childNext, *childNextPtr;
              LOAD_ADDR(childNext, child->next.load(std::memory_order_acquire), childNextPtr, record, 9);
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

              readRight(parent, child, childMark, opMark, parentDirection, record, 7, 8);
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

              LOAD_ADDR(grandParentNext, grandParent->next.load(std::memory_order_acquire), grandParentNextPtr, record, 9);

              if (!getMark(grandParentNextPtr))
              {
                if (grandParent->left.load(std::memory_order_acquire) == marked)
                {
                  if (grandParent->left.compare_exchange_strong(marked, const_cast<Node *>(parent), std::memory_order_acq_rel, std::memory_order_acquire))
                  {
                    readLeft(grandParent, child, childMark, opMark, parentDirection, record, 7, 8);
                    break;
                  }
                }
              }

              record->assign(grandParent, 6);
              parent = grandParent;
              readLeft(parent, child, childMark, opMark, parentDirection, record, 7, 8);
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

          record->assign(parent, 7);
          record->assign(child, 6);

          grandParent = parent;
          parent = child;

          readLeft(parent, child, childMark, opMark, parentDirection, record, 7, 8);
          continue;
        }
        else
        {
          Node *currentNext; // = child->next.load(std::memory_order_acquire);
          Node *childNext;   // = address(currentNext);

          LOAD_ADDR(childNext, child->next.load(std::memory_order_acquire), currentNext, record, 9);

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
            readLeft(parent, child, childMark, opMark, parentDirection, record, 7, 8);
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
        // delete newNode;
        // hazardAllocator.release(record);
        // os::print("%s\n---------\n", dump.c_str());
        record->retire<Node, memory::allocator::SystemAllocator<Node>>(allocator, 0);

        record->unassign(0);
        record->unassign(1);
        record->unassign(2);
        record->unassign(3);
        record->unassign(4);
        record->unassign(5);
        record->unassign(6);
        record->unassign(7);
        record->unassign(8);
        record->unassign(9);

        hazardAllocator.release(record);
        return false;
      }
      // 2 record
      Node *leaf = ins.child;
      if (!leaf)
      {
        continue;
      }

      // printf("insert point %i %p %p\n", ins.child.load()->priority, ins.child.load(), head.load());
      //  grab all CAS pointers and direction

      Node *cas1 = ins.cast1;    // 1 record
      Node *cas2 = ins.cast2;    // marked 1 record
      Node *nextLeaf = ins.next; // 6 record

      unsigned char parentDir = ins.parentDirection;

      newNode->left.store(mark(leaf, LEAF_MARK), std::memory_order_relaxed);
      newNode->parentDirection.store(parentDir, std::memory_order_relaxed);
      newNode->parent.store(cas1, std::memory_order_relaxed);
      newNode->next.store(nextLeaf, std::memory_order_relaxed);
      newNode->inserting.store(1, std::memory_order_release);
      unsigned char expected = 1;

      Node *leaf_next_addr, *leaf_next;
      LOAD_ADDR(leaf_next_addr, leaf->next.load(), leaf_next, record, 9);

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

              record->unassign(0);
              record->unassign(1);
              record->unassign(2);
              record->unassign(3);
              record->unassign(4);
              record->unassign(5);
              record->unassign(6);
              record->unassign(7);
              record->unassign(8);
              record->unassign(9);
              hazardAllocator.release(record);

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
              hazardAllocator.release(record);
              return true;
            }
          }
        }
      }
    }
  }

  bool tryDequeue(T &outValue)
  {
    HPRecord *record = hazardAllocator.acquire();

    outValue = -1;
    Node *h; // = head.load(std::memory_order_acquire);
    LOAD(h, head.load(), record, 0);

    Node *leafNode, *leafAddr; // = h->next.load(); // address(h->next.load(std::memory_order_acquire));
    LOAD_ADDR(leafAddr, h->next.load(), leafNode, record, 1);

    Node *headItemNode;
    LOAD(headItemNode, leafNode, record, 4);

    Node *ph = nullptr;

    // previousHead.get(ph);

    // if (ph == leafNode)
    // {
    //   previousDummy.get(leafNode);
    // }
    // else
    // {
    //   previousHead.set(headItemNode);
    // }

    while (true)
    {
      Node *currentNext, *nextLeaf;
      LOAD_ADDR(nextLeaf, leafNode->next.load(std::memory_order_acquire), currentNext, record, 2);

      if (!nextLeaf)
      {
        // previousDummy.set(leafNode);
        hazardAllocator.release(record);

        return false;
      }

      if (getMark(currentNext))
      {
        record->assign(nextLeaf, 1);
        leafNode = nextLeaf;
        continue;
      }

      // std::atomic<uintptr_t> *p = (std::atomic<uintptr_t> *)(&leafNode->next);
      // Node* leafNext, *leadNextAddr;
      // LOAD_ADDR(leadNextAddr, leafNode->next.load(std::memory_order_acquire), leafNext, record, 3);

      Node *xorNode;
      Node *curr;
      do
      {
        curr = address(leafNode->next.load());
        record->assign(curr, 3);

        std::atomic<uintptr_t> &raw = reinterpret_cast<std::atomic<uintptr_t> &>(leafNode->next);
        uintptr_t oldTagged = raw.fetch_or(1LL, std::memory_order_acquire);
        xorNode = ((Node *)(oldTagged));
        record->assign(xorNode, 3);
      } while (address(leafNode->next.load()) != curr);

      if (!getMark(xorNode))
      {
        outValue = std::move(xorNode->value);
        previousDummy.set(xorNode);

        const static size_t physicalDeleteRate = 5;

        if (false && randomGen() >= physicalDeleteRate)
        {
          hazardAllocator.release(record);
          return true;
        }

        if (h->next.load() == headItemNode)
        {
          if (h->next.compare_exchange_strong(headItemNode, xorNode))
          {
            // printf("entering here p = %i\n", xorNode->priority);
            // previousHead.set(xorNode);

            if (xorNode->priority != P(0))
            {
              xorNode->priority = P(0);
            }

            physicalDelete(xorNode, record);

            // Node *cur = headItemNode;
            // LOAD_ADDR(tmp, cur->next.load(std::memory_order_acquire), tmp_addr, record, 3);

            while (headItemNode != xorNode)
            {
              Node *tmp, *tmp_addr; //= address(cur->next.load(std::memory_order_acquire));

              record->assign(headItemNode, 5);

              LOAD_ADDR(tmp_addr, headItemNode->next.load(std::memory_order_acquire), tmp, record, 4);

              //headItemNode->next.store(nullptr);

              record->retire<Node, memory::allocator::SystemAllocator<Node>>(allocator, 5);

              headItemNode = tmp_addr;
            }
          }
        }

        hazardAllocator.release(record);
        return true;
      }
      Node *tmp;
      LOAD_ADDR(leafNode, xorNode, tmp, record, 1);
      // leafNode = address(xorNode);
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