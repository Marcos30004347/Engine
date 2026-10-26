#include "procedural/PoissonDiskSampler.hpp"

#include "procedural/Geometry.hpp"


#include <cmath>
#include <cstdlib>
#include <cassert>

namespace procedural
{

namespace
{


//#define POINTS_PER_CELL 8
//#define POINT_BYTES_PER_CELL 2 * POINTS_PER_CELL
#define BLOCK_WIDTH 5
#define BLOCK_HEIGHT 5
#define BLOCK_SIZE (BLOCK_WIDTH * BLOCK_HEIGHT)
#define BYTES_PER_VOID_TRIANGLE 6
#define VOID_TRIANGLES_COUNT 15
#define X_INDEX 0
#define Y_INDEX 1



 double grid_get_cell_size(double radius) {
	const double sqrt2 = 1.414213562373095;
	//return sqrt2 * radius;
	return radius / sqrt2;
}

 int64_t grid_get_axis_size(double mCellSize, double region_size_in_axis) {
	double value = region_size_in_axis / mCellSize;

	int64_t intValue = (int64_t)value;

	return value > intValue ? intValue + 1 : intValue;
}

 int64_t grid_get_cells_count(double radius, double width, double height) {
	double cell = grid_get_cell_size(radius);

	double mRows = grid_get_axis_size(cell, height);
	double mCols = grid_get_axis_size(cell, width);

	return mRows * mCols;
}

 double get_point_cell_axis_position(double mCellSize, uint8_t coord) {
	return (double)coord * (mCellSize / (double)256);
}

 Point2 get_point_grid_axis_position(int64_t row, int64_t col, double mCellSize, uint8_t x_coord, uint8_t y_coord) {
	double x = mCellSize * col;
	double y = mCellSize * row;

	x = x + get_point_cell_axis_position(mCellSize, x_coord);
	y = y + get_point_cell_axis_position(mCellSize, y_coord);

	Point2 result;
	
	result.x = x;
	result.y = y;

	return result;
}


 uint64_t shuffle_bits(long long seed) {
	(seed) ^= (seed) >> 12; // a
	(seed) ^= (seed) << 25; // b
	(seed) ^= (seed) >> 27; // c
	return (seed) * 2685821657736338717;
}

 Point2 random_point_inside_triangle(long long seed, Point2 p1, Point2 p2, Point2 p3) {
	 double alpha = (shuffle_bits(seed) % seed) / (double)seed;
	 double beta = (shuffle_bits(seed * 2) % seed) / (double)seed;
	 
	 if (alpha + beta > 1) {
		 alpha = 1 - alpha;
		 beta = 1 - beta;
	 }

	 double gamma = 1 - alpha - beta;

	 Point2 result;

	 result.x = alpha * p1.x + beta * p2.x + gamma * p3.x;
	 result.y = alpha * p1.y + beta * p2.y + gamma * p3.y;

	 printf("polygon((%f, %f), (%f, %f), (%f, %f))\n", p1.x, p1.y, p2.x, p2.x, p3.x, p3.y);
	 printf("(%f, %f)\n", result.x, result.y);
	 return result;
 }



 int circle_line_intersection(Point2 center, double radius, Point2 sega, Point2 segb, Point2* result) {
	double cx = center.x;
	double cy = center.y;
	double r = radius;

	double x1 = sega.x;
	double y1 = sega.y;
	double x2 = segb.x;
	double y2 = segb.y;

	double dx = x2 - x1;
	double dy = y2 - y1;

	double A = dx * dx + dy * dy;
	double B = 2 * (dx * (x1 - cx) + dy * (y1 - cy));
	double C = (x1 - cx) * (x1 - cx) + (y1 - cy) * (y1 - cy) - r * r;

	double det = B * B - 4 * A * C;

	if (A <= 0.000001 || det < 0) {
		return 0;
	}
	else {
		double t1 = (-B + sqrt(det)) / (2 * A);
		double t2 = (-B - sqrt(det)) / (2 * A);

		result[0].x = x1 + t1 * dx;
		result[0].y = y1 + t1 * dy;

		result[1].x = x1 + t2 * dx;
		result[1].y = y1 + t2 * dy;

		return 2;
	}
}


 uint64_t line_circle_intersection(Point2 line_start, Point2 line_end, Point2 circle_center, double circle_radius, Point2* result) {
	double dx = line_end.x - line_start.x;
	double dy = line_end.y - line_start.y;
	double a = dx * dx + dy * dy;
	double b = 2 * (dx * (line_start.x - circle_center.x) + dy * (line_start.y - circle_center.y));
	double c = (line_start.x - circle_center.x) * (line_start.x - circle_center.x) + (line_start.y - circle_center.y) * (line_start.y - circle_center.y) - circle_radius * circle_radius;

	double discriminant = b * b - 4 * a * c;

	int count = 0;

	if (discriminant >= 0) {
		double t1 = (-b + sqrt(discriminant)) / (2 * a);
		double t2 = (-b - sqrt(discriminant)) / (2 * a);

		if (t1 >= 0 && t1 <= 1) {
			result[count].x = line_start.x + t1 * dx;
			result[count].y = line_start.y + t1 * dy;
			count++;
		}

		if (t2 >= 0 && t2 <= 1) {
			result[count].x = line_start.x + t2 * dx;
			result[count].y = line_start.y + t2 * dy;
			count++;
		}

		if (count == 2 && t2 < t1) {
			Point2 tmp = result[1];
			result[1] = result[0];
			result[0] = tmp;
		}
	}

	return count;
}

 uint64_t line_circle_intersection_non_inclusive(Point2 line_start, Point2 line_end, Point2 circle_center, double circle_radius, Point2* result) {
	 double dx = line_end.x - line_start.x;
	 double dy = line_end.y - line_start.y;
	 double a = dx * dx + dy * dy;
	 double b = 2 * (dx * (line_start.x - circle_center.x) + dy * (line_start.y - circle_center.y));
	 double c = (line_start.x - circle_center.x) * (line_start.x - circle_center.x) + (line_start.y - circle_center.y) * (line_start.y - circle_center.y) - circle_radius * circle_radius;

	 double discriminant = b * b - 4 * a * c;

	 int count = 0;

	 if (discriminant >= 0) {
		 double t1 = (-b + sqrt(discriminant)) / (2 * a);
		 double t2 = (-b - sqrt(discriminant)) / (2 * a);

		// printf("%f %f\n", t1, t2);

		 if (t1 > 0 && t1 < 1) {
			 result[count].x = line_start.x + t1 * dx;
			 result[count].y = line_start.y + t1 * dy;
			 count++;
		 }

		 if (t2 > 0 && t2 < 1) {
			 result[count].x = line_start.x + t2 * dx;
			 result[count].y = line_start.y + t2 * dy;
			 count++;
		 }

		 if (count == 2 && t2 < t1) {
			 Point2 tmp = result[1];
			 result[1] = result[0];
			 result[0] = tmp;
		 }
	 }

	 return count;
 }

typedef struct {
	Point2 chord[2][2];
	uint64_t chords;
	int chord_side[2];
} chords_t;

void finalise_chord_sides(chords_t &result, Point2 ccenter) {
	for (uint64_t i = 0; i < result.chords;) {
		int circle_direction = signOf(orient2d(result.chord[i][0], result.chord[i][1], ccenter));
		if (circle_direction == 0) {
			for (uint64_t j = i + 1; j < result.chords; ++j) {
				result.chord[j - 1][0] = result.chord[j][0];
				result.chord[j - 1][1] = result.chord[j][1];
			}
			--result.chords;
			continue;
		}
		result.chord_side[i] = -circle_direction;
		++i;
	}
}

// TODO: refactor to consider just corner
 chords_t compute_chord(Point2 ccenter, double circle_radius, Point2 poly[16], int n) {
	Point2 intersection_point[2];
	
	chords_t result;
	result.chords = 0;

	int chord0 = 0;
	int chord1 = 1;

	//int count = 0;
	
	Point2 intersections[4];

	char intersects[4];

	intersects[0] = -1;
	intersects[1] = -1;
	intersects[2] = -1;
	intersects[3] = -1;

	int outside_vertice_a = -1;
	int outside_vertice_b = -1;
	
	int flip = 0;
	int chord = 0;
	int point = 0;

	int filled[4] = { 0,0,0,0 };

	for (int i = 0; i < 16 && i < n; i++) {
		int k = i + 1 > n - 1 ? 0 : i + 1;

		uint64_t intersections_count = line_circle_intersection(poly[i], poly[k], ccenter, circle_radius, intersection_point);
	
		int count = filled[0] + filled[1] + filled[2] + filled[3];
		
		if (count == 4) break;

		if (intersections_count == 2 && (count == 0 || count == 1)) {
			result.chord[0][1] = intersection_point[0];
			result.chord[1][0] = intersection_point[1];
			
			filled[1] = 1;
			filled[2] = 1;
		}

		if (intersections_count == 1) {
			if (filled[1] + filled[2] + filled[3] == 3) {
				result.chord[0][0] = intersection_point[0];
				filled[0] = 1;
			}

			else if (filled[1] + filled[2] == 2) {
				result.chord[1][1] = intersection_point[0];
				filled[3] = 1;
			}

			else {
				result.chord[count / 2][count % 2] = intersection_point[0];
				filled[count] = 1;
			}
		}
	}

	if (filled[0] + filled[1] == 2) {
		result.chords = 1;
	}
	
	if (filled[2] + filled[3] == 2) {
		assert(filled[0] + filled[1] == 2);
		result.chords = 2;
	}

	finalise_chord_sides(result, ccenter);

	return result;
}


 chords_t compute_chord_triangle(Point2 ccenter, double circle_radius, Point2 poly[3]) {
	 Point2 intersection_point[2];

	 chords_t result;
	 result.chords = 0;

	 int chord0 = 0;
	 int chord1 = 1;

	 //int count = 0;

	 Point2 intersections[4];

	 char intersects[4];

	 intersects[0] = -1;
	 intersects[1] = -1;
	 intersects[2] = -1;
	 intersects[3] = -1;

	 int outside_vertice_a = -1;
	 int outside_vertice_b = -1;

	 int flip = 0;
	 int chord = 0;
	 int point = 0;

	 int filled[4] = { 0,0,0,0 };

	 for (int i = 0; i < 3; i++) {
		 int k = i + 1 > 3 - 1 ? 0 : i + 1;

		 uint64_t intersections_count = line_circle_intersection(poly[i], poly[k], ccenter, circle_radius, intersection_point);

		 int count = filled[0] + filled[1] + filled[2] + filled[3];

		 if (count == 4) break;

		 if (intersections_count == 2 && (count == 0 || count == 1)) {
			 result.chord[0][1] = intersection_point[0];
			 result.chord[1][0] = intersection_point[1];

			 filled[1] = 1;
			 filled[2] = 1;
		 }

		 if (intersections_count == 1) {
			 if (filled[1] + filled[2] + filled[3] == 3) {
				 result.chord[0][0] = intersection_point[0];
				 filled[0] = 1;
			 }

			 else if (filled[1] + filled[2] == 2) {
				 result.chord[1][1] = intersection_point[0];
				 filled[3] = 1;
			 }

			 else {
				 result.chord[count / 2][count % 2] = intersection_point[0];
				 filled[count] = 1;
			 }
		 }
	 }

	 if (filled[0] + filled[1] == 2) {
		 result.chords = 1;
	 }

	 if (filled[2] + filled[3] == 2) {
		 assert(filled[0] + filled[1] == 2);
		 result.chords = 2;
	 }

	 finalise_chord_sides(result, ccenter);

	 return result;
 }

 Point2 find_intersection_point(Point2 p1, Point2 q1, Point2 p2, Point2 q2) {
	double A1 = q1.y - p1.y;
	double B1 = p1.x - q1.x;
	double C1 = A1 * p1.x + B1 * p1.y;

	double A2 = q2.y - p2.y;
	double B2 = p2.x - q2.x;
	double C2 = A2 * p2.x + B2 * p2.y;

	double determinant = A1 * B2 - A2 * B1;

	if (determinant == 0) {
		return q2;
	}

	Point2 intersection;

	intersection.x = (B2 * C1 - B1 * C2) / determinant;
	intersection.y = (A1 * C2 - A2 * C1) / determinant;

	return intersection;
}


int find_circle_intersection(Point2 circle1, Point2 circle2, double radius, Point2* intersection1, Point2* intersection2) {
	double d = sqrt(pow(circle2.x - circle1.x, 2) + pow(circle2.y - circle1.y, 2));

	if (distance(circle1, circle2) == 0) {
		return 0;
	}
	if (d > radius + radius || d < fabs(radius - radius)) {
		return 0; 
	}

	double a = (pow(radius, 2) - pow(radius, 2) + pow(d, 2)) / (2 * d);

	double x1 = circle1.x + a * (circle2.x - circle1.x) / d;
	double y1 = circle1.y + a * (circle2.y - circle1.y) / d;

	double h = sqrt(pow(radius, 2) - pow(a, 2));

	intersection1->x = x1 + h * (circle2.y - circle1.y) / d;
	intersection1->y = y1 - h * (circle2.x - circle1.x) / d;

	intersection2->x = x1 - h * (circle2.y - circle1.y) / d;
	intersection2->y = y1 + h * (circle2.x - circle1.x) / d;

	return 1;
}


bool is_point_inside_triangle(Point2 A, Point2 B, Point2 C, Point2 P) {
	double orientation1 = orient2d(A, B, P);
	double orientation2 = orient2d(B, C, P);
	double orientation3 = orient2d(C, A, P);

	return (orientation1 > 0 && orientation2 > 0 && orientation3 > 0) ||
		   (orientation1 < 0 && orientation2 < 0 && orientation3 < 0);
}

bool ccw(Point2 A, Point2 B, Point2 C) {
	return orient2d(A, B, C) < 0;
}

bool do_segments_intersect(Point2 A, Point2 B, Point2 C, Point2 D) {
	return ccw(A, C, D) != ccw(B, C, D) && ccw(A, B, C) != ccw(A, B, D);
}





#define SERIAL


} // namespace

uint64_t PoissonDiskSampler::dartThrowing(uint64_t seed, uint64_t block_x, uint64_t block_y) {
	const int blocks_per_row = mCols / BLOCK_WIDTH;
	const int block_start = block_y * BLOCK_SIZE * blocks_per_row + block_x * BLOCK_SIZE;

	uint64_t misses = 0;

	for (uint64_t iterations = 0; iterations < 5 && misses < 50; iterations++) {

		for (uint64_t i = 0; i < BLOCK_SIZE; i++) { // loop can be removed in gpu
			int index = block_start + i;

			if (mCellState[index] == CellState::Empty) {
				seed = shuffle_bits(seed);
				uint8_t x = (seed % 256);
				mCellPointCoords[2 * index + X_INDEX] = x;

				seed = shuffle_bits(seed);
				uint8_t y = (seed % 256);
				mCellPointCoords[2 * index + Y_INDEX] = y;

				mCellState[index] = CellState::Test;
			}
		}

		// NOTE: to implement kernel replace loop with global memory barrier

		for (uint64_t i = 0; i < BLOCK_SIZE; i++) {
			int index = block_start + i;
			if (mCellState[index] == CellState::Test) {
				int col = block_x * BLOCK_WIDTH + (i % BLOCK_WIDTH);
				int row = block_y * BLOCK_HEIGHT + (i / BLOCK_WIDTH);

				Point2 pa = get_point_grid_axis_position(row, col, mCellSize, 0, 0);
				Point2 pb = get_point_grid_axis_position(row, col, mCellSize, 255, 0);
				Point2 pc = get_point_grid_axis_position(row, col, mCellSize, 255, 255);
				Point2 pd = get_point_grid_axis_position(row, col, mCellSize, 0, 255);

				Point2 point = get_point_grid_axis_position(row, col, mCellSize, mCellPointCoords[index * 2 + X_INDEX], mCellPointCoords[index * 2 + Y_INDEX]);

				for (int r = -2; r <= +2; r++) {
					for (int c = -2; c <= +2; c++) {
						if (r == 0 && c == 0) continue;

						const int neighbour_row = row + r;
						const int neighbour_col = col + c;

						const int neighbour_y_block = neighbour_row / BLOCK_WIDTH;
						const int neighbour_x_block = neighbour_col / BLOCK_HEIGHT;
						const int neighbour_row_block = neighbour_row % BLOCK_WIDTH;
						const int neighbour_col_block = neighbour_col % BLOCK_HEIGHT;

						const int neighbour_block = neighbour_y_block * BLOCK_SIZE * blocks_per_row + neighbour_x_block * BLOCK_SIZE;
						const int neighbour_index = neighbour_block + neighbour_row_block * BLOCK_WIDTH + neighbour_col_block;


						if (mCellState[neighbour_index] == CellState::Done) {
							Point2 point_neighbour = get_point_grid_axis_position(neighbour_row, neighbour_col, mCellSize, mCellPointCoords[neighbour_index * 2 + X_INDEX], mCellPointCoords[neighbour_index * 2 + Y_INDEX]);

							if (distance(point_neighbour, point) < mRadius) {
								if (distance(pa, point_neighbour) < mRadius && distance(pb, point_neighbour) < mRadius && distance(pc, point_neighbour) < mRadius && distance(pd, point_neighbour) < mRadius) {
									mCellState[index] = CellState::Rejected;
								}
								else {
									mCellState[index] = CellState::Empty;
								}
							}
						}
					}
				}
			}
		}


		// NOTE: to implement kernel replace loop with global memory barrier
		for (uint64_t i = 0; i < BLOCK_SIZE; i++) {
			int index = block_start + i;
			if (mCellState[index] == CellState::Test) {
				int index = block_start + i;

				int col = block_x * BLOCK_WIDTH + (i % BLOCK_WIDTH);
				int row = block_y * BLOCK_HEIGHT + (i / BLOCK_WIDTH);

				Point2 pa = get_point_grid_axis_position(row, col, mCellSize, 0, 0);
				Point2 pb = get_point_grid_axis_position(row, col, mCellSize, 255, 0);
				Point2 pc = get_point_grid_axis_position(row, col, mCellSize, 255, 255);
				Point2 pd = get_point_grid_axis_position(row, col, mCellSize, 0, 255);

				Point2 point = get_point_grid_axis_position(row, col, mCellSize, mCellPointCoords[index * 2 + X_INDEX], mCellPointCoords[index * 2 + Y_INDEX]);

				for (int r = -2; r <= +2; r++) {
					for (int c = -2; c <= +2; c++) {
						if (r == 0 && c == 0) continue;

						const int neighbour_row = row + r;
						const int neighbour_col = col + c;

						const int neighbour_y_block = neighbour_row / BLOCK_WIDTH;
						const int neighbour_x_block = neighbour_col / BLOCK_HEIGHT;
						const int neighbour_row_block = neighbour_row % BLOCK_WIDTH;
						const int neighbour_col_block = neighbour_col % BLOCK_HEIGHT;

						const int neighbour_block = neighbour_y_block * BLOCK_SIZE * blocks_per_row + neighbour_x_block * BLOCK_SIZE;
						const int neighbour_index = neighbour_block + neighbour_row_block * BLOCK_WIDTH + neighbour_col_block;

						if (mCellState[neighbour_index] == CellState::Test || mCellState[neighbour_index] == CellState::Accepted) {
							Point2 point_neighbour = get_point_grid_axis_position(neighbour_row, neighbour_col, mCellSize, mCellPointCoords[neighbour_index * 2 + X_INDEX], mCellPointCoords[neighbour_index * 2 + Y_INDEX]);

							if (distance(point_neighbour, point) < mRadius) {
								mCellState[index] = CellState::Empty;
							}
						}
					}
				}

				if (mCellState[index] == CellState::Test) {

					mCellState[index] = CellState::Accepted;
				}
				else {
					misses += 1;
				}
			}
		}

		// NOTE: to implement kernel replace loop with global memory barrier

		for (uint64_t i = 0; i < BLOCK_SIZE; i++) {
			int index = block_start + i;

			if (mCellState[index] == CellState::Accepted) {
				mCellState[index] = CellState::Done;
				//mCellPointCoords[id * 2 + X_INDEX] = candidate_coords[2 * id + X_INDEX];
				//mCellPointCoords[id * 2 + Y_INDEX] = candidate_coords[2 * id + Y_INDEX];
			}
		}
	}

	//printf("misses = %u\n", misses);
	return seed;
}

uint64_t PoissonDiskSampler::dividePolygonByChord(uint64_t tail, Point2 polygon_vertices[16][16], uint64_t polygon_size[16], Point2 circle_center, double radius, int polygon, uint64_t polygon_cell) {
	// TODO: we have problems when chord points are equal to polygon points
	chords_t chords = compute_chord(circle_center, radius, polygon_vertices[polygon], polygon_size[polygon]);

	for (int c = 0; c < 2 && c < chords.chords && mCellState[polygon_cell] == CellState::Empty; c++) {
		Point2 poly[16];

		uint64_t size = 0;


		for (int i = 0; i < 16 && i < polygon_size[polygon]; i++) {
			Point2 curr_point = polygon_vertices[polygon][i];
			Point2 next_point = polygon_vertices[polygon][(i + 1) % polygon_size[polygon]];

			int orient_curr = signOf(orient2d(chords.chord[c][0], chords.chord[c][1], curr_point));
			int orient_next = signOf(orient2d(chords.chord[c][0], chords.chord[c][1], next_point));

			if (orient_curr == 0) {
				poly[size++] = curr_point;
			}
			else if (orient_curr != chords.chord_side[c] && orient_next == chords.chord_side[c]) {
				poly[size++] = find_intersection_point(chords.chord[c][0], chords.chord[c][1], curr_point, next_point);
			}
			else if (orient_curr == chords.chord_side[c] && orient_next != 0 && orient_next != chords.chord_side[c]) {
				poly[size++] = curr_point;
				poly[size++] = find_intersection_point(chords.chord[c][0], chords.chord[c][1], curr_point, next_point);
			}
			else if (orient_curr == chords.chord_side[c]) {
				poly[size++] = curr_point;
			}
		}
		
		for (int v = 0; v < 16 && v < size; v++) {
			polygon_vertices[tail][v] = poly[v];
		}

		polygon_size[tail] = size;

		tail = (tail + 1) % 16;
	}

	return tail;
}

uint64_t divide_triangle_by_chord(Point2 triangle[3], Point2 circle_center, double radius, Point2 result[2][3]) {
	// TODO: we have problems when chord points are equal to polygon points
	chords_t chords = compute_chord_triangle(circle_center, radius, triangle);
	
	int triangles = 0;

	for (int c = 0; c < 2 && c < chords.chords; c++) {
		Point2 poly[4];

		uint64_t size = 0;

		for (int i = 0; i < 3; i++) {
			Point2 curr_point = triangle[i];
			Point2 next_point = triangle[(i + 1) % 3];

			int orient_curr = signOf(orient2d(chords.chord[c][0], chords.chord[c][1], curr_point));
			int orient_next = signOf(orient2d(chords.chord[c][0], chords.chord[c][1], next_point));

			if (orient_curr == 0) {
				poly[size++] = curr_point;
			}
			else if (orient_curr != chords.chord_side[c] && orient_next == chords.chord_side[c]) {
				poly[size++] = find_intersection_point(chords.chord[c][0], chords.chord[c][1], curr_point, next_point);
			}
			else if (orient_curr == chords.chord_side[c] && orient_next != 0 && orient_next != chords.chord_side[c]) {
				poly[size++] = curr_point;
				poly[size++] = find_intersection_point(chords.chord[c][0], chords.chord[c][1], curr_point, next_point);
			}
			else if (orient_curr == chords.chord_side[c]) {
				poly[size++] = curr_point;
			}
		}
		/*
		printf("chords.chord = %u\n", chords.chords);
		printf("size = %u\n", size);
		printf("(x - %f)^2 + (y - %f)^2=%f^2\n", circle_center.x, circle_center.y, radius);

		printf("polygon(");
		for (int v = 0; v < size; v++) {
			printf("(%f, %f)", poly[v].x, poly[v].y);
			if (v < size - 1) printf(",");
		}
		printf(")\n");
		*/
		

		assert(size == 3 || size == 4);

		if (size == 4) {
			assert(chords.chords == 1);

			result[0][0] = poly[0];
			result[0][1] = poly[1];
			result[0][2] = poly[2];

			result[1][0] = poly[2];
			result[1][1] = poly[3];
			result[1][2] = poly[0];
			
			return 2;
		}

		if (size == 3) {
			for (int v = 0; v < 3; v++) {
				result[triangles][v] = poly[v];
			}
		}

		triangles = triangles + 1;
	}

	return triangles;
}

void PoissonDiskSampler::encodeTriangle(Point2 p1, Point2 p2, Point2 p3, Point2 origin, int64_t index, char t) {
	unsigned stride = index * (VOID_TRIANGLES_COUNT * BYTES_PER_VOID_TRIANGLE + 1);

	mCellTriangles[stride + 1 + t * BYTES_PER_VOID_TRIANGLE + 0] = (uint8_t)(((p1.x - origin.x) / mCellSize) * 256);
	mCellTriangles[stride + 1 + t * BYTES_PER_VOID_TRIANGLE + 1] = (uint8_t)(((p1.y - origin.y) / mCellSize) * 256);
	mCellTriangles[stride + 1 + t * BYTES_PER_VOID_TRIANGLE + 2] = (uint8_t)(((p2.x - origin.x) / mCellSize) * 256);
	mCellTriangles[stride + 1 + t * BYTES_PER_VOID_TRIANGLE + 3] = (uint8_t)(((p2.y - origin.y) / mCellSize) * 256);
	mCellTriangles[stride + 1 + t * BYTES_PER_VOID_TRIANGLE + 4] = (uint8_t)(((p3.x - origin.x) / mCellSize) * 256);
	mCellTriangles[stride + 1 + t * BYTES_PER_VOID_TRIANGLE + 5] = (uint8_t)(((p3.y - origin.x) / mCellSize) * 256);
}

// Exact. This was `dx*dx + dy*dy` compared against `r*r` with an epsilon
// deciding the boundary, which is the one decision in the void-filling path
// that was not already routed through an exact predicate - and it is why the
// sampler's output changed with compiler flags. `eps` is retained in the
// signature and ignored: an exact test has no boundary band to widen.
int is_point_inside_circle(Point2 p, Point2 c, double r, double eps) {
	(void)eps;
	return compareToDisc(p, c, r) < 0 ? 1 : 0;
}

uint64_t PoissonDiskSampler::generateTriangleVoids(uint64_t seed, uint64_t block_x, uint64_t block_y) {
	// POLYGON APROXIMATION TO ARC-VOIDS
	const int blocks_per_row = mCols / BLOCK_WIDTH;
	const int block_start = block_y * BLOCK_SIZE * blocks_per_row + block_x * BLOCK_SIZE;

	for (int it = 0; it < BLOCK_SIZE; it++) {
		int index = block_start + it;

		if (mCellState[index] != CellState::Empty) continue;

		int col = block_x * BLOCK_WIDTH + (it % BLOCK_WIDTH);
		int row = block_y * BLOCK_HEIGHT + (it / BLOCK_WIDTH);

		// TODO: 16 voids is probably too much
		Point2 polygon_vertices[16][16];
		uint64_t voids_size[16];

		polygon_vertices[0][0] = get_point_grid_axis_position(row, col, mCellSize, 0, 0);
		polygon_vertices[0][1] = get_point_grid_axis_position(row, col, mCellSize, 255, 0);
		polygon_vertices[0][2] = get_point_grid_axis_position(row, col, mCellSize, 255, 255);
		polygon_vertices[0][3] = get_point_grid_axis_position(row, col, mCellSize, 0, 255);

		voids_size[0] = 4;
		
		int head = 0;
		int tail = 1;

		int cells = 0;
		Point2 circles[15];

		// collect cells
		for (int r = -2; r <= +2; r++) {
			for (int c = -2; c <= +2; c++) {
				if (r == 0 && c == 0) continue;
				const int neighbour_row = row + r;
				const int neighbour_col = col + c;

				const int neighbour_y_block = neighbour_row / BLOCK_WIDTH;
				const int neighbour_x_block = neighbour_col / BLOCK_HEIGHT;
				const int neighbour_row_block = neighbour_row % BLOCK_WIDTH;
				const int neighbour_col_block = neighbour_col % BLOCK_HEIGHT;

				const int neighbour_block = neighbour_y_block * BLOCK_SIZE * blocks_per_row + neighbour_x_block * BLOCK_SIZE;
				const int neighbour_index = neighbour_block + neighbour_row_block * BLOCK_WIDTH + neighbour_col_block;
				/*
				if (block_x == 11 && block_y == 13 && it == 3) {
					Point2 polygon[4];

					polygon[0] = get_point_grid_axis_position(neighbour_row, neighbour_col, mCellSize, 0, 0);
					polygon[1] = get_point_grid_axis_position(neighbour_row, neighbour_col, mCellSize, 255, 0);
					polygon[2] = get_point_grid_axis_position(neighbour_row, neighbour_col, mCellSize, 255, 255);
					polygon[3] = get_point_grid_axis_position(neighbour_row, neighbour_col, mCellSize, 0, 255);
					printf("polygon((%f, %f), (%f, %f), (%f, %f), (%f, %f))\n", polygon[0].x, polygon[0].y,
						polygon[1].x, polygon[1].y,
						polygon[2].x, polygon[2].y,
						polygon[3].x, polygon[3].y);
				}
				*/
				if (mCellState[neighbour_index] == CellState::Done) {
					circles[cells++] = get_point_grid_axis_position(neighbour_row, neighbour_col, mCellSize, mCellPointCoords[2 * neighbour_index + X_INDEX], mCellPointCoords[2 * neighbour_index + Y_INDEX]);
				}
			}
		}

		assert(cells <= 15);
		/*
		printf("1:\n");
		for (int curr = head; curr != tail; curr = (curr + 1) % 16) {
			printf("polygon(");
			for (int v = 0; v < voids_size[curr]; v++) {
				printf("(%f, %f)", polygon_vertices[curr][v].x, polygon_vertices[curr][v].y);
				if (v < voids_size[curr] - 1) printf(",");
			}
			printf(")\n");
		}
		*/

		for (int k = 0; k < 1; k++) { // k == 2 is probably too much
			for (char n = 0; n < 15 && n < cells; n++) {
				int current_tail = tail;

				for (int curr = head; curr != current_tail; curr = (curr + 1) % 16) {
					tail = dividePolygonByChord(tail, polygon_vertices, voids_size, circles[n], mRadius, curr, index);
				}

				if (tail != current_tail) {
					head = current_tail;
				}
			}
		}
		/*
		printf("2:\n");
		for (int curr = head; curr != tail; curr = (curr + 1) % 16) {
			printf("polygon(");
			for (int v = 0; v < voids_size[curr]; v++) {
				printf("(%f, %f)", polygon_vertices[curr][v].x, polygon_vertices[curr][v].y);
				if (v < voids_size[curr] - 1) printf(",");
			}
			printf(")\n");
		}
		*/


		for (int curr = head; curr != tail; curr = (curr + 1) % 16) {

			for (char n = 0; n < 15 && n < cells; n++) {
				int contained = 1;

				for (int v = 0; v < 16 && v < voids_size[curr]; v++) {
					if (is_point_inside_circle(polygon_vertices[curr][v], circles[n], mRadius, kSegmentEpsilon) == 0) {
						contained = 0;
					}
				}

				if (contained) {
					voids_size[curr] = 0;
				}
			}
		}
		/*
		for (int curr = head; curr != tail; curr = (curr + 1) % 16) {
			if (voids_size[curr] == 0) continue;
			printf("r= polygon(");
			for (int v = 0; v < voids_size[curr]; v++) {
				printf("(%f, %f)", polygon_vertices[curr][v].x, polygon_vertices[curr][v].y);
				if (v < voids_size[curr] - 1) printf(",");
			}
			printf(")\n");
		}
		*/


		uint8_t t = 0;

		Point2 origin = get_point_grid_axis_position(row, col, mCellSize, 0, 0);

		for (int curr = head; curr != tail && t < VOID_TRIANGLES_COUNT; curr = (curr + 1) % 16) {
			if (voids_size[curr] == 0) continue;
			for (int i = 1; i < voids_size[curr] - 1; ++i) {
				Point2 p1 = polygon_vertices[curr][0];
				Point2 p2 = polygon_vertices[curr][i];
				Point2 p3 = polygon_vertices[curr][i + 1];

				encodeTriangle(p1, p2, p3, origin, index, t);

				t = t + 1;
			}
		}

		mCellTriangles[index * (VOID_TRIANGLES_COUNT * BYTES_PER_VOID_TRIANGLE + 1)] = t;
	
		//printf("triangles = %u\n", t);
	}

	return seed;
}





uint64_t PoissonDiskSampler::pointInsideTriangleVoids(uint64_t seed, uint64_t block_x, uint64_t block_y) {
	const int blocks_per_row = mCols / BLOCK_WIDTH;
	const int block_start = block_y * BLOCK_SIZE * blocks_per_row + block_x * BLOCK_SIZE;
	const double eps = 1 / (double)256;

	double best_area[BLOCK_SIZE];

	char best_triangle[BLOCK_SIZE];

	//printf("phase3: \n");

	for (int it = 0; it < BLOCK_SIZE; it++) {
		int index = block_start + it;

		best_area[it] = -1;
		best_triangle[it] = -1;
	}

	int tries = 0;

	for (; tries < 10; tries++) {
		//printf("try: \n");
		int empty_cells = 0;

		for (int it = 0; it < BLOCK_SIZE; it++) {
			int index = block_start + it;

			if (mCellState[index] == CellState::Empty) {
				empty_cells = empty_cells + 1;

				//if (valid_triangles == 0) {
					//mCellState[index] = CellState::Rejected;
			//	}
				//else {
					mCellState[index] = CellState::Test;
				//}

				best_triangle[it] = -1;
				best_area[it] = -1;
			}
		}



		// NOTE TODO: in gpu compute add global_barrier

		int total_triangles = 0;

		for (int it = 0; it < BLOCK_SIZE; it++) {
			int index = block_start + it;

			if (mCellState[index] != CellState::Test) continue;

			unsigned stride = index * (VOID_TRIANGLES_COUNT * BYTES_PER_VOID_TRIANGLE + 1);
			
			int col = block_x * BLOCK_WIDTH + (it % BLOCK_WIDTH);
			int row = block_y * BLOCK_HEIGHT + (it / BLOCK_WIDTH);

			Point2 origin = get_point_grid_axis_position(row, col, mCellSize, 0, 0);

			uint8_t triangles = mCellTriangles[stride];
			
			total_triangles += triangles;

			int valid = 0;

			for (int t = 0; t < VOID_TRIANGLES_COUNT && t < triangles; t++) {
				uint8_t p1x = mCellTriangles[stride + 1 + t * BYTES_PER_VOID_TRIANGLE + 0];
				uint8_t p1y = mCellTriangles[stride + 1 + t * BYTES_PER_VOID_TRIANGLE + 1];
				uint8_t p2x = mCellTriangles[stride + 1 + t * BYTES_PER_VOID_TRIANGLE + 2];
				uint8_t p2y = mCellTriangles[stride + 1 + t * BYTES_PER_VOID_TRIANGLE + 3];
				uint8_t p3x = mCellTriangles[stride + 1 + t * BYTES_PER_VOID_TRIANGLE + 4];
				uint8_t p3y = mCellTriangles[stride + 1 + t * BYTES_PER_VOID_TRIANGLE + 5];

				Point2 triangle[3];

				triangle[0].x = origin.x + ((p1x / (double)256) * mCellSize);
				triangle[0].y = origin.y + ((p1y / (double)256) * mCellSize);
				triangle[1].x = origin.x + ((p2x / (double)256) * mCellSize);
				triangle[1].y = origin.y + ((p2y / (double)256) * mCellSize);
				triangle[2].x = origin.x + ((p3x / (double)256) * mCellSize);
				triangle[2].y = origin.y + ((p3y / (double)256) * mCellSize);

				int invalid_triangles = 0;

				int invalid = fabs(orient2d(triangle[0], triangle[1], triangle[2])) < 1e-8;


				for (int r = -2; r <= +2 && invalid == 0; r++) {
					for (int c = -2; c <= +2 && invalid == 0; c++) {
						const int neighbour_row = row + r;
						const int neighbour_col = col + c;

						const int neighbour_y_block = neighbour_row / BLOCK_WIDTH;
						const int neighbour_x_block = neighbour_col / BLOCK_HEIGHT;
						const int neighbour_row_block = neighbour_row % BLOCK_WIDTH;
						const int neighbour_col_block = neighbour_col % BLOCK_HEIGHT;

						const int neighbour_block = neighbour_y_block * BLOCK_SIZE * blocks_per_row + neighbour_x_block * BLOCK_SIZE;
						const int neighbour_index = neighbour_block + neighbour_row_block * BLOCK_WIDTH + neighbour_col_block;

						if (mCellState[neighbour_index] == CellState::Done) {
							Point2 point_neighbour = get_point_grid_axis_position(neighbour_row, neighbour_col, mCellSize, mCellPointCoords[neighbour_index * 2 + X_INDEX], mCellPointCoords[neighbour_index * 2 + Y_INDEX]);


							if (is_point_inside_circle(triangle[0], point_neighbour, mRadius, eps) && is_point_inside_circle(triangle[1], point_neighbour, mRadius, eps) && is_point_inside_circle(triangle[2], point_neighbour, mRadius, eps)) {
								invalid = 1;
							}

							if (invalid == 0 && is_point_inside_circle(triangle[0], point_neighbour, mRadius, eps) || is_point_inside_circle(triangle[1], point_neighbour, mRadius, eps) || is_point_inside_circle(triangle[2], point_neighbour, mRadius, eps)) {

								Point2 divided_triangles[2][3];

								int resulting_triangles = divide_triangle_by_chord(triangle, point_neighbour, mRadius, divided_triangles);

								if (resulting_triangles > 0) {
									if (fabs(orient2d(divided_triangles[0][0], divided_triangles[0][1], divided_triangles[0][2])) < 1e-5) {
										invalid = 1;
									}
									else {
										encodeTriangle(divided_triangles[0][0], divided_triangles[0][1], divided_triangles[0][2], origin, index, t);
										//printf("t=polygon((%f, %f), (%f, %f), (%f, %f))\n", triangle[0].x, triangle[0].y, triangle[1].x, triangle[1].y, triangle[2].x, triangle[2].y);
										//printf("(x - %f)^2 + (y - %f)^2=%f^2\n", point_neighbour.x, point_neighbour.y, mRadius);
										
										uint8_t p1x = mCellTriangles[stride + 1 + t * BYTES_PER_VOID_TRIANGLE + 0];
										uint8_t p1y = mCellTriangles[stride + 1 + t * BYTES_PER_VOID_TRIANGLE + 1];
										uint8_t p2x = mCellTriangles[stride + 1 + t * BYTES_PER_VOID_TRIANGLE + 2];
										uint8_t p2y = mCellTriangles[stride + 1 + t * BYTES_PER_VOID_TRIANGLE + 3];
										uint8_t p3x = mCellTriangles[stride + 1 + t * BYTES_PER_VOID_TRIANGLE + 4];
										uint8_t p3y = mCellTriangles[stride + 1 + t * BYTES_PER_VOID_TRIANGLE + 5];

										triangle[0].x = origin.x + ((p1x / (double)256) * mCellSize);
										triangle[0].y = origin.y + ((p1y / (double)256) * mCellSize);
										triangle[1].x = origin.x + ((p2x / (double)256) * mCellSize);
										triangle[1].y = origin.y + ((p2y / (double)256) * mCellSize);
										triangle[2].x = origin.x + ((p3x / (double)256) * mCellSize);
										triangle[2].y = origin.y + ((p3y / (double)256) * mCellSize);

									}
								}

								if (resulting_triangles == 2 && mCellTriangles[stride] + 1 <= VOID_TRIANGLES_COUNT) {
									if (fabs(orient2d(divided_triangles[1][0], divided_triangles[1][1], divided_triangles[1][2])) < 1e-5) {

										encodeTriangle(divided_triangles[1][0], divided_triangles[1][1], divided_triangles[1][2], origin, index, mCellTriangles[stride]);

										uint8_t p1x = mCellTriangles[stride + 1 + mCellTriangles[stride] * BYTES_PER_VOID_TRIANGLE + 0];
										uint8_t p1y = mCellTriangles[stride + 1 + mCellTriangles[stride] * BYTES_PER_VOID_TRIANGLE + 1];
										uint8_t p2x = mCellTriangles[stride + 1 + mCellTriangles[stride] * BYTES_PER_VOID_TRIANGLE + 2];
										uint8_t p2y = mCellTriangles[stride + 1 + mCellTriangles[stride] * BYTES_PER_VOID_TRIANGLE + 3];
										uint8_t p3x = mCellTriangles[stride + 1 + mCellTriangles[stride] * BYTES_PER_VOID_TRIANGLE + 4];
										uint8_t p3y = mCellTriangles[stride + 1 + mCellTriangles[stride] * BYTES_PER_VOID_TRIANGLE + 5];

										mCellTriangles[stride] = mCellTriangles[stride] + 1;
									}
								}

								if (is_point_inside_circle(triangle[0], point_neighbour, mRadius, eps) && is_point_inside_circle(triangle[1], point_neighbour, mRadius, eps) && is_point_inside_circle(triangle[2], point_neighbour, mRadius, eps)) {
									invalid = 1;
								}

								if (fabs(orient2d(triangle[0], triangle[1], triangle[2])) < 1e-5) {
									invalid = 1;
								}
							}
						}
					}
				}


				if (invalid) { // pop triangle from list
					mCellTriangles[stride] = mCellTriangles[stride] - 1;

					for (int j = 0; j < BYTES_PER_VOID_TRIANGLE; j++) {
						mCellTriangles[stride + 1 + t * BYTES_PER_VOID_TRIANGLE + j] = mCellTriangles[stride + 1 + mCellTriangles[stride] * BYTES_PER_VOID_TRIANGLE + j];
					}
				}
				else {
					double area = fabs(orient2d(triangle[0], triangle[1], triangle[2]));

					if (area > best_area[it]) {
						best_area[it] = area;
						best_triangle[it] = t;

						seed = shuffle_bits(seed);

						Point2 point;// = random_point_inside_triangle(seed, triangle[0], triangle[1], triangle[2]);
						
						point.x = (triangle[0].x + triangle[1].x + triangle[2].x) / 3;
						point.y = (triangle[0].y + triangle[1].y + triangle[2].y) / 3;
						
						valid = 1;

						//printf("(%f, %f)\n", point.x, point.y);
						
						mCellPointCoords[2 * index + X_INDEX] = ((point.x - origin.x) / mCellSize) * 256;
						mCellPointCoords[2 * index + Y_INDEX] = ((point.y - origin.x) / mCellSize) * 256;
					}
				}
			}
			
			if (valid == 0) {
				mCellState[index] = CellState::Rejected;
			}
		}
		
		if (total_triangles == 0) {
			//printf("tries: %u\n", tries);
			break;
		}

		// NOTE TODO: in gpu compute add global_barrier

		for (uint64_t it = 0; it < BLOCK_SIZE; it++) {
			int index = block_start + it;

			if (mCellState[index] != CellState::Test) continue;

			int col = block_x * BLOCK_WIDTH + (it % BLOCK_WIDTH);
			int row = block_y * BLOCK_HEIGHT + (it / BLOCK_WIDTH);

			Point2 point = get_point_grid_axis_position(row, col, mCellSize, mCellPointCoords[index * 2 + X_INDEX], mCellPointCoords[index * 2 + Y_INDEX]);
			

			for (int r = -2; r <= +2; r++) {
				for (int c = -2; c <= +2; c++) {
					if (r == 0 && c == 0) continue;

					const int neighbour_row = row + r;
					const int neighbour_col = col + c;

					const int neighbour_y_block = neighbour_row / BLOCK_WIDTH;
					const int neighbour_x_block = neighbour_col / BLOCK_HEIGHT;
					const int neighbour_row_block = neighbour_row % BLOCK_WIDTH;
					const int neighbour_col_block = neighbour_col % BLOCK_HEIGHT;

					const int neighbour_block = neighbour_y_block * BLOCK_SIZE * blocks_per_row + neighbour_x_block * BLOCK_SIZE;
					const int neighbour_index = neighbour_block + neighbour_row_block * BLOCK_WIDTH + neighbour_col_block;


					if (mCellState[neighbour_index] == CellState::Done) {
						Point2 point_neighbour = get_point_grid_axis_position(neighbour_row, neighbour_col, mCellSize, mCellPointCoords[neighbour_index * 2 + X_INDEX], mCellPointCoords[neighbour_index * 2 + Y_INDEX]);

						if (is_point_inside_circle(point, point_neighbour, mRadius, eps)) {
							mCellState[index] = CellState::Empty;
						}
					}
				}
			}
		}

		// NOTE TODO: in gpu compute add global_barrier

		for (uint64_t it = 0; it < BLOCK_SIZE; it++) {
			int index = block_start + it;

			if (mCellState[index] != CellState::Test) continue;

			int col = block_x * BLOCK_WIDTH + (it % BLOCK_WIDTH);
			int row = block_y * BLOCK_HEIGHT + (it / BLOCK_WIDTH);

			Point2 point = get_point_grid_axis_position(row, col, mCellSize, mCellPointCoords[index * 2 + X_INDEX], mCellPointCoords[index * 2 + Y_INDEX]);
			
			for (int r = -2; r <= +2; r++) {
				for (int c = -2; c <= +2; c++) {
					if (r == 0 && c == 0) continue;

					const int neighbour_row = row + r;
					const int neighbour_col = col + c;

					const int neighbour_y_block = neighbour_row / BLOCK_WIDTH;
					const int neighbour_x_block = neighbour_col / BLOCK_HEIGHT;
					const int neighbour_row_block = neighbour_row % BLOCK_WIDTH;
					const int neighbour_col_block = neighbour_col % BLOCK_HEIGHT;

					const int neighbour_block = neighbour_y_block * BLOCK_SIZE * blocks_per_row + neighbour_x_block * BLOCK_SIZE;
					const int neighbour_index = neighbour_block + neighbour_row_block * BLOCK_WIDTH + neighbour_col_block;

					if (mCellState[neighbour_index] == CellState::Test || mCellState[neighbour_index] == CellState::Accepted) {
						Point2 point_neighbour = get_point_grid_axis_position(neighbour_row, neighbour_col, mCellSize, mCellPointCoords[neighbour_index * 2 + X_INDEX], mCellPointCoords[neighbour_index * 2 + Y_INDEX]);

						if (is_point_inside_circle(point, point_neighbour, mRadius, eps)) {
							mCellState[index] = CellState::Empty;
						}
					}
				}
			}

			if (mCellState[index] == CellState::Test) {
				mCellState[index] = CellState::Accepted;
			}
		}

		for (uint64_t i = 0; i < BLOCK_SIZE; i++) {
			int index = block_start + i;

			if (mCellState[index] == CellState::Accepted) {
				mCellState[index] = CellState::Done; 
			}
		}
	}


	return seed;
}


uint64_t PoissonDiskSampler::maximumSampleCount(const double radius, const double width, const double height)
{
	double mCellSize = grid_get_cell_size(radius);

	int64_t mCols = grid_get_axis_size(mCellSize, width);
	int64_t mRows = grid_get_axis_size(mCellSize, height);

	return mCols * mRows;
}


std::vector<Point2> PoissonDiskSampler::generate(const Settings &settings)
{
  mRadius = settings.radius;
  mCellSize = grid_get_cell_size(settings.radius);

  // The grid is padded by one block on every side. Blocks read a halo of their
  // neighbours, so the border blocks need neighbours that exist; the padding is
  // subtracted back off every emitted sample.
  mCols = grid_get_axis_size(mCellSize, settings.width) + 2 * BLOCK_WIDTH;
  mRows = grid_get_axis_size(mCellSize, settings.height) + 2 * BLOCK_HEIGHT;

  const size_t cellCount = static_cast<size_t>(mCols) * static_cast<size_t>(mRows);
  mCellState.assign(cellCount, CellState::Empty);
  mCellPointCoords.assign(cellCount * 2u, 0u);
  mCellTriangles.assign(cellCount * (BYTES_PER_VOID_TRIANGLE * VOID_TRIANGLES_COUNT + 1), 0u);

  const int64_t blocksPerRow = mCols / BLOCK_WIDTH;
  const int64_t blocksPerCol = mRows / BLOCK_HEIGHT;

  // Blocks are independent: this pair of loops is the dispatch.
  //
  // `blocksPerRow` counts blocks along x and `blocksPerCol` along y. Upstream
  // drove y by the x count and vice versa, which is invisible on a square
  // region and walks off the grid on any other.
  uint64_t seed = settings.seed;
  for (int64_t y = 1; y < blocksPerCol - 1; y += 1)
  {
    for (int64_t x = 1; x < blocksPerRow - 1; x += 1)
    {
      seed = dartThrowing(seed, static_cast<uint64_t>(x), static_cast<uint64_t>(y));

      if (settings.method != Method::FillVoids) continue;

      // Transcribed from the C driver rather than factored into a helper: the
      // empty count is recomputed between the two phases because the first one
      // fills cells, and collapsing that changes which blocks get the second.
      int emptyCount = 0;
      for (int64_t i = 0; i < BLOCK_SIZE; i++)
      {
        const int64_t blockStart = y * BLOCK_SIZE * blocksPerRow + x * BLOCK_SIZE;
        const int64_t index = blockStart + i;
        if (mCellState[index] == CellState::Empty) emptyCount = emptyCount + 1;
      }

      if (emptyCount != 0)
        seed = generateTriangleVoids(seed, static_cast<uint64_t>(x), static_cast<uint64_t>(y));

      emptyCount = 0;
      for (int64_t i = 0; i < BLOCK_SIZE; i++)
      {
        const int64_t blockStart = y * BLOCK_SIZE * blocksPerRow + x * BLOCK_SIZE;
        const int64_t index = blockStart + i;
        if (mCellState[index] == CellState::Empty) emptyCount = emptyCount + 1;
      }

      if (emptyCount != 0)
        seed = pointInsideTriangleVoids(seed, static_cast<uint64_t>(x), static_cast<uint64_t>(y));
    }
  }

  std::vector<Point2> samples;
  for (int64_t y = 1; y < blocksPerCol - 1; y += 1)
  {
    for (int64_t x = 1; x < blocksPerRow - 1; x += 1)
    {
      for (int64_t i = 0; i < BLOCK_SIZE; i++)
      {
        const int64_t blockStart = y * BLOCK_SIZE * blocksPerRow + x * BLOCK_SIZE;
        const int64_t index = blockStart + i;
        const int64_t col = x * BLOCK_WIDTH + (i % BLOCK_WIDTH);
        const int64_t row = y * BLOCK_HEIGHT + (i / BLOCK_WIDTH);

        if (mCellState[index] != CellState::Done) continue;

        Point2 p = get_point_grid_axis_position(
            row, col, mCellSize, mCellPointCoords[2 * index + X_INDEX],
            mCellPointCoords[2 * index + Y_INDEX]);

        // Undo the one-block padding the grid was built with.
        p.x -= mCellSize * BLOCK_WIDTH;
        p.y -= mCellSize * BLOCK_HEIGHT;

        if (p.x < 0.0 || p.y < 0.0 || p.x >= settings.width || p.y >= settings.height) continue;

        samples.push_back(p);
      }
    }
  }

  return samples;
}

} // namespace procedural
