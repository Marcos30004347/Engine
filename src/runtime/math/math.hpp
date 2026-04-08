#pragma once

#include <cmath>
#include <cstdint>
#include <type_traits>

namespace math
{
constexpr float PI = 3.14159265358979323846f;
constexpr float HALF_PI = PI * 0.5f;
constexpr float TWO_PI = PI * 2.0f;

inline float radians(float degrees)
{
  return degrees * (PI / 180.0f);
}

inline float degrees(float radians)
{
  return radians * (180.0f / PI);
}

// ============================================================================
// Type traits
// ============================================================================

template <typename T> struct is_valid_math_type
{
  static constexpr bool value = std::is_same<T, float>::value || std::is_same<T, int32_t>::value || std::is_same<T, uint32_t>::value;
};

// ============================================================================
// Vector (generic N)
// ============================================================================

template <typename T, size_t N> class Vector
{
  static_assert(is_valid_math_type<T>::value, "Vector only supports float, int32_t, or uint32_t");

public:
  T data[N]{};

  Vector() = default;

  explicit Vector(T v)
  {
    for (size_t i = 0; i < N; ++i)
      data[i] = v;
  }

  T &operator[](size_t i)
  {
    return data[i];
  }
  const T &operator[](size_t i) const
  {
    return data[i];
  }

  Vector operator+(const Vector &o) const
  {
    Vector r;
    for (size_t i = 0; i < N; ++i)
      r[i] = data[i] + o[i];
    return r;
  }

  Vector operator-(const Vector &o) const
  {
    Vector r;
    for (size_t i = 0; i < N; ++i)
      r[i] = data[i] - o[i];
    return r;
  }

  Vector operator*(T s) const
  {
    Vector r;
    for (size_t i = 0; i < N; ++i)
      r[i] = data[i] * s;
    return r;
  }

  T dot(const Vector &o) const
  {
    T r = T(0);
    for (size_t i = 0; i < N; ++i)
      r += data[i] * o[i];
    return r;
  }

  T length() const
  {
    static_assert(std::is_same<T, float>::value, "length() only valid for float vectors");
    return std::sqrt(dot(*this));
  }

  Vector normalize() const
  {
    static_assert(std::is_same<T, float>::value, "normalize() only valid for float vectors");
    T len = length();
    return (len == T(0)) ? *this : (*this) * (T(1) / len);
  }
};

// ============================================================================
// Vector specializations
// ============================================================================

template <typename T> class Vector<T, 3>
{
public:
  T data[3]{};

  Vector() = default;
  explicit Vector(T v) : data{v, v, v}
  {
  }
  Vector(T x, T y, T z) : data{x, y, z}
  {
  }

  T &x()
  {
    return data[0];
  }
  T &y()
  {
    return data[1];
  }
  T &z()
  {
    return data[2];
  }
  const T &x() const
  {
    return data[0];
  }
  const T &y() const
  {
    return data[1];
  }
  const T &z() const
  {
    return data[2];
  }

  T &operator[](size_t i)
  {
    return data[i];
  }
  const T &operator[](size_t i) const
  {
    return data[i];
  }

  Vector operator+(const Vector &o) const
  {
    return {data[0] + o[0], data[1] + o[1], data[2] + o[2]};
  }

  Vector operator-(const Vector &o) const
  {
    return {data[0] - o[0], data[1] - o[1], data[2] - o[2]};
  }

  Vector operator*(T s) const
  {
    return {data[0] * s, data[1] * s, data[2] * s};
  }

  T dot(const Vector &o) const
  {
    return data[0] * o[0] + data[1] * o[1] + data[2] * o[2];
  }

  Vector cross(const Vector &o) const
  {
    return {data[1] * o[2] - data[2] * o[1], data[2] * o[0] - data[0] * o[2], data[0] * o[1] - data[1] * o[0]};
  }

  T length() const
  {
    static_assert(std::is_same<T, float>::value, "length() only valid for float vectors");
    return std::sqrt(dot(*this));
  }

  Vector normalize() const
  {
    static_assert(std::is_same<T, float>::value, "normalize() only valid for float vectors");
    T len = length();
    return (len == T(0)) ? *this : (*this) * (T(1) / len);
  }
};

template <typename T> class Vector<T, 4>
{
public:
  T data[4]{};

  Vector() = default;
  explicit Vector(T v) : data{v, v, v, v}
  {
  }
  Vector(T x, T y, T z, T w) : data{x, y, z, w}
  {
  }

  T &operator[](size_t i)
  {
    return data[i];
  }
  const T &operator[](size_t i) const
  {
    return data[i];
  }

  T dot(const Vector &o) const
  {
    return data[0] * o[0] + data[1] * o[1] + data[2] * o[2] + data[3] * o[3];
  }
};

// ============================================================================
// Matrix (column-major, Vulkan-style)
// ============================================================================

template <typename T, size_t Rows, size_t Cols> class Matrix
{
  static_assert(is_valid_math_type<T>::value, "Matrix only supports float, int32_t, or uint32_t");

public:
  T data[Rows * Cols]{};

  T &at(size_t row, size_t col)
  {
    return data[col * Rows + row];
  }

  const T &at(size_t row, size_t col) const
  {
    return data[col * Rows + row];
  }

  static Matrix identity()
  {
    static_assert(Rows == Cols, "Identity requires square matrix");
    Matrix m;
    for (size_t i = 0; i < Rows; ++i)
      m.at(i, i) = T(1);
    return m;
  }

  Matrix operator*(const Matrix<T, Cols, Cols> &o) const
  {
    Matrix r;
    for (size_t c = 0; c < Cols; ++c)
      for (size_t r0 = 0; r0 < Rows; ++r0)
      {
        T sum = T(0);
        for (size_t k = 0; k < Cols; ++k)
          sum += at(r0, k) * o.at(k, c);
        r.at(r0, c) = sum;
      }
    return r;
  }

  Vector<T, Rows> operator*(const Vector<T, Cols> &v) const
  {
    Vector<T, Rows> r;
    for (size_t i = 0; i < Rows; ++i)
    {
      T sum = T(0);
      for (size_t j = 0; j < Cols; ++j)
        sum += at(i, j) * v[j];
      r[i] = sum;
    }
    return r;
  }
};

// ============================================================================
// Mat4 specialization
// ============================================================================
template <typename T> class Quaternion;

template <typename T> class Matrix<T, 4, 4>
{
public:
  T data[16]{};

  T &at(size_t r, size_t c)
  {
    return data[c * 4 + r];
  }
  const T &at(size_t r, size_t c) const
  {
    return data[c * 4 + r];
  }

  static Matrix identity()
  {
    Matrix m;
    m.at(0, 0) = m.at(1, 1) = m.at(2, 2) = m.at(3, 3) = T(1);
    return m;
  }

  static Matrix fromQuaternion(const Quaternion<T> &q)
  {
    return q.toMatrix();
  }

  static Matrix translation(const Vector<T, 3> &t)
  {
    Matrix m = identity();
    m.at(0, 3) = t.x();
    m.at(1, 3) = t.y();
    m.at(2, 3) = t.z();
    return m;
  }

  static Matrix scale(const Vector<T, 3> &s)
  {
    Matrix m = identity();
    m.at(0, 0) = s.x();
    m.at(1, 1) = s.y();
    m.at(2, 2) = s.z();
    return m;
  }

  static Matrix translate(const Vector<T, 3> &t)
  {
    return translation(t);
  }

  // Orthographic projection matching the perspective matrix conventions:
  //   - right-hand coordinate system, camera looks along -Z
  //   - Vulkan depth range [0, 1] (or reverse-Z: near→1, far→0)
  //   - Y is NOT flipped here; shaders handle the Vulkan Y-flip
  static Matrix orthographic(T left, T right, T bottom, T top, T near, T far, bool reverseZ)
  {
    static_assert(std::is_same<T, float>::value, "float only");

    Matrix m{};
    m.at(0, 0) = T(2) / (right - left);
    m.at(1, 1) = T(2) / (top - bottom);
    m.at(0, 3) = -(right + left) / (right - left);
    m.at(1, 3) = -(top + bottom) / (top - bottom);
    m.at(3, 3) = T(1);

    if (reverseZ)
    {
      // near→1, far→0
      m.at(2, 2) = T(1) / (far - near);
      m.at(2, 3) = far / (far - near);
    }
    else
    {
      // near→0, far→1
      m.at(2, 2) = T(1) / (near - far);
      m.at(2, 3) = near / (near - far);
    }

    return m;
  }

  static Matrix perspective(T fov, T aspect, T near, T far, bool reverseZ)
  {
    static_assert(std::is_same<T, float>::value, "float only");

    Matrix m{};
    T tanHalf = std::tan(fov * T(0.5));

    m.at(0, 0) = T(1) / (aspect * tanHalf);
    m.at(1, 1) = T(1) / tanHalf;
    m.at(3, 2) = T(-1);

    if (reverseZ)
    {
      // Vulkan reverse-Z, depth range [0, 1]
      // m.at(2, 2) = near / (far - near);
      // m.at(2, 3) = (far * near) / (far - near);
      m.at(2, 2) = 0.0f;
      m.at(2, 3) = near;
    }
    else
    {
      // Normal Z
      m.at(2, 2) = far / (near - far);
      m.at(2, 3) = (far * near) / (near - far);
    }

    return m;
  }

  static Matrix lookAt(const Vector<T, 3> &eye, const Vector<T, 3> &target, const Vector<T, 3> &up)
  {
    Vector<T, 3> f = (target - eye).normalize();
    Vector<T, 3> s = f.cross(up).normalize();
    Vector<T, 3> u = s.cross(f);

    Matrix m = identity();
    // Row vectors of the rotation part go into columns of the matrix
    m.at(0, 0) = s.x();
    m.at(0, 1) = s.y();
    m.at(0, 2) = s.z();
    m.at(1, 0) = u.x();
    m.at(1, 1) = u.y();
    m.at(1, 2) = u.z();
    m.at(2, 0) = -f.x();
    m.at(2, 1) = -f.y();
    m.at(2, 2) = -f.z();
    // Translation in column 3
    m.at(0, 3) = -s.dot(eye);
    m.at(1, 3) = -u.dot(eye);
    m.at(2, 3) = f.dot(eye);
    return m;
  }

  Matrix operator*(const Matrix &o) const
  {
    Matrix r{};
    for (size_t c = 0; c < 4; ++c)
    {
      for (size_t r0 = 0; r0 < 4; ++r0)
      {
        T sum = T(0);
        for (size_t k = 0; k < 4; ++k)
          sum += at(r0, k) * o.at(k, c);
        r.at(r0, c) = sum;
      }
    }
    return r;
  }

  Vector<T, 4> operator*(const Vector<T, 4> &v) const
  {
    Vector<T, 4> r{};
    for (size_t i = 0; i < 4; ++i)
    {
      r[i] = at(i, 0) * v[0] + at(i, 1) * v[1] + at(i, 2) * v[2] + at(i, 3) * v[3];
    }
    return r;
  }

  Matrix inverse() const
  {
    static_assert(std::is_same<T, float>::value, "float only");
    T s0 = at(0,0)*at(1,1) - at(1,0)*at(0,1);
    T s1 = at(0,0)*at(1,2) - at(1,0)*at(0,2);
    T s2 = at(0,0)*at(1,3) - at(1,0)*at(0,3);
    T s3 = at(0,1)*at(1,2) - at(1,1)*at(0,2);
    T s4 = at(0,1)*at(1,3) - at(1,1)*at(0,3);
    T s5 = at(0,2)*at(1,3) - at(1,2)*at(0,3);
    T c5 = at(2,2)*at(3,3) - at(3,2)*at(2,3);
    T c4 = at(2,1)*at(3,3) - at(3,1)*at(2,3);
    T c3 = at(2,1)*at(3,2) - at(3,1)*at(2,2);
    T c2 = at(2,0)*at(3,3) - at(3,0)*at(2,3);
    T c1 = at(2,0)*at(3,2) - at(3,0)*at(2,2);
    T c0 = at(2,0)*at(3,1) - at(3,0)*at(2,1);
    T invDet = T(1) / (s0*c5 - s1*c4 + s2*c3 + s3*c2 - s4*c1 + s5*c0);
    Matrix inv{};
    inv.at(0,0) = ( at(1,1)*c5 - at(1,2)*c4 + at(1,3)*c3) * invDet;
    inv.at(0,1) = (-at(0,1)*c5 + at(0,2)*c4 - at(0,3)*c3) * invDet;
    inv.at(0,2) = ( at(3,1)*s5 - at(3,2)*s4 + at(3,3)*s3) * invDet;
    inv.at(0,3) = (-at(2,1)*s5 + at(2,2)*s4 - at(2,3)*s3) * invDet;
    inv.at(1,0) = (-at(1,0)*c5 + at(1,2)*c2 - at(1,3)*c1) * invDet;
    inv.at(1,1) = ( at(0,0)*c5 - at(0,2)*c2 + at(0,3)*c1) * invDet;
    inv.at(1,2) = (-at(3,0)*s5 + at(3,2)*s2 - at(3,3)*s1) * invDet;
    inv.at(1,3) = ( at(2,0)*s5 - at(2,2)*s2 + at(2,3)*s1) * invDet;
    inv.at(2,0) = ( at(1,0)*c4 - at(1,1)*c2 + at(1,3)*c0) * invDet;
    inv.at(2,1) = (-at(0,0)*c4 + at(0,1)*c2 - at(0,3)*c0) * invDet;
    inv.at(2,2) = ( at(3,0)*s4 - at(3,1)*s2 + at(3,3)*s0) * invDet;
    inv.at(2,3) = (-at(2,0)*s4 + at(2,1)*s2 - at(2,3)*s0) * invDet;
    inv.at(3,0) = (-at(1,0)*c3 + at(1,1)*c1 - at(1,2)*c0) * invDet;
    inv.at(3,1) = ( at(0,0)*c3 - at(0,1)*c1 + at(0,2)*c0) * invDet;
    inv.at(3,2) = (-at(3,0)*s3 + at(3,1)*s1 - at(3,2)*s0) * invDet;
    inv.at(3,3) = ( at(2,0)*s3 - at(2,1)*s1 + at(2,2)*s0) * invDet;
    return inv;
  }
};

// ============================================================================
// Quaternion
// ============================================================================

template <typename T> class Quaternion
{
  static_assert(std::is_same<T, float>::value, "Quaternion is float-only");

public:
  T x = 0, y = 0, z = 0, w = 1;

  Quaternion() = default;
  Quaternion(T x, T y, T z, T w) : x(x), y(y), z(z), w(w)
  {
  }

  static Quaternion fromAxisAngle(const Vector<T, 3> &axis, T angle)
  {
    T h = angle * T(0.5);
    T s = std::sin(h);
    auto a = axis.normalize();
    return {a.x() * s, a.y() * s, a.z() * s, std::cos(h)};
  }

  static Quaternion identity()
  {
    return Quaternion(T(0), T(0), T(0), T(1));
  }

  static Quaternion fromEuler(T pitch, T yaw, T roll)
  {
    T cy = std::cos(yaw * T(0.5));
    T sy = std::sin(yaw * T(0.5));
    T cp = std::cos(pitch * T(0.5));
    T sp = std::sin(pitch * T(0.5));
    T cr = std::cos(roll * T(0.5));
    T sr = std::sin(roll * T(0.5));

    Quaternion q;
    q.w = cr * cp * cy + sr * sp * sy;
    q.x = sr * cp * cy - cr * sp * sy;
    q.y = cr * sp * cy + sr * cp * sy;
    q.z = cr * cp * sy - sr * sp * cy;
    return q;
  }

  Quaternion normalize() const
  {
    T len = std::sqrt(x * x + y * y + z * z + w * w);
    if (len == T(0))
      return *this;
    T inv = T(1) / len;
    return Quaternion(x * inv, y * inv, z * inv, w * inv);
  }

  Vector<T, 3> operator*(const Vector<T, 3> &v) const
  {
    Vector<T, 3> qv(x, y, z);
    Vector<T, 3> uv = qv.cross(v);
    Vector<T, 3> uuv = qv.cross(uv);
    return v + (uv * w + uuv) * T(2);
  }

  Matrix<T, 4, 4> toMatrix() const
  {
    Matrix<T, 4, 4> m = Matrix<T, 4, 4>::identity();

    T xx = x * x, yy = y * y, zz = z * z;
    T xy = x * y, xz = x * z, yz = y * z;
    T wx = w * x, wy = w * y, wz = w * z;

    m.at(0, 0) = 1 - 2 * (yy + zz);
    m.at(1, 0) = 2 * (xy + wz);
    m.at(2, 0) = 2 * (xz - wy);

    m.at(0, 1) = 2 * (xy - wz);
    m.at(1, 1) = 1 - 2 * (xx + zz);
    m.at(2, 1) = 2 * (yz + wx);

    m.at(0, 2) = 2 * (xz + wy);
    m.at(1, 2) = 2 * (yz - wx);
    m.at(2, 2) = 1 - 2 * (xx + yy);

    return m;
  }
};

// ============================================================================
// Aliases
// ============================================================================

using Vec2f = Vector<float, 2>;
using Vec3f = Vector<float, 3>;
using Vec4f = Vector<float, 4>;

using Mat4f = Matrix<float, 4, 4>;
using Mat3f = Matrix<float, 3, 3>;

using Quatf = Quaternion<float>;

} // namespace math
