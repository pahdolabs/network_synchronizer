
#ifndef NETWORK_SYNCHRONIZER_BIT_ARRAY_H
#define NETWORK_SYNCHRONIZER_BIT_ARRAY_H
#include "bit_array.h"
#include "catch2/catch_test_macros.hpp"

TEST_CASE("Consistent size in bits and bytes", "[netsync][data_type]") {
	constexpr int kTestDataSize = 4;
	constexpr int kBitsPerByte = 8;
	auto bit_array = BitArray(0);
	REQUIRE(bit_array.size_in_bytes() == 0);
	REQUIRE(bit_array.size_in_bits() == 0);
	bit_array.resize_in_bytes(kTestDataSize);
	CHECK(bit_array.size_in_bytes() == kTestDataSize);
	CHECK(bit_array.size_in_bits() == kTestDataSize * kBitsPerByte);

	// Currently `BitArray` makes no distinction between stored bits and reserved
	// bits. The expected value in the final check should change if we decide to
	// make that distinction in the future.
	bit_array.resize_in_bits(bit_array.size_in_bits() + 1);
	CHECK(bit_array.size_in_bytes() == kTestDataSize + 1);
	CHECK(bit_array.size_in_bits() == (kTestDataSize + 1) * kBitsPerByte);
}
#endif // NETWORK_SYNCHRONIZER_BIT_ARRAY_H