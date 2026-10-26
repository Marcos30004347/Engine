#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

namespace lib
{

template <typename Tag, typename T> class BoundedTaggedRectTreap
{
public:
  struct Rect
  {
    T x1, y1, x2, y2;
    Tag tag;
    Rect() = default;
    Rect(T _x1, T _y1, T _x2, T _y2, const Tag &t) : x1(std::min(_x1, _x2)), y1(std::min(_y1, _y2)), x2(std::max(_x1, _x2)), y2(std::max(_y1, _y2)), tag(t)
    {
    }
    bool valid() const
    {
      return x1 <= x2 && y1 <= y2;
    }
    bool overlaps(const Rect &o) const
    {
      return !(x2 < o.x1 || o.x2 < x1 || y2 < o.y1 || o.y2 < y1);
    }
    Rect intersection(const Rect &o) const
    {
      return Rect(std::max(x1, o.x1), std::max(y1, o.y1), std::min(x2, o.x2), std::min(y2, o.y2), tag);
    }
  };

private:
  struct Node
  {
    Rect rect;
    uint32_t prio;
    int left = -1;
    int right = -1;
    T subtreeMaxX2;
    T subtreeMaxY2;
    bool used = false;
  };

  std::vector<Node> nodes;
  std::vector<int> freeList;
  int root = -1;
  std::mt19937 rng;

public:
  explicit BoundedTaggedRectTreap(size_t capacity) : nodes(capacity + 1), rng(std::random_device{}())
  {
    capacity += 1;
    freeList.reserve(capacity + 1);
    for (int i = (int)capacity - 1; i >= 0; --i)
      freeList.push_back(i);
  }

  bool full() const
  {
    return freeList.empty();
  }
  size_t capacity() const
  {
    return nodes.size();
  }
  size_t size() const
  {
    return nodes.size() - freeList.size();
  }

  bool insert(T x1, T y1, T x2, T y2, const Tag &tag)
  {
    Rect r(x1, y1, x2, y2, tag);
    if (!r.valid())
      return true;
    if (full())
      return false;

    eraseOverlapDifferent(root, r, tag);
    int idx = allocateNode(r);
    if (idx < 0)
      return false;
    root = treapInsert(root, idx);
    return true;
  }

  void remove(T x1, T y1, T x2, T y2, const Tag &tag)
  {
    Rect r(x1, y1, x2, y2, tag);
    if (!r.valid())
      return;
    root = removeRange(root, r);
  }

  void query(T x1, T y1, T x2, T y2, std::vector<Rect> &out) const
  {
    Rect q(x1, y1, x2, y2, Tag{});
    out.clear();
    if (root == -1)
      return;

    std::vector<int> stack;
    stack.reserve(64);
    stack.push_back(root);

    while (!stack.empty())
    {
      int n = stack.back();
      stack.pop_back();
      const Node &node = nodes[n];
      if (!node.used)
        continue;

      if (node.rect.overlaps(q))
      {
        T ix1 = std::max(node.rect.x1, q.x1);
        T iy1 = std::max(node.rect.y1, q.y1);
        T ix2 = std::min(node.rect.x2, q.x2);
        T iy2 = std::min(node.rect.y2, q.y2);
        if (ix1 <= ix2 && iy1 <= iy2)
        {
          out.emplace_back(ix1, iy1, ix2, iy2, node.rect.tag);
        }
      }

      // prune traversal using subtree bounding info
      if (node.right != -1)
      {
        const Node &rnode = nodes[node.right];
        if (rnode.used && (rnode.rect.x1 <= q.x2 || rnode.subtreeMaxX2 >= q.x1))
        {
          stack.push_back(node.right);
        }
      }
      if (node.left != -1)
      {
        const Node &lnode = nodes[node.left];
        if (lnode.used && (lnode.subtreeMaxX2 >= q.x1 || lnode.rect.x1 <= q.x2))
        {
          stack.push_back(node.left);
        }
      }
    }
  }
  void queryAll(T x1, T y1, T x2, T y2, std::vector<Rect> &out) const
  {
    Rect q(x1, y1, x2, y2, Tag{});
    out.clear();
    if (root == -1)
      return;

    std::vector<int> stack;
    stack.reserve(64);
    stack.push_back(root);

    while (!stack.empty())
    {
      int n = stack.back();
      stack.pop_back();
      const Node &node = nodes[n];
      if (!node.used)
        continue;

      if (node.rect.overlaps(q))
      {
        T ix1 = std::max(node.rect.x1, q.x1);
        T iy1 = std::max(node.rect.y1, q.y1);
        T ix2 = std::min(node.rect.x2, q.x2);
        T iy2 = std::min(node.rect.y2, q.y2);
        if (ix1 <= ix2 && iy1 <= iy2)
        {
          out.emplace_back(ix1, iy1, ix2, iy2, node.rect.tag);
        }
      }

      if (node.right != -1)
      {
        const Node &rnode = nodes[node.right];
        if (rnode.used && (rnode.rect.x1 <= q.x2 || rnode.subtreeMaxX2 >= q.x1))
        {
          stack.push_back(node.right);
        }
      }
      if (node.left != -1)
      {
        const Node &lnode = nodes[node.left];
        if (lnode.used && (lnode.subtreeMaxX2 >= q.x1 || lnode.rect.x1 <= q.x2))
        {
          stack.push_back(node.left);
        }
      }
    }
  }
  void print() const
  {
    printImpl(root, 0);
  }

private:
  int allocateNode(const Rect &r)
  {
    if (freeList.empty())
      return -1;
    int idx = freeList.back();
    freeList.pop_back();
    Node &n = nodes[idx];
    n.rect = r;
    n.prio = rng();
    n.left = n.right = -1;
    n.subtreeMaxX2 = r.x2;
    n.subtreeMaxY2 = r.y2;
    n.used = true;
    return idx;
  }

  void freeNode(int idx)
  {
    if (idx < 0)
      return;
    Node &n = nodes[idx];
    n.used = false;
    n.left = n.right = -1;
    freeList.push_back(idx);
  }

  static bool rectBefore(const Rect &a, const Rect &b)
  {
    if (a.x1 != b.x1)
      return a.x1 < b.x1;
    if (a.y1 != b.y1)
      return a.y1 < b.y1;
    if (a.x2 != b.x2)
      return a.x2 < b.x2;
    return a.y2 < b.y2;
  }

  void updateNode(int idx)
  {
    if (idx < 0)
      return;
    Node &n = nodes[idx];
    n.subtreeMaxX2 = n.rect.x2;
    n.subtreeMaxY2 = n.rect.y2;
    if (n.left != -1)
    {
      const Node &L = nodes[n.left];
      if (L.used)
      {
        n.subtreeMaxX2 = std::max(n.subtreeMaxX2, L.subtreeMaxX2);
        n.subtreeMaxY2 = std::max(n.subtreeMaxY2, L.subtreeMaxY2);
      }
    }
    if (n.right != -1)
    {
      const Node &R = nodes[n.right];
      if (R.used)
      {
        n.subtreeMaxX2 = std::max(n.subtreeMaxX2, R.subtreeMaxX2);
        n.subtreeMaxY2 = std::max(n.subtreeMaxY2, R.subtreeMaxY2);
      }
    }
  }

  int rotateRight(int y)
  {
    int x = nodes[y].left;
    assert(x != -1);
    nodes[y].left = nodes[x].right;
    nodes[x].right = y;
    updateNode(y);
    updateNode(x);
    return x;
  }
  int rotateLeft(int x)
  {
    int y = nodes[x].right;
    assert(y != -1);
    nodes[x].right = nodes[y].left;
    nodes[y].left = x;
    updateNode(x);
    updateNode(y);
    return y;
  }

  int treapInsert(int cur, int insIdx)
  {
    if (cur == -1)
      return insIdx;
    if (rectBefore(nodes[insIdx].rect, nodes[cur].rect))
    {
      nodes[cur].left = treapInsert(nodes[cur].left, insIdx);
      if (nodes[nodes[cur].left].prio < nodes[cur].prio)
        cur = rotateRight(cur);
    }
    else
    {
      nodes[cur].right = treapInsert(nodes[cur].right, insIdx);
      if (nodes[nodes[cur].right].prio < nodes[cur].prio)
        cur = rotateLeft(cur);
    }
    updateNode(cur);
    return cur;
  }

  int treapJoin(int left, int right)
  {
    if (left == -1)
      return right;
    if (right == -1)
      return left;
    if (nodes[left].prio < nodes[right].prio)
    {
      nodes[left].right = treapJoin(nodes[left].right, right);
      updateNode(left);
      return left;
    }
    else
    {
      nodes[right].left = treapJoin(left, nodes[right].left);
      updateNode(right);
      return right;
    }
  }

  void eraseOverlapDifferent(int &nodeIdx, const Rect &r, const Tag &newTag)
  {
    if (nodeIdx == -1)
      return;
    Node &node = nodes[nodeIdx];

    if (node.rect.x2 < r.x1)
    {
      eraseOverlapDifferent(node.right, r, newTag);
      updateNode(nodeIdx);
      return;
    }
    if (node.rect.x1 > r.x2)
    {
      eraseOverlapDifferent(node.left, r, newTag);
      updateNode(nodeIdx);
      return;
    }

    if (!node.rect.overlaps(r))
    {
      eraseOverlapDifferent(node.left, r, newTag);
      eraseOverlapDifferent(node.right, r, newTag);
      updateNode(nodeIdx);
      return;
    }

    if (node.rect.tag == newTag)
    {
      eraseOverlapDifferent(node.left, r, newTag);
      eraseOverlapDifferent(node.right, r, newTag);
      updateNode(nodeIdx);
      return;
    }

    Rect A = node.rect;
    Rect B = r;

    if (B.x1 <= A.x1 && B.y1 <= A.y1 && B.x2 >= A.x2 && B.y2 >= A.y2)
    {
      int left = node.left;
      int right = node.right;
      freeNode(nodeIdx);
      nodeIdx = treapJoin(left, right);
      if (nodeIdx != -1)
        eraseOverlapDifferent(nodeIdx, r, newTag);
      return;
    }

    std::vector<Rect> frags;
    if (B.x1 > A.x1)
    {
      T rx2 = std::min(static_cast<T>(B.x1 - 1), A.x2);
      if (A.x1 <= rx2)
        frags.emplace_back(A.x1, A.y1, rx2, A.y2, A.tag);
    }
    if (B.x2 < A.x2)
    {
      T rx1 = std::max(static_cast<T>(B.x2 + 1), A.x1);
      if (rx1 <= A.x2)
        frags.emplace_back(rx1, A.y1, A.x2, A.y2, A.tag);
    }
    T mx1 = std::max(A.x1, B.x1);
    T mx2 = std::min(A.x2, B.x2);
    if (mx1 <= mx2)
    {
      if (B.y1 > A.y1)
      {
        T ry2 = std::min(static_cast<T>(B.y1 - 1), A.y2);
        if (A.y1 <= ry2)
          frags.emplace_back(mx1, A.y1, mx2, ry2, A.tag);
      }
      if (B.y2 < A.y2)
      {
        T ry1 = std::max(static_cast<T>(B.y2 + 1), A.y1);
        if (ry1 <= A.y2)
          frags.emplace_back(mx1, ry1, mx2, A.y2, A.tag);
      }
    }

    int left = node.left;
    int right = node.right;
    freeNode(nodeIdx);
    nodeIdx = treapJoin(left, right);

    for (const Rect &f : frags)
    {
      if (!f.valid())
        continue;
      if (full())
        break; // can't insert more
      int fi = allocateNode(f);
      if (fi >= 0)
        nodeIdx = treapInsert(nodeIdx, fi);
    }

    if (nodeIdx != -1)
      eraseOverlapDifferent(nodeIdx, r, newTag);
  }

  int removeRange(int nodeIdx, const Rect &r)
  {
    if (nodeIdx == -1)
      return -1;
    Node &node = nodes[nodeIdx];
    if (!node.rect.overlaps(r))
    {
      node.left = removeRange(node.left, r);
      node.right = removeRange(node.right, r);
      updateNode(nodeIdx);
      return nodeIdx;
    }

    if (node.rect.tag != r.tag)
    {
      node.left = removeRange(node.left, r);
      node.right = removeRange(node.right, r);
      updateNode(nodeIdx);
      return nodeIdx;
    }

    Rect A = node.rect;
    Rect B = r;

    if (B.x1 <= A.x1 && B.y1 <= A.y1 && B.x2 >= A.x2 && B.y2 >= A.y2)
    {
      int left = node.left;
      int right = node.right;
      freeNode(nodeIdx);
      return treapJoin(left, right);
    }

    std::vector<Rect> frags;
    if (B.x1 > A.x1)
    {
      T rx2 = std::min(static_cast<T>(B.x1 - 1), A.x2);
      if (A.x1 <= rx2)
        frags.emplace_back(A.x1, A.y1, rx2, A.y2, A.tag);
    }
    if (B.x2 < A.x2)
    {
      T rx1 = std::max(static_cast<T>(B.x2 + 1), A.x1);
      if (rx1 <= A.x2)
        frags.emplace_back(rx1, A.y1, A.x2, A.y2, A.tag);
    }
    T mx1 = std::max(A.x1, B.x1);
    T mx2 = std::min(A.x2, B.x2);
    if (mx1 <= mx2)
    {
      if (B.y1 > A.y1)
      {
        T ry2 = std::min(static_cast<T>(B.y1 - 1), A.y2);
        if (A.y1 <= ry2)
          frags.emplace_back(mx1, A.y1, mx2, ry2, A.tag);
      }
      if (B.y2 < A.y2)
      {
        T ry1 = std::max(static_cast<T>(B.y2 + 1), A.y1);
        if (ry1 <= A.y2)
          frags.emplace_back(mx1, ry1, mx2, A.y2, A.tag);
      }
    }

    int left = node.left;
    int right = node.right;
    freeNode(nodeIdx);
    int res = treapJoin(left, right);
    for (const Rect &f : frags)
    {
      if (!f.valid())
        continue;
      if (full())
        break;
      int fi = allocateNode(f);
      if (fi >= 0)
        res = treapInsert(res, fi);
    }

    if (res != -1)
    {
      nodes[res].left = removeRange(nodes[res].left, r);
      nodes[res].right = removeRange(nodes[res].right, r);
      updateNode(res);
    }
    return res;
  }

  void printImpl(int idx, int depth) const
  {
    if (idx == -1)
      return;
    printImpl(nodes[idx].left, depth + 1);
    for (int i = 0; i < depth; ++i)
      std::cout << "  ";
    const Rect &r = nodes[idx].rect;
    std::cout << "[(" << r.x1 << "," << r.y1 << ")-(" << r.x2 << "," << r.y2 << ")] tag\n";
    printImpl(nodes[idx].right, depth + 1);
  }
};

} // namespace lib
