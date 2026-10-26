#include <algorithm>
#include <iostream>
#include <memory>
#include <vector>

namespace lib
{
template <typename Tag, typename T> class TaggedIntervalTree
{
public:
  struct Interval
  {
    T start, end;
    Tag tag;
    T maxEnd;

    Interval(T s, T e, Tag t) : start(s), end(e), tag(t), maxEnd(e)
    {
    }
  };

  struct Node
  {
    Interval interval;
    std::unique_ptr<Node> left;
    std::unique_ptr<Node> right;

    Node(const Interval &i) : interval(i)
    {
    }
  };

private:
  std::unique_ptr<Node> root;

public:
  void insert(T start, T end, const Tag &tag)
  {
    if (start > end)
      std::swap(start, end);

    eraseOverlapDifferent(root, start, end, tag);

    root = insert(std::move(root), Interval(start, end, tag));
  }

  void remove(T start, T end, const Tag &tag)
  {
    if (start > end)
      std::swap(start, end);
    root = remove(std::move(root), Interval(start, end, tag));
  }

  void query(T start, T end, const Tag &tag, std::vector<Interval> &out) const
  {
    query(root.get(), start, end, tag, out);
  }

  void print() const
  {
    print(root.get(), 0);
  }

private:
  static bool overlap(const Interval &a, const Interval &b)
  {
    return a.start <= b.end && b.start <= a.end;
  }

  static std::unique_ptr<Node> insert(std::unique_ptr<Node> node, Interval i)
  {
    if (!node)
      return std::make_unique<Node>(i);

    if (node->interval.tag == i.tag && overlap(node->interval, i))
    {
      node->interval.start = std::min(node->interval.start, i.start);
      node->interval.end = std::max(node->interval.end, i.end);
    }
    else if (i.start < node->interval.start)
    {
      node->left = insert(std::move(node->left), i);
    }
    else
    {
      node->right = insert(std::move(node->right), i);
    }

    updateMaxEnd(node);
    node = mergeChildren(std::move(node));
    return node;
  }

  static std::unique_ptr<Node> mergeChildren(std::unique_ptr<Node> node)
  {
    if (!node)
      return nullptr;

    if (node->left && node->left->interval.tag == node->interval.tag && overlap(node->interval, node->left->interval))
    {
      node->interval.start = std::min(node->interval.start, node->left->interval.start);
      node->interval.end = std::max(node->interval.end, node->left->interval.end);
      node->left = std::move(node->left->left);
    }

    if (node->right && node->right->interval.tag == node->interval.tag && overlap(node->interval, node->right->interval))
    {
      node->interval.start = std::min(node->interval.start, node->right->interval.start);
      node->interval.end = std::max(node->interval.end, node->right->interval.end);
      node->right = std::move(node->right->right);
    }

    updateMaxEnd(node);
    return node;
  }

  static std::unique_ptr<Node> remove(std::unique_ptr<Node> node, const Interval &i)
  {
    if (!node)
      return nullptr;

    if (i.end < node->interval.start)
      node->left = remove(std::move(node->left), i);
    else if (i.start > node->interval.end)
      node->right = remove(std::move(node->right), i);
    else if (node->interval.tag == i.tag && overlap(node->interval, i))
    {
      T c = node->interval.start;
      T d = node->interval.end;
      T a = i.start;
      T b = i.end;

      if (a <= c && b >= d)
        return join(std::move(node->left), std::move(node->right));

      if (a > c && b < d)
      {
        T oldEnd = node->interval.end;
        node->interval.end = a - 1;
        if (b + 1 <= oldEnd)
        {
          Interval rightPiece(b + 1, oldEnd, node->interval.tag);
          node->right = insert(std::move(node->right), rightPiece);
        }
      }
      else if (a <= c)
      {
        node->interval.start = b + 1;
        if (node->interval.start > node->interval.end)
          return join(std::move(node->left), std::move(node->right));
      }
      else
      {
        node->interval.end = a - 1;
        if (node->interval.start > node->interval.end)
          return join(std::move(node->left), std::move(node->right));
      }
    }

    updateMaxEnd(node);
    return node;
  }

  static void eraseOverlapDifferent(std::unique_ptr<Node> &node, T start, T end, const Tag &newTag)
  {
    if (!node)
      return;

    if (end < node->interval.start)
    {
      eraseOverlapDifferent(node->left, start, end, newTag);
      updateMaxEnd(node);
      return;
    }
    if (start > node->interval.end)
    {
      eraseOverlapDifferent(node->right, start, end, newTag);
      updateMaxEnd(node);
      return;
    }

    if (node->interval.tag == newTag)
    {
      eraseOverlapDifferent(node->left, start, end, newTag);
      eraseOverlapDifferent(node->right, start, end, newTag);
      updateMaxEnd(node);
      return;
    }

    T a = node->interval.start;
    T b = node->interval.end;

    if (start <= a && end >= b)
    {
      node = join(std::move(node->left), std::move(node->right));
      if (node)
      {
        eraseOverlapDifferent(node, start, end, newTag);
        updateMaxEnd(node);
      }
      return;
    }

    if (start > a && end < b)
    {
      node->interval.end = start;
      if (end < b)
      {
        Interval rightPiece(end, b, node->interval.tag);
        node->right = insert(std::move(node->right), rightPiece);
      }
      updateMaxEnd(node);
      return;
    }

    if (start <= a && end < b)
    {
      node->interval.start = end;
      if (!(node->interval.start < node->interval.end))
      {
        node = join(std::move(node->left), std::move(node->right));
        if (node)
          eraseOverlapDifferent(node, start, end, newTag);
        return;
      }

      eraseOverlapDifferent(node->left, start, end, newTag);
      updateMaxEnd(node);
      return;
    }

    if (start > a && end >= b)
    {
      node->interval.end = start;
      if (!(node->interval.start < node->interval.end))
      {
        node = join(std::move(node->left), std::move(node->right));
        if (node)
          eraseOverlapDifferent(node, start, end, newTag);
        return;
      }
      eraseOverlapDifferent(node->right, start, end, newTag);
      updateMaxEnd(node);
      return;
    }

    eraseOverlapDifferent(node->left, start, end, newTag);
    eraseOverlapDifferent(node->right, start, end, newTag);
    updateMaxEnd(node);
  }

  static std::unique_ptr<Node> join(std::unique_ptr<Node> left, std::unique_ptr<Node> right)
  {
    if (!left)
      return right;
    if (!right)
      return left;

    // pop min from right subtree
    Interval minInt = popMin(right);
    auto newRoot = std::make_unique<Node>(minInt);
    newRoot->left = std::move(left);
    newRoot->right = std::move(right);
    updateMaxEnd(newRoot);
    return newRoot;
  }

  static Interval popMin(std::unique_ptr<Node> &node)
  {
    std::unique_ptr<Node> *cur = &node;
    while ((*cur)->left)
      cur = &((*cur)->left);

    Interval minInterval = (*cur)->interval;
    *cur = std::move((*cur)->right);
    return minInterval;
  }

  static void updateMaxEnd(std::unique_ptr<Node> &node)
  {
    if (!node)
      return;
    T leftMax = node->left ? node->left->interval.maxEnd : node->interval.end;
    T rightMax = node->right ? node->right->interval.maxEnd : node->interval.end;
    node->interval.maxEnd = std::max({node->interval.end, leftMax, rightMax});
  }

  static void query(const Node *node, T start, T end, const Tag &tag, std::vector<Interval> &out)
  {
    if (!node)
      return;

    if (node->interval.start <= end && node->interval.end >= start && node->interval.tag != tag)
    {
      T overlapStart = std::max(node->interval.start, start);
      T overlapEnd = std::min(node->interval.end, end);
      if (overlapStart < overlapEnd)
        out.emplace_back(overlapStart, overlapEnd, node->interval.tag);
    }

    if (node->left && node->left->interval.maxEnd >= start)
      query(node->left.get(), start, end, tag, out);

    if (node->right && node->interval.start <= end)
      query(node->right.get(), start, end, tag, out);
  }

  static void print(const Node *node, int depth)
  {
    if (!node)
      return;
    print(node->left.get(), depth + 1);
    for (int i = 0; i < depth; ++i)
      std::cout << "  ";
    std::cout << "[" << std::to_string(node->interval.start) << ", " << std::to_string(node->interval.end) << "] tag=" << std::to_string((uint32_t)node->interval.tag) << "\n";
    print(node->right.get(), depth + 1);
  }
};

template <typename Tag, typename T> class BoundedTaggedIntervalTree
{
public:
  struct Interval
  {
    T start, end, maxEnd;
    Tag tag;
  };

  struct Node
  {
    Interval interval;
    int left = -1;
    int right = -1;
  };

private:
  std::vector<Node> nodes;
  int root = -1;
  int freeList = -1;
  size_t capacity = 0;
  size_t size = 0;

public:
  explicit BoundedTaggedIntervalTree(size_t maxIntervals) : capacity(maxIntervals + 1)
  {
    maxIntervals += 1;

    nodes.resize(maxIntervals);
    for (size_t i = 0; i + 1 < maxIntervals; ++i)
      nodes[i].left = static_cast<int>(i + 1);
    nodes[maxIntervals - 1].left = -1;
    freeList = 0;
  }

  void insert(T start, T end, const Tag &tag)
  {
    if (start > end)
      std::swap(start, end);
    Interval i{start, end, end, tag};
    if (root == -1)
      root = newNode(i);
    else
      insert(root, i);
  }

  void remove(T start, T end, const Tag &tag)
  {
    if (start > end)
      std::swap(start, end);
    root = remove(root, Interval{start, end, end, tag});
  }

  void query(T start, T end, const Tag &tag, std::vector<Interval> &out) const
  {
    query(root, start, end, tag, out);
  }

  void queryAll(T start, T end, std::vector<Interval> &out) const
  {
    queryAll(root, start, end, out);
  }

  void print() const
  {
    print(root, 0);
  }

  size_t getSize() const
  {
    return size;
  }

private:
  int newNode(const Interval &i)
  {
    if (freeList == -1)
      throw std::runtime_error("BoundedTaggedIntervalTree capacity exceeded");
    int idx = freeList;
    freeList = nodes[idx].left;
    nodes[idx].interval = i;
    nodes[idx].left = nodes[idx].right = -1;
    ++size;
    return idx;
  }

  void freeNode(int idx)
  {
    nodes[idx].left = freeList;
    freeList = idx;
    --size;
  }

  static bool overlap(const Interval &a, const Interval &b)
  {
    return a.start <= b.end && b.start <= a.end;
  }

  void queryAll(int nodeIdx, T start, T end, std::vector<Interval> &out) const
  {
    if (nodeIdx == -1)
      return;

    const Node &node = nodes[nodeIdx];
    
    if (node.interval.start <= end && node.interval.end >= start)
    {
      T overlapStart = std::max(node.interval.start, start);
      T overlapEnd = std::min(node.interval.end, end);
      if (overlapStart < overlapEnd)
        out.push_back({overlapStart, overlapEnd, overlapEnd, node.interval.tag});
    }

    if (node.left != -1 && nodes[node.left].interval.maxEnd >= start)
      queryAll(node.left, start, end, out);

    if (node.right != -1 && node.interval.start <= end)
      queryAll(node.right, start, end, out);
  }

  void insert(int nodeIdx, const Interval &i)
  {
    Node &node = nodes[nodeIdx];
    if (node.interval.tag == i.tag && overlap(node.interval, i))
    {
      node.interval.start = std::min(node.interval.start, i.start);
      node.interval.end = std::max(node.interval.end, i.end);
    }
    else if (i.start < node.interval.start)
    {
      if (node.left == -1)
        node.left = newNode(i);
      else
        insert(node.left, i);
    }
    else
    {
      if (node.right == -1)
        node.right = newNode(i);
      else
        insert(node.right, i);
    }

    updateMaxEnd(nodeIdx);
  }

  void updateMaxEnd(int nodeIdx)
  {
    Node &node = nodes[nodeIdx];
    T leftMax = node.left != -1 ? nodes[node.left].interval.maxEnd : node.interval.end;
    T rightMax = node.right != -1 ? nodes[node.right].interval.maxEnd : node.interval.end;
    node.interval.maxEnd = std::max({node.interval.end, leftMax, rightMax});
  }

  int remove(int nodeIdx, const Interval &i)
  {
    if (nodeIdx == -1)
      return -1;

    Node &node = nodes[nodeIdx];
    if (i.end < node.interval.start)
    {
      node.left = remove(node.left, i);
    }
    else if (i.start > node.interval.end)
    {
      node.right = remove(node.right, i);
    }
    else if (node.interval.tag == i.tag && overlap(node.interval, i))
    {
      T a = i.start, b = i.end;
      T c = node.interval.start, d = node.interval.end;

      if (a <= c && b >= d)
      {
        int newRoot = join(node.left, node.right);
        freeNode(nodeIdx);
        return newRoot;
      }

      if (a > c && b < d)
      {
        T oldEnd = d;
        node.interval.end = a - 1;
        if (b + 1 <= oldEnd)
        {
          Interval rightPiece{b + 1, oldEnd, oldEnd, node.interval.tag};
          if (node.right == -1)
            node.right = newNode(rightPiece);
          else
            insert(node.right, rightPiece);
        }
      }
      else if (a <= c)
      {
        node.interval.start = b + 1;
        if (node.interval.start > node.interval.end)
        {
          int newRoot = join(node.left, node.right);
          freeNode(nodeIdx);
          return newRoot;
        }
      }
      else
      {
        node.interval.end = a - 1;
        if (node.interval.start > node.interval.end)
        {
          int newRoot = join(node.left, node.right);
          freeNode(nodeIdx);
          return newRoot;
        }
      }
    }

    updateMaxEnd(nodeIdx);
    return nodeIdx;
  }

  int join(int leftIdx, int rightIdx)
  {
    if (leftIdx == -1)
      return rightIdx;
    if (rightIdx == -1)
      return leftIdx;

    int minIdx = popMin(rightIdx);
    Node &minNode = nodes[minIdx];
    minNode.left = leftIdx;
    minNode.right = rightIdx;
    updateMaxEnd(minIdx);
    return minIdx;
  }

  int popMin(int &rootIdx)
  {
    int parent = -1;
    int curr = rootIdx;
    while (nodes[curr].left != -1)
    {
      parent = curr;
      curr = nodes[curr].left;
    }
    if (parent != -1)
      nodes[parent].left = nodes[curr].right;
    else
      rootIdx = nodes[curr].right;
    return curr;
  }

  void query(int nodeIdx, T start, T end, const Tag &tag, std::vector<Interval> &out) const
  {
    if (nodeIdx == -1)
      return;

    const Node &node = nodes[nodeIdx];
    if (node.interval.start <= end && node.interval.end >= start && node.interval.tag != tag)
    {
      T overlapStart = std::max(node.interval.start, start);
      T overlapEnd = std::min(node.interval.end, end);
      if (overlapStart < overlapEnd)
        out.push_back({overlapStart, overlapEnd, overlapEnd, node.interval.tag});
    }

    if (node.left != -1 && nodes[node.left].interval.maxEnd >= start)
      query(node.left, start, end, tag, out);

    if (node.right != -1 && node.interval.start <= end)
      query(node.right, start, end, tag, out);
  }

  void print(int nodeIdx, int depth) const
  {
    if (nodeIdx == -1)
      return;
    print(nodes[nodeIdx].left, depth + 1);
    for (int i = 0; i < depth; ++i)
      std::cout << "  ";
    const auto &i = nodes[nodeIdx].interval;
    std::cout << "[" << i.start << ", " << i.end << "] tag=" << std::to_string((uint64_t)i.tag) << "\n";
    print(nodes[nodeIdx].right, depth + 1);
  }
};
} // namespace lib
