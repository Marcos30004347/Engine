#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

union Link
{
  long long dword;
  //<p,d>: <pointer to Node, boolean>
  struct
  {
    struct Node *p;
    bool d;
  };
};

union VLink
{
  long long dword;
  //<p,d>: <pointer to Value, boolean>
  struct
  {
    void *p; // Pointer to value
    bool d;
  };
};

struct Node
{
  atomic_int refCount; // reference count
  int key, level, validLevel;
  union VLink value;
  union Link *next;
  struct Node *prev;
};

int maxLevel = 16; // maximum levels in the skip list

struct Node *head, *tail;

// Allocate a new node with refCount = 1
struct Node *MALLOC_NODE()
{
  struct Node *node = (Node *)malloc(sizeof(*node));
  atomic_init(&node->refCount, 1);
  node->prev = NULL;
  node->next = NULL;
  return node;
}

// Atomically read a link; if not deleted, bump its refCount and return it
struct Node *READ_NODE(union Link *address)
{
  long long packed = __atomic_load_n(&address->dword, __ATOMIC_ACQUIRE);
  bool deleted = (packed & 1);
  struct Node *n = (struct Node *)(packed & ~1ULL);
  if (deleted || n == NULL)
    return NULL;
  atomic_fetch_add_explicit(&n->refCount, 1, __ATOMIC_ACQ_REL);
  return n;
}

// Increment refCount and return the same node
struct Node *COPY_NODE(struct Node *node)
{
  if (node)
    atomic_fetch_add_explicit(&node->refCount, 1, __ATOMIC_ACQ_REL);
  return node;
}

// Decrement refCount; when it hits zero, recursively release and free
void RELEASE_NODE(struct Node *node)
{
  if (!node)
    return;
  int prev = atomic_fetch_sub_explicit(&node->refCount, 1, __ATOMIC_ACQ_REL);
  if (prev != 1)
    return; // still in use

  // refCount hit zero: clean up
  if (node->prev)
    RELEASE_NODE(node->prev);
  free(node->next);
  free(node);
}

struct Node *CreateNode(int level, int key, void *value)
{
  union VLink vlink_value;
  vlink_value.p = value;
  vlink_value.d = false;

  struct Node *node = MALLOC_NODE();
  node->prev = NULL;
  node->validLevel = 0;
  node->level = level;
  node->key = key;
  node->next = (union Link *)malloc(sizeof(union Link) * level);
  node->value = vlink_value;
  return node;
}

struct Node *HelpDelete(struct Node *node, int level);

struct Node *ReadNext(struct Node **node1, int level)
{
  struct Node *node2;

  if ((*node1)->value.d == true)
    *node1 = HelpDelete(*node1, level);

  node2 = READ_NODE(&(*node1)->next[level]);

  while (node2 == NULL)
  {
    *node1 = HelpDelete(*node1, level);
    node2 = READ_NODE(&(*node1)->next[level]);
  }
  return node2;
}

struct Node *ScanKey(struct Node **node1, int level, int key)
{
  struct Node *node2;

  node2 = ReadNext(node1, level);
  while (node2->key < key)
  {
    RELEASE_NODE(*node1);
    *node1 = node2;
    node2 = ReadNext(node1, level);
  }
  return node2;
}

bool Insert(int key, void *value)
{
  struct Node *newNode, *savedNodes[maxLevel];
  struct Node *node1, *node2;
  int i;

  int level = 1; // you can implement a random-level selector

  newNode = CreateNode(level, key, value);
  COPY_NODE(newNode);
  node1 = COPY_NODE(head);

  for (i = maxLevel - 1; i >= 1; i--)
  {
    node2 = ScanKey(&node1, 0, key);
    RELEASE_NODE(node2);
    if (i < level)
      savedNodes[i] = COPY_NODE(node1);
  }

  while (true)
  {
    node2 = ScanKey(&node1, 0, key);

    if (node2->value.d == false && node2->key == key)
    {
      if (__sync_bool_compare_and_swap((long long *)&node2->value, *(long long *)&node2->value, *(long long *)value))
      {
        RELEASE_NODE(node1);
        RELEASE_NODE(node2);
        for (i = 1; i <= level - 1; i++)
          RELEASE_NODE(savedNodes[i]);
        RELEASE_NODE(newNode);
        RELEASE_NODE(newNode);
        return true;
      }
      else
      {
        RELEASE_NODE(node2);
        continue;
      }
    }

    newNode->next[0] = node1->next[0];
    RELEASE_NODE(node2);

    union Link newNode_link;
    newNode_link.p = newNode;
    newNode_link.d = false;

    if (__sync_bool_compare_and_swap((long long *)&node1->next[0], *(long long *)&node1->next[0], *(long long *)&newNode_link))
    {
      RELEASE_NODE(node1);
      break;
    }
  }

  for (i = 1; i <= level - 1; i++)
  {
    newNode->validLevel = i;
    node1 = savedNodes[i];

    while (true)
    {
      node2 = ScanKey(&node1, i, key);
      newNode->next[i] = node1->next[i];
      RELEASE_NODE(node2);

      if (newNode->value.d == true || __sync_bool_compare_and_swap((long long *)&node1->next[i], *(long long *)&node1->next[i], *(long long *)&newNode))
      {
        RELEASE_NODE(node1);
        break;
      }
    }
  }

  newNode->validLevel = level;
  if (newNode->value.d == true)
    newNode = HelpDelete(newNode, 0);

  RELEASE_NODE(newNode);
  return true;
}

void RemoveNode(struct Node *node, struct Node **prev, int level)
{
  union Link empty_link;
  empty_link.p = NULL;
  empty_link.d = true;

  while (true)
  {
    if (node->next[level].p == empty_link.p && node->next[level].d == true)
      break;

    struct Node *last = ScanKey(prev, level, node->key);
    RELEASE_NODE(last);

    if (last != node || (node->next[level].p == empty_link.p && node->next[level].d == empty_link.d))
      break;

    union Link new_link;
    new_link.p = node->next[level].p;
    new_link.d = false;
    if (__sync_bool_compare_and_swap((long long *)&(*prev)->next[level], *(long long *)&(*prev)->next[level], *(long long *)&new_link))
    {
      node->next[level] = empty_link;
      break;
    }

    if (node->next[level].p == empty_link.p && node->next[level].d == empty_link.d)
      break;
  }
}
void *DeleteMin()
{
  // Local variables (for all functions/procedures)
  struct Node *newNode, *savedNodes[maxLevel];
  struct Node *node1, *node2, *prev, *last;
  int i;

  prev = COPY_NODE(head);

  while (true)
  {
    // Checking if the head is the same as the tail
    node1 = ReadNext(&prev, 0);
    if (node1 == tail)
    {
      RELEASE_NODE(prev);
      RELEASE_NODE(node1);
      return NULL;
    }
  retry:
    if (node1 != prev->next[0].p)
    {
      RELEASE_NODE(node1);
      continue;
    }

    if (node1->value.d == false)
    {
      union VLink new_value;
      new_value.p = node1->value.p;
      new_value.d = true;
      if (__sync_bool_compare_and_swap((long long *)&node1->value, *(long long *)&node1->value, *(long long *)&new_value))
      {
        node1->prev = prev;
        break;
      }
      else
        goto retry;
    }
    else if (node1->value.d == true)
      node1 = HelpDelete(node1, 0);
    RELEASE_NODE(prev);
    prev = node1;
  }

  for (i = 0; i <= node1->level - 1; i++)
  {
    union Link new_link;
    new_link.d = true;
    do
    {
      new_link = node1->next[i];
      // Until d = true or CAS
    } while ((node1->next[i].d != true) && !(__sync_bool_compare_and_swap((long long *)&node1->next[i], *(long long *)&node1->next[i], *(long long *)&new_link)));
  }

  prev = COPY_NODE(head);
  for (i = node1->level - 1; i >= 0; i--)
    RemoveNode(node1, &prev, i);

  union VLink value = node1->value;

  RELEASE_NODE(prev);
  RELEASE_NODE(node1);
  RELEASE_NODE(node1); /*Delete the node*/

  return value.p;
}

struct Node *HelpDelete(struct Node *node, int level)
{
  union Link new_link = {.d = true};
  for (int i = level; i <= node->level - 1; i++)
  {
    do
    {
      new_link.p = node->next[i].p;
    } while (node->next[i].d != true && !(__sync_bool_compare_and_swap((long long *)&node->next[i], *(long long *)&node->next[i], *(long long *)&new_link)));
  }

  struct Node *prev = node->prev;
  if (!prev || level >= prev->validLevel)
  {
    prev = COPY_NODE(head);
    for (int i = maxLevel - 1; i >= level; i--)
    {
      struct Node *node2 = ScanKey(&prev, i, node->key);
      RELEASE_NODE(node2);
    }
  }
  else
  {
    COPY_NODE(prev);
  }

  RemoveNode(node, &prev, level);
  RELEASE_NODE(node);
  return prev;
}

int main(void)
{
  int val0 = 0, val1 = 1, val2 = 2, val3 = 3;
  head = CreateNode(1, -1000000, &val0);
  tail = CreateNode(1, 1000000, &val0);

  head->next[0].p = tail;
  head->next[0].d = false;

  Insert(5, &val1);
  Insert(2, &val2);
  Insert(50, &val3);

  printf("Minimum value %d\n", *(int *)DeleteMin());
  printf("Minimum value %d\n", *(int *)DeleteMin());
  printf("Minimum value %d\n", *(int *)DeleteMin());

  RELEASE_NODE(head);
  RELEASE_NODE(tail);
  return 0;
}
