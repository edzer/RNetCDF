/*=============================================================================*\
 *
 *  Name:       convert.c
 *
 *  Version:    2.0-1
 *
 *  Purpose:    Type conversions for RNetCDF
 *
 *  Author:     Pavel Michna (rnetcdf-devel@bluewin.ch)
 *              Milton Woods (miltonjwoods@gmail.com)
 *
 *  Copyright:  (C) 2004-2017 Pavel Michna, Milton Woods
 *
 *=============================================================================*
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *=============================================================================*
 */


/*=============================================================================*\
 *  Includes
\*=============================================================================*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <float.h>

#include <R.h>
#include <Rinternals.h>

#include <netcdf.h>

#include "common.h"
#include "convert.h"

/*=============================================================================*\
 *  Local macros, constants and variables
\*=============================================================================*/

#define RNC_CHARSXP_MAXLEN 2147483647

#ifdef __WIN32__
# define RNC_FMT_LL "%I64d"
#else
# define RNC_FMT_LL "%lld"
#endif

#ifdef __WIN32__
# define RNC_FMT_ULL "%I64u"
#else
# define RNC_FMT_ULL "%llu"
#endif

#define RNC_DBL_DIG 24

/* Conversion from 64-bit integers to double may round upwards,
   so that the double cannot be converted back to the original type.
   The following limits can be safely converted both ways.
 */
static const double LLONG_MAX_DBL = \
  ((double) LLONG_MAX) * (1.0 - DBL_EPSILON);
static const double LLONG_MIN_DBL = \
  ((double) LLONG_MIN) * (1.0 - DBL_EPSILON);
static const double ULLONG_MAX_DBL = \
  ((double) ULLONG_MAX) * (1.0 - DBL_EPSILON);
static const double SIZE_MAX_DBL = \
  ((double) SIZE_MAX) * (1.0 - DBL_EPSILON);


/*=============================================================================*\
 *  Memory management.
\*=============================================================================*/

size_t
R_nc_length (int ndims, const size_t *count)
{
  int ii;
  size_t length;

  if (ndims < 0) {
    /* Vector of length count[0] */
    ndims = 1;
  }

  length = 1;
  for ( ii=0; ii<ndims; ii++ ) {
    length *= count[ii]; 
  }
  return (length);
}


size_t
R_nc_length_sexp (SEXP count)
{
  size_t length, ii, ndims;
  double *rcount;
  int *icount;

  ndims = xlength (count);

  // Assume scalar if count is empty
  length = 1;

  if (isReal (count)) {
    rcount = REAL (count);
    for ( ii=0; ii<ndims; ii++ ) {
      length *= rcount[ii]; 
    }
    if (!R_FINITE (length)) {
      R_nc_error ("Non-finite length in R_nc_length_sexp");
    }
  } else if (isInteger (count)) {
    icount = INTEGER (count);
    for ( ii=0; ii<ndims; ii++ ) {
      if (icount[ii] != NA_INTEGER) {
        length *= icount[ii];
      } else {
        R_nc_error ("Missing value in R_nc_length_sexp");
      }
    }
  } else if (!isNull (count)) {
    R_nc_error ("Unsupported type in R_nc_length_sexp");
  }

  return (length);
}


SEXP
R_nc_allocArray (SEXPTYPE type, int ndims, const size_t *ccount) {
  SEXP result, rdim;
  int *intp, ii, jj;
  if (ndims > 0) {
    rdim = R_nc_protect( allocVector (INTSXP, ndims));
    intp = INTEGER (rdim);
    for ( ii=0, jj=ndims-1; ii<ndims; ii++, jj-- ) {
      if (ccount[jj] <= INT_MAX) {
        intp[ii] = ccount[jj];
      } else {
        RERROR ("R array dimension cannot exceed range of type int");
      }
    }
    result = R_nc_protect (allocArray (type, rdim));
  } else if (ndims == 0) {
    /* R scalar with no dimensions */
    result = R_nc_protect (allocVector (type, 1));
  } else {
    /* R vector of length ccount[0] without a dimension attribute */
    result = R_nc_protect (allocVector (type, ccount[0]));
  }
  return result;
}


/*=============================================================================*\
 *  String conversions.
\*=============================================================================*/


static char *
R_nc_strsxp_char (SEXP rstr, int ndim, const size_t *xdim)
{
  size_t ii, strlen, cnt;
  char *carr, *thisstr;
  if (ndim > 0) {
    /* Omit fastest-varying dimension from R character array */
    strlen = xdim[ndim-1];
    cnt = R_nc_length (ndim-1, xdim);
  } else if (ndim == 0) {
    /* Scalar character */
    strlen = 1;
    cnt = 1;
  } else {
    /* Single string */
    strlen = xdim[0];
    cnt = 1;
  }
  if (xlength (rstr) < cnt) {
    RERROR (RNC_EDATALEN);
  }
  carr = R_alloc (cnt*strlen, sizeof (char));
  for (ii=0, thisstr=carr; ii<cnt; ii++, thisstr+=strlen) {
    strncpy(thisstr, CHAR( STRING_ELT (rstr, ii)), strlen);
  }
  return carr;
}


static void
R_nc_char_strsxp_init (R_nc_buf *io)
{
  if (io->ndim > 0) {
    io->rxp = R_nc_allocArray (STRSXP, (io->ndim)-1, io->xdim);
  } else {
    /* Single character or string */
    io->rxp = R_nc_allocArray (STRSXP, 0, io->xdim);
  }
  if (!io->cbuf) {
    io->cbuf = R_alloc (R_nc_length (io->ndim, io->xdim), sizeof (char));
  }
}


static void
R_nc_char_strsxp (R_nc_buf *io)
{
  size_t ii, cnt, clen, rlen;
  char *thisstr, *endstr;
  if (io->ndim > 0) {
    /* Omit fastest-varying dimension from R character array */
    clen = io->xdim[(io->ndim)-1];
  } else if (io->ndim == 0) {
    /* Scalar character */
    clen = 1;
  } else {
    /* Single string */
    clen = io->xdim[0];
  }
  rlen = (clen <= RNC_CHARSXP_MAXLEN) ? clen : RNC_CHARSXP_MAXLEN;
  cnt = xlength (io->rxp);
  for (ii=0, thisstr=io->cbuf; ii<cnt; ii++, thisstr+=clen) {
    /* Check if string is null-terminated */
    endstr = memchr (thisstr, 0, rlen);
    if (!endstr) {
      SET_STRING_ELT (io->rxp, ii, mkCharLen (thisstr, rlen));
    } else {
      SET_STRING_ELT (io->rxp, ii, mkChar (thisstr));
    }
  }
}


static const char *
R_nc_raw_char (SEXP rarr, int ndim, const size_t *xdim)
{
  size_t cnt;
  cnt = R_nc_length (ndim, xdim);
  if (xlength (rarr) < cnt) {
    RERROR (RNC_EDATALEN);
  }
  return (const char *) RAW (rarr);
}


static void
R_nc_char_raw_init (R_nc_buf *io)
{
  io->rxp = R_nc_allocArray (RAWSXP, io->ndim, io->xdim);
  io->rbuf = RAW (io->rxp);
  if (!io->cbuf) {
    io->cbuf = io->rbuf;
  }
}


static void
R_nc_char_raw (R_nc_buf *io)
{
  if (io->cbuf != io->rbuf) {
    memcpy(io->rbuf, io->cbuf, xlength(io->rxp) * sizeof(char));
  }
  return;
}


static const char **
R_nc_strsxp_str (SEXP rstr, int ndim, const size_t *xdim)
{
  size_t ii, cnt;
  const char **cstr;
  cnt = R_nc_length (ndim, xdim);
  if (xlength (rstr) < cnt) {
    RERROR (RNC_EDATALEN);
  }
  cstr = (const char **) R_alloc (cnt, sizeof(size_t));
  for (ii=0; ii<cnt; ii++) {
    cstr[ii] = CHAR( STRING_ELT (rstr, ii));
  }
  return cstr;
}


static void
R_nc_str_strsxp_init (R_nc_buf *io)
{
  io->rxp = R_nc_allocArray (STRSXP, io->ndim, io->xdim);
  if (!io->cbuf) {
    io->cbuf = R_alloc (xlength (io->rxp), sizeof(size_t));
  }
}


static void
R_nc_str_strsxp (R_nc_buf *io)
{
  size_t ii, nchar, cnt;
  char **cstr;
  cnt = xlength (io->rxp);
  cstr = (char **) io->cbuf;
  for (ii=0; ii<cnt; ii++) {
    nchar = strlen (cstr[ii]);
    if (nchar > RNC_CHARSXP_MAXLEN) {
      /* Truncate excessively long strings while reading into R */
      SET_STRING_ELT (io->rxp, ii, mkCharLen (cstr[ii], RNC_CHARSXP_MAXLEN));
    } else if (nchar > 0) {
      SET_STRING_ELT (io->rxp, ii, mkChar (cstr[ii]));
    }
  }
  /* Free pointers to strings created by netcdf */
  if (cnt > 0) {
    R_nc_check (nc_free_string (cnt, io->cbuf));
  }
}


/*=============================================================================*\
 *  Numeric type conversions
\*=============================================================================*/

#define R_NC_ISNA_INT(value) (value==NA_INTEGER)
#define R_NC_ISNA_REAL(value) (ISNAN(value))
#define R_NC_ISNA_BIT64(value) (value==NA_INTEGER64)

#define R_NC_RANGE_MIN(VAL,LIM,TYPE) ((TYPE) LIM <= (TYPE) VAL)
#define R_NC_RANGE_MAX(VAL,LIM,TYPE) ((TYPE) VAL <= (TYPE) LIM)
#define R_NC_RANGE_NONE(VAL,LIM,TYPE) (1)


/* Convert numeric values from R to C format.
   Memory for the result is allocated if necessary (and freed by R).
   In special cases, the output is a pointer to the input data,
   so the output data should not be modified.
   An error is raised if any input values are outside the range of the output type.
   For certain combinations of types, some or all range checks are always true,
   and we assume that an optimising compiler will remove these checks.
 */
#define R_NC_R2C_NUM(FUN, \
  NCITYPE, ITYPE, IFUN, NCOTYPE, OTYPE, \
  NATEST, MINTEST, MINVAL, MAXTEST, MAXVAL) \
static const OTYPE* \
FUN (SEXP rv, int ndim, const size_t *xdim, \
     const OTYPE *fill, const double *scale, const double *add) \
{ \
  size_t ii, cnt; \
  int erange=0, efill=0; \
  double factor, offset; \
  const ITYPE *in; \
  OTYPE fillval, *out; \
  in = (ITYPE *) IFUN (rv); \
  cnt = R_nc_length (ndim, xdim); \
  if (xlength (rv) < cnt) { \
    RERROR (RNC_EDATALEN); \
  } \
  if (fill || scale || add || (NCITYPE != NCOTYPE)) { \
    out = (OTYPE *) R_alloc (cnt, sizeof(OTYPE)); \
  } else { \
    out = (OTYPE *) IFUN (rv); \
  } \
  if (scale) { \
    factor = *scale; \
  } else { \
    factor = 1.0; \
  } \
  if (add) { \
    offset = *add; \
  } else { \
    offset = 0.0; \
  } \
  if (fill) { \
    fillval = *fill; \
  } \
  for (ii=0; ii<cnt; ii++) { \
    if (NATEST(in[ii])) { \
      if (fill) { \
        out[ii] = fillval; \
      } else { \
        efill = 1; \
      } \
    } else if (MINTEST(in[ii],MINVAL,ITYPE) && MAXTEST(in[ii],MAXVAL,ITYPE)) { \
      if (scale || add) { \
        out[ii] = round((in[ii] - offset) / factor); \
      } else { \
        out[ii] = in[ii]; \
      } \
    } else { \
      erange = 1; \
      break; \
    } \
  } \
  if ( erange ) { \
    R_nc_error (nc_strerror (NC_ERANGE)); \
  } else if ( efill ) { \
    R_nc_error ("NA values sent to netcdf without conversion to fill value"); \
  } \
  return out; \
}

R_NC_R2C_NUM(R_nc_r2c_int_schar, NC_INT, int, INTEGER, NC_BYTE, signed char, \
  R_NC_ISNA_INT, R_NC_RANGE_MIN, SCHAR_MIN, R_NC_RANGE_MAX, SCHAR_MAX);
R_NC_R2C_NUM(R_nc_r2c_int_uchar, NC_INT, int, INTEGER, NC_UBYTE, unsigned char, \
  R_NC_ISNA_INT, R_NC_RANGE_MIN, 0, R_NC_RANGE_MAX, UCHAR_MAX);
R_NC_R2C_NUM(R_nc_r2c_int_short, NC_INT, int, INTEGER, NC_SHORT, short, \
  R_NC_ISNA_INT, R_NC_RANGE_MIN, SHRT_MIN, R_NC_RANGE_MAX, SHRT_MAX);
R_NC_R2C_NUM(R_nc_r2c_int_ushort, NC_INT, int, INTEGER, NC_USHORT, unsigned short, \
  R_NC_ISNA_INT, R_NC_RANGE_MIN, 0, R_NC_RANGE_MAX, USHRT_MAX);
R_NC_R2C_NUM(R_nc_r2c_int_int, NC_INT, int, INTEGER, NC_INT, int, \
  R_NC_ISNA_INT, R_NC_RANGE_NONE, , R_NC_RANGE_NONE, );
R_NC_R2C_NUM(R_nc_r2c_int_uint, NC_INT, int, INTEGER, NC_UINT, unsigned int, \
  R_NC_ISNA_INT, R_NC_RANGE_MIN, 0, R_NC_RANGE_NONE, );
R_NC_R2C_NUM(R_nc_r2c_int_ll, NC_INT, int, INTEGER, NC_INT64, long long, \
  R_NC_ISNA_INT, R_NC_RANGE_NONE, , R_NC_RANGE_NONE, );
R_NC_R2C_NUM(R_nc_r2c_int_ull, NC_INT, int, INTEGER, NC_UINT64, unsigned long long, \
  R_NC_ISNA_INT, R_NC_RANGE_MIN, 0, R_NC_RANGE_NONE, );
/* Assume int and size_t are different types */
R_NC_R2C_NUM(R_nc_r2c_int_size, NC_INT, int, INTEGER, NC_UINT64, size_t, \
  R_NC_ISNA_INT, R_NC_RANGE_MIN, 0, R_NC_RANGE_NONE, );
R_NC_R2C_NUM(R_nc_r2c_int_float, NC_INT, int, INTEGER, NC_FLOAT, float, \
  R_NC_ISNA_INT, R_NC_RANGE_NONE, , R_NC_RANGE_NONE, );
R_NC_R2C_NUM(R_nc_r2c_int_dbl, NC_INT, int, INTEGER, NC_DOUBLE, double, \
  R_NC_ISNA_INT, R_NC_RANGE_NONE, , R_NC_RANGE_NONE, );

R_NC_R2C_NUM(R_nc_r2c_dbl_schar, NC_DOUBLE, double, REAL, NC_BYTE, signed char, \
  R_NC_ISNA_REAL, R_NC_RANGE_MIN, SCHAR_MIN, R_NC_RANGE_MAX, SCHAR_MAX);
R_NC_R2C_NUM(R_nc_r2c_dbl_uchar, NC_DOUBLE, double, REAL, NC_UBYTE, unsigned char, \
  R_NC_ISNA_REAL, R_NC_RANGE_MIN, 0, R_NC_RANGE_MAX, UCHAR_MAX);
R_NC_R2C_NUM(R_nc_r2c_dbl_short, NC_DOUBLE, double, REAL, NC_SHORT, short, \
  R_NC_ISNA_REAL, R_NC_RANGE_MIN, SHRT_MIN, R_NC_RANGE_MAX, SHRT_MAX);
R_NC_R2C_NUM(R_nc_r2c_dbl_ushort, NC_DOUBLE, double, REAL, NC_USHORT, unsigned short, \
  R_NC_ISNA_REAL, R_NC_RANGE_MIN, 0, R_NC_RANGE_MAX, USHRT_MAX);
R_NC_R2C_NUM(R_nc_r2c_dbl_int, NC_DOUBLE, double, REAL, NC_INT, int, \
  R_NC_ISNA_REAL, R_NC_RANGE_MIN, INT_MIN, R_NC_RANGE_MAX, INT_MAX);
R_NC_R2C_NUM(R_nc_r2c_dbl_uint, NC_DOUBLE, double, REAL, NC_UINT, unsigned int, \
  R_NC_ISNA_REAL, R_NC_RANGE_MIN, 0, R_NC_RANGE_MAX, UINT_MAX);
R_NC_R2C_NUM(R_nc_r2c_dbl_ll, NC_DOUBLE, double, REAL, NC_INT64, long long, \
  R_NC_ISNA_REAL, R_NC_RANGE_MIN, LLONG_MIN_DBL, R_NC_RANGE_MAX, LLONG_MAX_DBL);
R_NC_R2C_NUM(R_nc_r2c_dbl_ull, NC_DOUBLE, double, REAL, NC_UINT64, unsigned long long, \
  R_NC_ISNA_REAL, R_NC_RANGE_MIN, 0, R_NC_RANGE_MAX, ULLONG_MAX_DBL);
R_NC_R2C_NUM(R_nc_r2c_dbl_size, NC_DOUBLE, double, REAL, NC_UINT64, size_t, \
  R_NC_ISNA_REAL, R_NC_RANGE_MIN, 0, R_NC_RANGE_MAX, SIZE_MAX_DBL);
R_NC_R2C_NUM(R_nc_r2c_dbl_float, NC_DOUBLE, double, REAL, NC_FLOAT, float, \
  R_NC_ISNA_REAL, R_NC_RANGE_MIN, -FLT_MAX, R_NC_RANGE_MAX, FLT_MAX);
R_NC_R2C_NUM(R_nc_r2c_dbl_dbl, NC_DOUBLE, double, REAL, NC_DOUBLE, double, \
  R_NC_ISNA_REAL, R_NC_RANGE_NONE, , R_NC_RANGE_NONE, );

/* bit64 is treated by R as signed long long,
   but we may need to store unsigned long long,
   with very large positive values wrapping to negative values in R.
   We allow wrapping in reverse for conversion of bit64 to unsigned long long.
 */
R_NC_R2C_NUM(R_nc_r2c_bit64_schar, NC_INT64, long long, REAL, NC_BYTE, signed char, \
  R_NC_ISNA_BIT64, R_NC_RANGE_MIN, SCHAR_MIN, R_NC_RANGE_MAX, SCHAR_MAX);
R_NC_R2C_NUM(R_nc_r2c_bit64_uchar, NC_INT64, long long, REAL, NC_UBYTE, unsigned char, \
  R_NC_ISNA_BIT64, R_NC_RANGE_MIN, 0, R_NC_RANGE_MAX, UCHAR_MAX);
R_NC_R2C_NUM(R_nc_r2c_bit64_short, NC_INT64, long long, REAL, NC_SHORT, short, \
  R_NC_ISNA_BIT64, R_NC_RANGE_MIN, SHRT_MIN, R_NC_RANGE_MAX, SHRT_MAX);
R_NC_R2C_NUM(R_nc_r2c_bit64_ushort, NC_INT64, long long, REAL, NC_USHORT, unsigned short, \
  R_NC_ISNA_BIT64, R_NC_RANGE_MIN, 0, R_NC_RANGE_MAX, USHRT_MAX);
R_NC_R2C_NUM(R_nc_r2c_bit64_int, NC_INT64, long long, REAL, NC_INT, int, \
  R_NC_ISNA_BIT64, R_NC_RANGE_MIN, INT_MIN, R_NC_RANGE_MAX, INT_MAX);
R_NC_R2C_NUM(R_nc_r2c_bit64_uint, NC_INT64, long long, REAL, NC_UINT, unsigned int, \
  R_NC_ISNA_BIT64, R_NC_RANGE_MIN, 0, R_NC_RANGE_MAX, UINT_MAX);
R_NC_R2C_NUM(R_nc_r2c_bit64_ll, NC_INT64, long long, REAL, NC_INT64, long long, \
  R_NC_ISNA_BIT64, R_NC_RANGE_NONE, , R_NC_RANGE_NONE, );
R_NC_R2C_NUM(R_nc_r2c_bit64_ull, NC_INT64, long long, REAL, NC_UINT64, unsigned long long, \
  R_NC_ISNA_BIT64, R_NC_RANGE_NONE, , R_NC_RANGE_NONE, );
R_NC_R2C_NUM(R_nc_r2c_bit64_float, NC_INT64, long long, REAL, NC_FLOAT, float, \
  R_NC_ISNA_BIT64, R_NC_RANGE_MIN, -FLT_MAX, R_NC_RANGE_MAX, FLT_MAX);
R_NC_R2C_NUM(R_nc_r2c_bit64_dbl, NC_INT64, long long, REAL, NC_DOUBLE, double, \
  R_NC_ISNA_BIT64, R_NC_RANGE_NONE, , R_NC_RANGE_NONE, );
#if LLONG_MAX > SIZE_MAX
/* size_t is smaller than unsigned long long.
   Only allow positive values of bit64
 */
R_NC_R2C_NUM(R_nc_r2c_bit64_size, NC_INT64, long long, REAL, NC_NAT, size_t, \
  R_NC_ISNA_BIT64, R_NC_RANGE_MIN, 0, R_NC_RANGE_MAX, SIZE_MAX);
#else
/* Allow wrapping from negative bit64 to positive size_t */
R_NC_R2C_NUM(R_nc_r2c_bit64_size, NC_INT64, long long, REAL, NC_NAT, size_t, \
  R_NC_ISNA_BIT64, R_NC_RANGE_NONE, , R_NC_RANGE_MAX, SIZE_MAX);
#endif

/* Allocate memory for reading a netcdf variable slice
   and converting the results to an R variable.
   On input, the R_nc_buf structure contains dimensions of the buffer (ndim, *xdim).
   On output, the R_nc_buf structure contains an allocated SEXP and a pointer to its data.
 */
#define R_NC_C2R_NUM_INIT(FUN, SEXPTYPE, OFUN) \
static void \
FUN (R_nc_buf *io) \
{ \
  io->rxp = R_nc_allocArray (SEXPTYPE, io->ndim, io->xdim); \
  io->rbuf = OFUN (io->rxp); \
  if (!io->cbuf) { \
    io->cbuf = io->rbuf; \
  } \
}

R_NC_C2R_NUM_INIT(R_nc_c2r_int_init, INTSXP, INTEGER);
R_NC_C2R_NUM_INIT(R_nc_c2r_dbl_init, REALSXP, REAL);
R_NC_C2R_NUM_INIT(R_nc_c2r_bit64_init, REALSXP, REAL);


/* Convert numeric values from C to R format.
   Parameters and buffers for the conversion are passed via the R_nc_buf struct.
   The same buffer may be used for input and output.
   Output type may be larger (not smaller) than input,
   so convert in reverse order to avoid overwriting input with output.
   Fill values and values outside the valid range are set to missing.
 */
#define R_NC_C2R_NUM(FUN, NCITYPE, ITYPE, NCOTYPE, OTYPE, \
  MISSVAL, MINVAL, MAXVAL) \
static void \
FUN (R_nc_buf *io) \
{ \
  size_t ii; \
  ITYPE fillval, minval, maxval, *in; \
  OTYPE *out; \
  ii = xlength (io->rxp); \
  in = (ITYPE *) io->cbuf; \
  out = (OTYPE *) io->rbuf; \
  if (io->fill) { \
    fillval = *((ITYPE *) io->fill); \
  } \
  if (io->min) { \
    minval = *((ITYPE *) io->min); \
  } else { \
    minval = MINVAL; \
  } \
  if (io->max) { \
    maxval = *((ITYPE *) io->max); \
  } else { \
    maxval = MAXVAL; \
  } \
  if (io->fill) { \
    while (ii-- > 0) { \
      if ((in[ii] == fillval) || (in[ii] < minval) || (maxval < in[ii])) { \
        out[ii] = MISSVAL; \
      } else { \
        out[ii] = in[ii]; \
      } \
    } \
  } else { \
    while (ii-- > 0) { \
      if ((in[ii] < minval) || (maxval < in[ii])) { \
        out[ii] = MISSVAL; \
      } else { \
        out[ii] = in[ii]; \
      } \
    } \
  } \
}

R_NC_C2R_NUM(R_nc_c2r_schar_int, NC_BYTE, signed char, NC_INT, int, \
  NA_INTEGER, SCHAR_MIN, SCHAR_MAX);
R_NC_C2R_NUM(R_nc_c2r_uchar_int, NC_UBYTE, unsigned char, NC_INT, int, \
  NA_INTEGER, 0, UCHAR_MAX);
R_NC_C2R_NUM(R_nc_c2r_short_int, NC_SHORT, short, NC_INT, int, \
  NA_INTEGER, SHRT_MIN, SHRT_MAX);
R_NC_C2R_NUM(R_nc_c2r_ushort_int, NC_USHORT, unsigned short, NC_INT, int, \
  NA_INTEGER, 0, USHRT_MAX);
R_NC_C2R_NUM(R_nc_c2r_int_int, NC_INT, int, NC_INT, int, \
  NA_INTEGER, INT_MIN, INT_MAX);

R_NC_C2R_NUM(R_nc_c2r_schar_dbl, NC_BYTE, signed char, NC_DOUBLE, double, \
  NA_REAL, SCHAR_MIN, SCHAR_MAX);
R_NC_C2R_NUM(R_nc_c2r_uchar_dbl, NC_UBYTE, unsigned char, NC_DOUBLE, double, \
  NA_REAL, 0, UCHAR_MAX);
R_NC_C2R_NUM(R_nc_c2r_short_dbl, NC_SHORT, short, NC_DOUBLE, double, \
  NA_REAL, SHRT_MIN, SHRT_MAX);
R_NC_C2R_NUM(R_nc_c2r_ushort_dbl, NC_USHORT, unsigned short, NC_DOUBLE, double, \
  NA_REAL, 0, USHRT_MAX);
R_NC_C2R_NUM(R_nc_c2r_int_dbl, NC_INT, int, NC_DOUBLE, double, \
  NA_REAL, INT_MIN, INT_MAX);
R_NC_C2R_NUM(R_nc_c2r_uint_dbl, NC_UINT, unsigned int, NC_DOUBLE, double, \
  NA_REAL, 0, UINT_MAX);
R_NC_C2R_NUM(R_nc_c2r_float_dbl, NC_FLOAT, float, NC_DOUBLE, double, \
  NA_REAL, -FLT_MAX, FLT_MAX);
R_NC_C2R_NUM(R_nc_c2r_dbl_dbl, NC_DOUBLE, double, NC_DOUBLE, double, \
  NA_REAL, -DBL_MAX, DBL_MAX);
R_NC_C2R_NUM(R_nc_c2r_int64_dbl, NC_INT64, long long, NC_DOUBLE, double, \
  NA_REAL, LLONG_MIN, LLONG_MAX);
R_NC_C2R_NUM(R_nc_c2r_uint64_dbl, NC_UINT64, unsigned long long, NC_DOUBLE, double, \
  NA_REAL, 0, ULLONG_MAX);

/* bit64 is treated by R as signed long long,
   but we may need to store unsigned long long,
   with very large positive values wrapping to negative values in R.
 */
R_NC_C2R_NUM(R_nc_c2r_int64_bit64, NC_INT64, long long, NC_INT64, long long, \
  NA_INTEGER64, LLONG_MIN, LLONG_MAX);
R_NC_C2R_NUM(R_nc_c2r_uint64_bit64, NC_UINT64, unsigned long long, NC_INT64, long long, \
  NA_INTEGER64, 0, ULLONG_MAX);


/* Convert numeric values from C to R format with unpacking.
   Parameters and buffers for the conversion are passed via the R_nc_buf struct.
   Output type is assumed not to be smaller than input type,
   so the same buffer may be used for input and output
   by converting in reverse order.
   Fill values and values outside the valid range are set to missing.
 */

#define R_NC_C2R_NUM_UNPACK(FUN, ITYPE, MINVAL, MAXVAL) \
static void \
FUN (R_nc_buf *io) \
{ \
  size_t ii; \
  double factor, offset; \
  ITYPE fillval, minval, maxval, *in; \
  double *out; \
  ii = xlength (io->rxp); \
  in = (ITYPE *) io->cbuf; \
  out = (double *) io->rbuf; \
  if (io->scale) { \
    factor = *(io->scale); \
  } else { \
    factor = 1.0; \
  } \
  if (io->add) { \
    offset = *(io->add); \
  } else { \
    offset = 0.0; \
  } \
  if (io->fill) { \
    fillval = *((ITYPE *) io->fill); \
  } \
  if (io->min) { \
    minval = *((ITYPE *) io->min); \
  } else { \
    minval = MINVAL; \
  } \
  if (io->max) { \
    maxval = *((ITYPE *) io->max); \
  } else { \
    maxval = MAXVAL; \
  } \
  if (io->fill) { \
    while (ii-- > 0) { \
      if ((in[ii] == fillval) || (in[ii] < minval) || (maxval < in[ii])) { \
        out[ii] = NA_REAL; \
      } else { \
        out[ii] = in[ii] * factor + offset; \
      } \
    } \
  } else { \
    while (ii-- > 0) { \
      if ((in[ii] < minval) || (maxval < in[ii])) { \
        out[ii] = NA_REAL; \
      } else { \
        out[ii] = in[ii] * factor + offset; \
      } \
    } \
  } \
}

R_NC_C2R_NUM_UNPACK(R_nc_c2r_unpack_schar, signed char, SCHAR_MIN, SCHAR_MAX);
R_NC_C2R_NUM_UNPACK(R_nc_c2r_unpack_uchar, unsigned char, 0, UCHAR_MAX);
R_NC_C2R_NUM_UNPACK(R_nc_c2r_unpack_short, short, SHRT_MIN, SHRT_MAX);
R_NC_C2R_NUM_UNPACK(R_nc_c2r_unpack_ushort, unsigned short, 0, USHRT_MAX);
R_NC_C2R_NUM_UNPACK(R_nc_c2r_unpack_int, int, INT_MIN, INT_MAX);
R_NC_C2R_NUM_UNPACK(R_nc_c2r_unpack_uint, unsigned int, 0, UINT_MAX);
R_NC_C2R_NUM_UNPACK(R_nc_c2r_unpack_float, float, -FLT_MAX, FLT_MAX);
R_NC_C2R_NUM_UNPACK(R_nc_c2r_unpack_dbl, double, -DBL_MAX, DBL_MAX);
R_NC_C2R_NUM_UNPACK(R_nc_c2r_unpack_int64, long long, LLONG_MIN, LLONG_MAX);
R_NC_C2R_NUM_UNPACK(R_nc_c2r_unpack_uint64, unsigned long long, 0, ULLONG_MAX);


/*=============================================================================*\
 *  User-defined type conversions
\*=============================================================================*/

/* -- VLEN class -- */

/* Convert list of vectors from R to nc_vlen_t format.
   Memory for the result is allocated if necessary (and freed by R).
   In special cases, the output may point to the input data,
   so the output data should not be modified.
   An error is raised if input values cannot be converted to the vlen base type.
 */
static nc_vlen_t *
R_nc_vecsxp_vlen (SEXP rv, int ncid, nc_type xtype, int ndim, const size_t *xdim,
                  const void *fill, const double *scale, const double *add)
{
  size_t ii, cnt, len, size;
  int baseclass;
  nc_type basetype;
  nc_vlen_t *vbuf;
  SEXP item;

  cnt = R_nc_length (ndim, xdim);
  if (xlength (rv) < cnt) {
    RERROR (RNC_EDATALEN);
  }

  R_nc_check (nc_inq_user_type (ncid, xtype, NULL, NULL, &basetype, NULL, NULL));
  if (basetype > NC_MAX_ATOMIC_TYPE) {
    R_nc_check (nc_inq_user_type (ncid, basetype, NULL, &size, NULL, NULL, &baseclass));
  } else {
    baseclass = NC_NAT;
    size = 0;
  }

  vbuf = (nc_vlen_t *) R_alloc (cnt, sizeof(nc_vlen_t));
  for (ii=0; ii<cnt; ii++) {
    item = VECTOR_ELT(rv, ii);
    if (basetype == NC_CHAR && TYPEOF (item) == STRSXP) {
      if (xlength (item) > 0) {
        len = strlen (CHAR (STRING_ELT (item, 0)));
      } else {
        len = 0;
      }
    } else if (baseclass == NC_OPAQUE && TYPEOF (item) == RAWSXP) {
      len = xlength(item) / size;
    } else {
      len = xlength(item);
    }
    vbuf[ii].len = len;
    if (len > 0) {
      vbuf[ii].p = (void *) R_nc_r2c (item, ncid, basetype,
                                      -1, &len, fill, scale, add);
    } else {
      vbuf[ii].p = NULL;
    }
  }
  return vbuf;
}


/* Allocate memory for reading a slice of a netcdf vlen variable
   and converting the results to an R variable.
   On input, the R_nc_buf structure contains dimensions of the buffer (ndim, *xdim).
   On output, the R_nc_buf structure contains an allocated R list (with dim attribute),
     and the C buffer is an array of pointers which are allocated by netcdf
     when reading from the variable (and which must be freed later by netcdf).
 */
static void
R_nc_vlen_vecsxp_init (R_nc_buf *io)
{
  io->rxp = R_nc_allocArray (VECSXP, io->ndim, io->xdim);
  if (!io->cbuf) {
    io->cbuf = R_alloc (xlength (io->rxp), sizeof(nc_vlen_t));
  }
}


/* Convert netcdf vlen array from C to R format.
   Parameters and buffers for the conversion are passed via the R_nc_buf struct.
   On input, the C data is stored in io->cbuf.
   On output, the R data is copied to io->rxp, and memory used by netcdf is freed.
 */
static void
R_nc_vlen_vecsxp (R_nc_buf *io)
{
  size_t ii, cnt;
  nc_type basetype;
  nc_vlen_t *vbuf;
  R_nc_buf tmpio;

  vbuf = io->cbuf;
  cnt = xlength (io->rxp);
  R_nc_check (nc_inq_user_type (io->ncid, io->xtype, NULL, NULL, &basetype, NULL, NULL));

  for (ii=0; ii<cnt; ii++) {
    R_nc_c2r_init (&tmpio, vbuf[ii].p, io->ncid, basetype, -1, &(vbuf[ii].len),
                   io->rawchar, io->fitnum, io->fill, io->min, io->max,
                   io->scale, io->add);
    SET_VECTOR_ELT (io->rxp, ii, R_nc_c2r (&tmpio));
    nc_free_vlen(&(vbuf[ii]));
  }
}


/* -- Opaque class -- */


/* Convert raw array from R to netcdf opaque type.
   Memory for the result is allocated if necessary (and freed by R).
   In special cases, the output may point to the input data,
   so the output data should not be modified.
 */
static const char *
R_nc_raw_opaque (SEXP rv, int ncid, nc_type xtype, int ndim, const size_t *xdim)
{
  size_t cnt, size;
  R_nc_check (nc_inq_user_type (ncid, xtype, NULL, &size, NULL, NULL, NULL));
  cnt = R_nc_length (ndim, xdim);
  if (xlength (rv) < (cnt * size)) {
    RERROR (RNC_EDATALEN);
  }
  return (const char *) RAW (rv);
}


static void
R_nc_opaque_raw_init (R_nc_buf *io)
{
  int ndim;
  size_t *xdim, size;

  /* Fastest varying dimension of R array contains bytes of opaque data */
  R_nc_check (nc_inq_user_type (io->ncid, io->xtype, NULL, &size, NULL, NULL, NULL));

  ndim = io->ndim;
  if (ndim < 0) {
    /* Special case for an R vector without dimension attribute,
       but dimensions are needed to select opaque elements of a vector
     */
    ndim = 1;
  }
  xdim = (size_t *) R_alloc (ndim + 1, sizeof(size_t));
  memcpy (xdim, io->xdim, ndim * sizeof(size_t));
  xdim[ndim] = size;

  io->rxp = R_nc_allocArray (RAWSXP, ndim + 1, xdim);
  io->rbuf = RAW (io->rxp);
  if (!io->cbuf) {
    io->cbuf = io->rbuf;
  }
}


static void
R_nc_opaque_raw (R_nc_buf *io)
{
  if (io->cbuf != io->rbuf) {
    memcpy(io->rbuf, io->cbuf, xlength(io->rxp) * sizeof(char));
  }
  return;
}


/* -- Enum class -- */


/* Convert factor array from R to netcdf enum type.
   Memory for the result is allocated if necessary (and freed by R).
 */
static void *
R_nc_factor_enum (SEXP rv, int ncid, nc_type xtype, int ndim, const size_t *xdim,
                  const void *fill)
{
  SEXP levels;
  size_t size, imem, nmem, ilev, nlev, *ilev2mem, ifac, nfac;
  char *memnames, *memname, *memvals, *memval, *out;
  const char **levnames;
  int ismatch, *in, inval;

  /* Extract indices and level names of R factor */
  in = INTEGER (rv);

  levels = getAttrib (rv, R_LevelsSymbol);
  if (!isString (levels)) {
    RERROR ("Expected character vector for levels of factor array")
  }

  nlev = xlength (levels);

  levnames = (const char **) R_alloc (nlev, sizeof(size_t));

  for (ilev=0; ilev<nlev; ilev++) {
    levnames[ilev] = CHAR( STRING_ELT (levels, ilev));
  }

  /* Read values and names of enum members */
  R_nc_check (nc_inq_enum(ncid, xtype, NULL, NULL, &size, &nmem));

  memnames = R_alloc (nmem, NC_MAX_NAME+1);
  memvals = R_alloc (nmem, size);

  for (imem=0, memname=memnames, memval=memvals; imem<nmem;
       imem++, memname+=(NC_MAX_NAME+1), memval+=size) {
    R_nc_check (nc_inq_enum_member (ncid, xtype, imem, memname, memval));
  }

  /* Find enum member for each R level */
  ilev2mem = (size_t *) R_alloc (nlev, sizeof(size_t));

  for (ilev=0; ilev<nlev; ilev++) {
    ismatch = 0;
    for (imem=0, memname=memnames; imem<nmem;
         imem++, memname+=(NC_MAX_NAME+1)) {
      if (strcmp(memname, levnames[ilev]) == 0) {
        ismatch = 1;
        ilev2mem[ilev] = imem;
        break;
      }
    }
    if (!ismatch) {
      RERROR ("Level has no matching member in enum type")
    }
  }

  /* Convert factor indices to enum values */
  nfac = xlength (rv);
  out = R_alloc (nfac, size);

  for (ifac=0; ifac<nfac; ifac++) {
    inval = in[ifac];
    if (inval==NA_INTEGER && fill) {
      memcpy(out + ifac*size, fill, size);
    } else if (0 < inval && inval <= nlev) {
      imem = ilev2mem[inval-1];
      memcpy(out + ifac*size, memvals + imem*size, size);
    } else {
      RERROR ("Invalid index in factor")
    }
  }

  return out;
}


static void
R_nc_enum_factor_init (R_nc_buf *io)
{
  size_t size;
  io->rxp = R_nc_allocArray (INTSXP, io->ndim, io->xdim);
  io->rbuf = INTEGER (io->rxp);
  if (!io->cbuf) {
    R_nc_check (nc_inq_type (io->ncid, io->xtype, NULL, &size));
    io->cbuf = R_alloc (xlength (io->rxp), size);
  }
}


/* Convert specified number of bytes to an R symbol,
   as required to store and retrieve values from a hashed environment.
   The work array must have minimum size 2*size+2 bytes.
 */
static SEXP
R_nc_char_symbol (char *in, size_t size, char *work)
{
  size_t ii;
  work[0]='X';
  for (ii=0; ii<size; ii++) {
    sprintf(work+1+ii*2, "%02X", in[ii]);
  }
  work[2*size+1]='\0';
  return install(work);
}


/* Convert netcdf enum values in io->cbuf to R factor array in io->rbuf.
   Memory for the result must be pre-allocated by R_nc_enum_factor_init.
 */
static void
R_nc_enum_factor (R_nc_buf *io)
{
  SEXP levels, classname, env, symbol, value;
  size_t size, nmem, ifac, nfac;
  char *memname, *memval, *work, *inval, *fill;
  int ncid, imem, imemmax, *out;
  nc_type xtype;

  /* Read values and names of netcdf enum members.
     Store names in an R character vector for use as R factor levels.
     Store values and their R indices (1-based) in a hashed environment.
     The env is PROTECTed, so individual variables need not be.
     But values do need PROTECTing before assignment to env, 
     otherwise gctorture reveals problems.
     I'm not sure if symbols need PROTECTing, but better safe than sorry.
   */
  ncid = io->ncid;
  xtype = io->xtype;
  R_nc_check (nc_inq_enum(ncid, xtype, NULL, NULL, &size, &nmem));
  env = R_nc_protect (eval(lang1(install("new.env")),R_BaseEnv));

  levels = R_nc_allocArray (STRSXP, -1, &nmem);
  memname = R_alloc (nmem, NC_MAX_NAME+1);
  memval = R_alloc (1, size);
  work = R_alloc (2*size+2, 1);

  imemmax = nmem; // netcdf member index is int
  for (imem=0; imem<imemmax; imem++) {
    R_nc_check (nc_inq_enum_member (ncid, xtype, imem, memname, memval));
    SET_STRING_ELT (levels, imem, mkChar (memname));
    symbol = PROTECT (R_nc_char_symbol (memval, size, work));
    value = PROTECT (ScalarInteger (imem+1));
    defineVar (symbol, value, env);
    UNPROTECT(2);
  }

  /* Add fill value (if defined) to the hashed environment.
   */
  fill = io->fill;
  if (fill) {
    symbol = PROTECT (R_nc_char_symbol (fill, size, work));
    value = PROTECT (ScalarInteger (NA_INTEGER));
    defineVar (symbol, value, env);
    UNPROTECT(2);
  }

  /* Convert netcdf enum values to R indices.
     Use hashed environment prepared above for efficient lookups.
   */
  nfac = xlength (io->rxp);

  out = io->rbuf;
  for (ifac=0, inval=io->cbuf; ifac<nfac; ifac++, inval+=size) {
    symbol = PROTECT (R_nc_char_symbol (inval, size, work));
    value = findVarInFrame3 (env, symbol, TRUE);
    UNPROTECT(1);
    if (value == R_UnboundValue) {
      R_nc_error ("Unknown enum value in variable");
    } else {
      out[ifac] = INTEGER (value)[0];
    }
  }

  /* Set attributes for R factor */
  setAttrib(io->rxp, R_LevelsSymbol, levels);
  classname = R_nc_protect (allocVector (STRSXP, 1));
  SET_STRING_ELT(classname, 0, mkChar("factor"));
  setAttrib(io->rxp, R_ClassSymbol, classname);
}


/* -- Compound class -- */

/* Convert list of arrays from R to netcdf compound type.
   Memory for the result is allocated (and freed by R).
 */
static void *
R_nc_vecsxp_compound (SEXP rv, int ncid, nc_type xtype, int ndim, const size_t *xdim)
{
  size_t cnt, size, nfld, offset, fldsize, fldcnt, fldlen,
         nlist, ilist, ielem, *dimsizefld;
  nc_type typefld;
  int ifldmax, ifld, idimfld, ndimfld, *dimlenfld, ismatch;
  char *bufout, namefld[NC_MAX_NAME+1];
  const char *buffld;
  void *highwater;
  SEXP namelist;

  /* Get size and number of fields in compound type */
  R_nc_check (nc_inq_compound(ncid, xtype, NULL, &size, &nfld));

  /* Check names attribute of R list */
  namelist = getAttrib (rv, R_NamesSymbol);
  if (!isString (namelist)) {
    R_nc_error ("Named list required for conversion to compound type");
  }
  nlist = xlength (namelist);
  if (nlist < nfld) {
    R_nc_error ("Not enough fields in list for conversion to compound type");
  }

  /* Allocate memory for compound array,
     filling with zeros so that valgrind does not complain about
     uninitialised values in gaps inserted for alignment */
  cnt = R_nc_length (ndim, xdim);
  bufout = R_alloc (cnt, size);
  memset(bufout, 0, cnt*size);

  /* Convert each field in turn */
  ifldmax = nfld;
  for (ifld=0; ifld<ifldmax; ifld++) {

    /* Save memory "highwater mark" to reclaim memory from R_alloc,
       which may consume large chunks of memory after R_nc_r2c.
     */
    highwater = vmaxget();

    /* Query the dataset for details of the field. */
    R_nc_check (nc_inq_compound_field (ncid, xtype, ifld, namefld,
                  &offset, &typefld, &ndimfld, NULL));
    dimlenfld = (int *) R_alloc (ndimfld, sizeof(int));
    R_nc_check (nc_inq_compound_fielddim_sizes(ncid, xtype, ifld, dimlenfld));
    R_nc_check (nc_inq_type (ncid, typefld, NULL, &fldsize));

    /* Find the field by name in the R input list */
    ismatch = 0;
    for (ilist=0; ilist<nlist; ilist++) {
      if (strcmp (CHAR (STRING_ELT (namelist, ilist)), namefld) == 0) {
        // ilist is the matching list index
        ismatch = 1;
        break;
      }
    }
    if (!ismatch) {
      R_nc_error ("Name of compound field not found in input list");
    }

    /* Convert the field from R to C.
       Convert the dimension lengths from integer to size_t,
       adding an extra dimension (slowest varying) for the total number
       of elements in the compound array (cnt). */
    dimsizefld = (size_t *) R_alloc (ndimfld+1, sizeof(size_t));
    dimsizefld[0] = cnt;
    for (idimfld=0; idimfld<ndimfld; idimfld++) {
      dimsizefld[idimfld+1] = dimlenfld[idimfld];
    }
    buffld = R_nc_r2c (VECTOR_ELT (rv, ilist), ncid, typefld, ndimfld+1, dimsizefld,
                       NULL, NULL, NULL);

    /* Copy elements from the field array into the compound array */
    fldcnt = R_nc_length (ndimfld, dimsizefld+1);
    fldlen = fldsize * fldcnt;
    for (ielem=0; ielem<cnt; ielem++) {
      memcpy (bufout+ielem*size+offset, buffld+ielem*fldlen, fldlen);
    }

    /* Allow memory from R_alloc since vmaxget to be reclaimed */
    vmaxset (highwater);
  }

  return bufout;
}


static void
R_nc_compound_vecsxp_init (R_nc_buf *io)
{
  size_t size, nfld, cnt;

  /* The in-memory layout of compound types can differ between writing and reading.
     When writing, an arbitrary layout is specified by the user.
     When reading, a "native" layout is returned by the netcdf library.
     If the layout has been defined since the dataset was opened,
     the type inquiry functions always return the layout used for writing.
     We can get the layout for reading from a read-only instance of the dataset.
   */
  if (R_nc_redef (io->ncid) == NC_NOERR) {
    /* Dataset must be writable because it is now in define mode */
    R_nc_error ("Please read compound type from a read-only dataset");
  }

  /* Get number of fields in compound type */
  R_nc_check (nc_inq_compound(io->ncid, io->xtype, NULL, &size, &nfld));

  /* Allocate memory for output list */
  io->rxp = R_nc_allocArray (VECSXP, -1, &nfld);

  /* Allocate memory for compound array */
  if (!io->cbuf) {
    cnt = R_nc_length (io->ndim, io->xdim);
    io->cbuf = R_alloc (cnt, size);
  }
}


/* Convert netcdf compound values in io->cbuf to R list of arrays in io->rxp.
   Data structures are prepared by a prior call to R_nc_compound_vecsxp_init.
 */
static void
R_nc_compound_vecsxp (R_nc_buf *io)
{
  int ncid, ifld, ifldmax, idim, ndim, idimfld, ndimfld, *dimlenfld, ndimslice;
  nc_type xtype, typefld;
  size_t size, nfld, cnt, offset, fldsize, *dimslice, fldcnt, fldlen, ielem;
  SEXP namelist, rxpfld;
  char namefld[NC_MAX_NAME+1], *buffld, *bufcmp;
  R_nc_buf iofld;
  void *highwater;

  /* Get size and number of fields in compound type */
  ncid = io->ncid;
  xtype = io->xtype;
  R_nc_check (nc_inq_compound(ncid, xtype, NULL, &size, &nfld));
  cnt = R_nc_length (io->ndim, io->xdim);

  /* Set names attribute of R list */
  namelist = R_nc_allocArray (STRSXP, -1, &nfld);
  setAttrib(io->rxp, R_NamesSymbol, namelist);

  /* Convert each field in turn */
  bufcmp = io->cbuf;
  ifldmax = nfld;
  for (ifld=0; ifld<ifldmax; ifld++) {

    /* Save memory "highwater mark" to reclaim memory from R_alloc,
       which may consume large chunks of memory after R_nc_r2c.
     */
    highwater = vmaxget();

    /* Query the dataset for details of the field. */
    R_nc_check (nc_inq_compound_field (ncid, xtype, ifld, namefld,
                  &offset, &typefld, &ndimfld, NULL));
    dimlenfld = (int *) R_alloc (ndimfld, sizeof(int));
    R_nc_check (nc_inq_compound_fielddim_sizes(ncid, xtype, ifld, dimlenfld));
    R_nc_check (nc_inq_type (ncid, typefld, NULL, &fldsize));

    /* Set the field name in the R list */
    SET_STRING_ELT (namelist, ifld, mkChar (namefld));

    /* Append field dimensions to the variable dimensions */
    ndim = io->ndim;
    ndimslice = ndim + ndimfld;
    dimslice = (size_t *) R_alloc (ndimslice, sizeof(size_t));
    for (idim=0; idim<ndim; idim++) {
      dimslice[idim] = io->xdim[idim];
    }
    for (idimfld=0; idimfld<ndimfld; idimfld++) {
      dimslice[ndim+idimfld] = dimlenfld[idimfld];
    }
    fldcnt = R_nc_length (ndimfld, dimslice+ndim);

    /* Prepare to convert field data from C to R */
    buffld = R_nc_c2r_init (&iofld, NULL, ncid, typefld, ndimslice, dimslice,
               io->rawchar, io->fitnum, NULL, NULL, NULL, NULL, NULL);

    /* Copy elements from the compound array into the field array */
    fldlen = fldsize * fldcnt;
    for (ielem=0; ielem<cnt; ielem++) {
      memcpy (buffld+ielem*fldlen, bufcmp+ielem*size+offset, fldlen);
    }

    /* Convert field data from C to R */
    rxpfld = R_nc_c2r (&iofld);

    /* Insert field data into R list */
    SET_VECTOR_ELT (io->rxp, ifld, rxpfld);

    /* Allow memory from R_alloc since vmaxget to be reclaimed */
    vmaxset (highwater);
  }
}


/*=============================================================================*\
 *  Generic type conversions
\*=============================================================================*/

const void *
R_nc_r2c (SEXP rv, int ncid, nc_type xtype, int ndim, const size_t *xdim,
          const void *fill, const double *scale, const double *add)
{
  int class;

  if (xtype > NC_MAX_ATOMIC_TYPE) {
    R_nc_check (nc_inq_user_type (ncid, xtype, NULL, NULL, NULL, NULL, &class));
  }

  switch (TYPEOF(rv)) {
  case INTSXP:
    switch (xtype) {
    case NC_BYTE:
      return R_nc_r2c_int_schar (rv, ndim, xdim, fill, scale, add);
    case NC_UBYTE:
      return R_nc_r2c_int_uchar (rv, ndim, xdim, fill, scale, add);
    case NC_SHORT:
      return R_nc_r2c_int_short (rv, ndim, xdim, fill, scale, add);
    case NC_USHORT:
      return R_nc_r2c_int_ushort (rv, ndim, xdim, fill, scale, add);
    case NC_INT:
      return R_nc_r2c_int_int (rv, ndim, xdim, fill, scale, add);
    case NC_UINT:
      return R_nc_r2c_int_uint (rv, ndim, xdim, fill, scale, add);
    case NC_INT64:
      return R_nc_r2c_int_ll (rv, ndim, xdim, fill, scale, add);
    case NC_UINT64:
      return R_nc_r2c_int_ull (rv, ndim, xdim, fill, scale, add);
    case NC_FLOAT:
      return R_nc_r2c_int_float (rv, ndim, xdim, fill, scale, add);
    case NC_DOUBLE:
      return R_nc_r2c_int_dbl (rv, ndim, xdim, fill, scale, add);
    }
    if (xtype > NC_MAX_ATOMIC_TYPE &&
        class == NC_ENUM &&
        R_nc_inherits (rv, "factor")) {
      return R_nc_factor_enum (rv, ncid, xtype, ndim, xdim, fill);
    }
    break;
  case REALSXP:  
    if (R_nc_inherits (rv, "integer64")) {
      switch (xtype) {
      case NC_BYTE:
	return R_nc_r2c_bit64_schar (rv, ndim, xdim, fill, scale, add);
      case NC_UBYTE:
	return R_nc_r2c_bit64_uchar (rv, ndim, xdim, fill, scale, add);
      case NC_SHORT:
	return R_nc_r2c_bit64_short (rv, ndim, xdim, fill, scale, add);
      case NC_USHORT:
	return R_nc_r2c_bit64_ushort (rv, ndim, xdim, fill, scale, add);
      case NC_INT:
	return R_nc_r2c_bit64_int (rv, ndim, xdim, fill, scale, add);
      case NC_UINT:
	return R_nc_r2c_bit64_uint (rv, ndim, xdim, fill, scale, add);
      case NC_INT64:
	return R_nc_r2c_bit64_ll (rv, ndim, xdim, fill, scale, add);
      case NC_UINT64:
	return R_nc_r2c_bit64_ull (rv, ndim, xdim, fill, scale, add);
      case NC_FLOAT:
	return R_nc_r2c_bit64_float (rv, ndim, xdim, fill, scale, add);
      case NC_DOUBLE:
	return R_nc_r2c_bit64_dbl (rv, ndim, xdim, fill, scale, add);
      }
    } else {
      switch (xtype) {
      case NC_BYTE:
	return R_nc_r2c_dbl_schar (rv, ndim, xdim, fill, scale, add);
      case NC_UBYTE:
	return R_nc_r2c_dbl_uchar (rv, ndim, xdim, fill, scale, add);
      case NC_SHORT:
	return R_nc_r2c_dbl_short (rv, ndim, xdim, fill, scale, add);
      case NC_USHORT:
	return R_nc_r2c_dbl_ushort (rv, ndim, xdim, fill, scale, add);
      case NC_INT:
	return R_nc_r2c_dbl_int (rv, ndim, xdim, fill, scale, add);
      case NC_UINT:
	return R_nc_r2c_dbl_uint (rv, ndim, xdim, fill, scale, add);
      case NC_INT64:
	return R_nc_r2c_dbl_ll (rv, ndim, xdim, fill, scale, add);
      case NC_UINT64:
	return R_nc_r2c_dbl_ull (rv, ndim, xdim, fill, scale, add);
      case NC_FLOAT:
	return R_nc_r2c_dbl_float (rv, ndim, xdim, fill, scale, add);
      case NC_DOUBLE:
	return R_nc_r2c_dbl_dbl (rv, ndim, xdim, fill, scale, add);
      }
    }
    break;
  case STRSXP:
    switch (xtype) {
    case NC_CHAR:
      return R_nc_strsxp_char (rv, ndim, xdim);
    case NC_STRING:
      return R_nc_strsxp_str (rv, ndim, xdim);
    }
    break;
  case RAWSXP:
    if (xtype == NC_CHAR) {
      return R_nc_raw_char (rv, ndim, xdim);
    } else if (xtype > NC_MAX_ATOMIC_TYPE && class == NC_OPAQUE) {
      return R_nc_raw_opaque (rv, ncid, xtype, ndim, xdim);
    }
    break;
  case VECSXP:
    if (xtype > NC_MAX_ATOMIC_TYPE) {
      switch (class) {
      case NC_VLEN:
        return R_nc_vecsxp_vlen (rv, ncid, xtype, ndim, xdim, fill, scale, add);
      case NC_COMPOUND:
        return R_nc_vecsxp_compound (rv, ncid, xtype, ndim, xdim);
      }
    }
    break;
  }
  RERROR (RNC_EDATATYPE);
}

void * \
R_nc_c2r_init (R_nc_buf *io, void *cbuf,
               int ncid, nc_type xtype, int ndim, const size_t *xdim,
               int rawchar, int fitnum,
               const void *fill, const void *min, const void *max,
               const double *scale, const double *add)
{
  int class;
  size_t size;

  if (!io) {
    RERROR ("Pointer to R_nc_buf must not be NULL in R_nc_c2r_init");
  }

  /* Initialise the R_nc_buf, making copies of pointer arguments */
  io->rxp = NULL;
  io->cbuf = cbuf;
  io->rbuf = NULL;
  io->xtype = xtype;
  io->ncid = ncid;
  io->ndim = ndim;
  io->rawchar = rawchar;
  io->fitnum = fitnum;
  io->xdim = NULL;
  io->fill = NULL;
  io->min = NULL;
  io->max = NULL;
  io->scale = NULL;
  io->add = NULL;

  if (xdim) {
    if (ndim > 0) {
      io->xdim = (size_t *) R_alloc (ndim, sizeof(size_t));
      memcpy (io->xdim, xdim, ndim*sizeof(size_t));
    } else if (ndim < 0) {
      /* Special case for vector without dim attribute */
      io->xdim = (size_t *) R_alloc (1, sizeof(size_t));
      memcpy (io->xdim, xdim, sizeof(size_t));
    }
    /* Scalar has no dimensions */
  }

  if (fill || min || max) {
    R_nc_check (nc_inq_type (ncid, xtype, NULL, &size));
  }

  if (fill) {
    io->fill = R_alloc (1, size);
    memcpy (io->fill, fill, size);
  }

  if (min) {
    io->min = R_alloc (1, size);
    memcpy (io->min, min, size);
  }

  if (max) {
    io->max = R_alloc (1, size);
    memcpy (io->max, max, size);
  }

  if (scale) {
    io->scale = (double *) R_alloc (1, sizeof(double));
    *(io->scale) = *scale;
  }

  if (add) {
    io->add = (double *) R_alloc (1, sizeof(double));
    *(io->add) = *add;
  }

  /* Prepare buffers */ 
  switch (xtype) {
    case NC_BYTE:
    case NC_UBYTE:
    case NC_SHORT:
    case NC_USHORT:
    case NC_INT:
      if (fitnum && !scale && !add) {
        R_nc_c2r_int_init (io);
        break;
      }
    case NC_INT64:
    case NC_UINT64:
      if (fitnum && !scale && !add) {
        R_nc_c2r_bit64_init (io);
        classgets(io->rxp, mkString("integer64"));
        break;
      }
    case NC_UINT:
    case NC_FLOAT:
    case NC_DOUBLE:
      R_nc_c2r_dbl_init (io);
      break;
    case NC_CHAR:
      if (rawchar) {
        R_nc_char_raw_init (io);
      } else {
        R_nc_char_strsxp_init (io);
      }
      break;
    case NC_STRING:
      R_nc_str_strsxp_init (io);
      break;
    default:
      if (xtype > NC_MAX_ATOMIC_TYPE) {
        R_nc_check (nc_inq_user_type (ncid, xtype, NULL, NULL, NULL, NULL, &class));
        switch (class) {
        case NC_COMPOUND:
          R_nc_compound_vecsxp_init (io);
          break;
        case NC_ENUM:
          R_nc_enum_factor_init (io);
          break;
        case NC_VLEN:
          R_nc_vlen_vecsxp_init (io);
          break;
        case NC_OPAQUE:
          R_nc_opaque_raw_init (io);
          break;
        default:
          RERROR (RNC_ETYPEDROP);
        }
      } else {
        RERROR (RNC_ETYPEDROP);
      }
  }
  return io->cbuf;
}


SEXP
R_nc_c2r (R_nc_buf *io)
{
  int unpack, class;

  unpack = (io->scale || io->add);

  /* Type conversions */
  switch (io->xtype) {
    case NC_BYTE:
      if (unpack) {
        R_nc_c2r_unpack_schar (io);
      } else if (io->fitnum) {
        R_nc_c2r_schar_int (io);
      } else {
        R_nc_c2r_schar_dbl (io);
      }
      break;
    case NC_UBYTE:
      if (unpack) {
        R_nc_c2r_unpack_uchar (io);
      } else if (io->fitnum) {
        R_nc_c2r_uchar_int (io);
      } else {
        R_nc_c2r_uchar_dbl (io);
      }
      break;
    case NC_SHORT:
      if (unpack) {
        R_nc_c2r_unpack_short (io);
      } else if (io->fitnum) {
        R_nc_c2r_short_int (io);
      } else {
        R_nc_c2r_short_dbl (io);
      }
      break;
    case NC_USHORT:
      if (unpack) {
        R_nc_c2r_unpack_ushort (io);
      } else if (io->fitnum) {
        R_nc_c2r_ushort_int (io);
      } else {
        R_nc_c2r_ushort_dbl (io);
      }
      break;
    case NC_INT:
      if (unpack) {
        R_nc_c2r_unpack_int (io);
      } else if (io->fitnum) {
        R_nc_c2r_int_int (io);
      } else {
        R_nc_c2r_int_dbl (io);
      }
      break;
    case NC_UINT:
      if (unpack) {
        R_nc_c2r_unpack_uint (io);
      } else {
        R_nc_c2r_uint_dbl (io);
      }
      break;
    case NC_FLOAT:
      if (unpack) {
        R_nc_c2r_unpack_float (io);
      } else {
        R_nc_c2r_float_dbl (io);
      }
      break;
    case NC_DOUBLE:
      if (unpack) {
        R_nc_c2r_unpack_dbl (io);
      } else {
        R_nc_c2r_dbl_dbl (io);
      }
      break;
    case NC_INT64:
      if (unpack) {
        R_nc_c2r_unpack_int64 (io);
      } else if (io->fitnum) {
        R_nc_c2r_int64_bit64 (io);
      } else {
        R_nc_c2r_int64_dbl (io);
      }
      break;
    case NC_UINT64:
      if (unpack) {
        R_nc_c2r_unpack_uint64 (io);
      } else if (io->fitnum) {
        R_nc_c2r_uint64_bit64 (io);
      } else {
        R_nc_c2r_uint64_dbl (io);
      }
      break;
    case NC_CHAR:
      if (io->rawchar) {
        R_nc_char_raw (io);
      } else {
        R_nc_char_strsxp (io);
      }
      break;
    case NC_STRING:
      R_nc_str_strsxp (io);
      break;
    default:
      if (io->xtype > NC_MAX_ATOMIC_TYPE) {
        R_nc_check (nc_inq_user_type (
          io->ncid, io->xtype, NULL, NULL, NULL, NULL, &class));
        switch (class) {
        case NC_COMPOUND:
          R_nc_compound_vecsxp (io);
          break;
        case NC_ENUM:
          R_nc_enum_factor (io);
          break;
        case NC_VLEN:
          R_nc_vlen_vecsxp (io);
          break;
        case NC_OPAQUE:
          R_nc_opaque_raw (io);
          break;
        default:
          RERROR (RNC_ETYPEDROP);
        }
      } else {
        RERROR (RNC_ETYPEDROP);
      }
  }
  return io->rxp;
}


/*=============================================================================*\
 *  Dimension conversions
\*=============================================================================*/

/* Reverse a vector in-place.
   Example: R_nc_rev_int (cv, cnt);
 */
#define R_NC_REVERSE(FUN, TYPE) \
void \
FUN (TYPE *data, size_t cnt) \
{ \
  size_t ii, jj; \
  TYPE tmp; \
  if (cnt<=0) return; \
  for (ii=0, jj=cnt-1; ii<jj; ii++, jj--) { \
    tmp = data[ii]; \
    data[ii] = data[jj]; \
    data[jj] = tmp; \
  } \
}

R_NC_REVERSE(R_nc_rev_int, int);
R_NC_REVERSE(R_nc_rev_size, size_t);
/* Define R_nc_rev for other types as needed */


/* Copy the leading N elements of R vector rv into a new C vector of type TYPE,
   reversing from Fortran to C storage order.
   Elements beyond the length of rv and non-finite values are stored as fillval.
 */
#define R_NC_DIM_R2C(FUN, TYPENAME, TYPE) \
TYPE * \
FUN (SEXP rv, size_t N, TYPE fillval) \
{ \
  TYPE *cv; \
  const void *voidbuf; \
  size_t nr, ii; \
\
  /* Allocate new C vector (freed by R) */ \
  cv = (TYPE *) R_alloc (N, sizeof (TYPE)); \
\
  /* Number of elements to copy must not exceed N */ \
  nr = xlength (rv); \
  nr = (nr < N) ? nr : N; \
\
  /* Copy R elements to cv */ \
  if (isReal (rv)) { \
    if (R_nc_inherits (rv, "integer64")) { \
      voidbuf = R_nc_r2c_bit64_##TYPENAME (rv, 1, &nr, &fillval, NULL, NULL); \
    } else { \
      voidbuf = R_nc_r2c_dbl_##TYPENAME (rv, 1, &nr, &fillval, NULL, NULL); \
    } \
  } else if (isInteger (rv)) { \
    voidbuf = R_nc_r2c_int_##TYPENAME (rv, 1, &nr, &fillval, NULL, NULL); \
  } else { \
    RERROR ("Unsupported R type in R_NC_DIM_R2C"); \
  } \
  memcpy (cv, voidbuf, nr*sizeof (TYPE)); \
\
  /* Reverse from Fortran to C order */ \
  R_nc_rev_##TYPENAME (cv, nr); \
\
  /* Fill any remaining elements beyond length of rv */ \
  for ( ii=nr; ii<N; ii++ ) { \
    cv[ii] = fillval; \
  } \
\
  return cv; \
}

R_NC_DIM_R2C (R_nc_dim_r2c_int, int, int);
R_NC_DIM_R2C (R_nc_dim_r2c_size, size, size_t);


