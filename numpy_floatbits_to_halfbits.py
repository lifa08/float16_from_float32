uint16_t numpy_floatbits_to_halfbits(uint32_t f)
{
	uint16_t h_sgn = (uint16_t)((f & 0x80000000u) >> 16);
	uint32_t f_exp = f & 0x7f800000u;
	uint32_t f_sig = f & 0x007fffffu;

	// Exponent overflow/NaN converts to signed inf/NaN
	if (f_exp >= 0x47800000u) { // 0x47800000u = 143 --> 143 - 127 = 16
		if ((f_exp == 0x7f800000u) && (f_sig != 0)) {
			// NaN - propagate the flag in the significand...
			uint16_t ret = (uint16_t)(0x7c00u + (f_sig >> 13));
			// If non-zero bits in f_sig have been shifted away
			ret += (ret == 0x7c00u); // but make sure it stays a NaN,
			return h_sgn + ret;
		} else {
			// (overflow to) signed inf
			return (uint16_t)(h_sgn + 0x7c00u);
		}
	}

	// Exponent underflow converts to a subnormal half or signed zero
	// 0x38000000u: 112 --> 112 -127 = -15
	if (f_exp <= 0x38000000u) { // Means f_exp is less than 112 whose biased exponent is 112 - 127 = -15
		// Signed zeros, subnormal floats, and floats with small
		// exponents all convert to signed zero half-floats.
		if (f_exp < 0x33000000u) { // 0x33000000u: 102 - 127 = -25, exponent less than -25, map to signed 0 in fp16
			return h_sgn;
		}

		// Following is when exponent value is between -25 and -15, normal values in fp32 map to subnormal values
		// Make the subnormal significand
		f_exp >>= 23; // Now exponent bits are at the least significant 8 bits

		f_sig += 0x00800000u; // Preserves mantissa bits and adds the hidden bit to bit23

		// f_exp is in range[102, 112], biased exponent can only be in range [-25, -15], thus
		// 113 - f_exp is in between 11 and 1
		// So f_sig >>= (113 - f_exp) shift to right between 11 and 1 bits, so the mantissa bits in the LSB
		// will be shifted out, and the added hidden bit will be shifted to mantissa bits.
		// Right shifting 11 bits shifted the LSB 11 mantissa bits away
		// e.g f_exp = 0110 1111 == 111 -> biased exponent 111 - 127 = -16. 113 - 111 = 2
		f_sig >>= (113 - f_exp);

		// Handle rounding by adding 1 to the bit beyond half precision
		//
		// If the last bit in the half significand is 0 (already even),
		// and the remaining bit pattern is 1000...0, then we do not add
		// one to the bit after the half significand. However, the
		// (113 - f_exp) shift can lose up to 11 bits, so the || checks
		// them in the original. In all other cases, we can just add one.
		if (((f_sig & 0x3fffu) != 0x1000u) || (f & 0x07ffu)) {
			f_sig += 0x1000u; // Add 1 to bit 12, which is the MSB to be droped from mantiss
		}
		// Since fp16 has 10 bit mantissa, fp32 has 23 bits, so need to shift the last  13 bits away while preserve the 10 bits
		uint16_t h_sig = (uint16_t)(f_sig >> 13);
		// If the rounding causes a bit to spill into h_exp, it will
		// increment h_exp from zero to one and h_sig will be zero.
		// This is the correct result.
		return (uint16_t)(h_sgn + h_sig); // h_exp is all zero -->subnomral
	}

	// Regular case with no overflow or underflow
	// f_exp is between 0x38800000u (113) and 0x47000000u (142) whose biased exponent is from -14 to 15
	// Biased exponent in fp16 = exponent - 127 + 15 = exponent - 112(0x38000000u)
	// exp_bits needs to be right shifted 13 bits from bit27 in fp32 to bit14 in fp16
	// bit 28, bit 29, bit30 of exponent bits are dicarded in fp16
	uint16_t h_exp = (uint16_t)((f_exp - 0x38000000u) >> 13);

	// Handle rounding by adding 1 to the bit beyond half precision
	//
	// If the last bit in the half significand is 0 (already even), and
	// the remaining bit pattern is 1000...0, then we do not add one to
	// the bit after the half significand. In all other cases, we do.
	if ((f_sig & 0x3fffu) != 0x1000u) {
		f_sig += 0x1000u;
	}

	uint16_t h_sig = (uint16_t)(f_sig >> 13); // Shifted 13 extra bits away
	// If the rounding causes a bit to spill into h_exp, it will
	// increment h_exp by one and h_sig will be zero. This is the
	// correct result. h_exp may increment to 15, at greatest, in
	// which case the result overflows to a signed inf.
	return (uint16_t)(h_sgn + h_exp + h_sig);
}
