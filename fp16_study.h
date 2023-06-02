/*
The MIT License (MIT)

Copyright (c) 2017 Facebook Inc.
Copyright (c) 2017 Georgia Institute of Technology
Copyright 2019 Google LLC

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#pragma once
#ifndef FP16_FP16_H
#define FP16_FP16_H

#if defined(__cplusplus) && (__cplusplus >= 201103L)
	#include <cstdint>
	#include <cmath>
#elif !defined(__OPENCL_VERSION__)
	#include <stdint.h>
	#include <math.h>
#endif

#ifdef _MSC_VER
	#include <intrin.h>
#endif

#include <fp16/bitcasts.h>


/*
 * Convert a 16-bit floating-point number in IEEE half-precision format, in bit representation, to
 * a 32-bit floating-point number in IEEE single-precision format, in bit representation.
 *
 * @note The implementation doesn't use any floating-point operations.
 */
static inline uint32_t fp16_ieee_to_fp32_bits(uint16_t h) {
	/*
	 * Extend the half-precision floating-point number to 32 bits and shift to the upper part of the 32-bit word:
	 *      +---+-----+------------+-------------------+
	 *      | S |EEEEE|MM MMMM MMMM|0000 0000 0000 0000|
	 *      +---+-----+------------+-------------------+
	 * Bits  31  26-30    16-25            0-15
	 *
	 * S - sign bit, E - bits of the biased exponent, M - bits of the mantissa, 0 - zero bits.
	 */
	const uint32_t w = (uint32_t) h << 16;
	/*
	 * Extract the sign of the input number into the high bit of the 32-bit word:
	 *
	 *      +---+----------------------------------+
	 *      | S |0000000 00000000 00000000 00000000|
	 *      +---+----------------------------------+
	 * Bits  31                 0-31
	 */
	const uint32_t sign = w & UINT32_C(0x80000000);
	/*
	 * Extract mantissa and biased exponent of the input number into the bits 0-30 of the 32-bit word:
	 *
	 *      +---+-----+------------+-------------------+
	 *      | 0 |EEEEE|MM MMMM MMMM|0000 0000 0000 0000|
	 *      +---+-----+------------+-------------------+
	 * Bits  30  27-31     17-26            0-16
	 */
	const uint32_t nonsign = w & UINT32_C(0x7FFFFFFF);
	/*
	 * Renorm shift is the number of bits to shift mantissa left to make the half-precision number normalized.
	 * If the initial number is normalized, some of its high 6 bits (sign == 0 and 5-bit exponent) equals one.
	 * In this case renorm_shift == 0. If the number is denormalize, renorm_shift > 0. Note that if we shift
	 * denormalized nonsign by renorm_shift, the unit bit of mantissa will shift into exponent, turning the
	 * biased exponent into 1, and making mantissa normalized (i.e. without leading 1).
	 */
#ifdef _MSC_VER
	unsigned long nonsign_bsr;
	_BitScanReverse(&nonsign_bsr, (unsigned long) nonsign);
	uint32_t renorm_shift = (uint32_t) nonsign_bsr ^ 31;
#else
	uint32_t renorm_shift = __builtin_clz(nonsign);
#endif
	renorm_shift = renorm_shift > 5 ? renorm_shift - 5 : 0;
	/*
	 * Iff half-precision number has exponent of 15, the addition overflows it into bit 31,
	 * and the subsequent shift turns the high 9 bits into 1. Thus
	 *   inf_nan_mask ==
	 *                   0x7F800000 if the half-precision number had exponent of 15 (i.e. was NaN or infinity)
	 *                   0x00000000 otherwise
	 */
	const int32_t inf_nan_mask = ((int32_t) (nonsign + 0x04000000) >> 8) & INT32_C(0x7F800000);
	/*
	 * Iff nonsign is 0, it overflows into 0xFFFFFFFF, turning bit 31 into 1. Otherwise, bit 31 remains 0.
	 * The signed shift right by 31 broadcasts bit 31 into all bits of the zero_mask. Thus
	 *   zero_mask ==
	 *                0xFFFFFFFF if the half-precision number was zero (+0.0h or -0.0h)
	 *                0x00000000 otherwise
	 */
	const int32_t zero_mask = (int32_t) (nonsign - 1) >> 31;
	/*
	 * 1. Shift nonsign left by renorm_shift to normalize it (if the input was denormal)
	 * 2. Shift nonsign right by 3 so the exponent (5 bits originally) becomes an 8-bit field and 10-bit mantissa
	 *    shifts into the 10 high bits of the 23-bit mantissa of IEEE single-precision number.
	 * 3. Add 0x70 to the exponent (starting at bit 23) to compensate the different in exponent bias
	 *    (0x7F for single-precision number less 0xF for half-precision number).
	 * 4. Subtract renorm_shift from the exponent (starting at bit 23) to account for renormalization. As renorm_shift
	 *    is less than 0x70, this can be combined with step 3.
	 * 5. Binary OR with inf_nan_mask to turn the exponent into 0xFF if the input was NaN or infinity.
	 * 6. Binary ANDNOT with zero_mask to turn the mantissa and exponent into zero if the input was zero.
	 * 7. Combine with the sign of the input number.
	 */
	return sign | ((((nonsign << renorm_shift >> 3) + ((0x70 - renorm_shift) << 23)) | inf_nan_mask) & ~zero_mask);
}

/*
 * Convert a 16-bit floating-point number in IEEE half-precision format, in bit representation, to
 * a 32-bit floating-point number in IEEE single-precision format.
 *
 * @note The implementation relies on IEEE-like (no assumption about rounding mode and no operations on denormals)
 * floating-point operations and bitcasts between integer and floating-point variables.
 */
static inline float fp16_ieee_to_fp32_value(uint16_t h) {
	/*
	 * Extend the half-precision floating-point number to 32 bits and shift to the upper part of the 32-bit word:
	 *      +---+-----+------------+-------------------+
	 *      | S |EEEEE|MM MMMM MMMM|0000 0000 0000 0000|
	 *      +---+-----+------------+-------------------+
	 * Bits  31  26-30    16-25            0-15
	 *
	 * S - sign bit, E - bits of the biased exponent, M - bits of the mantissa, 0 - zero bits.
	 */
	const uint32_t w = (uint32_t) h << 16;
	/*
	 * Extract the sign of the input number into the high bit of the 32-bit word:
	 *
	 *      +---+----------------------------------+
	 *      | S |0000000 00000000 00000000 00000000|
	 *      +---+----------------------------------+
	 * Bits  31                 0-31
	 */
	const uint32_t sign = w & UINT32_C(0x80000000);
	/*
	 * Extract mantissa and biased exponent of the input number into the high bits of the 32-bit word:
	 *
	 *      +-----+------------+---------------------+
	 *      |EEEEE|MM MMMM MMMM|0 0000 0000 0000 0000|
	 *      +-----+------------+---------------------+
	 * Bits  27-31    17-26            0-16
	 */
	const uint32_t two_w = w + w;

	/*
	 * Shift mantissa and exponent into bits 23-28 and bits 13-22 so they become mantissa and exponent
	 * of a single-precision floating-point number:
	 *
	 *       S|Exponent |          Mantissa
	 *      +-+---+-----+------------+----------------+
	 *      |0|000|EEEEE|MM MMMM MMMM|0 0000 0000 0000|
	 *      +-+---+-----+------------+----------------+
	 * Bits   | 23-31   |           0-22
	 *
	 * Next, there are some adjustments to the exponent:
	 * - The exponent needs to be corrected by the difference in exponent bias between single-precision and half-precision
	 *   formats (0x7F - 0xF = 0x70)
	 * - Inf and NaN values in the inputs should become Inf and NaN values after conversion to the single-precision number.
	 *   Therefore, if the biased exponent of the half-precision input was 0x1F (max possible value), the biased exponent
	 *   of the single-precision output must be 0xFF (max possible value). We do this correction in two steps:
	 *   - First, we adjust the exponent by (0xFF - 0x1F) = 0xE0 (see exp_offset below) rather than by 0x70 suggested
	 *     by the difference in the exponent bias (see above).
	 *   - Then we multiply the single-precision result of exponent adjustment by 2**(-112) to reverse the effect of
	 *     exponent adjustment by 0xE0 less the necessary exponent adjustment by 0x70 due to difference in exponent bias.
	 *     The floating-point multiplication hardware would ensure than Inf and NaN would retain their value on at least
	 *     partially IEEE754-compliant implementations.
	 *
	 * Note that the above operations do not handle denormal inputs (where biased exponent == 0). However, they also do not
	 * operate on denormal inputs, and do not produce denormal results.
	 */
	const uint32_t exp_offset = UINT32_C(0xE0) << 23; // Gets 0x38000000
	// exp_scale is needed here because adding exp_offset adds the exponent by 0xE0, namely 224
	// which is for Inf or NaN, but for normally exponents, we only need to adjust 0x70, namly 112,
	// To make the effact of adds only 112, we need to minus 112 from 224 to get the same effect.
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L) || defined(__GNUC__) && !defined(__STRICT_ANSI__)
	const float exp_scale = 0x1.0p-112f;
#else
	const float exp_scale = fp32_from_bits(UINT32_C(0x7800000));
#endif
	// two_w moves the exponent bits to bit 23 and bit 27 in fp32 format
	// Why multiply exp_scale instead of minus??
	const float normalized_value = fp32_from_bits((two_w >> 4) + exp_offset) * exp_scale;

	/*
	 * Convert denormalized half-precision inputs into single-precision results (always normalized).
	 * Zero inputs are also handled here.
	 *
	 * In a denormalized number the biased exponent is zero, and mantissa has on-zero bits.
	 * First, we shift mantissa into bits 0-9 of the 32-bit word.
	 *
	 *                  zeros           |  mantissa
	 *      +---------------------------+------------+
	 *      |0000 0000 0000 0000 0000 00|MM MMMM MMMM|
	 *      +---------------------------+------------+
	 * Bits             10-31                0-9
	 *
	 * Now, remember that denormalized half-precision numbers are represented as:
	 *    FP16 = mantissa * 2**(-24).
	 * The trick is to construct a normalized single-precision number with the same mantissa as the half-precision input
	 * and with an exponent which would scale the corresponding mantissa bits to 2**(-24).
	 * A normalized single-precision floating-point number is represented as:
	 *    FP32 = (1 + mantissa * 2**(-23)) * 2**(exponent - 127)
	 * Therefore, when the biased exponent is 126, a unit change in the mantissa of the input denormalized half-precision
	 * number causes a change of the constructud single-precision number by 2**(-24), i.e. the same ammount.
	 *
	 * The last step is to adjust the bias of the constructed single-precision number. When the input half-precision number
	 * is zero, the constructed single-precision number has the value of
	 *    FP32 = 1 * 2**(126 - 127) = 2**(-1) = 0.5
	 * Therefore, we need to subtract 0.5 from the constructed single-precision number to get the numerical equivalent of
	 * the input half-precision number.
	 */
	const uint32_t magic_mask = UINT32_C(126) << 23;
	const float magic_bias = 0.5f;
	const float denormalized_value = fp32_from_bits((two_w >> 17) | magic_mask) - magic_bias;

	/*
	 * - Choose either results of conversion of input as a normalized number, or as a denormalized number, depending on the
	 *   input exponent. The variable two_w contains input exponent in bits 27-31, therefore if its smaller than 2**27, the
	 *   input is either a denormal number, or zero.
	 * - Combine the result of conversion of exponent and mantissa with the sign of the input number.
	 */
	const uint32_t denormalized_cutoff = UINT32_C(1) << 27;
	const uint32_t result = sign |
		(two_w < denormalized_cutoff ? fp32_to_bits(denormalized_value) : fp32_to_bits(normalized_value));
	return fp32_from_bits(result);
}

/*
 * Convert a 32-bit floating-point number in IEEE single-precision format to a 16-bit floating-point number in
 * IEEE half-precision format, in bit representation.
 *
 * @note The implementation relies on IEEE-like (no assumption about rounding mode and no operations on denormals)
 * floating-point operations and bitcasts between integer and floating-point variables.
 */
static inline uint16_t fp16_ieee_from_fp32_value(float f) {
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L) || defined(__GNUC__) && !defined(__STRICT_ANSI__)
	const float scale_to_inf = 0x1.0p+112f;
	const float scale_to_zero = 0x1.0p-110f;
#else
	const float scale_to_inf = fp32_from_bits(UINT32_C(0x77800000)); //? exponent is 112
	const float scale_to_zero = fp32_from_bits(UINT32_C(0x08800000)); // exponent is -110
#endif
	// If 15 < e then inf, otherwise e += 2
	float base = (fabsf(f) * scale_to_inf) * scale_to_zero; // Sign bit shifted away??

	const uint32_t w = fp32_to_bits(f);
	const uint32_t shl1_w = w + w;// Right shift 1, so the sign bit is dropped, only exponent and mantissa are left
	const uint32_t sign = w & UINT32_C(0x80000000);
	uint32_t bias = shl1_w & UINT32_C(0xFF000000); // Get the exponent bits/value of fp32
	if (bias < UINT32_C(0x71000000)) { // 113 - 127 = -14, if less than -14, then it is -14
		bias = UINT32_C(0x71000000);
	}

	// bias >> 1 move exponent bits to the bit positions it is supposed to be and assume sign to be positive
	// 0x07800000 (0000 0111 1000 000 ...) --> exponent 15 = 15 << 23
	// fp32_from_bits((bias >> 1) + UINT32_C(0x07800000)) --> exponent + 15
	// base exponent + 2
	// To make the exponent the same, namely, exponent + 15, the base needes to be
	// shifted right 13 bits, the result of exponent would be exponent + 15, which
	// is the exponent in fp16
	base = fp32_from_bits((bias >> 1) + UINT32_C(0x07800000)) + base;
	const uint32_t bits = fp32_to_bits(base);
	// exp_bits needs to be right shifted 13 bits to shift from bit27 in fp32 to bit14 in fp16, droping 3 exponent bits away
	// If input exponent is 142 (1000 1110) (142 - 127 = 15), 142 + 15 = 1001 1101, discarding top 3 bits
	// 1 1101 --> 5 bits = 29
	// mantissa_bits takes bit10 also, which should be 1 when input exponent is 142, so 1 1101 + 1 = 1 1110 = 30
	// true exponent for 30 in fp16 is 30 - 15 = 15 which is the same as 142 in fp32
	// So droping top 3 bits is the same as substracting 128, and later adding mantissa_bits adds
	// 1 in exponent, so the whole effect of exponent + 15 - 128 + 1 = exponent - 127 + 15, which
	// is the formular to change exponent biased in fp32 to exponent biased in fp16
	const uint32_t exp_bits = (bits >> 13) & UINT32_C(0x00007C00);// Get the 5-bit exponents 0111 1100
	const uint32_t mantissa_bits = bits & UINT32_C(0x00000FFF); //?? FFF has 12 bits, but fp16 has 10 bits mantissa
	const uint32_t nonsign = exp_bits + mantissa_bits;
	return (sign >> 16) | (shl1_w > UINT32_C(0xFF000000) ? UINT16_C(0x7E00) : nonsign);
}

/*
 * Convert a 16-bit floating-point number in ARM alternative half-precision format, in bit representation, to
 * a 32-bit floating-point number in IEEE single-precision format, in bit representation.
 *
 * @note The implementation doesn't use any floating-point operations.
 */
static inline uint32_t fp16_alt_to_fp32_bits(uint16_t h) {
	/*
	 * Extend the half-precision floating-point number to 32 bits and shift to the upper part of the 32-bit word:
	 *      +---+-----+------------+-------------------+
	 *      | S |EEEEE|MM MMMM MMMM|0000 0000 0000 0000|
	 *      +---+-----+------------+-------------------+
	 * Bits  31  26-30    16-25            0-15
	 *
	 * S - sign bit, E - bits of the biased exponent, M - bits of the mantissa, 0 - zero bits.
	 */
	const uint32_t w = (uint32_t) h << 16;
	/*
	 * Extract the sign of the input number into the high bit of the 32-bit word:
	 *
	 *      +---+----------------------------------+
	 *      | S |0000000 00000000 00000000 00000000|
	 *      +---+----------------------------------+
	 * Bits  31                 0-31
	 */
	const uint32_t sign = w & UINT32_C(0x80000000);
	/*
	 * Extract mantissa and biased exponent of the input number into the bits 0-30 of the 32-bit word:
	 *
	 *      +---+-----+------------+-------------------+
	 *      | 0 |EEEEE|MM MMMM MMMM|0000 0000 0000 0000|
	 *      +---+-----+------------+-------------------+
	 * Bits  30  27-31     17-26            0-16
	 */
	const uint32_t nonsign = w & UINT32_C(0x7FFFFFFF);
	/*
	 * Renorm shift is the number of bits to shift mantissa left to make the half-precision number normalized.
	 * If the initial number is normalized, some of its high 6 bits (sign == 0 and 5-bit exponent) equals one.
	 * In this case renorm_shift == 0. If the number is denormalize, renorm_shift > 0. Note that if we shift
	 * denormalized nonsign by renorm_shift, the unit bit of mantissa will shift into exponent, turning the
	 * biased exponent into 1, and making mantissa normalized (i.e. without leading 1).
	 */
#ifdef _MSC_VER
	unsigned long nonsign_bsr;
	_BitScanReverse(&nonsign_bsr, (unsigned long) nonsign);
	uint32_t renorm_shift = (uint32_t) nonsign_bsr ^ 31;
#else
	uint32_t renorm_shift = __builtin_clz(nonsign);
#endif
	renorm_shift = renorm_shift > 5 ? renorm_shift - 5 : 0;
	/*
	 * Iff nonsign is 0, it overflows into 0xFFFFFFFF, turning bit 31 into 1. Otherwise, bit 31 remains 0.
	 * The signed shift right by 31 broadcasts bit 31 into all bits of the zero_mask. Thus
	 *   zero_mask ==
	 *                0xFFFFFFFF if the half-precision number was zero (+0.0h or -0.0h)
	 *                0x00000000 otherwise
	 */
	const int32_t zero_mask = (int32_t) (nonsign - 1) >> 31;
	/*
	 * 1. Shift nonsign left by renorm_shift to normalize it (if the input was denormal)
	 * 2. Shift nonsign right by 3 so the exponent (5 bits originally) becomes an 8-bit field and 10-bit mantissa
	 *    shifts into the 10 high bits of the 23-bit mantissa of IEEE single-precision number.
	 * 3. Add 0x70 to the exponent (starting at bit 23) to compensate the different in exponent bias
	 *    (0x7F for single-precision number less 0xF for half-precision number).
	 * 4. Subtract renorm_shift from the exponent (starting at bit 23) to account for renormalization. As renorm_shift
	 *    is less than 0x70, this can be combined with step 3.
	 * 5. Binary ANDNOT with zero_mask to turn the mantissa and exponent into zero if the input was zero.
	 * 6. Combine with the sign of the input number.
	 */
	return sign | (((nonsign << renorm_shift >> 3) + ((0x70 - renorm_shift) << 23)) & ~zero_mask);
}

/*
 * Convert a 16-bit floating-point number in ARM alternative half-precision format, in bit representation, to
 * a 32-bit floating-point number in IEEE single-precision format.
 *
 * @note The implementation relies on IEEE-like (no assumption about rounding mode and no operations on denormals)
 * floating-point operations and bitcasts between integer and floating-point variables.
 */
static inline float fp16_alt_to_fp32_value(uint16_t h) {
	/*
	 * Extend the half-precision floating-point number to 32 bits and shift to the upper part of the 32-bit word:
	 *      +---+-----+------------+-------------------+
	 *      | S |EEEEE|MM MMMM MMMM|0000 0000 0000 0000|
	 *      +---+-----+------------+-------------------+
	 * Bits  31  26-30    16-25            0-15
	 *
	 * S - sign bit, E - bits of the biased exponent, M - bits of the mantissa, 0 - zero bits.
	 */
	const uint32_t w = (uint32_t) h << 16;
	/*
	 * Extract the sign of the input number into the high bit of the 32-bit word:
	 *
	 *      +---+----------------------------------+
	 *      | S |0000000 00000000 00000000 00000000|
	 *      +---+----------------------------------+
	 * Bits  31                 0-31
	 */
	const uint32_t sign = w & UINT32_C(0x80000000);
	/*
	 * Extract mantissa and biased exponent of the input number into the high bits of the 32-bit word:
	 *
	 *      +-----+------------+---------------------+
	 *      |EEEEE|MM MMMM MMMM|0 0000 0000 0000 0000|
	 *      +-----+------------+---------------------+
	 * Bits  27-31    17-26            0-16
	 */
	const uint32_t two_w = w + w;

	/*
	 * Shift mantissa and exponent into bits 23-28 and bits 13-22 so they become mantissa and exponent
	 * of a single-precision floating-point number:
	 *
	 *       S|Exponent |          Mantissa
	 *      +-+---+-----+------------+----------------+
	 *      |0|000|EEEEE|MM MMMM MMMM|0 0000 0000 0000|
	 *      +-+---+-----+------------+----------------+
	 * Bits   | 23-31   |           0-22
	 *
	 * Next, the exponent is adjusted for the difference in exponent bias between single-precision and half-precision
	 * formats (0x7F - 0xF = 0x70). This operation never overflows or generates non-finite values, as the largest
	 * half-precision exponent is 0x1F and after the adjustment is can not exceed 0x8F < 0xFE (largest single-precision
	 * exponent for non-finite values).
	 *
	 * Note that this operation does not handle denormal inputs (where biased exponent == 0). However, they also do not
	 * operate on denormal inputs, and do not produce denormal results.
	 */
	const uint32_t exp_offset = UINT32_C(0x70) << 23;
	const float normalized_value = fp32_from_bits((two_w >> 4) + exp_offset);

	/*
	 * Convert denormalized half-precision inputs into single-precision results (always normalized).
	 * Zero inputs are also handled here.
	 *
	 * In a denormalized number the biased exponent is zero, and mantissa has on-zero bits.
	 * First, we shift mantissa into bits 0-9 of the 32-bit word.
	 *
	 *                  zeros           |  mantissa
	 *      +---------------------------+------------+
	 *      |0000 0000 0000 0000 0000 00|MM MMMM MMMM|
	 *      +---------------------------+------------+
	 * Bits             10-31                0-9
	 *
	 * Now, remember that denormalized half-precision numbers are represented as:
	 *    FP16 = mantissa * 2**(-24).
	 * The trick is to construct a normalized single-precision number with the same mantissa and thehalf-precision input
	 * and with an exponent which would scale the corresponding mantissa bits to 2**(-24).
	 * A normalized single-precision floating-point number is represented as:
	 *    FP32 = (1 + mantissa * 2**(-23)) * 2**(exponent - 127)
	 * Therefore, when the biased exponent is 126, a unit change in the mantissa of the input denormalized half-precision
	 * number causes a change of the constructud single-precision number by 2**(-24), i.e. the same ammount.
	 *
	 * The last step is to adjust the bias of the constructed single-precision number. When the input half-precision number
	 * is zero, the constructed single-precision number has the value of
	 *    FP32 = 1 * 2**(126 - 127) = 2**(-1) = 0.5
	 * Therefore, we need to subtract 0.5 from the constructed single-precision number to get the numerical equivalent of
	 * the input half-precision number.
	 */
	const uint32_t magic_mask = UINT32_C(126) << 23;
	const float magic_bias = 0.5f;
	const float denormalized_value = fp32_from_bits((two_w >> 17) | magic_mask) - magic_bias;

	/*
	 * - Choose either results of conversion of input as a normalized number, or as a denormalized number, depending on the
	 *   input exponent. The variable two_w contains input exponent in bits 27-31, therefore if its smaller than 2**27, the
	 *   input is either a denormal number, or zero.
	 * - Combine the result of conversion of exponent and mantissa with the sign of the input number.
	 */
	const uint32_t denormalized_cutoff = UINT32_C(1) << 27;
	const uint32_t result = sign |
		(two_w < denormalized_cutoff ? fp32_to_bits(denormalized_value) : fp32_to_bits(normalized_value));
	return fp32_from_bits(result);
}

/*
 * Convert a 32-bit floating-point number in IEEE single-precision format to a 16-bit floating-point number in
 * ARM alternative half-precision format, in bit representation.
 *
 * @note The implementation relies on IEEE-like (no assumption about rounding mode and no operations on denormals)
 * floating-point operations and bitcasts between integer and floating-point variables.
 */
static inline uint16_t fp16_alt_from_fp32_value(float f) {
	const uint32_t w = fp32_to_bits(f);
	const uint32_t sign = w & UINT32_C(0x80000000);
	const uint32_t shl1_w = w + w;

	const uint32_t shl1_max_fp16_fp32 = UINT32_C(0x8FFFC000);
	const uint32_t shl1_base = shl1_w > shl1_max_fp16_fp32 ? shl1_max_fp16_fp32 : shl1_w;
	uint32_t shl1_bias = shl1_base & UINT32_C(0xFF000000);
	const uint32_t exp_difference = 23 - 10;
	const uint32_t shl1_bias_min = (127 - 1 - exp_difference) << 24;
	if (shl1_bias < shl1_bias_min) {
		shl1_bias = shl1_bias_min;
	}

	const float bias = fp32_from_bits((shl1_bias >> 1) + ((exp_difference + 2) << 23));
	const float base = fp32_from_bits((shl1_base >> 1) + (2 << 23)) + bias;

	const uint32_t exp_f = fp32_to_bits(base) >> 13;
	return (sign >> 16) | ((exp_f & UINT32_C(0x00007C00)) + (fp32_to_bits(base) & UINT32_C(0x00000FFF)));
}

#endif /* FP16_FP16_H */
