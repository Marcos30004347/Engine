#include "procedural/ConstrainedDelaunay.hpp"

#include "procedural/Geometry.hpp"

#include <cmath>





namespace procedural
{

// Loops with no cross-iteration dependency. On CPU this is an ordinary `for`;
// each is a candidate to become a compute dispatch, and the marker keeps them
// identifiable once the bodies grow.
#define kForInParallel for

namespace
{

// Consecutive passes with no point or constraint newly inserted after which the
// run is declared stalled. Convergent inputs settle in single-digit passes, so
// this only fires on a genuine livelock.
constexpr uint64_t kStallLimit = 8u;

// Distance below which two vertices are treated as the same point. Used only
// where the question is genuinely "is this the same vertex", never to decide an
// orientation - those go through the exact predicates.
constexpr double kVertexCoincidenceEpsilon = 1.0e-11;


#include <math.h>
#include <stdio.h>
#include <stdlib.h>


static const int edge_jump[3][3] = { 0, 1, 2, 1, 2, 0, 2, 0, 1 };

// can be removed in case the code is executed in gpu compute shader, or opencl/cuda
#define for_in_parallel for 
#define global_barrier() ;
#define threads_wait_all(a) a; global_barrier(); // 
//#define fabs(a) (a < 0 ? -a : a)

} // namespace

// Element accessors over the parallel arrays. Index arithmetic lives here and
// nowhere else, so the passes below read as the algorithm rather than as
// offsets.

Point2 ConstrainedDelaunayTriangulator::vertexCoordinates(const int64_t point) const
{
  return Point2(mPointCoordinates[point * 2], mPointCoordinates[point * 2 + 1]);
}

void ConstrainedDelaunayTriangulator::setVertexCoordinates(const int64_t point, const Point2 &value)
{
  mPointCoordinates[point * 2] = value.x;
  mPointCoordinates[point * 2 + 1] = value.y;
}

int64_t ConstrainedDelaunayTriangulator::triangleVertexToInsert(const int64_t triangle) const
{
  return mTriangleVertexToInsert[triangle];
}

void ConstrainedDelaunayTriangulator::setTriangleVertexToInsert(const int64_t triangle, const int64_t vertex)
{
  mTriangleVertexToInsert[triangle] = vertex;
}

int8_t ConstrainedDelaunayTriangulator::vertexIsIncluded(const int64_t point) const
{
  return mPointIsIncluded[point];
}

void ConstrainedDelaunayTriangulator::setVertexIsIncluded(const int64_t point, const int8_t included)
{
  mPointIsIncluded[point] = included;
}

int64_t ConstrainedDelaunayTriangulator::pointContainingTriangle(const int64_t point) const
{
  return mPointContainingTriangle[point];
}

void ConstrainedDelaunayTriangulator::setPointContainingTriangle(const int64_t point, const int64_t triangle)
{
  mPointContainingTriangle[point] = triangle;
}

int64_t ConstrainedDelaunayTriangulator::triangleVertex(const int64_t triangle, const int64_t corner) const
{
  return mTriangleVertices[triangle * 3 + corner];
}

void ConstrainedDelaunayTriangulator::setTriangleVertex(const int64_t triangle, const int64_t corner, const int64_t vertex)
{
  mTriangleVertices[triangle * 3 + corner] = vertex;
}

// Neighbours are stored six per triangle: the current three followed by the
// three the relink pass is about to install. Writing the new set beside the old
// is what lets every triangle be updated independently, which is the property a
// dispatch needs.
int64_t ConstrainedDelaunayTriangulator::triangleNeighbour(const int64_t triangle, const int64_t corner) const
{
  return mTriangleNeighbours[triangle * 6 + corner];
}

void ConstrainedDelaunayTriangulator::setTriangleNeighbour(const int64_t triangle, const int64_t corner, const int64_t value)
{
  mTriangleNeighbours[triangle * 6 + corner] = value;
}

int64_t ConstrainedDelaunayTriangulator::triangleFutureNeighbour(const int64_t triangle, const int64_t corner) const
{
  return mTriangleNeighbours[triangle * 6 + 3 + corner];
}

void ConstrainedDelaunayTriangulator::setTriangleFutureNeighbour(const int64_t triangle, const int64_t corner, const int64_t value)
{
  mTriangleNeighbours[triangle * 6 + 3 + corner] = value;
}

int8_t ConstrainedDelaunayTriangulator::triangleEdgeState(const int64_t triangle, const int64_t corner) const
{
  return mTriangleEdgeState[triangle * 3 + corner];
}

void ConstrainedDelaunayTriangulator::setTriangleEdgeState(const int64_t triangle, const int64_t corner, const int8_t state)
{
  mTriangleEdgeState[triangle * 3 + corner] = state;
}

int8_t ConstrainedDelaunayTriangulator::triangleEdgeToFlip(const int64_t triangle) const
{
  return mTriangleEdgeToFlip[triangle];
}

void ConstrainedDelaunayTriangulator::setTriangleEdgeToFlip(const int64_t triangle, const int8_t edge)
{
  mTriangleEdgeToFlip[triangle] = edge;
}

int64_t ConstrainedDelaunayTriangulator::triangleSegmentId(const int64_t triangle) const
{
  return mTriangleSegmentIndex[triangle];
}

void ConstrainedDelaunayTriangulator::setTriangleSegmentId(const int64_t triangle, const int64_t segment)
{
  mTriangleSegmentIndex[triangle] = segment;
}

int64_t ConstrainedDelaunayTriangulator::segmentVertex(const int64_t segment, const int64_t which) const
{
  return mSegmentPointIndices[segment * 2 + which];
}

void ConstrainedDelaunayTriangulator::setSegmentVertex(const int64_t segment, const int64_t which, const int64_t vertex)
{
  mSegmentPointIndices[segment * 2 + which] = vertex;
}

int8_t ConstrainedDelaunayTriangulator::segmentIsIncluded(const int64_t segment) const
{
  return mSegmentIsIncluded[segment];
}

void ConstrainedDelaunayTriangulator::setSegmentIsIncluded(const int64_t segment, const int8_t included)
{
  mSegmentIsIncluded[segment] = included;
}

namespace
{


int inside_bounding_box(const Point2 v1, const Point2 v2, const Point2 v) {
	int inside = 0;

	if (((v1.x <= v.x && v.x <= v2.x) || (v2.x <= v.x && v.x <= v1.x)) && ((v1.y <= v.y && v.y <= v2.y) || (v2.y <= v.y && v.y <= v1.y))) {
		inside = 1;
	}

	return inside;
}
int seg1(int j)
{
	int s = j + 1;
	if (s == 3)
		s = 0;
	return s;
}

//TODO: remove

int seg2(int j)
{
	int s = j - 1;
	if (s == -1)
		s = 2;
	return s;
}

int vertex_contained(Point2 v1, Point2 v2, Point2 v3, Point2 v)
{
	int contained = 0;

	if ((fabs(v1.x - v.x) < kVertexCoincidenceEpsilon && fabs(v1.y - v.y) < kVertexCoincidenceEpsilon) ||
		(fabs(v2.x - v.x) < kVertexCoincidenceEpsilon && fabs(v2.y - v.y) < kVertexCoincidenceEpsilon) ||
		(fabs(v3.x - v.x) < kVertexCoincidenceEpsilon && fabs(v3.y - v.y) < kVertexCoincidenceEpsilon))
		contained = 1;

	return contained;
}

int get_edge(int b, int c, int d) {
	int e = 0;

	if (b == d) {
		e = 1;
	}
	else if (c == d) {
		e = 2;
	}

	return e;
}

} // namespace

void ConstrainedDelaunayTriangulator::locationStep() {
	for_in_parallel(uint64_t p = 0; p < mPointsCount; p++) {
		int64_t t = pointContainingTriangle(p);

		uint64_t upperbound = 120;
		uint64_t count = 0;
		char is_vertex = 0;

		Point2 v = vertexCoordinates(p);

		int64_t n;

		if (vertexIsIncluded(p)) {
			continue;
		}

		int contained = 0;

		while (contained == 0 && count < upperbound) {
			count = count + 1;

			Point2 a = vertexCoordinates(triangleVertex(t, 0));
			Point2 b = vertexCoordinates(triangleVertex(t, 1));
			Point2 c = vertexCoordinates(triangleVertex(t, 2));

			is_vertex = vertex_contained(a, b, c, v);

			if (!is_vertex) {
				double l1 = orient2d(b, c, v);
				double l2 = orient2d(c, a, v);
				double l3 = orient2d(a, b, v);


				if (l3 > 0 && l1 > 0 && l2 > 0) {
					contained = 1;
				}

				if (contained == 0 && l3 == 0 && l1 >= 0 && l2 >= 0) {
					n = triangleNeighbour(t, 0);
					if (n < t) {
						t = n;
					}
					contained = 1;
				}

				if (contained == 0 && l1 == 0 && l3 >= 0 && l2 >= 0)
				{
					n = triangleNeighbour(t, 1);
					if (n < t)
						t = n;
					contained = 1;
				}

				if (contained == 0 && l2 == 0 && l3 >= 0 && l1 >= 0)
				{
					n = triangleNeighbour(t, 2);
					if (n < t)
						t = n;
					contained = 1;
				}

				if (contained == 0 && l3 < 0) {
					t = triangleNeighbour(t, 0);
				}
				else if (contained == 0 && l1 < 0) {
					t = triangleNeighbour(t, 1);
				}
				else if (contained == 0 && l2 < 0) {
					t = triangleNeighbour(t, 2);
				}
			}
		}


		if (is_vertex || count == upperbound) {
			setVertexIsIncluded(p, 2);
			setPointContainingTriangle(p, t);
		}
		else {
			setPointContainingTriangle(p, t);

			if (triangleVertexToInsert(t) == -1) {
				setTriangleVertexToInsert(t, p);
				mThereIsPointsToInsert = 1;
			}
			else {

				Point2 a = vertexCoordinates(triangleVertex(t, 0));
				Point2 b = vertexCoordinates(triangleVertex(t, 1));
				Point2 c = vertexCoordinates(triangleVertex(t, 2));

				double area = orient2d(a, b, c);

				if (area > 0) {
					double l01 = orient2d(b, c, v) / area;
					double l02 = orient2d(c, a, v) / area;
					double l03 = orient2d(a, b, v) / area;

					Point2 v2 = vertexCoordinates(triangleVertexToInsert(t));

					double l11 = orient2d(b, c, v2) / area;
					double l12 = orient2d(c, a, v2) / area;
					double l13 = orient2d(a, b, v2) / area;

					double measure_0 = fabs(l01 - 0.3333) + fabs(l02 - 0.3333) + fabs(l03 - 0.3333);
					double measure_1 = fabs(l11 - 0.3333) + fabs(l12 - 0.3333) + fabs(l13 - 0.3333);

					if (measure_0 < measure_1) {
						setTriangleVertexToInsert(t, p);
						mThereIsPointsToInsert = 1;
					}
				}
			}
		}
	}
}


void ConstrainedDelaunayTriangulator::preUpdate() {
	for_in_parallel(uint64_t id = 0; id < mTrianglesCount; id++) {
		// TODO: validate and understand
		if (triangleExists(id) == 0) {
			continue;
		}

		int64_t t = -1;
		int64_t e = -1;
		int64_t p = triangleVertexToInsert(id);

		if (p != -1) {
			// TODO: use loop
			Point2 a0 = vertexCoordinates(triangleVertex(id, 0));
			Point2 a1 = vertexCoordinates(triangleVertex(id, 1));
			Point2 a2 = vertexCoordinates(triangleVertex(id, 2));
			Point2 v = vertexCoordinates(p);

			t = triangleNeighbour(id, 1);
			if (t != -1) {
				e = get_edge(triangleNeighbour(t, 1), triangleNeighbour(t, 2), id);
				setTriangleFutureNeighbour(t, e, 2 * p + 1);
				int8_t aux = triangleEdgeState(id, 1);

				if (aux != 2 && aux != 7) {
					double area = orient2d(a1, a2, v);

					if (inside_bounding_box(a1, a2, v) && area == 0) {
						setTriangleEdgeState(t, e, 7);
						setTriangleSegmentId(t, 1);
					}
					else {
						setTriangleEdgeState(t, e, 1);
					}
				}
			}
			else {
				setTriangleEdgeState(id, 1, 0);
			}

			t = triangleNeighbour(id, 2);
			if (t != -1) {
				e = get_edge(triangleNeighbour(t, 1), triangleNeighbour(t, 2), id);
				setTriangleFutureNeighbour(t, e, 2 * p + 2);
				int8_t aux = triangleEdgeState(id, 2);

				if (aux != 2 && aux != 7) {
					double area = orient2d(a2, a0, v);

					if (inside_bounding_box(a2, a0, v) && area == 0) {
						setTriangleEdgeState(t, e, 7);
						setTriangleSegmentId(t, 1);
					}
					else {
						setTriangleEdgeState(t, e, 1);
					}
				}

			}
			else {
				setTriangleEdgeState(id, 2, 0);
			}

			t = triangleNeighbour(id, 0);
			if (t != -1) {
				e = get_edge(triangleNeighbour(t, 1), triangleNeighbour(t, 2), id);
				setTriangleFutureNeighbour(t, e, id);
				int8_t aux = triangleEdgeState(id, 0);

				if (aux != 2 && aux != 7) {
					double area = orient2d(a0, a1, v);

					if (inside_bounding_box(a0, a1, v) && area == 0) {
						setTriangleEdgeState(t, e, 7);
						setTriangleSegmentId(t, 1);
					}
					else {
						setTriangleEdgeState(t, e, 1);
					}
				}
			}
			else {
				setTriangleEdgeState(id, 0, 0);
			}

			setVertexIsIncluded(p, 1);
		}
	}
}



void ConstrainedDelaunayTriangulator::insertionStep() {
	for_in_parallel(uint64_t id = 0; id < mTrianglesCount; id++) {
		if (triangleExists(id) == 0) {
			continue;
		}
		// TODO: check again
		int64_t p = triangleVertexToInsert(id);

		if (p != -1) {
			int64_t t = -1;

			Point2 v = vertexCoordinates(p);

			Point2 a0 = vertexCoordinates(triangleVertex(id, 0));
			Point2 a1 = vertexCoordinates(triangleVertex(id, 1));
			Point2 a2 = vertexCoordinates(triangleVertex(id, 2));

			setTriangleSegmentId(id, 0);

			t = triangleNeighbour(id, 1);

			if (t != -1) {
				int8_t aux = triangleEdgeState(id, 1);

				if (aux != 2 && aux != 7) {
					double area = orient2d(a1, a2, v);

					if (inside_bounding_box(a1, a2, v) && area == 0) { // collinear
						setTriangleEdgeState(id, 1, 7);
						setTriangleSegmentId(2 * p + 1, 1);
					}
					else {
						setTriangleEdgeState(id, 1, 1);
					}
				}
			}

			t = triangleNeighbour(id, 2);

			if (t != -1) {
				int8_t aux = triangleEdgeState(id, 2);

				if (aux != 2 && aux != 7) {
					double area = orient2d(a2, a0, v);

					if (inside_bounding_box(a2, a0, v) && area == 0) { // collinear
						setTriangleEdgeState(id, 2, 7);
						setTriangleSegmentId(2 * p + 2, 1);
					}
					else {
						setTriangleEdgeState(id, 2, 1);
					}
				}
			}


			t = triangleNeighbour(id, 0);

			if (t != -1) {
				int8_t aux = triangleEdgeState(id, 0);

				if (aux != 2 && aux != 7) {
					double area = orient2d(a0, a1, v);

					if (inside_bounding_box(a0, a1, v) && area == 0) { // collinear
						setTriangleEdgeState(id, 0, 7);
						setTriangleSegmentId(id, 1);
					}
					else {
						setTriangleEdgeState(id, 0, 1);
					}
				}
			}



			// update neighbours
			setTriangleNeighbour(id, 0, triangleFutureNeighbour(id, 0));
			setTriangleNeighbour(id, 1, 2 * p + 1);
			setTriangleNeighbour(id, 2, 2 * p + 2);

			setTriangleNeighbour(2 * p + 1, 0, triangleFutureNeighbour(id, 1));
			setTriangleNeighbour(2 * p + 1, 1, 2 * p + 2);
			setTriangleNeighbour(2 * p + 1, 2, id);

			setTriangleNeighbour(2 * p + 2, 0, triangleFutureNeighbour(id, 2));
			setTriangleNeighbour(2 * p + 2, 1, id);
			setTriangleNeighbour(2 * p + 2, 2, 2 * p + 1);

			// update future neighbours
			setTriangleFutureNeighbour(2 * p + 1, 0, triangleFutureNeighbour(id, 1));
			setTriangleFutureNeighbour(2 * p + 1, 1, 2 * p + 2);
			setTriangleFutureNeighbour(2 * p + 1, 2, id);
			setTriangleFutureNeighbour(2 * p + 2, 0, triangleFutureNeighbour(id, 2));
			setTriangleFutureNeighbour(2 * p + 2, 1, id);
			setTriangleFutureNeighbour(2 * p + 2, 2, 2 * p + 1);
			setTriangleFutureNeighbour(id, 1, 2 * p + 1);
			setTriangleFutureNeighbour(id, 2, 2 * p + 2);



			// update flags
			if (triangleEdgeState(id, 1) > 0) {
				setTriangleEdgeState(2 * p + 1, 0, triangleEdgeState(id, 1));
				setTriangleEdgeState(id, 1, 0);
			}

			if (triangleEdgeState(id, 2) > 0) {
				setTriangleEdgeState(2 * p + 2, 0, triangleEdgeState(id, 2));
				setTriangleEdgeState(id, 2, 0);
			}

			// update containing triangle
			setPointContainingTriangle(triangleVertex(id, 2), 2 * p + 1);

			// update points
			setTriangleVertex(2 * p + 1, 0, triangleVertex(id, 1));
			setTriangleVertex(2 * p + 1, 1, triangleVertex(id, 2));
			setTriangleVertex(2 * p + 1, 2, p);

			setTriangleVertex(2 * p + 2, 0, triangleVertex(id, 2));
			setTriangleVertex(2 * p + 2, 1, triangleVertex(id, 0));
			setTriangleVertex(2 * p + 2, 2, p);

			setTriangleVertex(id, 2, p);
		}
		else {
			// NOTE: Future neighbour needs to be same as neighbour on initialization
			setTriangleNeighbour(id, 0, triangleFutureNeighbour(id, 0));
			setTriangleNeighbour(id, 1, triangleFutureNeighbour(id, 1));
			setTriangleNeighbour(id, 2, triangleFutureNeighbour(id, 2));
		}

		setTriangleVertexToInsert(id, -1);
	}
}

int flipable(const Point2 v1, const Point2 v2, const Point2 v3, const Point2 v)
{
	return (orient2d(v1, v2, v) > 0.0 && orient2d(v1, v3, v) < 0.0) || (orient2d(v, v3, v1) > 0.0 && orient2d(v, v2, v1) < 0.0);
}

void ConstrainedDelaunayTriangulator::releaseFinishedSegmentClaims() {
	// Claims survive flips now, so they have to be released explicitly. A
	// finished segment holding triangles would block every higher-indexed
	// segment, since a claim only ever transfers to a smaller index.
	for_in_parallel(int t = 0; t < mTrianglesCount; t++) {
		if (triangleExists(t) == 0) {
			continue;
		}

		const int64_t claim = triangleSegmentId(t);

		// 0 is unclaimed, 1 marks point insertion; segment claims are the
		// segment index offset by two.
		if (claim <= 1) {
			continue;
		}

		const int64_t segment = claim - 2;

		if (segment < static_cast<int64_t>(mSegmentsCount) && segmentIsIncluded(segment) != 0) {
			setTriangleSegmentId(t, 0);
		}
	}
}

void ConstrainedDelaunayTriangulator::markingStep() {
	releaseFinishedSegmentClaims();

	for_in_parallel(int id = 0; id < mSegmentsCount; id++) {
		int64_t i0 = segmentVertex(id, 0);
		int64_t i1 = segmentVertex(id, 1);

		Point2 vo = vertexCoordinates(i0);
		Point2 vt = vertexCoordinates(i1);

		Point2 a, b, c, d;

		if (vertexIsIncluded(i0) && vertexIsIncluded(i1) && segmentIsIncluded(id) == 0 && segmentVertex(id, 0) != segmentVertex(id, 1)) {
			int isvertex = 0;

			int insert = 1;

			int64_t previous_face = -1;
			int64_t n = -1;
			uint64_t j = 0;

			int64_t id2 = id + 2;
			int fl = 0;

			while (insert && isvertex == 0 && j < 2) {
				int count = 0;
				int found = 0;

				j = j + 1;

				i0 = segmentVertex(id, 0);
				i1 = segmentVertex(id, 1);

				vo = vertexCoordinates(i0);
				vt = vertexCoordinates(i1);

				if (j == 1) { // invert endpoints to move backwards
					int64_t tm = i0;
					i0 = i1;
					i1 = tm;

					Point2 vm = vo;
					vo = vt;
					vt = vm;
				}

				int64_t face = pointContainingTriangle(i0);
				// NOTE:neighbour0 does not need to be checked for swaping, because 
				int64_t edge = get_edge(triangleVertex(face, 1), triangleVertex(face, 2), i0);

				int64_t ib = triangleVertex(face, seg1(edge));
				b = vertexCoordinates(ib);

				int64_t ic = triangleVertex(face, seg2(edge));
				c = vertexCoordinates(ic);

				int upperbound = 100;


				while (face != -1 && found == 0 && isvertex == 0 && (count < upperbound)) { // visit all triangles arround point until the one with the right direction is picked, the triangle fan
					count = count + 1;
					// TODO: assert all triangles are counter clock wise

					if (ib == i1) {
						// it is a segment already included in the triangulation
						setTriangleEdgeState(face, edge, 2);
						setPointContainingTriangle(i0, face);

						n = triangleNeighbour(face, edge);
						edge = get_edge(triangleVertex(n, 1), triangleVertex(n, 2), i1);

						setTriangleEdgeState(n, edge, 2);
						setSegmentIsIncluded(id, 1);

						insert = 0;
						found = 1;
						isvertex = 1;
					}
					else if (orient2d(vo, vt, b) < 0 && orient2d(vo, vt, c) > 0 && orient2d(vo, b, c) > 0) {
						// get the edge with the right direction, this assumes the triangles are all counter clock wise
						found = 1;
						setPointContainingTriangle(i0, face);
						previous_face = face;
						edge = seg1(edge);
						a = vo;
					}

					if (found == 0) {
						face = triangleNeighbour(face, seg2(edge));
						edge = get_edge(triangleVertex(face, 1), triangleVertex(face, 2), i0);
						ib = ic;
						b = c;
						ic = triangleVertex(face, seg2(edge));
						c = vertexCoordinates(ic);
					}
				}


				if (found == 0 && count == upperbound) {
					insert = 0;
				}
				else {
					count = 0;

					if (found) {
						found = 0;

						// get neighbour of edge in the right direction
						n = triangleNeighbour(face, edge);

						// get edge in neighbour
						int64_t edgen = get_edge(triangleNeighbour(n, 1), triangleNeighbour(n, 2), face);

						int64_t iid = triangleVertex(n, seg2(edgen));
						Point2 d = vertexCoordinates(iid);

						while (face != -1 && found == 0 && isvertex == 0 && orient2d(a, b, c) > 0 && count < upperbound) { // straight walk
							count += 1;

							int64_t segface = triangleSegmentId(face);

							if (segface == 0 || (segface > 1 && id2 < segface)) { // choose smaller segment
								setTriangleSegmentId(face, id2);
								segface = id2;
							}

							if (segface == id2) { // segment in t is same being processed, necessary for parallel processing
								if (iid == i1) { // arrived at endpoint
									isvertex = 1;
									found = 1;
									int64_t segn = triangleSegmentId(n);

									if (segn == 0 || (segn > 1 && id2 < segn)) { // choose smaller segment
										setTriangleSegmentId(n, id2);
										segn = id2;
									}

									if (segface == id2 && segn == id2 && edge != triangleEdgeToFlip(face) && flipable(a, b, c, vt)) {
										// both have same segment and e is not marked to be flipped and b and c are at oposite sides in the segment 
										// if e is crossed and not marked to be flipped, e have a point in the segment, so flag it as such in both triangles
										setTriangleEdgeState(face, edge, 3); // x2 priority
										setTriangleEdgeState(n, edgen, 3);
									}
								}
								else {
									double prod = orient2d(vo, vt, a) * orient2d(vo, vt, d);
									// if prod == 1 a and d are at same side of segment so no intersection

									if (flipable(a, b, c, d) && (face == previous_face || prod > 0 || (face != previous_face && fl))) {
										// prod > 0 -> case 1
										// flipable(a, b, c, d) -> a, b, c, d is concave and double intersection
										// note that flipable(a,b,c,d) and prod case 2 of the paper
										// fl means that inverting the current edge will make the previous polygon concave
										// if t != from && fl we have case 3
										int64_t segn = triangleSegmentId(n);

										if (segn == 0 || (segn > 1 && id2 < segn)) {
											// set tge segment of neighbour to smalled segment
											setTriangleSegmentId(n, id2);
											segn = id2;
										}

										if (segface == id2 && segn == id2 && edge != triangleEdgeToFlip(face) && flipable(a, b, c, vt)) {
											//printf("polygon((%f, %f), (%f, %f), (%f, %f))\n", a.x, a.y, b.x, b.y, c.x, c.y);
											//printf("a=(%f, %f)\n", vo.point.x, vo.point.y);
											//printf("b=(%f, %f)\n", vt.point.x, vt.point.y);
											// both have same segment and e is not marked to be flipped and b and c are at oposite sides in the segment (e crosses the segment)
											// if e is crossed and not marked to be flipped, e have a point in the segment, so flag it as such in both triangles
											found = 1;
											setTriangleEdgeState(face, edge, 3);
											setTriangleEdgeState(n, edgen, 3);
										}
									}
									else {
										// pick next direction
										previous_face = face;
										face = n;
										Point2 dd = d;
										if (orient2d(vo, vt, b) < 0 && orient2d(vo, vt, d) > 0)
										{
											edge = seg1(edgen);
											n = triangleNeighbour(face, edge);
											edgen = get_edge(triangleNeighbour(n, 1), triangleNeighbour(n, 2), face);
											iid = triangleVertex(n, seg2(edgen));
											d = vertexCoordinates(iid);
											// the polygon a, b, c, d is concave
											fl = flipable(a, b, c, d);
											a = c;
											c = dd;
										}
										else
										{
											edge = seg2(edgen);
											n = triangleNeighbour(face, edge);
											edgen = get_edge(triangleNeighbour(n, 1), triangleNeighbour(n, 2), face);
											iid = triangleVertex(n, seg2(edgen));
											d = vertexCoordinates(iid);
											// the polygon a, b, c, d is concave
											fl = flipable(a, b, c, d);
											a = b;
											b = dd;
										}
									}
								}
							}
							else {
								found = 1;
							}
						}
					}
				}
			}
		}


		if (vertexIsIncluded(segmentVertex(id, 0)) == 2 || vertexIsIncluded(segmentVertex(id, 1)) == 2 || segmentVertex(id, 0) == segmentVertex(id, 1)) {
			setSegmentIsIncluded(id, 2);
		}
		if (segmentIsIncluded(id) == 0) {
			mThereIsSegmentToInsert = 1;
		}
	}
}


// second part of marking
void ConstrainedDelaunayTriangulator::determineInCircleStep() {
	for_in_parallel(int id = 0; id < mTrianglesCount; id++) {
		if (triangleExists(id) == 0) continue;

		int e[3];
		int fln[3];
		int fl[3];
		int n[3];

		for (int j = 0; j < 3; j++) {
			n[j] = triangleNeighbour(id, j);
			fl[j] = triangleEdgeState(id, j);

			if (n[j] != -1) {
				e[j] = get_edge(triangleNeighbour(n[j], 1), triangleNeighbour(n[j], 2), id);
				fln[j] = triangleEdgeState(n[j], e[j]);
			}
		}

		int64_t segid = triangleSegmentId(id);

		if (segid == 0) {
			Point2 v1 = vertexCoordinates(triangleVertex(id, 0));
			Point2 v2 = vertexCoordinates(triangleVertex(id, 1));
			Point2 v3 = vertexCoordinates(triangleVertex(id, 2));

			Point2 w1, w2, w3;

			for (int j = 0; j < 3; j++) {

				switch (j)
				{
				case 0:
					w1 = v3;
					w2 = v1;
					w3 = v2;
					break;
				case 1:
					w1 = v1;
					w2 = v2;
					w3 = v3;
					break;
				case 2:
					w1 = v2;
					w2 = v3;
					w3 = v1;
					break;
				};

				if (n[j] > id) {

					// candidate to be not local delaunay
					if (fl[j] == 1 || fln[j] == 1) {
						// take the oposite vertice
						Point2 v = vertexCoordinates(triangleVertex(n[j], seg2(e[j])));

						// check marked edges to be local delaunay
						if (inCircle(w1, w2, w3, v) <= 0) {
							setTriangleEdgeState(id, j, 0); // marked with D
							setTriangleEdgeState(n[j], e[j], 0);
						}
					}
				}
			}
		}
		else { // is segment, zero area
			for (int j = 0; j < 3; j++) {
				if (n[j] == -1) continue;

				if (fl[j] == 1 || (fl[j] == 3 && segid != triangleSegmentId(n[j]))) { // edge not crossed by segment
					setTriangleEdgeState(id, j, 0); // x0 priority
					setTriangleEdgeState(n[j], e[j], 0);
				}
			}
		}
	}
}


void ConstrainedDelaunayTriangulator::determineSwapping()
{
	for_in_parallel(int id = 0; id < mTrianglesCount; id++) {

		int ss[8] = { 0, 1, 0, 1, 0, 0, 0, 1 };

		if (triangleExists(id) == 0) continue;

		int8_t fl[3];

		setTriangleEdgeToFlip(id, 3);

		for (char j = 0; j < 3; j++) {
			fl[j] = triangleEdgeState(id, j);
		}

		int count = ss[fl[0]] + ss[fl[1]] + ss[fl[2]];
		int found = 0;

		int ef;
		int n;
		int ne;

		if (count != 0) {
			int j = 0;
			while (j < 3 && !found) {
				if (ss[fl[j]] == 1) {
					n = triangleNeighbour(id, j);
					ne = get_edge(triangleNeighbour(n, 1), triangleNeighbour(n, 2), id);

					int8_t fn[3];

					fn[0] = triangleEdgeState(n, 0);
					fn[1] = triangleEdgeState(n, 1);
					fn[2] = triangleEdgeState(n, 2);

					if ((fn[ne] == fl[j]) && ((ss[fn[0]] + ss[fn[1]] + ss[fn[2]]) == 1) && (count >= 2 || (count == 1 && id < n))) {
						ef = j;
						found = 1;
					}
				}
				j++;
			}
		}

		if (found == 1) {
			if (id < n || fl[ef] == 1) {
				setTriangleEdgeToFlip(id, ef);
			}
			else {
				setTriangleEdgeToFlip(n, ne);
			}
		}
	}
}

void ConstrainedDelaunayTriangulator::swapEdges() {
	for_in_parallel(int id = 0; id < mTrianglesCount; id++) {
		if (triangleExists(id) == 0) continue;

		int ets = triangleEdgeToFlip(id);

		if (ets < 3) {
			// update neighbours of adjacent triangles to triangles that are goind to swap edges
			int64_t n = triangleNeighbour(id, ets);
			int64_t ne = get_edge(triangleNeighbour(n, 1), triangleNeighbour(n, 2), id);

			// indices of adjacent triangles that need to change neighbours
			int64_t nid = triangleNeighbour(id, seg1(ets));
			int64_t nn = triangleNeighbour(n, seg1(ne));

			int8_t aux = 0;

			if (nid != -1) {
				int nide = get_edge(triangleNeighbour(nid, 1), triangleNeighbour(nid, 2), id);

				setTriangleFutureNeighbour(nid, nide, n);

				aux = triangleEdgeState(nid, nide);
				if (aux != 2 && aux != 7) {
					setTriangleEdgeState(nid, nide, 1);
				}

				aux = triangleEdgeState(id, seg1(ets));
				if (aux != 2 && aux != 7) {
					setTriangleEdgeState(id, seg1(ets), 1);
				}
			}
			else {
				setTriangleEdgeState(id, seg1(ets), 0);
			}

			if (nn != -1) {
				int nne = get_edge(triangleNeighbour(nn, 1), triangleNeighbour(nn, 2), n);

				setTriangleFutureNeighbour(nn, nne, id);

				aux = triangleEdgeState(nn, nne);
				if (aux != 2 && aux != 7) {
					setTriangleEdgeState(nn, nne, 1);
				}

				aux = triangleEdgeState(n, seg1(ne));
				if (aux != 2 && aux != 7) {
					setTriangleEdgeState(n, seg1(ne), 1);
				}
			}
			else {
				setTriangleEdgeState(n, seg1(ne), 0);
			}


			nid = triangleNeighbour(id, seg2(ets));
			nn = triangleNeighbour(n, seg2(ne));

			if (nid != -1) {
				int nide = get_edge(triangleNeighbour(nid, 1), triangleNeighbour(nid, 2), id);

				aux = triangleEdgeState(nid, nide);
				if (aux != 2 && aux != 7) {
					setTriangleEdgeState(nid, nide, 1);
				}

				aux = triangleEdgeState(id, seg2(ets));
				if (aux != 2 && aux != 7) {
					setTriangleEdgeState(id, seg2(ets), 1);
				}
			}
			else {
				setTriangleEdgeState(id, seg2(ets), 0);
			}

			if (nn != -1) {
				int nne = get_edge(triangleNeighbour(nn, 1), triangleNeighbour(nn, 2), n);

				aux = triangleEdgeState(nn, nne);
				if (aux != 2 && aux != 7) {
					setTriangleEdgeState(nn, nne, 1);
				}

				aux = triangleEdgeState(n, seg2(ne));
				if (aux != 2 && aux != 7) {
					setTriangleEdgeState(n, seg2(ne), 1);
				}
			}
			else {
				setTriangleEdgeState(n, seg2(ne), 0);
			}

		}
		else {
			for (int j = 0; j < 3; j++) {
				int n = triangleNeighbour(id, j);

				if (n != -1 && triangleEdgeToFlip(n) <= 2 && triangleNeighbour(n, triangleEdgeToFlip(n)) == id) {
					// if neighbour will swap edge from this triangle
					if (triangleEdgeState(id, j) == 3) {
						setTriangleEdgeToFlip(id, 4 + ((j + 1) % 3));
					}
					else {
						setTriangleEdgeToFlip(id, 7);
					}
				}
			}
		}

		if (ets < 3) {
			mThereIsEdgesToSwap = 1;
		}
	}
}



void ConstrainedDelaunayTriangulator::updateNeighbours() {
	// getEdge[e][i] = (e + i) % 3

	for_in_parallel(int id = 0; id < mTrianglesCount; id++) {
		if (triangleExists(id) == 0) {
			continue;
		}

		char ets = triangleEdgeToFlip(id);

		if (ets < 3) {
			int n = triangleNeighbour(id, ets);

			int edge = get_edge(triangleNeighbour(n, 1), triangleNeighbour(n, 2), id);

			// Only the point-insertion marker is cleared here. Dropping a
			// *segment* claim on every flip is what stalled constraint
			// insertion: `determineInCircleStep` clears a crossing mark
			// (state 3) whenever the two triangles sharing that edge disagree
			// about which segment claims them, and zeroing one side of a
			// just-flipped pair manufactures precisely that disagreement. The
			// mark vanishes, the flip that would have realised the constraint
			// never happens, and the segment is re-walked forever. Claims are
			// released instead once the segment is actually included.
			if (triangleSegmentId(id) <= 1) setTriangleSegmentId(id, 0);
			if (triangleSegmentId(n) <= 1) setTriangleSegmentId(n, 0);

			setTriangleNeighbour(id, ets, triangleFutureNeighbour(n, edge_jump[edge][1]));
			setTriangleNeighbour(id, edge_jump[ets][1], n);
			setTriangleNeighbour(id, edge_jump[ets][2], triangleFutureNeighbour(id, edge_jump[ets][2]));

			setTriangleNeighbour(n, edge, triangleFutureNeighbour(id, edge_jump[ets][1]));
			setTriangleNeighbour(n, edge_jump[edge][1], id);
			setTriangleNeighbour(n, edge_jump[edge][2], triangleFutureNeighbour(n, edge_jump[edge][2]));

			// update future neighbours
			setTriangleFutureNeighbour(id, 0, triangleNeighbour(id, 0));
			setTriangleFutureNeighbour(id, 1, triangleNeighbour(id, 1));
			setTriangleFutureNeighbour(id, 2, triangleNeighbour(id, 2));

			setTriangleFutureNeighbour(n, 0, triangleNeighbour(n, 0));
			setTriangleFutureNeighbour(n, 1, triangleNeighbour(n, 1));
			setTriangleFutureNeighbour(n, 2, triangleNeighbour(n, 2));

			if (triangleEdgeState(id, ets) == 3) {
				setTriangleEdgeToFlip(id, seg1(ets));
			}
			else {
				setTriangleEdgeToFlip(id, 3);
			}

			// update incircle
			setTriangleEdgeState(id, ets, triangleEdgeState(n, edge_jump[edge][1]));
			setTriangleEdgeState(n, edge, triangleEdgeState(id, edge_jump[ets][1]));

			setTriangleEdgeState(id, edge_jump[ets][1], 0);
			setTriangleEdgeState(n, edge_jump[edge][1], 0);

			// update containing triangle
			setPointContainingTriangle(triangleVertex(id, ets), id);
			setPointContainingTriangle(triangleVertex(n, edge), n);

			//swap
			setTriangleVertex(id, edge_jump[ets][1], triangleVertex(n, edge_jump[edge][2]));
			setTriangleVertex(n, edge_jump[edge][1], triangleVertex(id, edge_jump[ets][2]));

			for (int j = 0; j < 3; j++) {
				if (triangleEdgeState(id, j) == 7) {
					setTriangleSegmentId(id, 1);
					setTriangleSegmentId(triangleNeighbour(id, j), 1);
				}
				if (triangleEdgeState(n, j) == 7) {
					setTriangleSegmentId(n, 1);
					setTriangleSegmentId(triangleNeighbour(n, j), 1);
				}
			}
		}
		if (ets == 3) {
			setTriangleNeighbour(id, 0, triangleFutureNeighbour(id, 0));
			setTriangleNeighbour(id, 1, triangleFutureNeighbour(id, 1));
			setTriangleNeighbour(id, 2, triangleFutureNeighbour(id, 2));
		}
		if (ets > 3) {
			setTriangleEdgeToFlip(id, ets - 4);
		}
	}
}

void ConstrainedDelaunayTriangulator::initialiseTriangles() {
	for_in_parallel(int t = 0; t < mTrianglesCount; t++) {
		setTriangleVertex(t, 0, -1);
		setTriangleVertex(t, 1, -1);
		setTriangleVertex(t, 2, -1);
		setTriangleEdgeState(t, 0, 0);
		setTriangleEdgeState(t, 1, 0);
		setTriangleEdgeState(t, 2, 0);
		setTriangleNeighbour(t, 0, -1);
		setTriangleNeighbour(t, 1, -1);
		setTriangleNeighbour(t, 2, -1);
		setTriangleFutureNeighbour(t, 0, -1);
		setTriangleFutureNeighbour(t, 1, -1);
		setTriangleFutureNeighbour(t, 2, -1);
		setTriangleSegmentId(t, 0);
		setTriangleEdgeToFlip(t, 3);
		if (t == 0) {
			setTriangleVertexToInsert(t, 0);
		}
		else {
			setTriangleVertexToInsert(t, -1);
		}
	}
}

void ConstrainedDelaunayTriangulator::initialisePoints() {
	for_in_parallel(int p = 0; p < mPointsCount; p++) {
		setVertexIsIncluded(p, 0);
		setPointContainingTriangle(p, 0);
	}
}

void ConstrainedDelaunayTriangulator::initialiseSegments() {
	for_in_parallel(int s = 0; s < mSegmentsCount; s++) {
		setSegmentIsIncluded(s, 0);
	}
}

void ConstrainedDelaunayTriangulator::initialiseSuperTriangle() {
	Point2 center;

	center.x = 0;
	center.y = 0;

	for (int p = 0; p < mPointsCount; p++) {
		center.x += vertexCoordinates(p).x / (mPointsCount);
		center.y += vertexCoordinates(p).y / (mPointsCount);
	}

	for (int p = 0; p < mPointsCount; p++) {
		Point2 t = vertexCoordinates(p);

		t.x -= center.x;
		t.y -= center.y;

		setVertexCoordinates(p, t);
	}

	center.x = 0;
	center.y = 0;

	double radius = 0;

	for (int p = 0; p < mPointsCount; p++) {
		double dist = distance(center, vertexCoordinates(p));

		if (dist > radius) {
			radius = dist;
		}
	}


	double sqrt3 = 1.73205080757;
	double pi = 3.141592653589793238462643383279502884L;

	radius = radius * 1.5L;
	double h = 2 * sqrt3 * radius;


	double angle1 = 0.0;
	double angle2 = (2.0 * pi) / 3.0;
	double angle3 = (4.0 * pi) / 3.0;

	Point2 point[3];

	point[0].x = h / 2;
	point[0].y = h * sqrt3 / 2.0f;

	point[1].x = 0;
	point[1].y = 0;

	point[2].x = h;
	point[2].y = 0;

	for (int i = 0; i < 3; i++) {
		center.x += point[i].x / 3;
		center.y += point[i].y / 3;
	}

	for (int i = 0; i < 3; i++) {
		point[i].x -= center.x;
		point[i].y -= center.y;
	}

	if (orient2d(point[0], point[1], point[2]) < 0) {
		Point2 temp = point[1];
		point[1] = point[2];
		point[2] = temp;
	}

	/* Left-over debug print; it fires once per triangulation. */
	/* printf("triangle area = %f\n", orient2d(point[0], point[1], point[2])); */

	setVertexCoordinates(mPointsCount, point[0]);
	setVertexCoordinates(mPointsCount + 1, point[1]);
	setVertexCoordinates(mPointsCount + 2, point[2]);

	setTriangleVertex(0, 0, mPointsCount);
	setTriangleVertex(0, 1, mPointsCount + 1);
	setTriangleVertex(0, 2, mPointsCount + 2);
}




void ConstrainedDelaunayTriangulator::initialiseState(
    const std::vector<Point2> &points, const std::vector<Constraint> &constraints)
{
  mPointsCount = points.size();
  mSegmentsCount = constraints.size();
  // Two triangles per point plus one bounds a triangulation built by
  // incremental insertion into a super-triangle.
  mTrianglesCount = 2u * mPointsCount + 1u;

  // Three extra points hold the super-triangle's vertices.
  // All three are indexed by point, and "point" includes the three
  // super-triangle vertices at pointsCount..+2 - the location and insertion
  // passes address them exactly like input points. Upstream sized the latter
  // two to pointsCount and wrote past them; malloc slack absorbed it, so the
  // corruption only showed as a wrong answer on degenerate input. An
  // exactly-sized vector turns that into an ASan report instead.
  mPointCoordinates.assign((mPointsCount + 3u) * 2u, 0.0);
  mPointContainingTriangle.assign(mPointsCount + 3u, 0);
  mPointIsIncluded.assign(mPointsCount + 3u, 0);
  mTriangleVertices.assign(mTrianglesCount * 3u, 0);
  mTriangleEdgeState.assign(mTrianglesCount * 3u, 0);
  mTriangleNeighbours.assign(mTrianglesCount * 6u, 0);
  mTriangleVertexToInsert.assign(mTrianglesCount, 0);
  mTriangleSegmentIndex.assign(mTrianglesCount, 0);
  mTriangleEdgeToFlip.assign(mTrianglesCount, 0);
  mSegmentPointIndices.assign(mSegmentsCount * 2u, 0);
  mSegmentIsIncluded.assign(mSegmentsCount, 0);

  for (uint64_t i = 0; i < mPointsCount; i++)
    setVertexCoordinates(static_cast<int64_t>(i), points[i]);

  for (uint64_t i = 0; i < mSegmentsCount; i++)
  {
    setSegmentVertex(static_cast<int64_t>(i), 0, constraints[i].origin);
    setSegmentVertex(static_cast<int64_t>(i), 1, constraints[i].end);
  }

  mThereIsEdgesToSwap = 0;
  mThereIsPointsToInsert = 1;
  mThereIsSegmentToInsert = 1;

  initialiseTriangles();
  initialiseSegments();
  initialisePoints();
  initialiseSuperTriangle();
}

bool ConstrainedDelaunayTriangulator::triangulate(
    const std::vector<Point2> &points, const std::vector<Constraint> &constraints,
    const Settings &settings)
{
  initialisePredicates();

  if (points.empty()) return true;

  initialiseState(points, constraints);

  uint64_t depth = 0;
  uint64_t stalled = 0;
  uint64_t previousIncluded = 0;

  while ((mThereIsPointsToInsert || mThereIsSegmentToInsert || mThereIsEdgesToSwap) &&
         (depth < settings.maximumIterations || settings.maximumIterations == 0u))
  {
    if (mThereIsPointsToInsert)
    {
      preUpdate();
      insertionStep();
    }

    if (mThereIsSegmentToInsert)
    {
      mThereIsSegmentToInsert = 0;
      markingStep();
    }

    do
    {
      mThereIsEdgesToSwap = 0;

      determineInCircleStep();
      determineSwapping();
      swapEdges();
      updateNeighbours();
    } while (settings.ensureDelaunay && mThereIsEdgesToSwap == 1);

    if (mThereIsPointsToInsert)
    {
      mThereIsPointsToInsert = 0;
      locationStep();
    }

    depth = depth + 1;

    // Progress is counted on what the iteration is trying to achieve rather
    // than on the swap flag, because the swap flag is exactly what oscillates
    // when constraint insertion and Delaunay restoration undo each other.
    uint64_t included = 0;
    for (uint64_t p = 0; p < mPointsCount; p++)
      if (mPointIsIncluded[p]) included++;
    for (uint64_t s = 0; s < mSegmentsCount; s++)
      if (mSegmentIsIncluded[s]) included++;

    if (included == previousIncluded)
    {
      stalled = stalled + 1;
      if (stalled >= kStallLimit) return false;
    }
    else
    {
      stalled = 0;
      previousIncluded = included;
    }
  }

  return true;
}

std::vector<int64_t> ConstrainedDelaunayTriangulator::collectTriangles() const
{
  std::vector<int64_t> out;
  for (uint64_t t = 0; t < mTrianglesCount; t++)
  {
    if (!triangleExists(t)) continue;
    out.push_back(vertexOfTriangle(t, 0));
    out.push_back(vertexOfTriangle(t, 1));
    out.push_back(vertexOfTriangle(t, 2));
  }
  return out;
}

} // namespace procedural
