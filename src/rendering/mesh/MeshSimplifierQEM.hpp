#include <array>
#include <cmath>
#include <limits>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Custom vector and matrix implementations
struct Vec3
{
  float x, y, z;

  Vec3() : x(0), y(0), z(0)
  {
  }
  Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_)
  {
  }

  Vec3 operator+(const Vec3 &other) const
  {
    return Vec3(x + other.x, y + other.y, z + other.z);
  }

  Vec3 operator-(const Vec3 &other) const
  {
    return Vec3(x - other.x, y - other.y, z - other.z);
  }

  Vec3 operator*(float scalar) const
  {
    return Vec3(x * scalar, y * scalar, z * scalar);
  }

  float dot(const Vec3 &other) const
  {
    return x * other.x + y * other.y + z * other.z;
  }

  Vec3 cross(const Vec3 &other) const
  {
    return Vec3(y * other.z - z * other.y, z * other.x - x * other.z, x * other.y - y * other.x);
  }

  float length() const
  {
    return std::sqrt(x * x + y * y + z * z);
  }

  Vec3 normalized() const
  {
    float len = length();
    if (len < 1e-8f)
      return Vec3(0, 0, 0);
    return Vec3(x / len, y / len, z / len);
  }
};

struct Vec4
{
  float x, y, z, w;

  Vec4() : x(0), y(0), z(0), w(0)
  {
  }
  Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_)
  {
  }
  Vec4(const Vec3 &v, float w_) : x(v.x), y(v.y), z(v.z), w(w_)
  {
  }

  Vec4 operator+(const Vec4 &other) const
  {
    return Vec4(x + other.x, y + other.y, z + other.z, w + other.w);
  }

  Vec4 operator*(float scalar) const
  {
    return Vec4(x * scalar, y * scalar, z * scalar, w * scalar);
  }

  float dot(const Vec4 &other) const
  {
    return x * other.x + y * other.y + z * other.z + w * other.w;
  }
};

struct Mat4
{
  float m[4][4];

  Mat4()
  {
    for (int i = 0; i < 4; i++)
    {
      for (int j = 0; j < 4; j++)
      {
        m[i][j] = 0.0f;
      }
    }
  }

  Mat4 operator+(const Mat4 &other) const
  {
    Mat4 result;
    for (int i = 0; i < 4; i++)
    {
      for (int j = 0; j < 4; j++)
      {
        result.m[i][j] = m[i][j] + other.m[i][j];
      }
    }
    return result;
  }

  Mat4 &operator+=(const Mat4 &other)
  {
    for (int i = 0; i < 4; i++)
    {
      for (int j = 0; j < 4; j++)
      {
        m[i][j] += other.m[i][j];
      }
    }
    return *this;
  }

  Vec4 operator*(const Vec4 &v) const
  {
    return Vec4(
        m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z + m[0][3] * v.w,
        m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z + m[1][3] * v.w,
        m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z + m[2][3] * v.w,
        m[3][0] * v.x + m[3][1] * v.y + m[3][2] * v.z + m[3][3] * v.w);
  }

  // Create outer product matrix from vector
  static Mat4 outerProduct(const Vec4 &v)
  {
    Mat4 result;
    for (int i = 0; i < 4; i++)
    {
      for (int j = 0; j < 4; j++)
      {
        result.m[i][j] = (&v.x)[i] * (&v.x)[j];
      }
    }
    return result;
  }

  // Quadric cost calculation: v^T * M * v
  float quadricCost(const Vec4 &v) const
  {
    Vec4 Mv = (*this) * v;
    return v.dot(Mv);
  }
};

struct Mat3
{
  float m[3][3];

  Mat3()
  {
    for (int i = 0; i < 3; i++)
    {
      for (int j = 0; j < 3; j++)
      {
        m[i][j] = 0.0f;
      }
    }
  }

  Mat3(const Mat4 &mat4)
  {
    for (int i = 0; i < 3; i++)
    {
      for (int j = 0; j < 3; j++)
      {
        m[i][j] = mat4.m[i][j];
      }
    }
  }

  float determinant() const
  {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  }

  // Solve 3x3 system using Cramer's rule
  Vec3 solve(const Vec3 &b) const
  {
    float det = determinant();
    if (std::abs(det) < 1e-8f)
    {
      return Vec3(0, 0, 0); // Singular matrix
    }

    // Cramer's rule
    Mat3 mx = *this, my = *this, mz = *this;

    // Replace columns with b vector
    mx.m[0][0] = b.x;
    mx.m[1][0] = b.y;
    mx.m[2][0] = b.z;
    my.m[0][1] = b.x;
    my.m[1][1] = b.y;
    my.m[2][1] = b.z;
    mz.m[0][2] = b.x;
    mz.m[1][2] = b.y;
    mz.m[2][2] = b.z;

    return Vec3(mx.determinant() / det, my.determinant() / det, mz.determinant() / det);
  }
};

struct BoneWeight
{
  std::array<int, 4> boneIndices{};
  std::array<float, 4> weights{};
};

struct Vertex
{
  std::array<float, 3> position{};
  std::array<float, 3> normal{};
  std::array<float, 2> uv{};
  std::array<float, 4> color{};
  BoneWeight boneWeight{};
  bool deleted = false;
};

struct Triangle
{
  std::array<int, 3> vertices;
  bool deleted = false;
};

struct Edge
{
  int v0, v1;
  bool operator==(const Edge &o) const
  {
    return (v0 == o.v0 && v1 == o.v1) || (v0 == o.v1 && v1 == o.v0);
  }
};

struct EdgeHash
{
  size_t operator()(const Edge &e) const
  {
    return std::hash<int>()(std::min(e.v0, e.v1)) ^ (std::hash<int>()(std::max(e.v0, e.v1)) << 1);
  }
};

struct EdgeCollapse
{
  Edge edge;
  float cost;
  Vec4 newPos;
  size_t timestamp;

  bool operator<(const EdgeCollapse &other) const
  {
    return cost > other.cost; // Min-heap
  }
};

struct SimplificationMetrics
{
  float totalError = 0.0f;      // Sum of all collapse costs
  float averageError = 0.0f;    // Average cost per collapse
  float maxError = 0.0f;        // Highest single collapse cost
  size_t collapseCount = 0;     // Number of edge collapses performed
  size_t originalTriangles = 0; // Triangle count before simplification
  size_t finalTriangles = 0;    // Triangle count after simplification
  float reductionRatio = 0.0f;  // Percentage of triangles removed
};

struct SimplifiedMesh
{
  std::vector<Vertex> vertices;
  std::vector<Triangle> triangles;
  SimplificationMetrics metrics;
};

class MeshSimplifierQEM
{
public:
  MeshSimplifierQEM(const std::vector<Vertex> &inputVertices, const std::vector<Triangle> &inputTriangles, const std::unordered_set<Edge, EdgeHash> &protectedEdges)
      : inputVertices(inputVertices), inputTriangles(inputTriangles), protectedEdges(protectedEdges), globalTimestamp(0)
  {
  }

  SimplifiedMesh simplify(size_t targetTriangles)
  {
    // Create working copies
    workingVertices = inputVertices;
    workingTriangles = inputTriangles;

    // Initialize metrics
    metrics = SimplificationMetrics();
    metrics.originalTriangles = triangleCount();

    // Add deleted flags to working copies
    for (auto &v : workingVertices)
      v.deleted = false;
    for (auto &t : workingTriangles)
      t.deleted = false;

    computeVertexQuadrics();
    buildEdgeSet();
    populatePriorityQueue();

    while (triangleCount() > targetTriangles && !edgeQueue.empty())
    {
      if (!processNextCollapse())
        break;
    }

    // Finalize metrics
    metrics.finalTriangles = triangleCount();
    metrics.reductionRatio = metrics.originalTriangles > 0 ? (1.0f - float(metrics.finalTriangles) / float(metrics.originalTriangles)) : 0.0f;

    if (metrics.collapseCount > 0)
    {
      metrics.averageError = metrics.totalError / float(metrics.collapseCount);
    }

    return createCompactedMesh();
  }

private:
  const std::vector<Vertex> &inputVertices;
  const std::vector<Triangle> &inputTriangles;
  std::vector<Vertex> workingVertices;
  std::vector<Triangle> workingTriangles;
  std::unordered_set<Edge, EdgeHash> protectedEdges;
  std::vector<Mat4> vertexQuadrics;
  std::unordered_set<Edge, EdgeHash> activeEdges;
  std::priority_queue<EdgeCollapse> edgeQueue;
  std::unordered_map<Edge, size_t, EdgeHash> edgeTimestamps;
  size_t globalTimestamp;
  SimplificationMetrics metrics;

  size_t triangleCount() const
  {
    size_t count = 0;
    for (const auto &tri : workingTriangles)
    {
      if (!tri.deleted)
        count++;
    }
    return count;
  }

  void computeVertexQuadrics()
  {
    vertexQuadrics.assign(workingVertices.size(), Mat4());

    for (const auto &tri : workingTriangles)
    {
      if (tri.deleted)
        continue;

      const auto &v0 = workingVertices[tri.vertices[0]];
      const auto &v1 = workingVertices[tri.vertices[1]];
      const auto &v2 = workingVertices[tri.vertices[2]];

      Vec3 p0(v0.position[0], v0.position[1], v0.position[2]);
      Vec3 p1(v1.position[0], v1.position[1], v1.position[2]);
      Vec3 p2(v2.position[0], v2.position[1], v2.position[2]);

      Vec3 n = (p1 - p0).cross(p2 - p0).normalized();
      float d = -n.dot(p0);

      Vec4 plane(n.x, n.y, n.z, d);
      Mat4 Kp = Mat4::outerProduct(plane);

      for (int vi : tri.vertices)
      {
        vertexQuadrics[vi] += Kp;
      }
    }
  }

  void buildEdgeSet()
  {
    activeEdges.clear();

    for (const auto &tri : workingTriangles)
    {
      if (tri.deleted)
        continue;

      for (int i = 0; i < 3; i++)
      {
        int v1 = tri.vertices[i];
        int v2 = tri.vertices[(i + 1) % 3];
        Edge e{std::min(v1, v2), std::max(v1, v2)};

        if (protectedEdges.count(e) == 0)
        {
          activeEdges.insert(e);
        }
      }
    }
  }

  void populatePriorityQueue()
  {
    edgeTimestamps.clear();

    while (!edgeQueue.empty())
      edgeQueue.pop();

    for (const Edge &e : activeEdges)
    {
      EdgeCollapse collapse;
      collapse.edge = e;
      collapse.cost = computeEdgeCost(e, collapse.newPos);
      collapse.timestamp = globalTimestamp;

      edgeTimestamps[e] = globalTimestamp;
      edgeQueue.push(collapse);
    }

    globalTimestamp++;
  }

  bool processNextCollapse()
  {
    while (!edgeQueue.empty())
    {
      EdgeCollapse collapse = edgeQueue.top();
      edgeQueue.pop();

      if (isVertexDeleted(collapse.edge.v0) || isVertexDeleted(collapse.edge.v1))
      {
        continue;
      }

      auto it = edgeTimestamps.find(collapse.edge);
      if (it != edgeTimestamps.end() && it->second != collapse.timestamp)
      {
        continue;
      }

      // Update metrics before performing collapse
      metrics.totalError += collapse.cost;
      metrics.maxError = std::max(metrics.maxError, collapse.cost);
      metrics.collapseCount++;

      performCollapse(collapse.edge, collapse.newPos);
      updateAffectedEdges(collapse.edge);
      return true;
    }
    return false;
  }

  float computeEdgeCost(const Edge &e, Vec4 &outPos)
  {
    if (isVertexDeleted(e.v0) || isVertexDeleted(e.v1))
    {
      return std::numeric_limits<float>::max();
    }

    Mat4 Q = vertexQuadrics[e.v0] + vertexQuadrics[e.v1];

    // Extract 3x3 upper-left submatrix and right-hand side
    Mat3 Q3(Q);
    Vec3 b(-Q.m[0][3], -Q.m[1][3], -Q.m[2][3]);

    if (Q3.determinant() > 1e-6f)
    {
      Vec3 v_opt = Q3.solve(b);
      outPos = Vec4(v_opt.x, v_opt.y, v_opt.z, 1.0f);
    }
    else
    {
      // Fallback to midpoint
      const auto &v0 = workingVertices[e.v0];
      const auto &v1 = workingVertices[e.v1];
      Vec3 mid(0.5f * (v0.position[0] + v1.position[0]), 0.5f * (v0.position[1] + v1.position[1]), 0.5f * (v0.position[2] + v1.position[2]));
      outPos = Vec4(mid.x, mid.y, mid.z, 1.0f);
    }

    return Q.quadricCost(outPos);
  }

  void performCollapse(const Edge &e, const Vec4 &newPos)
  {
    int keep = e.v0, remove = e.v1;

    workingVertices[keep].position = {newPos.x, newPos.y, newPos.z};
    vertexQuadrics[keep] += vertexQuadrics[remove];
    workingVertices[remove].deleted = true;

    for (auto &tri : workingTriangles)
    {
      if (tri.deleted)
        continue;

      for (int &v : tri.vertices)
      {
        if (v == remove)
          v = keep;
      }

      if (tri.vertices[0] == tri.vertices[1] || tri.vertices[1] == tri.vertices[2] || tri.vertices[2] == tri.vertices[0])
      {
        tri.deleted = true;
      }
    }
  }

  void updateAffectedEdges(const Edge &collapsedEdge)
  {
    std::unordered_set<Edge, EdgeHash> affectedEdges;

    for (const auto &tri : workingTriangles)
    {
      if (tri.deleted)
        continue;

      bool hasCollapsedVertex = false;
      for (int v : tri.vertices)
      {
        if (v == collapsedEdge.v0 || v == collapsedEdge.v1)
        {
          hasCollapsedVertex = true;
          break;
        }
      }

      if (hasCollapsedVertex)
      {
        for (int i = 0; i < 3; i++)
        {
          int v1 = tri.vertices[i];
          int v2 = tri.vertices[(i + 1) % 3];
          Edge e{std::min(v1, v2), std::max(v1, v2)};

          if (protectedEdges.count(e) == 0 && !isVertexDeleted(e.v0) && !isVertexDeleted(e.v1))
          {
            affectedEdges.insert(e);
          }
        }
      }
    }

    for (const Edge &e : affectedEdges)
    {
      EdgeCollapse collapse;
      collapse.edge = e;
      collapse.cost = computeEdgeCost(e, collapse.newPos);
      collapse.timestamp = globalTimestamp;

      edgeTimestamps[e] = globalTimestamp;
      edgeQueue.push(collapse);
    }

    globalTimestamp++;
  }

  bool isVertexDeleted(int vertexIndex) const
  {
    return vertexIndex < 0 || vertexIndex >= workingVertices.size() || workingVertices[vertexIndex].deleted;
  }

  SimplifiedMesh createCompactedMesh()
  {
    std::vector<int> vertexMap(workingVertices.size(), -1);
    std::vector<Vertex> newVertices;

    for (size_t i = 0; i < workingVertices.size(); i++)
    {
      if (!workingVertices[i].deleted)
      {
        vertexMap[i] = newVertices.size();
        Vertex newVertex = workingVertices[i];
        newVertex.deleted = false; // Clean up flag
        newVertices.push_back(newVertex);
      }
    }

    std::vector<Triangle> newTriangles;
    for (const auto &tri : workingTriangles)
    {
      if (!tri.deleted)
      {
        Triangle newTri;
        newTri.vertices[0] = vertexMap[tri.vertices[0]];
        newTri.vertices[1] = vertexMap[tri.vertices[1]];
        newTri.vertices[2] = vertexMap[tri.vertices[2]];
        newTri.deleted = false; // Clean up flag

        if (newTri.vertices[0] >= 0 && newTri.vertices[1] >= 0 && newTri.vertices[2] >= 0)
        {
          newTriangles.push_back(newTri);
        }
      }
    }

    return {std::move(newVertices), std::move(newTriangles), metrics};
  }
};