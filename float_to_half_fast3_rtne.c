uint16_t tursa_floatbits_to_halfbits(uint32_t x)
{
	uint32_t xs = x & 0x80000000u; // Pick off sign bit
	uint32_t xe = x & 0x7f800000u; // Pick off exponent bits
	uint32_t xm = x & 0x007fffffu; // Pick off mantissa bits
	uint16_t hs = (uint16_t)(xs >> 16); // Sign bit

	if (xe == 0) { // Denormal will underflow, return a signed zero
		return hs;
	}

	if (xe == 0x7f800000u) { // Inf or NaN
		if (xm == 0) { // If mantissa is zero ...
			return (uint16_t)(hs | 0x7c00u); // Signed Inf
		} else {
			return (uint16_t)0xfe00u; // NaN, only 1st mantissa bit set
		}
	}
	// Normalized number
	int hes = ((int)(xe >> 23)) - 127 + 15; // Exponent unbias the single, then bias the halfp

	if (hes >= 0x1f) { // Overflow
		return (uint16_t)(hs | 0x7c00u); // Signed Inf
	} else if (hes <= 0) { // Underflow
		uint16_t hm;

		if ((14 - hes) > 24) { // Mantissa shifted all the way off & no rounding possibility
			hm = (uint16_t)0u; // Set mantissa to zero
		} else {
			xm |= 0x00800000u; // Add the hidden leading bit
			hm = (uint16_t)(xm >> (14 - hes)); // Mantissa
			if ((xm >> (13 - hes)) & 1u) { // Check for rounding
				hm += (uint16_t)1u; // Round, might overflow into exp bit, but this is OK
			}
		}
		return (hs | hm); // Combine sign bit and mantissa bits, biased exponent is zero
	} else {
		uint16_t he = (uint16_t)(hes << 10); // Exponent
		uint16_t hm = (uint16_t)(xm >> 13); // Mantissa

		if (xm & 0x1000u) { // Check for rounding
			return (hs | he | hm) + (uint16_t)1u; // Round, might overflow to inf, this is OK
		} else {
			return (hs | he | hm); // No rounding
		}
	}
}

uint16_t float_to_half_fast3_rtne(uint32_t x)
{
	uint32_t x_sgn = x & 0x80000000u;

	// bitwise XOR, removes sign bit from x
	x ^= x_sgn;

	uint16_t o;

	// 0x47800000u --> exponent: 143 --> biased exponent: 143 -127 = 16 map to infinity or NaN in fp16
	// fp16 exponent maximum is 15
	if (x >= 0x47800000u) { // result is Inf or NaN
		// x > 0x7f800000u, e.g. 0x7f800001u, significand != 0 --> NaN
		o = (x > 0x7f800000u) ? 0x7e00  // NaN->qNaN
				      : 0x7c00; // and Inf->Inf
	} else { // (De)normalized number or zero, exponent <= 15
		// 0x38800000u: 113 --> biased exponent 113 -127 = -14
		// e < -14 --> zero (e < -24) or subnormal ([-24, -15])
		if (x < 0x38800000u) { // resulting FP16 is subnormal or zero
			// use a magic value to align our 10 mantissa bits at
			// the bottom of the float. as long as FP addition
			// is round-to-nearest-even this just works.
			union { uint32_t u; float f; } f, denorm_magic;
			f.u = x;
			// demorm_magic.u = 126 << 23 --> biased exponent: 126 - 127 = -1
			denorm_magic.u = ((127 - 14) + (23 - 10)) << 23;

			// How float point addition works: 2**(e) * 1.fraction + 2**(-1) * 1.0
			// Since e < -14, e.g. e = -15
			// 2**(-15) * 1.fraction + 2**(-1) * 1.0 = 2**(-1)(2**(-14) * 1.fraction + 1)
			// = 2**(-1)(0.00000000000001fraction + 1) = 2**(-1) (1.00000000000001fraction)
			// --> the 14 end of fraction bits are shifted away
			// If e = -24, 23 bits will be shifted away --> 2**(-1)(1.00000000000000000000001)
			// So all the significant bits are shifted away
			// So when e [-24, -15], significant bits have a least one non-zero bit
			// if e < -24, even the 1 is shifted away, --> 2**(-1)(1.00000000000000000000000)
			// so the significant bits and the implicit 1 is shifted away, resulting all significant
			// bits being 0
			f.f += denorm_magic.f;

			// and one integer subtract of the bias later, we have our
			// final float!
			// E.g when e = -15, f.f = 2**(-1) (1.00000000000001fraction), the fraction preserves
			// original first 9 significand bits, namely bit 22 to bit 14,
			// since o is 16 bits wide, so the high 16 bits in f.u are dropped, including all
			// exponent bits. So the resulting o has all 0 in its exponent bit because of the shift
			// When e = -15, bit9 of o is the 1 before fraction, making sure significand bits have
			// at least one non-zero bit, thus subnormal
			// e should be at largest -15, if e = -14, only 13 bits shifted away, and the
			// 1 moves to bit 10 of o, which is in exponent bit
			// When e < -24, all significand bits are 0, so the low 16 bits are all 0, meaning
			// resulting fp16 is 0
			o = f.u - denorm_magic.u;
		} else {
			// Take bit 13
			uint32_t mant_odd = (x >> 13) & 1; // resulting mantissa is odd

			// update exponent, rounding bias part 1
			// Note that, now is integer addition
			// 15 - 127 converts exponent from fp biased to fp16 biased
			// e.g e - 127 = true e --> in fp16: e - 127 + 15
			// +0xfff, rounds, bit12 either add 1 from rounding or keep original value
			// if e.g. bit11 is 0, bit12 keeps original value
			x += ((15 - 127) << 23) + 0xfff;
			// rounding bias part 2
			// Adds 0x0000000000000000000000000000001
			// if origin bit 0 is 1, this does not round up
			// if origin bit 0 is 0, adding 0xfff makes bit0 1, now adding another 1 makes it rounds up
			x += mant_odd;
			// take the bits!
			o = x >> 13;
		}
	}

	return (x_sgn >> 16) | o;
}
