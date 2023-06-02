uint16_t fp16_ieee_from_fp32_value(uint32_t x)
{
	uint32_t x_sgn = x & 0x80000000u;
	uint32_t x_exp = x & 0x7f800000u;

	x_exp = (x_exp < 0x38800000u) ? 0x38800000u : x_exp; // max(e, -14)
	x_exp += 15u << 23; // e += 15
	x &= 0x7fffffffu; // Discard sign

	union { uint32_t u; float f; } f, magic;
	f.u = x;
	magic.u = x_exp;

	// If 15 < e then inf, otherwise e += 2
	f.f = (f.f * 0x1.0p+112f) * 0x1.0p-110f;

	// If we were in the x_exp >= 0x38800000u case:
	// Replace f's exponent with that of x_exp, and discard 13 bits of
	// f's significand, leaving the remaining bits in the low bits of
	// f, relying on FP addition being round-to-nearest-even. If f's
	// significand was previously `a.bcdefghijk`, then it'll become
	// `1.000000000000abcdefghijk`, from which `bcdefghijk` will become
	// the FP16 mantissa, and `a` will add onto the exponent. Note that
	// rounding might add one to all this.
	// If we were in the x_exp < 0x38800000u case:
	// Replace f's exponent with the minimum FP16 exponent, discarding
	// however many bits are required to make that happen, leaving
	// whatever is left in the low bits.
	/* if x_exp >= 0x38800000u, f.f exponent is x_exp - 15 + 2 = x_exp -13, magic.f
	 * exponent is x_exp, in order to add them, shift the mantissa of f.f to the right
	 * by 13 bits, so the exponent of them would both be ex_exp, so the effect is the
	 * same as keep exponent to be x_exp, and then mantissa of f.f is shifted to the
	 * right 13 bits, including the hidden bit, which is shifted to bit10.
	 */

	/* If x_exp < 0x38800000u(113), x_exp = 113 + 15 = 128 = 1000 0000
	 * if original x exponent = 112, for example, then f.f exponent 112 + 2 = 114 - 127 = -13
	 * so adding f.f and magic.f needs to shif f.f mantissa to the right to make
	 * their exponent the same, namely x_exp = 128 - 127 = 1, so f.f needs to shift
	 * 14 bits to make this happen if original `a.bcdefghijk`, after the shifting,
	 * it is 1.0000000000000abcdefghij, hidden bit a is shifted from bit23 to bit9,
	 * k is shifted away. The same logic, if x exponent is 110, the f.f mantissa needs
	 * to be shifted 16 bits. i, j, k are shifted away. If original x exponent is -25(102),
	 * -25 + 2 = -23 --> 1, f.f mantissa needs to be shifted 24 bits, all the original
	 * significand bits are shifted away, leaving only 0, --> leads to 0 in fp16 */
	f.f += magic.f;

	// exp_bits needs to be right shifted 13 bits to shift from bit27 in fp32 to bit14 in fp16, droping 3 exponent bits away
	/* if original exponent of x is 128(1000 0000) for example, x_exp is 128 + 15 (1000 1111),
	 * f.u >> 13, bit27 is to bit14, &0x7c00 takes only low 5 bits of exponent, namely 0 1111
	 * 128 in bias fp32 is 1, which is 1 in fp16, thus biased exponent is 1 + 15 = 16, thus the
	 * exponent should be h_exp + 1 */

	/* When x < 0x38800000u, the exponent bit of f.f is fixed at 1000 0000, so taking
	 * low 5 bits would take only 0s, thus the value is either 0 or subnormal.*/
	uint32_t h_exp = (f.u >> 13) & 0x7c00u; // low 5 bits of exponent
	/* Takes low 12 bits of f.u, meaning that from bit0 to bit11, thus including
	 * bit10, the hidden bit 1, which will then add to exponent bit in
	 * (x_sgn >> 16) + h_exp + h_sig; */

	/* when x < 0x38800000u, the bit10 and bit11 is always 0, thus no 1 is adding to exponent */
	uint32_t h_sig = f.u & 0x0fffu; // low 12 bits (10 are mantissa)

	h_sig = (x > 0x7f800000u) ? 0x0200u : h_sig; // any NaN -> qNaN
	return (x_sgn >> 16) + h_exp + h_sig;
}
