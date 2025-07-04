#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>

namespace lib
{
template <typename T, typename P = int> class ConcurrentPriorityQueue
{
public:
  struct Element
  {
    T value;
    P priority;

    bool operator<(const Element &other) const
    {
      return priority > other.priority;
    }
  };

  void push(const T &value, P priority)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.emplace(Element{value, priority});
    cond_var_.notify_one();
  }

  void push(T &&value, P priority)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.emplace(Element{std::move(value), priority});
    cond_var_.notify_one();
  }

  T wait_and_pop()
  {
    std::unique_lock<std::mutex> lock(mutex_);
    cond_var_.wait(
        lock,
        [&]
        {
          return !queue_.empty();
        });

    T val = std::move(queue_.top().value);
    queue_.pop();
    return val;
  }
  bool try_pop(T &val)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty())
      return false;

    val = std::move(queue_.top().value);
    queue_.pop();
    return true;
  }

  bool try_pop(T &val, P &priority)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty())
      return false;

    val = std::move(queue_.top().value);
    priority = queue_.top().priority;

    queue_.pop();
    return true;
  }

  bool empty() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
  }

  std::size_t size() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable cond_var_;
  std::priority_queue<Element> queue_;
};
} // namespace lib

/*
 *   File: priorityqueue-tslqueue.h
 *   Author: Adones Rukundo
 *	 <adones@chalmers.se, adones@must.ac.ug, adon.rkd@gmail.com>
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include "common.h"
#include "ssalloc.h"
#include "ssmem.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>

/******************x86 code atomics*****************************/
#define CAE(a, b, c) __atomic_compare_exchange(a, b, c, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define CAS(a, b, c) __sync_bool_compare_and_swap(a, b, c)
#define FAXOR(a, b) __sync_fetch_and_or(a, b);
/***************************************************************/

#define DS_ADD(s, k, t) insert(s, k, k)
#define DS_REMOVE(s) delete_min(s)
#define DS_SIZE(s) pq_size(s)
#define DS_NEW(n) create_set(n)
#define DS_LOCAL() init_local()

#define DS_TYPE tsl_set_t
#define DS_NODE node_t
#define DS_KEY skey_t

#define LEFT_DIRECTION 1
#define RIGHT_DIRECTION 2
#define DUPLICATE_DIRECTION 3
#define NOT_MARKED 0
#define DELETE_MARK 1
#define INSERT_MARK 2
#define LEAF_MARK 3

/*********MACROS**********/
#define READ_LEFT()                                                                                                                                                                \
  operation_mark = GETMARK(parent_node->next);                                                                                                                                     \
  parent_node_left = parent_node->left;                                                                                                                                            \
  child_node = ADDRESS(parent_node_left);                                                                                                                                          \
  child_mark = GETMARK(parent_node_left);                                                                                                                                          \
  parent_direction = LEFT_DIRECTION;

#define READ_RIGHT()                                                                                                                                                               \
  operation_mark = GETMARK(parent_node->next);                                                                                                                                     \
  parent_node_right = parent_node->right;                                                                                                                                          \
  child_node = ADDRESS(parent_node_right);                                                                                                                                         \
  child_mark = GETMARK(parent_node_right);                                                                                                                                         \
  parent_direction = RIGHT_DIRECTION;

#define TRAVERSE()                                                                                                                                                                 \
  if (key <= parent_node->key)                                                                                                                                                     \
  {                                                                                                                                                                                \
    READ_LEFT();                                                                                                                                                                   \
  }                                                                                                                                                                                \
  else                                                                                                                                                                             \
  {                                                                                                                                                                                \
    READ_RIGHT();                                                                                                                                                                  \
  }

typedef ALIGNED(CACHE_LINE_SIZE) struct generic_node
{
  void *parent;
  void *left;
  void *next;
  void *right;
  sval_t value;
  skey_t key;

  uint8_t inserting;
  uint8_t parent_direction;
  uint8_t padding[CACHE_LINE_SIZE - (sizeof(skey_t) + sizeof(sval_t) + (sizeof(void *) * 4) + (sizeof(uint8_t) * 2))];
} node_t;

typedef ALIGNED(CACHE_LINE_SIZE) struct tslqueue_set
{
  node_t *head;
  node_t *root;
  size_t num_threads;
  uint32_t del_scale;
  uint8_t padding[CACHE_LINE_SIZE - ((sizeof(node_t *) * 2) + sizeof(size_t) + sizeof(uint32_t))];
} tsl_set_t;

typedef struct next_cast
{
  void *next;
  void *right;
} next_t;

typedef ALIGNED(CACHE_LINE_SIZE) struct insert_seek_record_info
{
  node_t *child;
  node_t *next_node;
  node_t *cas1_node;
  node_t *cas2_node;
  uint8_t duplicate;
  uint8_t parent_direction;
  uint8_t padding[CACHE_LINE_SIZE - ((sizeof(node_t *) * 4) + (sizeof(uint8_t) * 2))];
} insert_seek_t;

/*Thread local variables*/
extern __thread ssmem_allocator_t *alloc;

/* Interfaces */

static inline node_t *ADDRESS(volatile node_t *ptr)
{
  return (node_t *)(((uint64_t)ptr) & 0xfffffffffffffffc);
}

static inline uint64_t GETMARK(volatile node_t *ptr)
{
  return ((uint64_t)ptr) & 3;
}

static inline uint64_t MARK_DELETE_NODE(volatile node_t *ptr)
{
  return (((uint64_t)ptr)) | DELETE_MARK;
}

static inline uint64_t MARK_INSERT_NODE(volatile node_t *ptr)
{
  return (((uint64_t)ptr)) | INSERT_MARK;
}

static inline uint64_t MARK_LEAF_NODE(volatile node_t *ptr)
{
  return (((uint64_t)ptr)) | LEAF_MARK;
}

tsl_set_t *create_set(size_t num_threads);
node_t *create_node();
void init_local();

uint8_t insert(tsl_set_t *set, skey_t key, sval_t value);
sval_t delete_min(tsl_set_t *set);
void physical_delete(tsl_set_t *set, volatile node_t *dummy_node);

insert_seek_t insert_search(tsl_set_t *set, skey_t key);
static inline void try_helping_insert(volatile node_t *item_node);

uint32_t pq_size(tsl_set_t *set);
static inline uint32_t random_gen(tsl_set_t *set);

__thread node_t *previous_dummy, *previous_head;

/*********** optimisation variables ***********************/
uint32_t physical_delete_rate = 1;
uint32_t insert_clean_rate = 50;

void init_local()
{
}
node_t *create_node()
{
  volatile node_t *new_node;
#if GC == 1
  new_node = (volatile node_t *)ssmem_alloc(alloc, sizeof(node_t));
#else
  new_node = (volatile node_t *)ssalloc(sizeof(node_t));
#endif
  if (new_node == NULL)
  {
    perror("malloc in bst create node");
    exit(1);
  }
  new_node->key = 0;
  new_node->left = NULL;
  new_node->right = NULL;
  new_node->parent = NULL;
  new_node->value = 0;
  new_node->next = NULL;

  return new_node;
}

tsl_set_t *create_set(size_t num_threads)
{
  volatile node_t *root, *head, *dummy_node;
  tsl_set_t *new_set;

#if GC == 1
  new_set = (tsl_set_t *)ssalloc_aligned(CACHE_LINE_SIZE, sizeof(tsl_set_t));
  head = (volatile node_t *)ssalloc_aligned(CACHE_LINE_SIZE, sizeof(node_t));
  root = (volatile node_t *)ssalloc_aligned(CACHE_LINE_SIZE, sizeof(node_t));
  dummy_node = (volatile node_t *)ssalloc_aligned(CACHE_LINE_SIZE, sizeof(node_t));
#else
  new_set = (tsl_set_t *)ssalloc(sizeof(tsl_set_t));
  head = (volatile node_t *)ssalloc(sizeof(node_t));
  root = (volatile node_t *)ssalloc(sizeof(node_t));
  dummy_node = (volatile node_t *)ssalloc(sizeof(node_t));
#endif
  if (root == NULL || head == NULL)
  {
    perror("malloc in bst create new root");
    exit(1);
  }

  dummy_node->key = 0;
  dummy_node->left = head;
  dummy_node->right = MARK_LEAF_NODE(dummy_node);
  dummy_node->parent = root;
  dummy_node->value = 0;
  dummy_node->next = NULL;

  head->right = NULL;
  head->left = NULL;
  head->next = dummy_node;
  head->key = 0;

  root->right = NULL;
  root->left = dummy_node;
  root->parent = NULL;
  root->key = 1;

  new_set->head = head;
  new_set->root = root;
  new_set->num_threads = num_threads;
  new_set->del_scale = num_threads * 100;

  return new_set;
}

uint8_t insert(tsl_set_t *set, skey_t key, sval_t value)
{
  volatile node_t *cas1_node, *cas2_node, *leaf_node;
  node_t *next_leaf;
  uint8_t parent_direction;
  insert_seek_t ins_seek;

  volatile node_t *new_node = create_node();

  new_node->right = MARK_LEAF_NODE(new_node);
  new_node->key = key;
  new_node->value = value;
  while (1)
  {
    cas1_node = NULL;
    cas2_node = NULL;

    ins_seek = insert_search(set, key);
    if (ins_seek.duplicate == DUPLICATE_DIRECTION)
    {
#if GC == 1
      ssmem_free(alloc, (void *)new_node);
#endif
      return 0; // reject duplicates
    }
    else if (ins_seek.child == NULL)
      continue;

    parent_direction = ins_seek.parent_direction;
    cas1_node = ins_seek.cas1_node;
    cas2_node = ins_seek.cas2_node;

    leaf_node = ins_seek.child; // insertion point
    next_leaf = ins_seek.next_node;

    /***************** Prepare new internal node and its children ************************************/

    new_node->left = MARK_LEAF_NODE(leaf_node);
    new_node->parent_direction = parent_direction;
    new_node->parent = cas1_node;
    new_node->next = next_leaf; // create node link
    new_node->inserting = 1;
    /***************** Insert ************************************/
    if (leaf_node->next == next_leaf)
    {
      if (parent_direction == RIGHT_DIRECTION)
      {
#if ATOMIC_INSTRUCTION == 1
        next_t current_nextRight, new_nextRight;
        new_node->inserting = 0;
        current_nextRight.next = next_leaf;
        current_nextRight.right = cas2_node;
        new_nextRight.next = new_node;
        new_nextRight.right = new_node;
        if (leaf_node->next == next_leaf && cas1_node->right == cas2_node)
        {
          if (CAE((next_t *)&leaf_node->next, &current_nextRight, &new_nextRight))
          {
            return 1;
          }
        }
#else
        if (leaf_node->next == next_leaf)
        {
          if (CAS(&leaf_node->next, next_leaf, new_node))
          {
            if (new_node->inserting)
            {
              if (cas1_node->right == cas2_node)
                CAS(&cas1_node->right, cas2_node, new_node);
              // unmark inserted new item node
              if (new_node->inserting)
                new_node->inserting = 0;
            }
            return 1;
          }
        }
#endif
      }
      else if (parent_direction == LEFT_DIRECTION) //&& !GETMARK(cas1_node->next)
      {
        if (leaf_node->next == next_leaf)
        {
          if (CAS(&leaf_node->next, next_leaf, new_node))
          {
            if (new_node->inserting)
            {
              if (cas1_node->left == cas2_node)
                CAS(&cas1_node->left, cas2_node, new_node);
              // unmark inserted new item node
              if (new_node->inserting)
                new_node->inserting = 0;
            }
            return 1;
          }
        }
      }
    }
  }
}

insert_seek_t insert_search(tsl_set_t *set, skey_t key)
{
  volatile node_t *child_node, *grand_parent_node, *parent_node, *root = set->root;
  node_t *child_next, *current_next, *parent_node_right, *parent_node_left, *marked_node;
  uint8_t parent_direction;
  volatile uint64_t operation_mark, child_mark = 0;

  insert_seek_t ins_seek;
  ins_seek.child = ins_seek.next_node = ins_seek.cas1_node = ins_seek.cas2_node = NULL;
  ins_seek.parent_direction = ins_seek.duplicate = 0;

  marked_node = grand_parent_node = NULL;
  parent_node = root;
  child_node = root->left;
  while (1)
  {
    /***************** Tranverse deleted nodes (right tranverse)************************************/
    if (operation_mark == DELETE_MARK)
    {
      READ_RIGHT();
      marked_node = parent_node;

      while (1)
      {
        if (operation_mark == DELETE_MARK)
        {
          if (child_mark != LEAF_MARK)
          {
            parent_node = child_node;
            READ_RIGHT();
            continue;
          }
          else
          {
            parent_node = ADDRESS(child_node->next);
            READ_RIGHT();
            break;
          }
        }
        else
        {
          if (random_gen(set) < insert_clean_rate)
          {
            if (!GETMARK(grand_parent_node->next) && grand_parent_node->left == marked_node)
            {
              CAS(&grand_parent_node->left, marked_node, parent_node);
            }
          }
          TRAVERSE();
          break;
        }
      }
      continue;
    }
    /***************** Tranverse internal nodes ************************************/
    if (child_mark != LEAF_MARK)
    {
      grand_parent_node = parent_node;
      parent_node = child_node;
      TRAVERSE();
    }
    else
    {
      current_next = child_node->next;
      child_next = ADDRESS(current_next);
      if (GETMARK(current_next))
      {
      GO_NEXT:
        parent_node = child_next;
        READ_RIGHT();
      }
#if ATOMIC_INSTRUCTION == 1
      // double word CAS
      else if (parent_direction == LEFT_DIRECTION && child_next != NULL && child_next->inserting)
#else
      else if (child_next != NULL && child_next->inserting)
#endif
      {
        try_helping_insert(child_next);
        parent_node = child_next;
        TRAVERSE();
      }
      else if (child_next != NULL && child_next->key == key)
      {
        ins_seek.duplicate = DUPLICATE_DIRECTION;
        return ins_seek;
      }
      else if (
          (parent_direction == LEFT_DIRECTION && parent_node->left == MARK_LEAF_NODE(child_node)) ||
          (parent_direction == RIGHT_DIRECTION && parent_node->right == MARK_LEAF_NODE(child_node)))
      {
        ins_seek.child = child_node;
        ins_seek.cas1_node = parent_node;
        ins_seek.cas2_node = MARK_LEAF_NODE(child_node);
        ins_seek.next_node = child_next;
        ins_seek.parent_direction = parent_direction;
        return ins_seek;
      }
      else
      {
        TRAVERSE();
      }
    }
  }
}
static inline void try_helping_insert(volatile node_t *new_node)
{
  uint8_t parent_direction;
  node_t *cas1_node, *cas2_node;

  parent_direction = new_node->parent_direction;
  cas1_node = new_node->parent;
  cas2_node = new_node->left;

  if (parent_direction == LEFT_DIRECTION && new_node->inserting)
  {
    if (new_node->inserting)
    {
      CAS(&cas1_node->left, cas2_node, new_node);
      if (new_node->inserting)
        new_node->inserting = 0;
    }
  }
  else if (parent_direction == RIGHT_DIRECTION && new_node->inserting)
  {
    if (new_node->inserting)
    {
      CAS(&cas1_node->right, cas2_node, new_node);
      if (new_node->inserting)
        new_node->inserting = 0;
    }
  }
}
void physical_delete(tsl_set_t *set, volatile node_t *dummy_node)
{
  volatile node_t *child_node, *child_next, *grand_parent_node, *parent_node, *root = set->root;
  uint8_t parent_direction, clear = 0;
  node_t *parent_node_left, *parent_node_right, *cas_val, *current_next, *marked_node;
  uint64_t operation_mark, child_mark;

  grand_parent_node = NULL;
  parent_node = root;
  child_node = root->left;
  child_mark = 0;
  operation_mark = 0;
  marked_node = NULL;

  while (1)
  {
    /***************** Tranverse deleted nodes ************************************/
    if (operation_mark == DELETE_MARK)
    {
      READ_RIGHT();
      marked_node = parent_node;

      while (1)
      {
        if (operation_mark == DELETE_MARK)
        {
          if (child_mark != LEAF_MARK)
          {
            parent_node = child_node;
            READ_RIGHT();
            continue;
          }
          else
          {
            child_next = ADDRESS(child_node->next);
            if (child_next->inserting && child_next->parent == parent_node)
            {
              try_helping_insert(child_next);
            }
            // Confirm edge is complete
            else if (parent_node->right == MARK_LEAF_NODE(child_node))
            {
              if (grand_parent_node->key != 0)
              {
                grand_parent_node->key = 0;
              }
              goto FINISH;
            }
            READ_RIGHT();
            continue;
          }
        }
        else
        {
          if (!GETMARK(grand_parent_node->next))
          {
            if (grand_parent_node->left == marked_node)
            {
              if (CAS(&grand_parent_node->left, marked_node, parent_node))
              {
                READ_LEFT();
                break;
              }
            }
            parent_node = grand_parent_node;
            READ_LEFT();
            break;
          }
          goto FINISH;
        }
      }
    }
    else
    {
      /***************** Tranverse active nodes ************************************/
      if (child_mark != LEAF_MARK)
      {
        if (parent_node->key == 0 || parent_node == dummy_node)
        {
          if (parent_node->key != 0)
            parent_node->key = 0;
          goto FINISH;
        }
        grand_parent_node = parent_node;
        parent_node = child_node;
        READ_LEFT();
        continue;
      }
      else
      {
        current_next = child_node->next;
        child_next = ADDRESS(current_next);
        if (GETMARK(current_next))
        {
          if (child_next->inserting && child_next->parent == parent_node)
          {
            try_helping_insert(child_next);
          }
          else if (parent_node->left == MARK_LEAF_NODE(child_node))
          {
            if (child_next->key != 0)
              child_next->key = 0;
            goto FINISH;
          }
          READ_LEFT();
          continue;
        }
      }
    FINISH:
      break;
    }
  }
}

sval_t delete_min(tsl_set_t *set)
{
  volatile node_t *leaf_node, *next_leaf, *head = set->head;
  node_t *xor_node, *current_next, *head_item_node, *new_head = NULL;
  sval_t value;

  leaf_node = head_item_node = head->next;
  /*optimisation batch deleting*/
  if (previous_head == leaf_node)
  {
    leaf_node = previous_dummy;
  }
  else
    previous_head = head_item_node;

  while (1)
  {
    current_next = leaf_node->next;
    next_leaf = ADDRESS(current_next);

    if (next_leaf == NULL)
    {
      previous_dummy = leaf_node;
      return 0;
    }
    else
    {
      if (GETMARK(current_next))
      {
        /*
        if(head->next == head_item_node)
                leaf_node = next_leaf;
        else
                leaf_node = head_item_node = head->next;
        */
        leaf_node = next_leaf;
        continue;
      }
      xor_node = FAXOR(&leaf_node->next, 1);
      if (!GETMARK(xor_node))
      {
        value = xor_node->value;
        previous_dummy = xor_node;
        if (random_gen(set) >= physical_delete_rate)
        {
          return value;
        }

        if (head->next == head_item_node)
        {
          if (CAS(&head->next, head_item_node, xor_node))
          {
            previous_head = xor_node;
            if (xor_node->key != 0)
              xor_node->key = 0;
            physical_delete(set, xor_node);
            next_leaf = head_item_node;
            while (next_leaf != xor_node)
            {
              current_next = next_leaf;
              next_leaf = ADDRESS(next_leaf->next);
#if GC == 1
              ssmem_free(alloc, (void *)current_next);
#endif
            }
          }
        }
        return value;
      }

      leaf_node = ADDRESS(xor_node);
    }
  }
}

static inline uint32_t random_gen(tsl_set_t *set)
{
  return (my_random(&(seeds[0]), &(seeds[1]), &(seeds[2])) % (set->del_scale));
}

uint32_t pq_size(tsl_set_t *set)
{
  uint32_t count = 0;
  node_t *prev_node;
  node_t *leaf_node, *next_leaf, *head = set->head;

  leaf_node = ADDRESS(head->next);
  next_leaf = ADDRESS(leaf_node->next);

  while (leaf_node != NULL)
  {
    next_leaf = ADDRESS(leaf_node->next);
    if (!GETMARK(leaf_node->next) && next_leaf != NULL)
    {
      /** Count the number of leaf nodes excluding the dummy nodes and deleted nodes **/
      count += 1;
    }
    leaf_node = ADDRESS(leaf_node->next);
  }
  return count;
}