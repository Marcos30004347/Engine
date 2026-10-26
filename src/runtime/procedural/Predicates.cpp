#include "procedural/Predicates.hpp"

#include <cmath>

namespace procedural
{

// Shewchuk's adaptive predicates, moved here wholesale.
//
// This is a published reference implementation of exact floating-point
// arithmetic and it is deliberately left recognisable rather than restyled: its
// correctness rests on the exact sequence of roundings the expressions produce,
// and "tidying" an error-free transformation silently breaks it. It is confined
// to this namespace and reached only through the functions at the bottom of the
// file.
//
// The one substantive change from the published source is that the
// `enow = e[++eindex]` idiom, which reads one element past the end of an
// expansion and discards the value, is guarded. It is harmless on hardware but
// it is a genuine out-of-bounds read, and under optimisation here it was enough
// to make the triangulator's flip loop fail to terminate.
namespace detail
{

using real_t = double;
using natural_t = unsigned long long;

struct point_t
{
  real_t x;
  real_t y;
};

#define INEXACT


typedef struct {
	real_t epsilon;
	real_t splitter;
	real_t resulterrbound;
	real_t ccwerrboundA;
	real_t ccwerrboundB;
	real_t ccwerrboundC;
	real_t o3derrboundA;
	real_t o3derrboundB;
	real_t o3derrboundC;
	real_t iccerrboundA;
	real_t iccerrboundB;
	real_t iccerrboundC;
	real_t isperrboundA;
	real_t isperrboundB;
	real_t isperrboundC;
} coeficients_t;


static coeficients_t coef;


static coeficients_t get_coefficients()
{
	real_t half;
	real_t check, lastcheck;

	int every_other;


	every_other = 1;
	half = 0.5;
	coef.epsilon = 1.0;
	coef.splitter = 1.0;
	check = 1.0;

	/* Repeatedly divide `epsilon' by two until it is too small to add to    */
	/*   one without causing roundoff.  (Also check if the sum is equal to   */
	/*   the previous sum, for machines that round up instead of using exact */
	/*   rounding.  Not that this library will work on such machines anyway. */

	do {
		lastcheck = check;
		coef.epsilon *= half;
		if (every_other) {
			coef.splitter *= 2.0;
		}
		every_other = !every_other;
		check = 1.0 + coef.epsilon;
	} while ((check != 1.0) && (check != lastcheck));

	coef.splitter += 1.0;

	coef.resulterrbound = (3.0 + 8.0 * coef.epsilon) * coef.epsilon;
	coef.ccwerrboundA = (3.0 + 16.0 * coef.epsilon) * coef.epsilon;
	coef.ccwerrboundB = (2.0 + 12.0 * coef.epsilon) * coef.epsilon;
	coef.ccwerrboundC = (9.0 + 64.0 * coef.epsilon) * coef.epsilon * coef.epsilon;
	coef.o3derrboundA = (7.0 + 56.0 * coef.epsilon) * coef.epsilon;
	coef.o3derrboundB = (3.0 + 28.0 * coef.epsilon) * coef.epsilon;
	coef.o3derrboundC = (26.0 + 288.0 * coef.epsilon) * coef.epsilon * coef.epsilon;
	coef.iccerrboundA = (10.0 + 96.0 * coef.epsilon) * coef.epsilon;
	coef.iccerrboundB = (4.0 + 48.0 * coef.epsilon) * coef.epsilon;
	coef.iccerrboundC = (44.0 + 576.0 * coef.epsilon) * coef.epsilon * coef.epsilon;
	coef.isperrboundA = (16.0 + 224.0 * coef.epsilon) * coef.epsilon;
	coef.isperrboundB = (5.0 + 72.0 * coef.epsilon) * coef.epsilon;
	coef.isperrboundC = (71.0 + 1408.0 * coef.epsilon) * coef.epsilon * coef.epsilon;
}


void init_exact() {
	get_coefficients();
}

#define absolute(a)  ((a) >= 0.0 ? (a) : -(a))

#define fast_two_sum_tail(a, b, x, y) \
    bvirt = x - a; \
    y = b - bvirt

#define fast_two_sum(a, b, x, y) \
    x = (real_t) (a + b); \
    fast_two_sum_tail(a, b, x, y)

#define two_sum_tail(a, b, x, y) \
    bvirt = (real_t) (x - a); \
    avirt = x - bvirt; \
    bround = b - bvirt; \
    around = a - avirt; \
    y = around + bround

#define Two_Sum(a, b, x, y) \
    x = (real_t) (a + b); \
    two_sum_tail(a, b, x, y)

#define two_diff_Tail(a, b, x, y) \
    bvirt = (real_t) (a - x); \
    avirt = x + bvirt; \
    bround = bvirt - b; \
    around = a - avirt; \
    y = around + bround

#define two_diff(a, b, x, y) \
    x = (real_t) (a - b); \
    two_diff_Tail(a, b, x, y)

#define split(a, ahi, alo) \
    c = (real_t) (coef.splitter * a); \
    abig = (real_t) (c - a); \
    ahi = c - abig; \
    alo = a - ahi

#define two_product_tail(a, b, x, y) \
    split(a, ahi, alo); \
    split(b, bhi, blo); \
    err1 = x - (ahi * bhi); \
    err2 = err1 - (alo * bhi); \
    err3 = err2 - (ahi * blo); \
    y = (alo * blo) - err3

#define two_product(a, b, x, y) \
    x = (real_t) (a * b); \
    two_product_tail(a, b, x, y)

#define two_product_presplit(a, b, bhi, blo, x, y) \
    x = (real_t) (a * b); \
    split(a, ahi, alo); \
    err1 = x - (ahi * bhi); \
    err2 = err1 - (alo * bhi); \
    err3 = err2 - (ahi * blo); \
    y = (alo * blo) - err3

#define square_tail(a, x, y) \
    split(a, ahi, alo); \
    err1 = x - (ahi * ahi); \
    err3 = err1 - ((ahi + ahi) * alo); \
    y = (alo * alo) - err3

#define square(a, x, y) \
    x = (real_t) (a * a); \
    square_tail(a, x, y)

#define two_one_sum(a1, a0, b, x2, x1, x0) \
    Two_Sum(a0, b , _i, x0); \
    Two_Sum(a1, _i, x2, x1)

#define two_one_diff(a1, a0, b, x2, x1, x0) \
    two_diff(a0, b , _i, x0); \
    Two_Sum( a1, _i, x2, x1)

#define two_two_sum(a1, a0, b1, b0, x3, x2, x1, x0) \
    two_one_sum(a1, a0, b0, _j, _0, x0); \
    two_one_sum(_j, _0, b1, x3, x2, x1)

#define two_two_diff(a1, a0, b1, b0, x3, x2, x1, x0) \
    two_one_diff(a1, a0, b0, _j, _0, x0); \
    two_one_diff(_j, _0, b1, x3, x2, x1)

#define Two_One_Product(a1, a0, b, x3, x2, x1, x0) \
    split(b, bhi, blo); \
    two_product_presplit(a0, b, bhi, blo, _i, x0); \
    two_product_presplit(a1, b, bhi, blo, _j, _0); \
    Two_Sum(_i, _0, _k, x1); \
    fast_two_sum(_j, _k, x3, x2)


/* Divergence from upstream (and from Shewchuk's published source): the
   `enow = e[++eindex]` idiom reads one element past the end of the expansion on
   the last iteration of each loop, then exits without using the value. It is
   harmless on real hardware, which is why it has survived everywhere, but it is
   a genuine out-of-bounds read and AddressSanitizer aborts the process on it.
   The reads are guarded below; nothing else about the arithmetic changes. */
int fast_expansion_sum_zeroelim(int elen, real_t* e, int flen, real_t* f, real_t* h)
{
	real_t Q;
	INEXACT real_t Qnew;
	INEXACT real_t hh;
	INEXACT real_t bvirt;
	real_t avirt, bround, around;
	int eindex, findex, hindex;
	real_t enow, fnow;

	enow = e[0];
	fnow = f[0];
	eindex = findex = 0;
	if ((fnow > enow) == (fnow > -enow)) {
		Q = enow;
		++eindex; if (eindex < elen) enow = e[eindex];
	}
	else {
		Q = fnow;
		++findex; if (findex < flen) fnow = f[findex];
	}
	hindex = 0;
	if ((eindex < elen) && (findex < flen)) {
		if ((fnow > enow) == (fnow > -enow)) {
			fast_two_sum(enow, Q, Qnew, hh);
			++eindex; if (eindex < elen) enow = e[eindex];
		}
		else {
			fast_two_sum(fnow, Q, Qnew, hh);
			++findex; if (findex < flen) fnow = f[findex];
		}
		Q = Qnew;
		if (hh != 0.0) {
			h[hindex++] = hh;
		}
		while ((eindex < elen) && (findex < flen)) {
			if ((fnow > enow) == (fnow > -enow)) {
				Two_Sum(Q, enow, Qnew, hh);
				++eindex; if (eindex < elen) enow = e[eindex];
			}
			else {
				Two_Sum(Q, fnow, Qnew, hh);
				++findex; if (findex < flen) fnow = f[findex];
			}
			Q = Qnew;
			if (hh != 0.0) {
				h[hindex++] = hh;
			}
		}
	}
	while (eindex < elen) {
		Two_Sum(Q, enow, Qnew, hh);
		++eindex; if (eindex < elen) enow = e[eindex];
		Q = Qnew;
		if (hh != 0.0) {
			h[hindex++] = hh;
		}
	}
	while (findex < flen) {
		Two_Sum(Q, fnow, Qnew, hh);
		++findex; if (findex < flen) fnow = f[findex];
		Q = Qnew;
		if (hh != 0.0) {
			h[hindex++] = hh;
		}
	}
	if ((Q != 0.0) || (hindex == 0)) {
		h[hindex++] = Q;
	}
	return hindex;
}


int scale_expansion_zeroelim(int elen, real_t* e, real_t b, real_t* h)
{
	INEXACT real_t Q, sum;
	real_t hh;
	INEXACT real_t product1;
	real_t product0;
	int eindex, hindex;
	real_t enow;
	INEXACT real_t bvirt;
	real_t avirt, bround, around;
	INEXACT real_t c;
	INEXACT real_t abig;
	real_t ahi, alo, bhi, blo;
	real_t err1, err2, err3;

	split(b, bhi, blo);
	two_product_presplit(e[0], b, bhi, blo, Q, hh);
	hindex = 0;
	if (hh != 0) {
		h[hindex++] = hh;
	}
	for (eindex = 1; eindex < elen; eindex++) {
		enow = e[eindex];
		two_product_presplit(enow, b, bhi, blo, product1, product0);
		Two_Sum(Q, product0, sum, hh);
		if (hh != 0) {
			h[hindex++] = hh;
		}
		fast_two_sum(product1, sum, Q, hh);
		if (hh != 0) {
			h[hindex++] = hh;
		}
	}
	if ((Q != 0.0) || (hindex == 0)) {
		h[hindex++] = Q;
	}
	return hindex;
}

//*****************************************************************************
//*                                                                           *
//*  estimate()   Produce a one-word gpudt_estimate of an expansion's value.        *
//*                                                                           *
//*  See my Robust Predicates paper for details.                              *
//*                                                                           *
//*****************************************************************************

real_t estimate(int elen, real_t* e)
{
	real_t Q;
	int eindex;

	Q = e[0];
	for (eindex = 1; eindex < elen; eindex++) {
		Q += e[eindex];
	}
	return Q;
}

/*****************************************************************************/
/*                                                                           */
/*  orient2dfast()   Approximate 2D orientation test.  Nonrobust.            */
/*  orient2dexact()   Exact 2D orientation test.  Robust.                    */
/*  orient2dslow()   Another exact 2D orientation test.  Robust.             */
/*  orient2d()   Adaptive exact 2D orientation test.  Robust.                */
/*                                                                           */
/*               Return a positive value if the points pa, pb, and pc occur  */
/*               in counterclockwise order; a negative value if they occur   */
/*               in clockwise order; and zero if they are collinear.  The    */
/*               result is also a rough approximation of twice the signed    */
/*               area of the triangle defined by the three points.           */
/*                                                                           */
/*  Only the first and last routine should be used; the middle two are for   */
/*  timings.                                                                 */
/*                                                                           */
/*  The last three use exact arithmetic to ensure a correct answer.  The     */
/*  result returned is the determinant of a matrix.  In orient2d() only,     */
/*  this determinant is computed adaptively, in the sense that exact         */
/*  arithmetic is used only to the degree it is needed to ensure that the    */
/*  returned value has the correct sign.  Hence, orient2d() is usually quite */
/*  fast, but will run more slowly when the input points are collinear or    */
/*  nearly so.                                                               */
/*                                                                           */
/*****************************************************************************/
real_t orient2dadapt(const struct point_t pa, const struct point_t pb, const struct point_t pc, real_t detsum)
{
	INEXACT real_t acx, acy, bcx, bcy;
	real_t acxtail, acytail, bcxtail, bcytail;
	INEXACT real_t detleft, detright;
	real_t detlefttail, detrighttail;
	real_t det, errbound;
	real_t B[4], C1[8], C2[12], D[16];
	INEXACT real_t B3;
	int C1length, C2length, Dlength;
	real_t u[4];
	INEXACT real_t u3;
	INEXACT real_t s1, t1;
	real_t s0, t0;

	INEXACT real_t bvirt;
	real_t avirt, bround, around;
	INEXACT real_t c;
	INEXACT real_t abig;
	real_t ahi, alo, bhi, blo;
	real_t err1, err2, err3;
	INEXACT real_t _i, _j;
	real_t _0;

	acx = (real_t)(pa.x - pc.x);
	bcx = (real_t)(pb.x - pc.x);
	acy = (real_t)(pa.y - pc.y);
	bcy = (real_t)(pb.y - pc.y);

	two_product(acx, bcy, detleft, detlefttail);
	two_product(acy, bcx, detright, detrighttail);

	two_two_diff(detleft, detlefttail, detright, detrighttail,
		B3, B[2], B[1], B[0]);
	B[3] = B3;

	det = estimate(4, B);
	errbound = coef.ccwerrboundB * detsum;
	if ((det >= errbound) || (-det >= errbound)) {
		return det;
	}

	two_diff_Tail(pa.x, pc.x, acx, acxtail);
	two_diff_Tail(pb.x, pc.x, bcx, bcxtail);
	two_diff_Tail(pa.y, pc.y, acy, acytail);
	two_diff_Tail(pb.y, pc.y, bcy, bcytail);

	if ((acxtail == 0.0) && (acytail == 0.0)
		&& (bcxtail == 0.0) && (bcytail == 0.0)) {
		return det;
	}

	errbound = coef.ccwerrboundC * detsum + coef.resulterrbound * absolute(det);
	det += (acx * bcytail + bcy * acxtail)
		- (acy * bcxtail + bcx * acytail);
	if ((det >= errbound) || (-det >= errbound)) {
		return det;
	}

	two_product(acxtail, bcy, s1, s0);
	two_product(acytail, bcx, t1, t0);
	two_two_diff(s1, s0, t1, t0, u3, u[2], u[1], u[0]);
	u[3] = u3;
	C1length = fast_expansion_sum_zeroelim(4, B, 4, u, C1);

	two_product(acx, bcytail, s1, s0);
	two_product(acy, bcxtail, t1, t0);
	two_two_diff(s1, s0, t1, t0, u3, u[2], u[1], u[0]);
	u[3] = u3;
	C2length = fast_expansion_sum_zeroelim(C1length, C1, 4, u, C2);

	two_product(acxtail, bcytail, s1, s0);
	two_product(acytail, bcxtail, t1, t0);
	two_two_diff(s1, s0, t1, t0, u3, u[2], u[1], u[0]);
	u[3] = u3;
	Dlength = fast_expansion_sum_zeroelim(C2length, C2, 4, u, D);

	return(D[Dlength - 1]);
}

real_t orient2dexact(const struct point_t pa, const struct point_t pb, const struct point_t pc)
{
	INEXACT real_t axby1, axcy1, bxcy1, bxay1, cxay1, cxby1;
	real_t axby0, axcy0, bxcy0, bxay0, cxay0, cxby0;
	real_t aterms[4], bterms[4], cterms[4];
	INEXACT real_t aterms3, bterms3, cterms3;
	real_t v[8], w[12];
	int vlength, wlength;

	INEXACT real_t bvirt;
	real_t avirt, bround, around;
	INEXACT real_t c;
	INEXACT real_t abig;
	real_t ahi, alo, bhi, blo;
	real_t err1, err2, err3;
	INEXACT real_t _i, _j;
	real_t _0;

	two_product(pa.x, pb.y, axby1, axby0);
	two_product(pa.x, pc.y, axcy1, axcy0);
	two_two_diff(axby1, axby0, axcy1, axcy0,
		aterms3, aterms[2], aterms[1], aterms[0]);
	aterms[3] = aterms3;

	two_product(pb.x, pc.y, bxcy1, bxcy0);
	two_product(pb.x, pa.y, bxay1, bxay0);
	two_two_diff(bxcy1, bxcy0, bxay1, bxay0,
		bterms3, bterms[2], bterms[1], bterms[0]);
	bterms[3] = bterms3;

	two_product(pc.x, pa.y, cxay1, cxay0);
	two_product(pc.x, pb.y, cxby1, cxby0);
	two_two_diff(cxay1, cxay0, cxby1, cxby0,
		cterms3, cterms[2], cterms[1], cterms[0]);
	cterms[3] = cterms3;

	vlength = fast_expansion_sum_zeroelim(4, aterms, 4, bterms, v);
	wlength = fast_expansion_sum_zeroelim(vlength, v, 4, cterms, w);

	return w[wlength - 1];
}

real_t orient2d(const struct point_t pa, const struct point_t pb, const struct point_t pc)
{
	real_t detleft, detright, det;
	real_t detsum, errbound;

	detleft = (pa.x - pc.x) * (pb.y - pc.y);
	detright = (pa.y - pc.y) * (pb.x - pc.x);
	det = detleft - detright;

	if (detleft > 0.0) {
		if (detright <= 0.0) {
			return det;
		}
		else {
			detsum = detleft + detright;
		}
	}
	else if (detleft < 0.0) {
		if (detright >= 0.0) {
			return det;
		}
		else {
			detsum = -detleft - detright;
		}
	}
	else {
		return det;
	}

	errbound = coef.ccwerrboundA * detsum;
	if ((det >= errbound) || (-det >= errbound)) {
		return det;
	}

	return orient2dadapt(pa, pb, pc, detsum);
}


/*****************************************************************************/
/*                                                                           */
/*  incirclefast()   Approximate 2D incircle test.  Nonrobust.               */
/*  incircleexact()   Exact 2D incircle test.  Robust.                       */
/*  incircleslow()   Another exact 2D incircle test.  Robust.                */
/*  incircle()   Adaptive exact 2D incircle test.  Robust.                   */
/*                                                                           */
/*               Return a positive value if the point pd lies inside the     */
/*               circle passing through pa, pb, and pc; a negative value if  */
/*               it lies outside; and zero if the four points are cocircular.*/
/*               The points pa, pb, and pc must be in counterclockwise       */
/*               order, or the sign of the result will be reversed.          */
/*                                                                           */
/*  Only the first and last routine should be used; the middle two are for   */
/*  timings.                                                                 */
/*                                                                           */
/*  The last three use exact arithmetic to ensure a correct answer.  The     */
/*  result returned is the determinant of a matrix.  In incircle() only,     */
/*  this determinant is computed adaptively, in the sense that exact         */
/*  arithmetic is used only to the degree it is needed to ensure that the    */
/*  returned value has the correct sign.  Hence, incircle() is usually quite */
/*  fast, but will run more slowly when the input points are cocircular or   */
/*  nearly so.                                                               */
/*                                                                           */
/*****************************************************************************/

real_t incircleadapt(const struct point_t pa, const struct point_t pb, const struct point_t pc, const struct point_t pd, real_t permanent)
{
	INEXACT real_t adx, bdx, cdx, ady, bdy, cdy;
	real_t det, errbound;

	INEXACT real_t bdxcdy1, cdxbdy1, cdxady1, adxcdy1, adxbdy1, bdxady1;
	real_t bdxcdy0, cdxbdy0, cdxady0, adxcdy0, adxbdy0, bdxady0;
	real_t bc[4], ca[4], ab[4];
	INEXACT real_t bc3, ca3, ab3;
	real_t axbc[8], axxbc[16], aybc[8], ayybc[16], adet[32];
	int axbclen, axxbclen, aybclen, ayybclen, alen;
	real_t bxca[8], bxxca[16], byca[8], byyca[16], bdet[32];
	int bxcalen, bxxcalen, bycalen, byycalen, blen;
	real_t cxab[8], cxxab[16], cyab[8], cyyab[16], cdet[32];
	int cxablen, cxxablen, cyablen, cyyablen, clen;
	real_t abdet[64];
	int ablen;
	real_t fin1[1152], fin2[1152];
	real_t* finnow, * finother, * finswap;
	int finlength;

	real_t adxtail, bdxtail, cdxtail, adytail, bdytail, cdytail;
	INEXACT real_t adxadx1, adyady1, bdxbdx1, bdybdy1, cdxcdx1, cdycdy1;
	real_t adxadx0, adyady0, bdxbdx0, bdybdy0, cdxcdx0, cdycdy0;
	real_t aa[4], bb[4], cc[4];
	INEXACT real_t aa3, bb3, cc3;
	INEXACT real_t ti1, tj1;
	real_t ti0, tj0;
	real_t u[4], v[4];
	INEXACT real_t u3, v3;
	real_t temp8[8], temp16a[16], temp16b[16], temp16c[16];
	real_t temp32a[32], temp32b[32], temp48[48], temp64[64];
	int temp8len, temp16alen, temp16blen, temp16clen;
	int temp32alen, temp32blen, temp48len, temp64len;
	real_t axtbb[8], axtcc[8], aytbb[8], aytcc[8];
	int axtbblen, axtcclen, aytbblen, aytcclen;
	real_t bxtaa[8], bxtcc[8], bytaa[8], bytcc[8];
	int bxtaalen, bxtcclen, bytaalen, bytcclen;
	real_t cxtaa[8], cxtbb[8], cytaa[8], cytbb[8];
	int cxtaalen, cxtbblen, cytaalen, cytbblen;
	real_t axtbc[8], aytbc[8], bxtca[8], bytca[8], cxtab[8], cytab[8];
	int axtbclen, aytbclen, bxtcalen, bytcalen, cxtablen, cytablen;
	real_t axtbct[16], aytbct[16], bxtcat[16], bytcat[16], cxtabt[16], cytabt[16];
	int axtbctlen, aytbctlen, bxtcatlen, bytcatlen, cxtabtlen, cytabtlen;
	real_t axtbctt[8], aytbctt[8], bxtcatt[8];
	real_t bytcatt[8], cxtabtt[8], cytabtt[8];
	int axtbcttlen, aytbcttlen, bxtcattlen, bytcattlen, cxtabttlen, cytabttlen;
	real_t abt[8], bct[8], cat[8];
	int abtlen, bctlen, catlen;
	real_t abtt[4], bctt[4], catt[4];
	int abttlen, bcttlen, cattlen;
	INEXACT real_t abtt3, bctt3, catt3;
	real_t negate;

	INEXACT real_t bvirt;
	real_t avirt, bround, around;
	INEXACT real_t c;
	INEXACT real_t abig;
	real_t ahi, alo, bhi, blo;
	real_t err1, err2, err3;
	INEXACT real_t _i, _j;
	real_t _0;

	adx = (real_t)(pa.x - pd.x);
	bdx = (real_t)(pb.x - pd.x);
	cdx = (real_t)(pc.x - pd.x);
	ady = (real_t)(pa.y - pd.y);
	bdy = (real_t)(pb.y - pd.y);
	cdy = (real_t)(pc.y - pd.y);

	two_product(bdx, cdy, bdxcdy1, bdxcdy0);
	two_product(cdx, bdy, cdxbdy1, cdxbdy0);
	two_two_diff(bdxcdy1, bdxcdy0, cdxbdy1, cdxbdy0, bc3, bc[2], bc[1], bc[0]);
	bc[3] = bc3;
	axbclen = scale_expansion_zeroelim(4, bc, adx, axbc);
	axxbclen = scale_expansion_zeroelim(axbclen, axbc, adx, axxbc);
	aybclen = scale_expansion_zeroelim(4, bc, ady, aybc);
	ayybclen = scale_expansion_zeroelim(aybclen, aybc, ady, ayybc);
	alen = fast_expansion_sum_zeroelim(axxbclen, axxbc, ayybclen, ayybc, adet);

	two_product(cdx, ady, cdxady1, cdxady0);
	two_product(adx, cdy, adxcdy1, adxcdy0);
	two_two_diff(cdxady1, cdxady0, adxcdy1, adxcdy0, ca3, ca[2], ca[1], ca[0]);
	ca[3] = ca3;
	bxcalen = scale_expansion_zeroelim(4, ca, bdx, bxca);
	bxxcalen = scale_expansion_zeroelim(bxcalen, bxca, bdx, bxxca);
	bycalen = scale_expansion_zeroelim(4, ca, bdy, byca);
	byycalen = scale_expansion_zeroelim(bycalen, byca, bdy, byyca);
	blen = fast_expansion_sum_zeroelim(bxxcalen, bxxca, byycalen, byyca, bdet);

	two_product(adx, bdy, adxbdy1, adxbdy0);
	two_product(bdx, ady, bdxady1, bdxady0);
	two_two_diff(adxbdy1, adxbdy0, bdxady1, bdxady0, ab3, ab[2], ab[1], ab[0]);
	ab[3] = ab3;
	cxablen = scale_expansion_zeroelim(4, ab, cdx, cxab);
	cxxablen = scale_expansion_zeroelim(cxablen, cxab, cdx, cxxab);
	cyablen = scale_expansion_zeroelim(4, ab, cdy, cyab);
	cyyablen = scale_expansion_zeroelim(cyablen, cyab, cdy, cyyab);
	clen = fast_expansion_sum_zeroelim(cxxablen, cxxab, cyyablen, cyyab, cdet);

	ablen = fast_expansion_sum_zeroelim(alen, adet, blen, bdet, abdet);
	finlength = fast_expansion_sum_zeroelim(ablen, abdet, clen, cdet, fin1);

	det = estimate(finlength, fin1);
	errbound = coef.iccerrboundB * permanent;
	if ((det >= errbound) || (-det >= errbound)) {
		return det;
	}

	two_diff_Tail(pa.x, pd.x, adx, adxtail);
	two_diff_Tail(pa.y, pd.y, ady, adytail);
	two_diff_Tail(pb.x, pd.x, bdx, bdxtail);
	two_diff_Tail(pb.y, pd.y, bdy, bdytail);
	two_diff_Tail(pc.x, pd.x, cdx, cdxtail);
	two_diff_Tail(pc.y, pd.y, cdy, cdytail);
	if ((adxtail == 0.0) && (bdxtail == 0.0) && (cdxtail == 0.0)
		&& (adytail == 0.0) && (bdytail == 0.0) && (cdytail == 0.0)) {
		return det;
	}

	errbound = coef.iccerrboundC * permanent + coef.resulterrbound * absolute(det);
	det += ((adx * adx + ady * ady) * ((bdx * cdytail + cdy * bdxtail)
		- (bdy * cdxtail + cdx * bdytail))
		+ 2.0 * (adx * adxtail + ady * adytail) * (bdx * cdy - bdy * cdx))
		+ ((bdx * bdx + bdy * bdy) * ((cdx * adytail + ady * cdxtail)
			- (cdy * adxtail + adx * cdytail))
			+ 2.0 * (bdx * bdxtail + bdy * bdytail) * (cdx * ady - cdy * adx))
		+ ((cdx * cdx + cdy * cdy) * ((adx * bdytail + bdy * adxtail)
			- (ady * bdxtail + bdx * adytail))
			+ 2.0 * (cdx * cdxtail + cdy * cdytail) * (adx * bdy - ady * bdx));
	if ((det >= errbound) || (-det >= errbound)) {
		return det;
	}

	finnow = fin1;
	finother = fin2;

	if ((bdxtail != 0.0) || (bdytail != 0.0)
		|| (cdxtail != 0.0) || (cdytail != 0.0)) {
		square(adx, adxadx1, adxadx0);
		square(ady, adyady1, adyady0);
		two_two_sum(adxadx1, adxadx0, adyady1, adyady0, aa3, aa[2], aa[1], aa[0]);
		aa[3] = aa3;
	}
	if ((cdxtail != 0.0) || (cdytail != 0.0)
		|| (adxtail != 0.0) || (adytail != 0.0)) {
		square(bdx, bdxbdx1, bdxbdx0);
		square(bdy, bdybdy1, bdybdy0);
		two_two_sum(bdxbdx1, bdxbdx0, bdybdy1, bdybdy0, bb3, bb[2], bb[1], bb[0]);
		bb[3] = bb3;
	}
	if ((adxtail != 0.0) || (adytail != 0.0)
		|| (bdxtail != 0.0) || (bdytail != 0.0)) {
		square(cdx, cdxcdx1, cdxcdx0);
		square(cdy, cdycdy1, cdycdy0);
		two_two_sum(cdxcdx1, cdxcdx0, cdycdy1, cdycdy0, cc3, cc[2], cc[1], cc[0]);
		cc[3] = cc3;
	}

	if (adxtail != 0.0) {
		axtbclen = scale_expansion_zeroelim(4, bc, adxtail, axtbc);
		temp16alen = scale_expansion_zeroelim(axtbclen, axtbc, 2.0 * adx,
			temp16a);

		axtcclen = scale_expansion_zeroelim(4, cc, adxtail, axtcc);
		temp16blen = scale_expansion_zeroelim(axtcclen, axtcc, bdy, temp16b);

		axtbblen = scale_expansion_zeroelim(4, bb, adxtail, axtbb);
		temp16clen = scale_expansion_zeroelim(axtbblen, axtbb, -cdy, temp16c);

		temp32alen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
			temp16blen, temp16b, temp32a);
		temp48len = fast_expansion_sum_zeroelim(temp16clen, temp16c,
			temp32alen, temp32a, temp48);
		finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
			temp48, finother);
		finswap = finnow; finnow = finother; finother = finswap;
	}
	if (adytail != 0.0) {
		aytbclen = scale_expansion_zeroelim(4, bc, adytail, aytbc);
		temp16alen = scale_expansion_zeroelim(aytbclen, aytbc, 2.0 * ady,
			temp16a);

		aytbblen = scale_expansion_zeroelim(4, bb, adytail, aytbb);
		temp16blen = scale_expansion_zeroelim(aytbblen, aytbb, cdx, temp16b);

		aytcclen = scale_expansion_zeroelim(4, cc, adytail, aytcc);
		temp16clen = scale_expansion_zeroelim(aytcclen, aytcc, -bdx, temp16c);

		temp32alen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
			temp16blen, temp16b, temp32a);
		temp48len = fast_expansion_sum_zeroelim(temp16clen, temp16c,
			temp32alen, temp32a, temp48);
		finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
			temp48, finother);
		finswap = finnow; finnow = finother; finother = finswap;
	}
	if (bdxtail != 0.0) {
		bxtcalen = scale_expansion_zeroelim(4, ca, bdxtail, bxtca);
		temp16alen = scale_expansion_zeroelim(bxtcalen, bxtca, 2.0 * bdx,
			temp16a);

		bxtaalen = scale_expansion_zeroelim(4, aa, bdxtail, bxtaa);
		temp16blen = scale_expansion_zeroelim(bxtaalen, bxtaa, cdy, temp16b);

		bxtcclen = scale_expansion_zeroelim(4, cc, bdxtail, bxtcc);
		temp16clen = scale_expansion_zeroelim(bxtcclen, bxtcc, -ady, temp16c);

		temp32alen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
			temp16blen, temp16b, temp32a);
		temp48len = fast_expansion_sum_zeroelim(temp16clen, temp16c,
			temp32alen, temp32a, temp48);
		finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
			temp48, finother);
		finswap = finnow; finnow = finother; finother = finswap;
	}
	if (bdytail != 0.0) {
		bytcalen = scale_expansion_zeroelim(4, ca, bdytail, bytca);
		temp16alen = scale_expansion_zeroelim(bytcalen, bytca, 2.0 * bdy,
			temp16a);

		bytcclen = scale_expansion_zeroelim(4, cc, bdytail, bytcc);
		temp16blen = scale_expansion_zeroelim(bytcclen, bytcc, adx, temp16b);

		bytaalen = scale_expansion_zeroelim(4, aa, bdytail, bytaa);
		temp16clen = scale_expansion_zeroelim(bytaalen, bytaa, -cdx, temp16c);

		temp32alen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
			temp16blen, temp16b, temp32a);
		temp48len = fast_expansion_sum_zeroelim(temp16clen, temp16c,
			temp32alen, temp32a, temp48);
		finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
			temp48, finother);
		finswap = finnow; finnow = finother; finother = finswap;
	}
	if (cdxtail != 0.0) {
		cxtablen = scale_expansion_zeroelim(4, ab, cdxtail, cxtab);
		temp16alen = scale_expansion_zeroelim(cxtablen, cxtab, 2.0 * cdx,
			temp16a);

		cxtbblen = scale_expansion_zeroelim(4, bb, cdxtail, cxtbb);
		temp16blen = scale_expansion_zeroelim(cxtbblen, cxtbb, ady, temp16b);

		cxtaalen = scale_expansion_zeroelim(4, aa, cdxtail, cxtaa);
		temp16clen = scale_expansion_zeroelim(cxtaalen, cxtaa, -bdy, temp16c);

		temp32alen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
			temp16blen, temp16b, temp32a);
		temp48len = fast_expansion_sum_zeroelim(temp16clen, temp16c,
			temp32alen, temp32a, temp48);
		finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
			temp48, finother);
		finswap = finnow; finnow = finother; finother = finswap;
	}
	if (cdytail != 0.0) {
		cytablen = scale_expansion_zeroelim(4, ab, cdytail, cytab);
		temp16alen = scale_expansion_zeroelim(cytablen, cytab, 2.0 * cdy,
			temp16a);

		cytaalen = scale_expansion_zeroelim(4, aa, cdytail, cytaa);
		temp16blen = scale_expansion_zeroelim(cytaalen, cytaa, bdx, temp16b);

		cytbblen = scale_expansion_zeroelim(4, bb, cdytail, cytbb);
		temp16clen = scale_expansion_zeroelim(cytbblen, cytbb, -adx, temp16c);

		temp32alen = fast_expansion_sum_zeroelim(temp16alen, temp16a, temp16blen, temp16b, temp32a);
		temp48len = fast_expansion_sum_zeroelim(temp16clen, temp16c, temp32alen, temp32a, temp48);
		finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);

		finswap = finnow; finnow = finother; finother = finswap;
	}

	if ((adxtail != 0.0) || (adytail != 0.0)) {
		if ((bdxtail != 0.0) || (bdytail != 0.0)
			|| (cdxtail != 0.0) || (cdytail != 0.0)) {
			two_product(bdxtail, cdy, ti1, ti0);
			two_product(bdx, cdytail, tj1, tj0);
			two_two_sum(ti1, ti0, tj1, tj0, u3, u[2], u[1], u[0]);
			u[3] = u3;
			negate = -bdy;
			two_product(cdxtail, negate, ti1, ti0);
			negate = -bdytail;
			two_product(cdx, negate, tj1, tj0);
			two_two_sum(ti1, ti0, tj1, tj0, v3, v[2], v[1], v[0]);
			v[3] = v3;
			bctlen = fast_expansion_sum_zeroelim(4, u, 4, v, bct);

			two_product(bdxtail, cdytail, ti1, ti0);
			two_product(cdxtail, bdytail, tj1, tj0);
			two_two_diff(ti1, ti0, tj1, tj0, bctt3, bctt[2], bctt[1], bctt[0]);
			bctt[3] = bctt3;
			bcttlen = 4;
		}
		else {
			bct[0] = 0.0;
			bctlen = 1;
			bctt[0] = 0.0;
			bcttlen = 1;
		}

		if (adxtail != 0.0) {
			temp16alen = scale_expansion_zeroelim(axtbclen, axtbc, adxtail, temp16a);
			axtbctlen = scale_expansion_zeroelim(bctlen, bct, adxtail, axtbct);
			temp32alen = scale_expansion_zeroelim(axtbctlen, axtbct, 2.0 * adx,
				temp32a);
			temp48len = fast_expansion_sum_zeroelim(temp16alen, temp16a,
				temp32alen, temp32a, temp48);
			finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
				temp48, finother);
			finswap = finnow; finnow = finother; finother = finswap;
			if (bdytail != 0.0) {
				temp8len = scale_expansion_zeroelim(4, cc, adxtail, temp8);
				temp16alen = scale_expansion_zeroelim(temp8len, temp8, bdytail,
					temp16a);
				finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp16alen,
					temp16a, finother);
				finswap = finnow; finnow = finother; finother = finswap;
			}
			if (cdytail != 0.0) {
				temp8len = scale_expansion_zeroelim(4, bb, -adxtail, temp8);
				temp16alen = scale_expansion_zeroelim(temp8len, temp8, cdytail,
					temp16a);
				finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp16alen,
					temp16a, finother);
				finswap = finnow; finnow = finother; finother = finswap;
			}

			temp32alen = scale_expansion_zeroelim(axtbctlen, axtbct, adxtail,
				temp32a);
			axtbcttlen = scale_expansion_zeroelim(bcttlen, bctt, adxtail, axtbctt);
			temp16alen = scale_expansion_zeroelim(axtbcttlen, axtbctt, 2.0 * adx,
				temp16a);
			temp16blen = scale_expansion_zeroelim(axtbcttlen, axtbctt, adxtail,
				temp16b);
			temp32blen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
				temp16blen, temp16b, temp32b);
			temp64len = fast_expansion_sum_zeroelim(temp32alen, temp32a,
				temp32blen, temp32b, temp64);
			finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp64len,
				temp64, finother);
			finswap = finnow; finnow = finother; finother = finswap;
		}
		if (adytail != 0.0) {
			temp16alen = scale_expansion_zeroelim(aytbclen, aytbc, adytail, temp16a);
			aytbctlen = scale_expansion_zeroelim(bctlen, bct, adytail, aytbct);
			temp32alen = scale_expansion_zeroelim(aytbctlen, aytbct, 2.0 * ady,
				temp32a);
			temp48len = fast_expansion_sum_zeroelim(temp16alen, temp16a,
				temp32alen, temp32a, temp48);
			finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
				temp48, finother);
			finswap = finnow; finnow = finother; finother = finswap;


			temp32alen = scale_expansion_zeroelim(aytbctlen, aytbct, adytail,
				temp32a);
			aytbcttlen = scale_expansion_zeroelim(bcttlen, bctt, adytail, aytbctt);
			temp16alen = scale_expansion_zeroelim(aytbcttlen, aytbctt, 2.0 * ady,
				temp16a);
			temp16blen = scale_expansion_zeroelim(aytbcttlen, aytbctt, adytail,
				temp16b);
			temp32blen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
				temp16blen, temp16b, temp32b);
			temp64len = fast_expansion_sum_zeroelim(temp32alen, temp32a,
				temp32blen, temp32b, temp64);
			finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp64len,
				temp64, finother);
			finswap = finnow; finnow = finother; finother = finswap;
		}
	}
	if ((bdxtail != 0.0) || (bdytail != 0.0)) {
		if ((cdxtail != 0.0) || (cdytail != 0.0)
			|| (adxtail != 0.0) || (adytail != 0.0)) {
			two_product(cdxtail, ady, ti1, ti0);
			two_product(cdx, adytail, tj1, tj0);
			two_two_sum(ti1, ti0, tj1, tj0, u3, u[2], u[1], u[0]);
			u[3] = u3;
			negate = -cdy;
			two_product(adxtail, negate, ti1, ti0);
			negate = -cdytail;
			two_product(adx, negate, tj1, tj0);
			two_two_sum(ti1, ti0, tj1, tj0, v3, v[2], v[1], v[0]);
			v[3] = v3;
			catlen = fast_expansion_sum_zeroelim(4, u, 4, v, cat);

			two_product(cdxtail, adytail, ti1, ti0);
			two_product(adxtail, cdytail, tj1, tj0);
			two_two_diff(ti1, ti0, tj1, tj0, catt3, catt[2], catt[1], catt[0]);
			catt[3] = catt3;
			cattlen = 4;
		}
		else {
			cat[0] = 0.0;
			catlen = 1;
			catt[0] = 0.0;
			cattlen = 1;
		}

		if (bdxtail != 0.0) {
			temp16alen = scale_expansion_zeroelim(bxtcalen, bxtca, bdxtail, temp16a);
			bxtcatlen = scale_expansion_zeroelim(catlen, cat, bdxtail, bxtcat);
			temp32alen = scale_expansion_zeroelim(bxtcatlen, bxtcat, 2.0 * bdx, temp32a);
			temp48len = fast_expansion_sum_zeroelim(temp16alen, temp16a, temp32alen, temp32a, temp48);
			finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
			finswap = finnow; finnow = finother; finother = finswap;
			if (cdytail != 0.0) {
				temp8len = scale_expansion_zeroelim(4, aa, bdxtail, temp8);
				temp16alen = scale_expansion_zeroelim(temp8len, temp8, cdytail, temp16a);
				finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp16alen, temp16a, finother);
				finswap = finnow; finnow = finother; finother = finswap;
			}
			if (adytail != 0.0) {
				temp8len = scale_expansion_zeroelim(4, cc, -bdxtail, temp8);
				temp16alen = scale_expansion_zeroelim(temp8len, temp8, adytail, temp16a);
				finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp16alen, temp16a, finother);
				finswap = finnow; finnow = finother; finother = finswap;
			}

			temp32alen = scale_expansion_zeroelim(bxtcatlen, bxtcat, bdxtail, temp32a);
			bxtcattlen = scale_expansion_zeroelim(cattlen, catt, bdxtail, bxtcatt);
			temp16alen = scale_expansion_zeroelim(bxtcattlen, bxtcatt, 2.0 * bdx, temp16a);
			temp16blen = scale_expansion_zeroelim(bxtcattlen, bxtcatt, bdxtail, temp16b);
			temp32blen = fast_expansion_sum_zeroelim(temp16alen, temp16a, temp16blen, temp16b, temp32b);
			temp64len = fast_expansion_sum_zeroelim(temp32alen, temp32a, temp32blen, temp32b, temp64);
			finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp64len, temp64, finother);
			finswap = finnow; finnow = finother; finother = finswap;
		}
		if (bdytail != 0.0) {
			temp16alen = scale_expansion_zeroelim(bytcalen, bytca, bdytail, temp16a);
			bytcatlen = scale_expansion_zeroelim(catlen, cat, bdytail, bytcat);
			temp32alen = scale_expansion_zeroelim(bytcatlen, bytcat, 2.0 * bdy, temp32a);
			temp48len = fast_expansion_sum_zeroelim(temp16alen, temp16a, temp32alen, temp32a, temp48);
			finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
			finswap = finnow; finnow = finother; finother = finswap;


			temp32alen = scale_expansion_zeroelim(bytcatlen, bytcat, bdytail, temp32a);
			bytcattlen = scale_expansion_zeroelim(cattlen, catt, bdytail, bytcatt);
			temp16alen = scale_expansion_zeroelim(bytcattlen, bytcatt, 2.0 * bdy, temp16a);
			temp16blen = scale_expansion_zeroelim(bytcattlen, bytcatt, bdytail, temp16b);
			temp32blen = fast_expansion_sum_zeroelim(temp16alen, temp16a, temp16blen, temp16b, temp32b);
			temp64len = fast_expansion_sum_zeroelim(temp32alen, temp32a, temp32blen, temp32b, temp64);
			finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp64len, temp64, finother);
			finswap = finnow; finnow = finother; finother = finswap;
		}
	}
	if ((cdxtail != 0.0) || (cdytail != 0.0)) {
		if ((adxtail != 0.0) || (adytail != 0.0) || (bdxtail != 0.0) || (bdytail != 0.0)) {
			two_product(adxtail, bdy, ti1, ti0);
			two_product(adx, bdytail, tj1, tj0);
			two_two_sum(ti1, ti0, tj1, tj0, u3, u[2], u[1], u[0]);
			u[3] = u3;
			negate = -ady;
			two_product(bdxtail, negate, ti1, ti0);
			negate = -adytail;
			two_product(bdx, negate, tj1, tj0);
			two_two_sum(ti1, ti0, tj1, tj0, v3, v[2], v[1], v[0]);
			v[3] = v3;
			abtlen = fast_expansion_sum_zeroelim(4, u, 4, v, abt);

			two_product(adxtail, bdytail, ti1, ti0);
			two_product(bdxtail, adytail, tj1, tj0);
			two_two_diff(ti1, ti0, tj1, tj0, abtt3, abtt[2], abtt[1], abtt[0]);
			abtt[3] = abtt3;
			abttlen = 4;
		}
		else {
			abt[0] = 0.0;
			abtlen = 1;
			abtt[0] = 0.0;
			abttlen = 1;
		}

		if (cdxtail != 0.0) {
			temp16alen = scale_expansion_zeroelim(cxtablen, cxtab, cdxtail, temp16a);
			cxtabtlen = scale_expansion_zeroelim(abtlen, abt, cdxtail, cxtabt);
			temp32alen = scale_expansion_zeroelim(cxtabtlen, cxtabt, 2.0 * cdx, temp32a);
			temp48len = fast_expansion_sum_zeroelim(temp16alen, temp16a, temp32alen, temp32a, temp48);
			finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
			finswap = finnow; finnow = finother; finother = finswap;
			if (adytail != 0.0) {
				temp8len = scale_expansion_zeroelim(4, bb, cdxtail, temp8);
				temp16alen = scale_expansion_zeroelim(temp8len, temp8, adytail, temp16a);
				finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp16alen, temp16a, finother);
				finswap = finnow; finnow = finother; finother = finswap;
			}
			if (bdytail != 0.0) {
				temp8len = scale_expansion_zeroelim(4, aa, -cdxtail, temp8);
				temp16alen = scale_expansion_zeroelim(temp8len, temp8, bdytail, temp16a);
				finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp16alen, temp16a, finother);
				finswap = finnow; finnow = finother; finother = finswap;
			}

			temp32alen = scale_expansion_zeroelim(cxtabtlen, cxtabt, cdxtail, temp32a);
			cxtabttlen = scale_expansion_zeroelim(abttlen, abtt, cdxtail, cxtabtt);
			temp16alen = scale_expansion_zeroelim(cxtabttlen, cxtabtt, 2.0 * cdx, temp16a);
			temp16blen = scale_expansion_zeroelim(cxtabttlen, cxtabtt, cdxtail, temp16b);
			temp32blen = fast_expansion_sum_zeroelim(temp16alen, temp16a, temp16blen, temp16b, temp32b);
			temp64len = fast_expansion_sum_zeroelim(temp32alen, temp32a, temp32blen, temp32b, temp64);
			finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp64len, temp64, finother);
			finswap = finnow; finnow = finother; finother = finswap;
		}
		if (cdytail != 0.0) {
			temp16alen = scale_expansion_zeroelim(cytablen, cytab, cdytail, temp16a);
			cytabtlen = scale_expansion_zeroelim(abtlen, abt, cdytail, cytabt);
			temp32alen = scale_expansion_zeroelim(cytabtlen, cytabt, 2.0 * cdy, temp32a);
			temp48len = fast_expansion_sum_zeroelim(temp16alen, temp16a, temp32alen, temp32a, temp48);
			finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
			finswap = finnow; finnow = finother; finother = finswap;


			temp32alen = scale_expansion_zeroelim(cytabtlen, cytabt, cdytail, temp32a);
			cytabttlen = scale_expansion_zeroelim(abttlen, abtt, cdytail, cytabtt);
			temp16alen = scale_expansion_zeroelim(cytabttlen, cytabtt, 2.0 * cdy, temp16a);
			temp16blen = scale_expansion_zeroelim(cytabttlen, cytabtt, cdytail, temp16b);
			temp32blen = fast_expansion_sum_zeroelim(temp16alen, temp16a, temp16blen, temp16b, temp32b);
			temp64len = fast_expansion_sum_zeroelim(temp32alen, temp32a, temp32blen, temp32b, temp64);
			finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp64len, temp64, finother);
			finswap = finnow; finnow = finother; finother = finswap;
		}
	}

	return finnow[finlength - 1];
}

real_t incircleexact(const struct point_t pa, const struct point_t pb, const struct point_t pc, const struct point_t pd)
{
	INEXACT real_t axby1, bxcy1, cxdy1, dxay1, axcy1, bxdy1;
	INEXACT real_t bxay1, cxby1, dxcy1, axdy1, cxay1, dxby1;
	real_t axby0, bxcy0, cxdy0, dxay0, axcy0, bxdy0;
	real_t bxay0, cxby0, dxcy0, axdy0, cxay0, dxby0;
	real_t ab[4], bc[4], cd[4], da[4], ac[4], bd[4];
	real_t temp8[8];
	int templen;
	real_t abc[12], bcd[12], cda[12], dab[12];
	int abclen, bcdlen, cdalen, dablen;
	real_t det24x[24], det24y[24], det48x[48], det48y[48];
	int xlen, ylen;
	real_t adet[96], bdet[96], cdet[96], ddet[96];
	int alen, blen, clen, dlen;
	real_t abdet[192], cddet[192];
	int ablen, cdlen;
	real_t deter[384];
	int deterlen;
	int i;

	INEXACT real_t bvirt;
	real_t avirt, bround, around;
	INEXACT real_t c;
	INEXACT real_t abig;
	real_t ahi, alo, bhi, blo;
	real_t err1, err2, err3;
	INEXACT real_t _i, _j;
	real_t _0;

	two_product(pa.x, pb.y, axby1, axby0);
	two_product(pb.x, pa.y, bxay1, bxay0);
	two_two_diff(axby1, axby0, bxay1, bxay0, ab[3], ab[2], ab[1], ab[0]);

	two_product(pb.x, pc.y, bxcy1, bxcy0);
	two_product(pc.x, pb.y, cxby1, cxby0);
	two_two_diff(bxcy1, bxcy0, cxby1, cxby0, bc[3], bc[2], bc[1], bc[0]);

	two_product(pc.x, pd.y, cxdy1, cxdy0);
	two_product(pd.x, pc.y, dxcy1, dxcy0);
	two_two_diff(cxdy1, cxdy0, dxcy1, dxcy0, cd[3], cd[2], cd[1], cd[0]);

	two_product(pd.x, pa.y, dxay1, dxay0);
	two_product(pa.x, pd.y, axdy1, axdy0);
	two_two_diff(dxay1, dxay0, axdy1, axdy0, da[3], da[2], da[1], da[0]);

	two_product(pa.x, pc.y, axcy1, axcy0);
	two_product(pc.x, pa.y, cxay1, cxay0);
	two_two_diff(axcy1, axcy0, cxay1, cxay0, ac[3], ac[2], ac[1], ac[0]);

	two_product(pb.x, pd.y, bxdy1, bxdy0);
	two_product(pd.x, pb.y, dxby1, dxby0);
	two_two_diff(bxdy1, bxdy0, dxby1, dxby0, bd[3], bd[2], bd[1], bd[0]);

	templen = fast_expansion_sum_zeroelim(4, cd, 4, da, temp8);
	cdalen = fast_expansion_sum_zeroelim(templen, temp8, 4, ac, cda);
	templen = fast_expansion_sum_zeroelim(4, da, 4, ab, temp8);
	dablen = fast_expansion_sum_zeroelim(templen, temp8, 4, bd, dab);
	for (i = 0; i < 4; i++) {
		bd[i] = -bd[i];
		ac[i] = -ac[i];
	}
	templen = fast_expansion_sum_zeroelim(4, ab, 4, bc, temp8);
	abclen = fast_expansion_sum_zeroelim(templen, temp8, 4, ac, abc);
	templen = fast_expansion_sum_zeroelim(4, bc, 4, cd, temp8);
	bcdlen = fast_expansion_sum_zeroelim(templen, temp8, 4, bd, bcd);

	xlen = scale_expansion_zeroelim(bcdlen, bcd, pa.x, det24x);
	xlen = scale_expansion_zeroelim(xlen, det24x, pa.x, det48x);
	ylen = scale_expansion_zeroelim(bcdlen, bcd, pa.y, det24y);
	ylen = scale_expansion_zeroelim(ylen, det24y, pa.y, det48y);
	alen = fast_expansion_sum_zeroelim(xlen, det48x, ylen, det48y, adet);

	xlen = scale_expansion_zeroelim(cdalen, cda, pb.x, det24x);
	xlen = scale_expansion_zeroelim(xlen, det24x, -pb.x, det48x);
	ylen = scale_expansion_zeroelim(cdalen, cda, pb.y, det24y);
	ylen = scale_expansion_zeroelim(ylen, det24y, -pb.y, det48y);
	blen = fast_expansion_sum_zeroelim(xlen, det48x, ylen, det48y, bdet);

	xlen = scale_expansion_zeroelim(dablen, dab, pc.x, det24x);
	xlen = scale_expansion_zeroelim(xlen, det24x, pc.x, det48x);
	ylen = scale_expansion_zeroelim(dablen, dab, pc.y, det24y);
	ylen = scale_expansion_zeroelim(ylen, det24y, pc.y, det48y);
	clen = fast_expansion_sum_zeroelim(xlen, det48x, ylen, det48y, cdet);

	xlen = scale_expansion_zeroelim(abclen, abc, pd.x, det24x);
	xlen = scale_expansion_zeroelim(xlen, det24x, -pd.x, det48x);
	ylen = scale_expansion_zeroelim(abclen, abc, pd.y, det24y);
	ylen = scale_expansion_zeroelim(ylen, det24y, -pd.y, det48y);
	dlen = fast_expansion_sum_zeroelim(xlen, det48x, ylen, det48y, ddet);

	ablen = fast_expansion_sum_zeroelim(alen, adet, blen, bdet, abdet);
	cdlen = fast_expansion_sum_zeroelim(clen, cdet, dlen, ddet, cddet);
	deterlen = fast_expansion_sum_zeroelim(ablen, abdet, cdlen, cddet, deter);

	return deter[deterlen - 1];
}

real_t incircle(const struct point_t pa, const struct point_t pb, const struct point_t pc, const struct point_t pd)
{
	real_t adx, bdx, cdx, ady, bdy, cdy;
	real_t bdxcdy, cdxbdy, cdxady, adxcdy, adxbdy, bdxady;
	real_t alift, blift, clift;
	real_t det;
	real_t permanent, errbound;

	adx = pa.x - pd.x;
	bdx = pb.x - pd.x;
	cdx = pc.x - pd.x;
	ady = pa.y - pd.y;
	bdy = pb.y - pd.y;
	cdy = pc.y - pd.y;

	bdxcdy = bdx * cdy;
	cdxbdy = cdx * bdy;
	alift = adx * adx + ady * ady;

	cdxady = cdx * ady;
	adxcdy = adx * cdy;
	blift = bdx * bdx + bdy * bdy;

	adxbdy = adx * bdy;
	bdxady = bdx * ady;
	clift = cdx * cdx + cdy * cdy;

	det = alift * (bdxcdy - cdxbdy)
		+ blift * (cdxady - adxcdy)
		+ clift * (adxbdy - bdxady);

	permanent = (absolute(bdxcdy) + absolute(cdxbdy)) * alift
		+ (absolute(cdxady) + absolute(adxcdy)) * blift
		+ (absolute(adxbdy) + absolute(bdxady)) * clift;
	errbound = coef.iccerrboundA * permanent;
	if ((det > errbound) || (-det > errbound)) {
		return det;
	}

	return incircleadapt(pa, pb, pc, pd, permanent);
	//return 0.0;
}

/*****************************************************************************/
real_t restimate(int elen, real_t* e)
{
	real_t Q;
	int eindex;

	Q = e[0];
	for (eindex = 1; eindex < elen; eindex++) {
		Q += e[eindex];
	}
	return Q;
}

int rfast_expansion_sum_zeroelim(int elen, real_t* e, int flen, real_t* f, real_t* h)
{
	real_t Q;
	INEXACT real_t Qnew;
	INEXACT real_t hh;
	INEXACT real_t bvirt;
	real_t avirt, bround, around;
	int eindex, findex, hindex;
	real_t enow, fnow;

	enow = e[0];
	fnow = f[0];
	eindex = findex = 0;
	if ((fnow > enow) == (fnow > -enow)) {
		Q = enow;
		++eindex; if (eindex < elen) enow = e[eindex];
	}
	else {
		Q = fnow;
		++findex; if (findex < flen) fnow = f[findex];
	}
	hindex = 0;
	if ((eindex < elen) && (findex < flen)) {
		if ((fnow > enow) == (fnow > -enow)) {
			fast_two_sum(enow, Q, Qnew, hh);
			++eindex; if (eindex < elen) enow = e[eindex];
		}
		else {
			fast_two_sum(fnow, Q, Qnew, hh);
			++findex; if (findex < flen) fnow = f[findex];
		}
		Q = Qnew;
		if (hh != 0.0) {
			h[hindex++] = hh;
		}
		while ((eindex < elen) && (findex < flen)) {
			if ((fnow > enow) == (fnow > -enow)) {
				Two_Sum(Q, enow, Qnew, hh);
				++eindex; if (eindex < elen) enow = e[eindex];
			}
			else {
				Two_Sum(Q, fnow, Qnew, hh);
				++findex; if (findex < flen) fnow = f[findex];
			}
			Q = Qnew;
			if (hh != 0.0) {
				h[hindex++] = hh;
			}
		}
	}
	while (eindex < elen) {
		Two_Sum(Q, enow, Qnew, hh);
		++eindex; if (eindex < elen) enow = e[eindex];
		Q = Qnew;
		if (hh != 0.0) {
			h[hindex++] = hh;
		}
	}
	while (findex < flen) {
		Two_Sum(Q, fnow, Qnew, hh);
		++findex; if (findex < flen) fnow = f[findex];
		Q = Qnew;
		if (hh != 0.0) {
			h[hindex++] = hh;
		}
	}
	if ((Q != 0.0) || (hindex == 0)) {
		h[hindex++] = Q;
	}
	return hindex;
}

int rscale_expansion_zeroelim(int elen, real_t* e, real_t b, real_t* h)
{
	INEXACT real_t Q, sum;
	real_t hh;
	INEXACT real_t product1;
	real_t product0;
	int eindex, hindex;
	real_t enow;
	INEXACT real_t bvirt;
	real_t avirt, bround, around;
	INEXACT real_t c;
	INEXACT real_t abig;
	real_t ahi, alo, bhi, blo;
	real_t err1, err2, err3;

	split(b, bhi, blo);
	two_product_presplit(e[0], b, bhi, blo, Q, hh);
	hindex = 0;
	if (hh != 0) {
		h[hindex++] = hh;
	}
	for (eindex = 1; eindex < elen; eindex++) {
		enow = e[eindex];
		two_product_presplit(enow, b, bhi, blo, product1, product0);
		Two_Sum(Q, product0, sum, hh);
		if (hh != 0) {
			h[hindex++] = hh;
		}
		fast_two_sum(product1, sum, Q, hh);
		if (hh != 0) {
			h[hindex++] = hh;
		}
	}
	if ((Q != 0.0) || (hindex == 0)) {
		h[hindex++] = Q;
	}
	return hindex;
}

real_t rincircleadapt(const struct point_t pa, const struct point_t pb, const struct point_t pc, const struct point_t pd, real_t permanent)
{
	INEXACT real_t adx, bdx, cdx, ady, bdy, cdy;
	real_t det, errbound;

	INEXACT real_t bdxcdy1, cdxbdy1, cdxady1, adxcdy1, adxbdy1, bdxady1;
	real_t bdxcdy0, cdxbdy0, cdxady0, adxcdy0, adxbdy0, bdxady0;
	real_t bc[4], ca[4], ab[4];
	INEXACT real_t bc3, ca3, ab3;
	real_t axbc[8], axxbc[16], aybc[8], ayybc[16], adet[32];
	int axbclen, axxbclen, aybclen, ayybclen, alen;
	real_t bxca[8], bxxca[16], byca[8], byyca[16], bdet[32];
	int bxcalen, bxxcalen, bycalen, byycalen, blen;
	real_t cxab[8], cxxab[16], cyab[8], cyyab[16], cdet[32];
	int cxablen, cxxablen, cyablen, cyyablen, clen;
	real_t abdet[64];
	int ablen;
	real_t fin1[1152], fin2[1152];
	real_t* finnow, * finother, * finswap;
	int finlength;

	real_t adxtail, bdxtail, cdxtail, adytail, bdytail, cdytail;
	INEXACT real_t adxadx1, adyady1, bdxbdx1, bdybdy1, cdxcdx1, cdycdy1;
	real_t adxadx0, adyady0, bdxbdx0, bdybdy0, cdxcdx0, cdycdy0;
	real_t aa[4], bb[4], cc[4];
	INEXACT real_t aa3, bb3, cc3;
	INEXACT real_t ti1, tj1;
	real_t ti0, tj0;
	real_t u[4], v[4];
	INEXACT real_t u3, v3;
	real_t temp8[8], temp16a[16], temp16b[16], temp16c[16];
	real_t temp32a[32], temp32b[32], temp48[48], temp64[64];
	int temp8len, temp16alen, temp16blen, temp16clen;
	int temp32alen, temp32blen, temp48len, temp64len;
	real_t axtbb[8], axtcc[8], aytbb[8], aytcc[8];
	int axtbblen, axtcclen, aytbblen, aytcclen;
	real_t bxtaa[8], bxtcc[8], bytaa[8], bytcc[8];
	int bxtaalen, bxtcclen, bytaalen, bytcclen;
	real_t cxtaa[8], cxtbb[8], cytaa[8], cytbb[8];
	int cxtaalen, cxtbblen, cytaalen, cytbblen;
	real_t axtbc[8], aytbc[8], bxtca[8], bytca[8], cxtab[8], cytab[8];
	int axtbclen, aytbclen, bxtcalen, bytcalen, cxtablen, cytablen;
	real_t axtbct[16], aytbct[16], bxtcat[16], bytcat[16], cxtabt[16], cytabt[16];
	int axtbctlen, aytbctlen, bxtcatlen, bytcatlen, cxtabtlen, cytabtlen;
	real_t axtbctt[8], aytbctt[8], bxtcatt[8];
	real_t bytcatt[8], cxtabtt[8], cytabtt[8];
	int axtbcttlen, aytbcttlen, bxtcattlen, bytcattlen, cxtabttlen, cytabttlen;
	real_t abt[8], bct[8], cat[8];
	int abtlen, bctlen, catlen;
	real_t abtt[4], bctt[4], catt[4];
	int abttlen, bcttlen, cattlen;
	INEXACT real_t abtt3, bctt3, catt3;
	real_t negate;

	INEXACT real_t bvirt;
	real_t avirt, bround, around;
	INEXACT real_t c;
	INEXACT real_t abig;
	real_t ahi, alo, bhi, blo;
	real_t err1, err2, err3;
	INEXACT real_t _i, _j;
	real_t _0;

	adx = (real_t)(pa.x - pd.x);
	bdx = (real_t)(pb.x - pd.x);
	cdx = (real_t)(pc.x - pd.x);
	ady = (real_t)(pa.y - pd.y);
	bdy = (real_t)(pb.y - pd.y);
	cdy = (real_t)(pc.y - pd.y);

	two_product(bdx, cdy, bdxcdy1, bdxcdy0);
	two_product(cdx, bdy, cdxbdy1, cdxbdy0);
	two_two_diff(bdxcdy1, bdxcdy0, cdxbdy1, cdxbdy0, bc3, bc[2], bc[1], bc[0]);
	bc[3] = bc3;
	axbclen = rscale_expansion_zeroelim(4, bc, adx, axbc);
	axxbclen = rscale_expansion_zeroelim(axbclen, axbc, adx, axxbc);
	aybclen = rscale_expansion_zeroelim(4, bc, ady, aybc);
	ayybclen = rscale_expansion_zeroelim(aybclen, aybc, ady, ayybc);
	alen = rfast_expansion_sum_zeroelim(axxbclen, axxbc, ayybclen, ayybc, adet);

	two_product(cdx, ady, cdxady1, cdxady0);
	two_product(adx, cdy, adxcdy1, adxcdy0);
	two_two_diff(cdxady1, cdxady0, adxcdy1, adxcdy0, ca3, ca[2], ca[1], ca[0]);
	ca[3] = ca3;
	bxcalen = rscale_expansion_zeroelim(4, ca, bdx, bxca);
	bxxcalen = rscale_expansion_zeroelim(bxcalen, bxca, bdx, bxxca);
	bycalen = rscale_expansion_zeroelim(4, ca, bdy, byca);
	byycalen = rscale_expansion_zeroelim(bycalen, byca, bdy, byyca);
	blen = rfast_expansion_sum_zeroelim(bxxcalen, bxxca, byycalen, byyca, bdet);

	two_product(adx, bdy, adxbdy1, adxbdy0);
	two_product(bdx, ady, bdxady1, bdxady0);
	two_two_diff(adxbdy1, adxbdy0, bdxady1, bdxady0, ab3, ab[2], ab[1], ab[0]);
	ab[3] = ab3;
	cxablen = rscale_expansion_zeroelim(4, ab, cdx, cxab);
	cxxablen = rscale_expansion_zeroelim(cxablen, cxab, cdx, cxxab);
	cyablen = rscale_expansion_zeroelim(4, ab, cdy, cyab);
	cyyablen = rscale_expansion_zeroelim(cyablen, cyab, cdy, cyyab);
	clen = rfast_expansion_sum_zeroelim(cxxablen, cxxab, cyyablen, cyyab, cdet);

	ablen = rfast_expansion_sum_zeroelim(alen, adet, blen, bdet, abdet);
	finlength = rfast_expansion_sum_zeroelim(ablen, abdet, clen, cdet, fin1);

	det = restimate(finlength, fin1);
	errbound = coef.iccerrboundB * permanent;
	if ((det >= errbound) || (-det >= errbound)) {
		return det;
	}

	two_diff_Tail(pa.x, pd.x, adx, adxtail);
	two_diff_Tail(pa.y, pd.y, ady, adytail);
	two_diff_Tail(pb.x, pd.x, bdx, bdxtail);
	two_diff_Tail(pb.y, pd.y, bdy, bdytail);
	two_diff_Tail(pc.x, pd.x, cdx, cdxtail);
	two_diff_Tail(pc.y, pd.y, cdy, cdytail);
	if ((adxtail == 0.0) && (bdxtail == 0.0) && (cdxtail == 0.0)
		&& (adytail == 0.0) && (bdytail == 0.0) && (cdytail == 0.0)) {
		return det;
	}

	errbound = coef.iccerrboundC * permanent + coef.resulterrbound * absolute(det);
	det += ((adx * adx + ady * ady) * ((bdx * cdytail + cdy * bdxtail)
		- (bdy * cdxtail + cdx * bdytail))
		+ 2.0 * (adx * adxtail + ady * adytail) * (bdx * cdy - bdy * cdx))
		+ ((bdx * bdx + bdy * bdy) * ((cdx * adytail + ady * cdxtail)
			- (cdy * adxtail + adx * cdytail))
			+ 2.0 * (bdx * bdxtail + bdy * bdytail) * (cdx * ady - cdy * adx))
		+ ((cdx * cdx + cdy * cdy) * ((adx * bdytail + bdy * adxtail)
			- (ady * bdxtail + bdx * adytail))
			+ 2.0 * (cdx * cdxtail + cdy * cdytail) * (adx * bdy - ady * bdx));

	if ((det >= errbound) || (-det >= errbound)) {
		return det;
	}

	finnow = fin1;
	finother = fin2;

	if ((bdxtail != 0.0) || (bdytail != 0.0)
		|| (cdxtail != 0.0) || (cdytail != 0.0)) {
		square(adx, adxadx1, adxadx0);
		square(ady, adyady1, adyady0);
		two_two_sum(adxadx1, adxadx0, adyady1, adyady0, aa3, aa[2], aa[1], aa[0]);
		aa[3] = aa3;
	}
	if ((cdxtail != 0.0) || (cdytail != 0.0)
		|| (adxtail != 0.0) || (adytail != 0.0)) {
		square(bdx, bdxbdx1, bdxbdx0);
		square(bdy, bdybdy1, bdybdy0);
		two_two_sum(bdxbdx1, bdxbdx0, bdybdy1, bdybdy0, bb3, bb[2], bb[1], bb[0]);
		bb[3] = bb3;
	}
	if ((adxtail != 0.0) || (adytail != 0.0)
		|| (bdxtail != 0.0) || (bdytail != 0.0)) {
		square(cdx, cdxcdx1, cdxcdx0);
		square(cdy, cdycdy1, cdycdy0);
		two_two_sum(cdxcdx1, cdxcdx0, cdycdy1, cdycdy0, cc3, cc[2], cc[1], cc[0]);
		cc[3] = cc3;
	}

	if (adxtail != 0.0) {
		axtbclen = rscale_expansion_zeroelim(4, bc, adxtail, axtbc);
		temp16alen = rscale_expansion_zeroelim(axtbclen, axtbc, 2.0 * adx, temp16a);

		axtcclen = rscale_expansion_zeroelim(4, cc, adxtail, axtcc);
		temp16blen = rscale_expansion_zeroelim(axtcclen, axtcc, bdy, temp16b);

		axtbblen = rscale_expansion_zeroelim(4, bb, adxtail, axtbb);
		temp16clen = rscale_expansion_zeroelim(axtbblen, axtbb, -cdy, temp16c);

		temp32alen = rfast_expansion_sum_zeroelim(temp16alen, temp16a, temp16blen, temp16b, temp32a);
		temp48len = rfast_expansion_sum_zeroelim(temp16clen, temp16c, temp32alen, temp32a, temp48);
		finlength = rfast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
		finswap = finnow; finnow = finother; finother = finswap;
	}
	if (adytail != 0.0) {
		aytbclen = rscale_expansion_zeroelim(4, bc, adytail, aytbc);
		temp16alen = rscale_expansion_zeroelim(aytbclen, aytbc, 2.0 * ady, temp16a);

		aytbblen = rscale_expansion_zeroelim(4, bb, adytail, aytbb);
		temp16blen = rscale_expansion_zeroelim(aytbblen, aytbb, cdx, temp16b);

		aytcclen = rscale_expansion_zeroelim(4, cc, adytail, aytcc);
		temp16clen = rscale_expansion_zeroelim(aytcclen, aytcc, -bdx, temp16c);

		temp32alen = rfast_expansion_sum_zeroelim(temp16alen, temp16a, temp16blen, temp16b, temp32a);
		temp48len = rfast_expansion_sum_zeroelim(temp16clen, temp16c, temp32alen, temp32a, temp48);
		finlength = rfast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
		finswap = finnow; finnow = finother; finother = finswap;
	}
	if (bdxtail != 0.0) {
		bxtcalen = rscale_expansion_zeroelim(4, ca, bdxtail, bxtca);
		temp16alen = rscale_expansion_zeroelim(bxtcalen, bxtca, 2.0 * bdx, temp16a);

		bxtaalen = rscale_expansion_zeroelim(4, aa, bdxtail, bxtaa);
		temp16blen = rscale_expansion_zeroelim(bxtaalen, bxtaa, cdy, temp16b);

		bxtcclen = rscale_expansion_zeroelim(4, cc, bdxtail, bxtcc);
		temp16clen = rscale_expansion_zeroelim(bxtcclen, bxtcc, -ady, temp16c);

		temp32alen = rfast_expansion_sum_zeroelim(temp16alen, temp16a, temp16blen, temp16b, temp32a);
		temp48len = rfast_expansion_sum_zeroelim(temp16clen, temp16c, temp32alen, temp32a, temp48);
		finlength = rfast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
		finswap = finnow; finnow = finother; finother = finswap;
	}
	if (bdytail != 0.0) {
		bytcalen = rscale_expansion_zeroelim(4, ca, bdytail, bytca);
		temp16alen = rscale_expansion_zeroelim(bytcalen, bytca, 2.0 * bdy, temp16a);

		bytcclen = rscale_expansion_zeroelim(4, cc, bdytail, bytcc);
		temp16blen = rscale_expansion_zeroelim(bytcclen, bytcc, adx, temp16b);

		bytaalen = rscale_expansion_zeroelim(4, aa, bdytail, bytaa);
		temp16clen = rscale_expansion_zeroelim(bytaalen, bytaa, -cdx, temp16c);

		temp32alen = rfast_expansion_sum_zeroelim(temp16alen, temp16a, temp16blen, temp16b, temp32a);
		temp48len = rfast_expansion_sum_zeroelim(temp16clen, temp16c, temp32alen, temp32a, temp48);
		finlength = rfast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
		finswap = finnow; finnow = finother; finother = finswap;
	}
	if (cdxtail != 0.0) {
		cxtablen = rscale_expansion_zeroelim(4, ab, cdxtail, cxtab);
		temp16alen = rscale_expansion_zeroelim(cxtablen, cxtab, 2.0 * cdx, temp16a);

		cxtbblen = rscale_expansion_zeroelim(4, bb, cdxtail, cxtbb);
		temp16blen = rscale_expansion_zeroelim(cxtbblen, cxtbb, ady, temp16b);

		cxtaalen = rscale_expansion_zeroelim(4, aa, cdxtail, cxtaa);
		temp16clen = rscale_expansion_zeroelim(cxtaalen, cxtaa, -bdy, temp16c);

		temp32alen = rfast_expansion_sum_zeroelim(temp16alen, temp16a, temp16blen, temp16b, temp32a);
		temp48len = rfast_expansion_sum_zeroelim(temp16clen, temp16c, temp32alen, temp32a, temp48);
		finlength = rfast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
		finswap = finnow; finnow = finother; finother = finswap;
	}
	if (cdytail != 0.0) {
		cytablen = rscale_expansion_zeroelim(4, ab, cdytail, cytab);
		temp16alen = rscale_expansion_zeroelim(cytablen, cytab, 2.0 * cdy, temp16a);

		cytaalen = rscale_expansion_zeroelim(4, aa, cdytail, cytaa);
		temp16blen = rscale_expansion_zeroelim(cytaalen, cytaa, bdx, temp16b);

		cytbblen = rscale_expansion_zeroelim(4, bb, cdytail, cytbb);
		temp16clen = rscale_expansion_zeroelim(cytbblen, cytbb, -adx, temp16c);

		temp32alen = rfast_expansion_sum_zeroelim(temp16alen, temp16a, temp16blen, temp16b, temp32a);
		temp48len = rfast_expansion_sum_zeroelim(temp16clen, temp16c, temp32alen, temp32a, temp48);
		finlength = rfast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
		finswap = finnow; finnow = finother; finother = finswap;
	}

	if ((adxtail != 0.0) || (adytail != 0.0)) {
		if ((bdxtail != 0.0) || (bdytail != 0.0)
			|| (cdxtail != 0.0) || (cdytail != 0.0)) {
			two_product(bdxtail, cdy, ti1, ti0);
			two_product(bdx, cdytail, tj1, tj0);
			two_two_sum(ti1, ti0, tj1, tj0, u3, u[2], u[1], u[0]);
			u[3] = u3;
			negate = -bdy;
			two_product(cdxtail, negate, ti1, ti0);
			negate = -bdytail;
			two_product(cdx, negate, tj1, tj0);
			two_two_sum(ti1, ti0, tj1, tj0, v3, v[2], v[1], v[0]);
			v[3] = v3;
			bctlen = rfast_expansion_sum_zeroelim(4, u, 4, v, bct);

			two_product(bdxtail, cdytail, ti1, ti0);
			two_product(cdxtail, bdytail, tj1, tj0);
			two_two_diff(ti1, ti0, tj1, tj0, bctt3, bctt[2], bctt[1], bctt[0]);
			bctt[3] = bctt3;
			bcttlen = 4;
		}
		else {
			bct[0] = 0.0;
			bctlen = 1;
			bctt[0] = 0.0;
			bcttlen = 1;
		}

		if (adxtail != 0.0) {
			temp16alen = rscale_expansion_zeroelim(axtbclen, axtbc, adxtail, temp16a);
			axtbctlen = rscale_expansion_zeroelim(bctlen, bct, adxtail, axtbct);
			temp32alen = rscale_expansion_zeroelim(axtbctlen, axtbct, 2.0 * adx, temp32a);
			temp48len = rfast_expansion_sum_zeroelim(temp16alen, temp16a, temp32alen, temp32a, temp48);
			finlength = rfast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
			finswap = finnow; finnow = finother; finother = finswap;

			if (bdytail != 0.0) {
				temp8len = rscale_expansion_zeroelim(4, cc, adxtail, temp8);
				temp16alen = rscale_expansion_zeroelim(temp8len, temp8, bdytail, temp16a);
				finlength = rfast_expansion_sum_zeroelim(finlength, finnow, temp16alen, temp16a, finother);
				finswap = finnow; finnow = finother; finother = finswap;
			}
			if (cdytail != 0.0) {
				temp8len = rscale_expansion_zeroelim(4, bb, -adxtail, temp8);
				temp16alen = rscale_expansion_zeroelim(temp8len, temp8, cdytail, temp16a);
				finlength = rfast_expansion_sum_zeroelim(finlength, finnow, temp16alen, temp16a, finother);
				finswap = finnow; finnow = finother; finother = finswap;
			}

			temp32alen = rscale_expansion_zeroelim(axtbctlen, axtbct, adxtail, temp32a);
			axtbcttlen = rscale_expansion_zeroelim(bcttlen, bctt, adxtail, axtbctt);
			temp16alen = rscale_expansion_zeroelim(axtbcttlen, axtbctt, 2.0 * adx, temp16a);
			temp16blen = rscale_expansion_zeroelim(axtbcttlen, axtbctt, adxtail, temp16b);
			temp32blen = rfast_expansion_sum_zeroelim(temp16alen, temp16a, temp16blen, temp16b, temp32b);
			temp64len = rfast_expansion_sum_zeroelim(temp32alen, temp32a, temp32blen, temp32b, temp64);
			finlength = rfast_expansion_sum_zeroelim(finlength, finnow, temp64len, temp64, finother);
			finswap = finnow; finnow = finother; finother = finswap;
		}
		if (adytail != 0.0) {
			temp16alen = rscale_expansion_zeroelim(aytbclen, aytbc, adytail, temp16a);
			aytbctlen = rscale_expansion_zeroelim(bctlen, bct, adytail, aytbct);
			temp32alen = rscale_expansion_zeroelim(aytbctlen, aytbct, 2.0 * ady, temp32a);
			temp48len = rfast_expansion_sum_zeroelim(temp16alen, temp16a, temp32alen, temp32a, temp48);
			finlength = rfast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
			finswap = finnow; finnow = finother; finother = finswap;


			temp32alen = rscale_expansion_zeroelim(aytbctlen, aytbct, adytail, temp32a);
			aytbcttlen = rscale_expansion_zeroelim(bcttlen, bctt, adytail, aytbctt);
			temp16alen = rscale_expansion_zeroelim(aytbcttlen, aytbctt, 2.0 * ady, temp16a);
			temp16blen = rscale_expansion_zeroelim(aytbcttlen, aytbctt, adytail, temp16b);
			temp32blen = rfast_expansion_sum_zeroelim(temp16alen, temp16a, temp16blen, temp16b, temp32b);
			temp64len = rfast_expansion_sum_zeroelim(temp32alen, temp32a, temp32blen, temp32b, temp64);
			finlength = rfast_expansion_sum_zeroelim(finlength, finnow, temp64len, temp64, finother);
			finswap = finnow; finnow = finother; finother = finswap;
		}
	}
	if ((bdxtail != 0.0) || (bdytail != 0.0)) {
		if ((cdxtail != 0.0) || (cdytail != 0.0)
			|| (adxtail != 0.0) || (adytail != 0.0)) {
			two_product(cdxtail, ady, ti1, ti0);
			two_product(cdx, adytail, tj1, tj0);
			two_two_sum(ti1, ti0, tj1, tj0, u3, u[2], u[1], u[0]);
			u[3] = u3;
			negate = -cdy;
			two_product(adxtail, negate, ti1, ti0);
			negate = -cdytail;
			two_product(adx, negate, tj1, tj0);
			two_two_sum(ti1, ti0, tj1, tj0, v3, v[2], v[1], v[0]);
			v[3] = v3;
			catlen = rfast_expansion_sum_zeroelim(4, u, 4, v, cat);

			two_product(cdxtail, adytail, ti1, ti0);
			two_product(adxtail, cdytail, tj1, tj0);
			two_two_diff(ti1, ti0, tj1, tj0, catt3, catt[2], catt[1], catt[0]);
			catt[3] = catt3;
			cattlen = 4;
		}
		else {
			cat[0] = 0.0;
			catlen = 1;
			catt[0] = 0.0;
			cattlen = 1;
		}

		if (bdxtail != 0.0) {
			temp16alen = rscale_expansion_zeroelim(bxtcalen, bxtca, bdxtail, temp16a);
			bxtcatlen = rscale_expansion_zeroelim(catlen, cat, bdxtail, bxtcat);
			temp32alen = rscale_expansion_zeroelim(bxtcatlen, bxtcat, 2.0 * bdx, temp32a);
			temp48len = rfast_expansion_sum_zeroelim(temp16alen, temp16a, temp32alen, temp32a, temp48);
			finlength = rfast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
			finswap = finnow; finnow = finother; finother = finswap;

			if (cdytail != 0.0) {
				temp8len = rscale_expansion_zeroelim(4, aa, bdxtail, temp8);
				temp16alen = rscale_expansion_zeroelim(temp8len, temp8, cdytail, temp16a);
				finlength = rfast_expansion_sum_zeroelim(finlength, finnow, temp16alen, temp16a, finother);
				finswap = finnow; finnow = finother; finother = finswap;
			}
			if (adytail != 0.0) {
				temp8len = rscale_expansion_zeroelim(4, cc, -bdxtail, temp8);
				temp16alen = rscale_expansion_zeroelim(temp8len, temp8, adytail, temp16a);
				finlength = rfast_expansion_sum_zeroelim(finlength, finnow, temp16alen, temp16a, finother);
				finswap = finnow; finnow = finother; finother = finswap;
			}

			temp32alen = rscale_expansion_zeroelim(bxtcatlen, bxtcat, bdxtail, temp32a);
			bxtcattlen = rscale_expansion_zeroelim(cattlen, catt, bdxtail, bxtcatt);
			temp16alen = rscale_expansion_zeroelim(bxtcattlen, bxtcatt, 2.0 * bdx, temp16a);
			temp16blen = rscale_expansion_zeroelim(bxtcattlen, bxtcatt, bdxtail, temp16b);
			temp32blen = rfast_expansion_sum_zeroelim(temp16alen, temp16a, temp16blen, temp16b, temp32b);
			temp64len = rfast_expansion_sum_zeroelim(temp32alen, temp32a, temp32blen, temp32b, temp64);
			finlength = rfast_expansion_sum_zeroelim(finlength, finnow, temp64len, temp64, finother);
			finswap = finnow; finnow = finother; finother = finswap;
		}
		if (bdytail != 0.0) {
			temp16alen = rscale_expansion_zeroelim(bytcalen, bytca, bdytail, temp16a);
			bytcatlen = rscale_expansion_zeroelim(catlen, cat, bdytail, bytcat);
			temp32alen = rscale_expansion_zeroelim(bytcatlen, bytcat, 2.0 * bdy, temp32a);
			temp48len = rfast_expansion_sum_zeroelim(temp16alen, temp16a, temp32alen, temp32a, temp48);
			finlength = rfast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
			finswap = finnow; finnow = finother; finother = finswap;


			temp32alen = rscale_expansion_zeroelim(bytcatlen, bytcat, bdytail, temp32a);
			bytcattlen = rscale_expansion_zeroelim(cattlen, catt, bdytail, bytcatt);
			temp16alen = rscale_expansion_zeroelim(bytcattlen, bytcatt, 2.0 * bdy, temp16a);
			temp16blen = rscale_expansion_zeroelim(bytcattlen, bytcatt, bdytail, temp16b);
			temp32blen = rfast_expansion_sum_zeroelim(temp16alen, temp16a, temp16blen, temp16b, temp32b);
			temp64len = rfast_expansion_sum_zeroelim(temp32alen, temp32a, temp32blen, temp32b, temp64);
			finlength = rfast_expansion_sum_zeroelim(finlength, finnow, temp64len, temp64, finother);
			finswap = finnow; finnow = finother; finother = finswap;
		}
	}
	if ((cdxtail != 0.0) || (cdytail != 0.0)) {
		if ((adxtail != 0.0) || (adytail != 0.0)
			|| (bdxtail != 0.0) || (bdytail != 0.0)) {
			two_product(adxtail, bdy, ti1, ti0);
			two_product(adx, bdytail, tj1, tj0);
			two_two_sum(ti1, ti0, tj1, tj0, u3, u[2], u[1], u[0]);
			u[3] = u3;
			negate = -ady;
			two_product(bdxtail, negate, ti1, ti0);
			negate = -adytail;
			two_product(bdx, negate, tj1, tj0);
			two_two_sum(ti1, ti0, tj1, tj0, v3, v[2], v[1], v[0]);
			v[3] = v3;
			abtlen = rfast_expansion_sum_zeroelim(4, u, 4, v, abt);

			two_product(adxtail, bdytail, ti1, ti0);
			two_product(bdxtail, adytail, tj1, tj0);
			two_two_diff(ti1, ti0, tj1, tj0, abtt3, abtt[2], abtt[1], abtt[0]);
			abtt[3] = abtt3;
			abttlen = 4;
		}
		else {
			abt[0] = 0.0;
			abtlen = 1;
			abtt[0] = 0.0;
			abttlen = 1;
		}

		if (cdxtail != 0.0) {
			temp16alen = rscale_expansion_zeroelim(cxtablen, cxtab, cdxtail, temp16a);
			cxtabtlen = rscale_expansion_zeroelim(abtlen, abt, cdxtail, cxtabt);
			temp32alen = rscale_expansion_zeroelim(cxtabtlen, cxtabt, 2.0 * cdx, temp32a);
			temp48len = rfast_expansion_sum_zeroelim(temp16alen, temp16a, temp32alen, temp32a, temp48);
			finlength = rfast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
			finswap = finnow; finnow = finother; finother = finswap;

			if (adytail != 0.0) {
				temp8len = rscale_expansion_zeroelim(4, bb, cdxtail, temp8);
				temp16alen = rscale_expansion_zeroelim(temp8len, temp8, adytail, temp16a);
				finlength = rfast_expansion_sum_zeroelim(finlength, finnow, temp16alen, temp16a, finother);
				finswap = finnow; finnow = finother; finother = finswap;
			}
			if (bdytail != 0.0) {
				temp8len = rscale_expansion_zeroelim(4, aa, -cdxtail, temp8);
				temp16alen = rscale_expansion_zeroelim(temp8len, temp8, bdytail, temp16a);
				finlength = rfast_expansion_sum_zeroelim(finlength, finnow, temp16alen, temp16a, finother);
				finswap = finnow; finnow = finother; finother = finswap;
			}

			temp32alen = rscale_expansion_zeroelim(cxtabtlen, cxtabt, cdxtail, temp32a);
			cxtabttlen = rscale_expansion_zeroelim(abttlen, abtt, cdxtail, cxtabtt);
			temp16alen = rscale_expansion_zeroelim(cxtabttlen, cxtabtt, 2.0 * cdx, temp16a);
			temp16blen = rscale_expansion_zeroelim(cxtabttlen, cxtabtt, cdxtail, temp16b);
			temp32blen = rfast_expansion_sum_zeroelim(temp16alen, temp16a, temp16blen, temp16b, temp32b);
			temp64len = rfast_expansion_sum_zeroelim(temp32alen, temp32a, temp32blen, temp32b, temp64);
			finlength = rfast_expansion_sum_zeroelim(finlength, finnow, temp64len, temp64, finother);
			finswap = finnow; finnow = finother; finother = finswap;
		}
		if (cdytail != 0.0) {
			temp16alen = rscale_expansion_zeroelim(cytablen, cytab, cdytail, temp16a);
			cytabtlen = rscale_expansion_zeroelim(abtlen, abt, cdytail, cytabt);
			temp32alen = rscale_expansion_zeroelim(cytabtlen, cytabt, 2.0 * cdy, temp32a);
			temp48len = rfast_expansion_sum_zeroelim(temp16alen, temp16a, temp32alen, temp32a, temp48);
			finlength = rfast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
			finswap = finnow; finnow = finother; finother = finswap;


			temp32alen = rscale_expansion_zeroelim(cytabtlen, cytabt, cdytail, temp32a);
			cytabttlen = rscale_expansion_zeroelim(abttlen, abtt, cdytail, cytabtt);
			temp16alen = rscale_expansion_zeroelim(cytabttlen, cytabtt, 2.0 * cdy, temp16a);
			temp16blen = rscale_expansion_zeroelim(cytabttlen, cytabtt, cdytail, temp16b);
			temp32blen = rfast_expansion_sum_zeroelim(temp16alen, temp16a, temp16blen, temp16b, temp32b);
			temp64len = rfast_expansion_sum_zeroelim(temp32alen, temp32a, temp32blen, temp32b, temp64);
			finlength = rfast_expansion_sum_zeroelim(finlength, finnow, temp64len, temp64, finother);
			finswap = finnow; finnow = finother; finother = finswap;
		}
	}

	return finnow[finlength - 1];
}

real_t rincircleexact(const struct point_t pa, const struct point_t pb, const struct point_t pc, const struct point_t pd)
{
	INEXACT real_t axby1, bxcy1, cxdy1, dxay1, axcy1, bxdy1;
	INEXACT real_t bxay1, cxby1, dxcy1, axdy1, cxay1, dxby1;
	real_t axby0, bxcy0, cxdy0, dxay0, axcy0, bxdy0;
	real_t bxay0, cxby0, dxcy0, axdy0, cxay0, dxby0;
	real_t ab[4], bc[4], cd[4], da[4], ac[4], bd[4];
	real_t temp8[8];
	int templen;
	real_t abc[12], bcd[12], cda[12], dab[12];
	int abclen, bcdlen, cdalen, dablen;
	real_t det24x[24], det24y[24], det48x[48], det48y[48];
	int xlen, ylen;
	real_t adet[96], bdet[96], cdet[96], ddet[96];
	int alen, blen, clen, dlen;
	real_t abdet[192], cddet[192];
	int ablen, cdlen;
	real_t deter[384];
	int deterlen;
	int i;

	INEXACT real_t bvirt;
	real_t avirt, bround, around;
	INEXACT real_t c;
	INEXACT real_t abig;
	real_t ahi, alo, bhi, blo;
	real_t err1, err2, err3;
	INEXACT real_t _i, _j;
	real_t _0;

	two_product(pa.x, pb.y, axby1, axby0);
	two_product(pb.x, pa.y, bxay1, bxay0);
	two_two_diff(axby1, axby0, bxay1, bxay0, ab[3], ab[2], ab[1], ab[0]);

	two_product(pb.x, pc.y, bxcy1, bxcy0);
	two_product(pc.x, pb.y, cxby1, cxby0);
	two_two_diff(bxcy1, bxcy0, cxby1, cxby0, bc[3], bc[2], bc[1], bc[0]);

	two_product(pc.x, pd.y, cxdy1, cxdy0);
	two_product(pd.x, pc.y, dxcy1, dxcy0);
	two_two_diff(cxdy1, cxdy0, dxcy1, dxcy0, cd[3], cd[2], cd[1], cd[0]);

	two_product(pd.x, pa.y, dxay1, dxay0);
	two_product(pa.x, pd.y, axdy1, axdy0);
	two_two_diff(dxay1, dxay0, axdy1, axdy0, da[3], da[2], da[1], da[0]);

	two_product(pa.x, pc.y, axcy1, axcy0);
	two_product(pc.x, pa.y, cxay1, cxay0);
	two_two_diff(axcy1, axcy0, cxay1, cxay0, ac[3], ac[2], ac[1], ac[0]);

	two_product(pb.x, pd.y, bxdy1, bxdy0);
	two_product(pd.x, pb.y, dxby1, dxby0);
	two_two_diff(bxdy1, bxdy0, dxby1, dxby0, bd[3], bd[2], bd[1], bd[0]);

	templen = rfast_expansion_sum_zeroelim(4, cd, 4, da, temp8);
	cdalen = rfast_expansion_sum_zeroelim(templen, temp8, 4, ac, cda);
	templen = rfast_expansion_sum_zeroelim(4, da, 4, ab, temp8);
	dablen = rfast_expansion_sum_zeroelim(templen, temp8, 4, bd, dab);

	for (i = 0; i < 4; i++) {
		bd[i] = -bd[i];
		ac[i] = -ac[i];
	}

	templen = rfast_expansion_sum_zeroelim(4, ab, 4, bc, temp8);
	abclen = rfast_expansion_sum_zeroelim(templen, temp8, 4, ac, abc);
	templen = rfast_expansion_sum_zeroelim(4, bc, 4, cd, temp8);
	bcdlen = rfast_expansion_sum_zeroelim(templen, temp8, 4, bd, bcd);

	xlen = rscale_expansion_zeroelim(bcdlen, bcd, pa.x, det24x);
	xlen = rscale_expansion_zeroelim(xlen, det24x, pa.x, det48x);
	ylen = rscale_expansion_zeroelim(bcdlen, bcd, pa.y, det24y);
	ylen = rscale_expansion_zeroelim(ylen, det24y, pa.y, det48y);
	alen = rfast_expansion_sum_zeroelim(xlen, det48x, ylen, det48y, adet);

	xlen = rscale_expansion_zeroelim(cdalen, cda, pb.x, det24x);
	xlen = rscale_expansion_zeroelim(xlen, det24x, -pb.x, det48x);
	ylen = rscale_expansion_zeroelim(cdalen, cda, pb.y, det24y);
	ylen = rscale_expansion_zeroelim(ylen, det24y, -pb.y, det48y);
	blen = rfast_expansion_sum_zeroelim(xlen, det48x, ylen, det48y, bdet);

	xlen = rscale_expansion_zeroelim(dablen, dab, pc.x, det24x);
	xlen = rscale_expansion_zeroelim(xlen, det24x, pc.x, det48x);
	ylen = rscale_expansion_zeroelim(dablen, dab, pc.y, det24y);
	ylen = rscale_expansion_zeroelim(ylen, det24y, pc.y, det48y);
	clen = rfast_expansion_sum_zeroelim(xlen, det48x, ylen, det48y, cdet);

	xlen = rscale_expansion_zeroelim(abclen, abc, pd.x, det24x);
	xlen = rscale_expansion_zeroelim(xlen, det24x, -pd.x, det48x);
	ylen = rscale_expansion_zeroelim(abclen, abc, pd.y, det24y);
	ylen = rscale_expansion_zeroelim(ylen, det24y, -pd.y, det48y);
	dlen = rfast_expansion_sum_zeroelim(xlen, det48x, ylen, det48y, ddet);

	ablen = rfast_expansion_sum_zeroelim(alen, adet, blen, bdet, abdet);
	cdlen = rfast_expansion_sum_zeroelim(clen, cdet, dlen, ddet, cddet);
	deterlen = rfast_expansion_sum_zeroelim(ablen, abdet, cdlen, cddet, deter);

	return deter[deterlen - 1];
}

real_t rincircle(const struct point_t pa, const struct point_t pb, const struct point_t pc, const struct point_t pd)
{
	real_t adx, bdx, cdx, ady, bdy, cdy;
	real_t bdxcdy, cdxbdy, cdxady, adxcdy, adxbdy, bdxady;
	real_t alift, blift, clift;
	real_t det;
	real_t permanent, errbound;

	adx = pa.x - pd.x;
	bdx = pb.x - pd.x;
	cdx = pc.x - pd.x;
	ady = pa.y - pd.y;
	bdy = pb.y - pd.y;
	cdy = pc.y - pd.y;

	bdxcdy = bdx * cdy;
	cdxbdy = cdx * bdy;
	alift = adx * adx + ady * ady;

	cdxady = cdx * ady;
	adxcdy = adx * cdy;
	blift = bdx * bdx + bdy * bdy;

	adxbdy = adx * bdy;
	bdxady = bdx * ady;
	clift = cdx * cdx + cdy * cdy;

	det = alift * (bdxcdy - cdxbdy) + blift * (cdxady - adxcdy) + clift * (adxbdy - bdxady);

	permanent = (absolute(bdxcdy) + absolute(cdxbdy)) * alift + (absolute(cdxady) + absolute(adxcdy)) * blift + (absolute(adxbdy) + absolute(bdxady)) * clift;

	errbound = coef.iccerrboundA * permanent;

	if ((det > errbound) || (-det > errbound)) {
		return det;
	}

	return rincircleadapt(pa, pb, pc, pd, permanent);
}


#include <math.h>


// Exact |p - c|^2 - r^2, as an expansion whose leading term carries the sign.
//
// Every step is an error-free transformation: `two_diff` splits a subtraction
// into head plus exact remainder, `scale_expansion_zeroelim` multiplies an
// expansion by a scalar without loss, and `fast_expansion_sum_zeroelim` adds two
// expansions without loss. Nothing here rounds, so no compiler setting and no
// target's floating-point unit can change the result.
int compareToDisc(const real_t px, const real_t py, const real_t cx, const real_t cy, const real_t r)
{
  real_t bvirt, avirt, bround, around, c, abig, ahi, alo, bhi, blo, err1, err2, err3;

  real_t dxe[2];
  real_t dye[2];
  two_diff(px, cx, dxe[1], dxe[0]);
  two_diff(py, cy, dye[1], dye[0]);

  // Squaring a two-term expansion e = hi + lo is e*hi + e*lo, each of which the
  // scale routine performs exactly.
  real_t t1[4], t2[4], dx2[8], dy2[8];
  const int t1len = scale_expansion_zeroelim(2, dxe, dxe[1], t1);
  const int t2len = scale_expansion_zeroelim(2, dxe, dxe[0], t2);
  const int dx2len = fast_expansion_sum_zeroelim(t1len, t1, t2len, t2, dx2);

  real_t u1[4], u2[4];
  const int u1len = scale_expansion_zeroelim(2, dye, dye[1], u1);
  const int u2len = scale_expansion_zeroelim(2, dye, dye[0], u2);
  const int dy2len = fast_expansion_sum_zeroelim(u1len, u1, u2len, u2, dy2);

  real_t sum[16];
  const int sumlen = fast_expansion_sum_zeroelim(dx2len, dx2, dy2len, dy2, sum);

  // r * r, negated so the subtraction becomes another exact sum.
  real_t r2[2];
  square(r, r2[1], r2[0]);
  r2[0] = -r2[0];
  r2[1] = -r2[1];

  real_t total[20];
  const int totallen = fast_expansion_sum_zeroelim(sumlen, sum, 2, r2, total);

  // Expansion components are ordered by increasing magnitude, so the last
  // nonzero one carries the sign. An empty expansion is an exact zero.
  if (totallen == 0) return 0;
  const real_t leading = total[totallen - 1];
  if (leading > 0.0) return 1;
  if (leading < 0.0) return -1;
  return 0;
}


} // namespace detail

namespace
{

detail::point_t toKernel(const Point2 &p)
{
  detail::point_t out;
  out.x = p.x;
  out.y = p.y;
  return out;
}

} // namespace

void initialisePredicates()
{
  detail::init_exact();
}

double orient2d(const Point2 &a, const Point2 &b, const Point2 &c)
{
  return detail::orient2d(toKernel(a), toKernel(b), toKernel(c));
}

double inCircle(const Point2 &a, const Point2 &b, const Point2 &c, const Point2 &d)
{
  return detail::incircle(toKernel(a), toKernel(b), toKernel(c), toKernel(d));
}

int compareToDisc(const Point2 &p, const Point2 &c, const double r)
{
  return detail::compareToDisc(p.x, p.y, c.x, c.y, r);
}

double orient2dExact(const Point2 &a, const Point2 &b, const Point2 &c)
{
  return detail::orient2dexact(toKernel(a), toKernel(b), toKernel(c));
}

double inCircleExact(const Point2 &a, const Point2 &b, const Point2 &c, const Point2 &d)
{
  return detail::incircleexact(toKernel(a), toKernel(b), toKernel(c), toKernel(d));
}

} // namespace procedural
