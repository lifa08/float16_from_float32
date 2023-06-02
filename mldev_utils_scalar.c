/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2022 Marvell.
 */

#include "mldev_utils_scalar.h"

/* Convert a single precision floating point number (float32) into a half precision
 * floating point number (float16) using round to nearest rounding mode.
 */
static uint16_t
__float32_to_float16_scalar_rtn(float x)
{
	union float32 f32; /* float32 input */
	uint32_t f32_s;	   /* float32 sign */
	uint32_t f32_e;	   /* float32 exponent */
	uint32_t f32_m;	   /* float32 mantissa */
	uint16_t f16_s;	   /* float16 sign */
	uint16_t f16_e;	   /* float16 exponent */
	uint16_t f16_m;	   /* float16 mantissa */
	uint32_t tbits;	   /* number of truncated bits */
	uint32_t tmsb;	   /* MSB position of truncated bits */
	uint32_t m_32;	   /* temporary float32 mantissa */
	uint16_t m_16;	   /* temporary float16 mantissa */
	uint16_t u16;	   /* float16 output */
	int be_16;	   /* float16 biased exponent, signed */

	f32.f = x;
	f32_s = (f32.u & FP32_MASK_S) >> FP32_LSB_S;
	f32_e = (f32.u & FP32_MASK_E) >> FP32_LSB_E;
	f32_m = (f32.u & FP32_MASK_M) >> FP32_LSB_M;

	f16_s = f32_s;
	f16_e = 0;
	f16_m = 0;

	switch (f32_e) {
	case (0): /* float32: zero or subnormal number */
		f16_e = 0;
		f16_m = 0; /* convert to zero */
		break;
	case (FP32_MASK_E >> FP32_LSB_E): /* float32: infinity or nan */
		f16_e = FP16_MASK_E >> FP16_LSB_E;
		if (f32_m == 0) { /* infinity */
			f16_m = 0;
		} else { /* nan, propagate mantissa and set MSB of mantissa to 1 */
			f16_m = f32_m >> (FP32_MSB_M - FP16_MSB_M);
			f16_m |= BIT(FP16_MSB_M);
		}
		break;
	default: /* float32: normal number */
		/* compute biased exponent for float16 */
		be_16 = (int)f32_e - FP32_BIAS_E + FP16_BIAS_E;

		/* overflow, be_16 = [31-INF], set to infinity */
		if (be_16 >= (int)(FP16_MASK_E >> FP16_LSB_E)) {
			f16_e = FP16_MASK_E >> FP16_LSB_E;
			f16_m = 0;
		} else if ((be_16 >= 1) && (be_16 < (int)(FP16_MASK_E >> FP16_LSB_E))) {
			/* normal float16, be_16 = [1:30]*/
			f16_e = be_16;
			m_16 = f32_m >> (FP32_LSB_E - FP16_LSB_E);
			tmsb = FP32_MSB_M - FP16_MSB_M - 1;
			if ((f32_m & GENMASK_U32(tmsb, 0)) > BIT(tmsb)) {
				/* round: non-zero truncated bits except MSB */
				m_16++;

				/* overflow into exponent */
				if (((m_16 & FP16_MASK_E) >> FP16_LSB_E) == 0x1)
					f16_e++;
			} else if ((f32_m & GENMASK_U32(tmsb, 0)) == BIT(tmsb)) {
				/* round: MSB of truncated bits and LSB of m_16 is set */
				if ((m_16 & 0x1) == 0x1) {
					m_16++;

					/* overflow into exponent */
					if (((m_16 & FP16_MASK_E) >> FP16_LSB_E) == 0x1)
						f16_e++;
				}
			}
			f16_m = m_16 & FP16_MASK_M;
		} else if ((be_16 >= -(int)(FP16_MSB_M)) && (be_16 < 1)) {
			/* underflow: zero / subnormal, be_16 = [-9:0] */
			/* True exponent is: [-24, -15]*/
			/* In float32: it is normal number:  1.fraction * 2**e */
			/* e.g e = -15, value = 2^(-15) * (2^0 + 2^(-1) + ....+ 2^(-9) + 2^(-10) + ... + 2^(-23)) */
			/* In float16, subnormal value is calculated as 2^(-14) * 0.fraction */
			/* In order to make them represent the same value, right shift the mantissa bits in float32 */
			/* such that 2^0 becomes 2^(-1) in float16, which is in the MSB of mantissa in float16, whose */
			/* bit position is bit9, which is in bit23(the hidden bit) in float32, so need to right shift 14 bits */
			/* Thus tbits = FP32_LSB_E - FP16_LSB_E - be_16 + 1 = 14 - be_16 */
			/* If e = -16, float32= 2^(-16) * (2^0 + 2^(-1) + ....+ 2^(-9) + 2^(-10) + ... + 2^(-23)) */
			/* So original hidden bit (2^0: bit23) must now become (2^(-2): bit8)in float16, right shift 15 bits */
			f16_e = 0;

			/* add implicit leading zero */
			m_32 = f32_m | BIT(FP32_LSB_E);
			tbits = FP32_LSB_E - FP16_LSB_E - be_16 + 1;
			m_16 = m_32 >> tbits;

			/* if non-leading truncated bits are set */
			if ((f32_m & GENMASK_U32(tbits - 1, 0)) > BIT(tbits - 1)) {
				m_16++;

				/* overflow into exponent */
				/* 0.1111111111 + 0.0000000001 = 1.0000000000, note that bit on the left side of radix bit is hidden bit here*/
				/* 0.1111111111: 1- 2^(-10) */
				/* 1.0000000000: 2^0 = 1 */
				/* Thus float16 value = 2^(-14), which is represented as exponent bits is 1, and mantissa is all 0*/
				if (((m_16 & FP16_MASK_E) >> FP16_LSB_E) == 0x1)
					f16_e++;
			} else if ((f32_m & GENMASK_U32(tbits - 1, 0)) == BIT(tbits - 1)) { /* half way */
				/* if leading truncated bit is set which means half way */
				/* If LSB of fp16 mantissa is 1, adding it becomes 10, if LSB of fp16 is 0 */
				/* adding 1, the LSB becomes 1, ties to even will take the one with even LSB */
				/* Thus, m_16 plus one only if its LSB is 1 */
				if ((m_16 & 0x1) == 0x1) {
					m_16++;

					/* overflow into exponent */
					if (((m_16 & FP16_MASK_E) >> FP16_LSB_E) == 0x1)
						f16_e++;
				}
			}
			f16_m = m_16 & FP16_MASK_M;
		} else if (be_16 == -(int)(FP16_MSB_M + 1)) {
			/* underflow: zero, be_16 = [-10] */
			/* In float32, 2^(-25) * (2^0 + 2^(-1) + ... + 2^(-23))*/
			/* When f16_m = 1, f16_e = 0, 2^(-14) * (2^(-10)) = 2^-24*/
			f16_e = 0;
			if (f32_m != 0) /* If float32 value is between (2^-25, 2^-24 - 2^-48]*/
				f16_m = 1;
			else
				f16_m = 0; /* 0 */
		} else {
			/* underflow: zero, be_16 = [-INF:-11] */
			f16_e = 0;
			f16_m = 0;
		}

		break;
	}

	u16 = FP16_PACK(f16_s, f16_e, f16_m);

	return u16;
}

/* Convert a half precision floating point number (float16) into a single precision
 * floating point number (float32).
 */
static float
__float16_to_float32_scalar_rtx(uint16_t f16)
{
	union float32 f32; /* float32 output */
	uint16_t f16_s;	   /* float16 sign */
	uint16_t f16_e;	   /* float16 exponent */
	uint16_t f16_m;	   /* float16 mantissa */
	uint32_t f32_s;	   /* float32 sign */
	uint32_t f32_e;	   /* float32 exponent */
	uint32_t f32_m;	   /* float32 mantissa*/
	uint8_t shift;	   /* number of bits to be shifted */
	uint32_t clz;	   /* count of leading zeroes */
	int e_16;	   /* float16 exponent unbiased */

	f16_s = (f16 & FP16_MASK_S) >> FP16_LSB_S;
	f16_e = (f16 & FP16_MASK_E) >> FP16_LSB_E;
	f16_m = (f16 & FP16_MASK_M) >> FP16_LSB_M;

	f32_s = f16_s;
	switch (f16_e) {
	case (FP16_MASK_E >> FP16_LSB_E): /* float16: infinity or nan */
		f32_e = FP32_MASK_E >> FP32_LSB_E;
		if (f16_m == 0x0) { /* infinity */
			f32_m = f16_m;
		} else { /* nan, propagate mantissa, set MSB of mantissa to 1 */
			f32_m = f16_m;
			shift = FP32_MSB_M - FP16_MSB_M;
			f32_m = (f32_m << shift) & FP32_MASK_M;
			f32_m |= BIT(FP32_MSB_M);
		}
		break;
	case 0: /* float16: zero or sub-normal */
		f32_m = f16_m;
		if (f16_m == 0) { /* zero signed */
			f32_e = 0;
		} else { /* subnormal numbers */
			/* clz alwys >0, since there are at least 22 leading 0 bits in f16_m, 22 - 32 + 10 = 0*/
			/* so, when MSB of f16 mantissa is 1, clz = 0, when the second least MSB bit of 16 mantissa is 1,
			 * clz = 1 */
			clz = __builtin_clz((uint32_t)f16_m) - sizeof(uint32_t) * 8 + FP16_LSB_E;
			/* f16_e is 0 in this block, thus e_16 = -clz*/
			e_16 = (int)f16_e - clz;
			/* In fp16 subnormal numbers, formula is 2^(-14) * (2 ^(-1) + 2^(-2) + ... + 2^(-10)) */
			/* When mapping to fp32, it is normal, so formular is: 2^(e) * (2^0 + 2^(-1) + ... + 2^(-23))*/
			/* So we need to adjst f32_e bits accordingly */
			/* When only the MSB bit of fp16 mantissa is 1, then the value is 2^(-15)*/
			/* So in fp32, true exponent e = -15, thus biased exponent is -15 + 127 = 112 */
			/* And all mantissa bits should be 0, so bit9 (MSB) should be shifted to the hidden bit of fp32
			 * namely, bit23, which is basically shifted away after &FP32_MASK_M operation, which takes only
			 * the first 23 bits (bit0 to bit22)*/
			/* if fp16_m = 110000000000 (bit9 --> bit0), whose value is 2^(-15) + 2^(-16),
			 * to fp32, then bit9 --> bit23, bit8 --> bit22, after the bit shifting,
			 * the value in fp32 = 2^(-15)+ 2^(-16) (f32_e = -15)*/
			/* f32_t is mainly decided by clz, if clz is 9, value is 2^(-24), true exponent
			 * in fp32 should be -24, namely e_16 - FP16_BIAS_E, thus biased exponent is as follows:*/
			/* In this case, bit0 should be shifted to bit23, thus left shifting 23 bits */
			f32_e = FP32_BIAS_E + e_16 - FP16_BIAS_E;

			shift = clz + (FP32_MSB_M - FP16_MSB_M) + 1 /*For hidden bit*/;
			f32_m = (f32_m << shift) & FP32_MASK_M;
		}
		break;
	default: /* normal numbers */
		f32_m = f16_m;
		e_16 = (int)f16_e;
		f32_e = FP32_BIAS_E + e_16 - FP16_BIAS_E;

		shift = (FP32_MSB_M - FP16_MSB_M);
		f32_m = (f32_m << shift) & FP32_MASK_M;
	}

	f32.u = FP32_PACK(f32_s, f32_e, f32_m);

	return f32.f;
}
