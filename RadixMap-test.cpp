/*
 * Copyright 2026 L. Richard Moore Jr.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Opts into RadixMap's test-only structural self-check (see the
// RADIXMAP_ENABLE_INVARIANT_CHECKS comment near the top of RadixMap.hpp) --
// must be defined before RadixMap.hpp is included. Library users never
// define this, so they never pay for it, even in their own debug builds.
#define RADIXMAP_ENABLE_INVARIANT_CHECKS

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include "RadixMap.hpp"

namespace {

constexpr size_t SampleCount = 20000;

std::vector<std::string> makeRandomStrings(size_t count) {
	std::mt19937 generator(1);
	std::uniform_int_distribution<int> length(1, 25);
	std::uniform_int_distribution<int> letter('a', 'z');

	std::vector<std::string> strings;
	strings.reserve(count);
	for(size_t i = 0; i < count; ++i) {
		std::string s(length(generator), '\0');
		for(char &c : s)
			c = static_cast<char>(letter(generator));
		strings.push_back(std::move(s));
	}
	return strings;
}

std::vector<double> makeRandomDoubles(size_t count) {
	std::mt19937 generator(1);
	std::uniform_real_distribution<double> value(-1e6, 1e6);

	std::vector<double> doubles;
	doubles.reserve(count);
	for(size_t i = 0; i < count; ++i)
		doubles.push_back(value(generator));
	return doubles;
}

} // namespace

TEST_CASE( "find on an empty key fails to match" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("hello", "world");
	CHECK_FALSE(tree.contains("nope"));
}

TEST_CASE( "find on a never-inserted-into tree fails to match" ) {
	RadixMap<std::string,std::string> tree;
	CHECK_FALSE(tree.contains("nope"));
}

TEST_CASE( "insert then find round-trips a single key" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("hello", "world");
	CHECK(tree.contains("hello"));
}

TEST_CASE( "find distinguishes keys that share a common prefix" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("http://a", "1");
	tree.insert("http://b", "2");
	CHECK(tree.contains("http://a"));
	CHECK(tree.contains("http://b"));
	CHECK_FALSE(tree.contains("http://c"));
}

TEST_CASE( "find distinguishes a key that is a prefix of another" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("cat", "1");
	tree.insert("catalog", "2");
	CHECK(tree.contains("cat"));
	CHECK(tree.contains("catalog"));
	CHECK_FALSE(tree.contains("cata"));
}

TEST_CASE( "find does not match a structural branch-point that was never itself inserted" ) {
	// Regression test: inserting "apple" then "apply" forces a branch node whose
	// own accumulated subkey is exactly "appl", with no value of its own (it only
	// holds the two branches for 'e' and 'y'). find("appl") must not treat
	// reaching that node structurally as a match.
	RadixMap<std::string,std::string> tree;
	tree.insert("apple", "1");
	tree.insert("apply", "2");
	CHECK(tree.contains("apple"));
	CHECK(tree.contains("apply"));
	CHECK_FALSE(tree.contains("appl"));
}

TEST_CASE( "re-inserting an existing key does not corrupt the tree" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("key", "first");
	tree.insert("key", "second");
	CHECK(tree.contains("key"));
}

TEST_CASE( "many keys sharing branch bytes all remain findable" ) {
	RadixMap<std::string,std::string> tree;
	std::vector<std::string> keys = {
		"apple", "apply", "ape", "banana", "band", "bandana", "bandit", "can", "cane", "cannon"
	};
	for(const std::string &key : keys)
		tree.insert(key, key);
	for(const std::string &key : keys)
		CHECK(tree.contains(key));
	CHECK_FALSE(tree.contains("bandann"));
}

TEST_CASE( "RadixMap<double,...> round-trips positive, negative, and zero keys" ) {
	RadixMap<double,std::string> tree;
	tree.insert(0.0, "");
	tree.insert(1.5, "");
	tree.insert(-1.5, "");
	tree.insert(100.0, "");
	tree.insert(-100.0, "");
	CHECK(tree.contains(0.0));
	CHECK(tree.contains(1.5));
	CHECK(tree.contains(-1.5));
	CHECK(tree.contains(100.0));
	CHECK(tree.contains(-100.0));
	CHECK_FALSE(tree.contains(2.5));
}

TEST_CASE( "RadixMap<double,...> treats positive and negative zero as distinct keys" ) {
	// IEEE-754 0.0 and -0.0 have different bit patterns and encode to different
	// byte strings, so unlike std::map<double,...> (whose default std::less
	// makes them equivalent, i.e. the same key), RadixMap stores them as two
	// separate entries. This documents that intentional divergence rather than
	// asserting it's "correct" -- it falls directly out of encoding by raw bits.
	RadixMap<double,std::string> tree;
	tree.insert(0.0, "poszero");
	tree.insert(-0.0, "negzero");
	CHECK(tree.size() == 2);
	CHECK(tree.at(0.0) == "poszero");
	CHECK(tree.at(-0.0) == "negzero");
}

TEST_CASE( "RadixMap<double,...> orders NaN keys by raw bit pattern, not IEEE-754 comparison semantics" ) {
	// IEEE-754 says NaN is unordered and NaN != NaN, but RadixMap encodes by
	// raw bits (see RadixMapKeyTraits's @note on NaN), so: identical-bit NaNs
	// collide as one key, differing-bit NaNs (sign/signaling-vs-quiet/payload)
	// are distinct keys, and sort order places negative NaNs below -infinity
	// and positive NaNs above +infinity -- matching IEEE-754-2008's
	// totalOrder predicate.
	double qnan = std::numeric_limits<double>::quiet_NaN();
	double qnanSameBits = std::numeric_limits<double>::quiet_NaN();
	double snan = std::numeric_limits<double>::signaling_NaN();
	double negNan = -std::numeric_limits<double>::quiet_NaN();
	double posInf = std::numeric_limits<double>::infinity();
	double negInf = -std::numeric_limits<double>::infinity();

	RadixMap<double,std::string> tree;
	tree.insert(qnan, "qnan-first");
	tree.insert(qnanSameBits, "qnan-second"); // same bits as qnan -- overwrites, not a second key
	CHECK(tree.size() == 1);
	CHECK(tree.at(qnan) == "qnan-second");

	tree.insert(snan, "snan");
	tree.insert(negNan, "negnan");
	tree.insert(posInf, "posinf");
	tree.insert(negInf, "neginf");
	tree.insert(0.0, "zero");
	CHECK(tree.size() == 6);

	std::vector<std::string> visited;
	for(auto &&entry : tree)
		visited.push_back(entry.second);
	CHECK(visited == std::vector<std::string>{"negnan", "neginf", "zero", "posinf", "snan", "qnan-second"});
}

TEST_CASE( "signed integer keys sort correctly across negative, zero, and positive" ) {
	RadixMap<int,std::string> tree;
	tree.insert(3, "");
	tree.insert(-100, "");
	tree.insert(0, "");
	tree.insert(-5, "");

	std::vector<int> visited;
	for(auto it = tree.begin(); it != tree.end(); ++it)
		visited.push_back(it->first);
	CHECK(visited == std::vector<int>{-100, -5, 0, 3});
}

TEST_CASE( "unsigned integer keys sort correctly, including large values that would look negative if signed" ) {
	RadixMap<unsigned int,std::string> tree;
	tree.insert(3000000000u, "");
	tree.insert(5u, "");
	tree.insert(0u, "");

	std::vector<unsigned int> visited;
	for(auto it = tree.begin(); it != tree.end(); ++it)
		visited.push_back(it->first);
	CHECK(visited == std::vector<unsigned int>{0u, 5u, 3000000000u});
}

TEST_CASE( "single-byte integer keys (int8_t) sort correctly" ) {
	RadixMap<int8_t,std::string> tree;
	tree.insert(int8_t{-5}, "");
	tree.insert(int8_t{120}, "");
	tree.insert(int8_t{0}, "");

	std::vector<int8_t> visited;
	for(auto it = tree.begin(); it != tree.end(); ++it)
		visited.push_back(it->first);
	CHECK(visited == std::vector<int8_t>{-5, 0, 120});
}

TEST_CASE( "single-byte integer keys (int8_t) sort correctly across every two's-complement value" ) {
	// Exhaustive rather than sampled: inserts all 256 representable values, so
	// a sign-flip bug at any boundary (INT8_MIN, the -1/0 crossing, INT8_MAX)
	// can't hide between sample points the way a handful of scattered values
	// could.
	RadixMap<int8_t,int> tree;
	for(int v = -128; v <= 127; ++v)
		tree.insert(static_cast<int8_t>(v), v);
	REQUIRE(tree.size() == 256);

	std::vector<int8_t> visited;
	for(auto &&entry : tree)
		visited.push_back(entry.first);
	REQUIRE(std::is_sorted(visited.begin(), visited.end()));
	CHECK(visited.front() == std::numeric_limits<int8_t>::min());
	CHECK(visited.back() == std::numeric_limits<int8_t>::max());
}

TEST_CASE( "two-byte integer keys (int16_t) sort correctly" ) {
	RadixMap<int16_t,std::string> tree;
	tree.insert(int16_t{-500}, "");
	tree.insert(int16_t{3000}, "");
	tree.insert(int16_t{0}, "");

	std::vector<int16_t> visited;
	for(auto it = tree.begin(); it != tree.end(); ++it)
		visited.push_back(it->first);
	CHECK(visited == std::vector<int16_t>{-500, 0, 3000});
}

TEST_CASE( "two-byte integer keys (int16_t) sort correctly across every two's-complement value" ) {
	// Exhaustive, same reasoning as the int8_t version above -- cheap enough
	// (65536 values) to just check them all rather than sample.
	RadixMap<int16_t,int> tree;
	for(int v = -32768; v <= 32767; ++v)
		tree.insert(static_cast<int16_t>(v), v);
	REQUIRE(tree.size() == 65536);

	std::vector<int16_t> visited;
	for(auto &&entry : tree)
		visited.push_back(entry.first);
	REQUIRE(std::is_sorted(visited.begin(), visited.end()));
	CHECK(visited.front() == std::numeric_limits<int16_t>::min());
	CHECK(visited.back() == std::numeric_limits<int16_t>::max());
}

TEST_CASE( "single-byte unsigned integer keys (uint8_t) sort correctly, including large values that would look negative if signed" ) {
	RadixMap<uint8_t,std::string> tree;
	tree.insert(uint8_t{200}, "");
	tree.insert(uint8_t{5}, "");
	tree.insert(uint8_t{0}, "");

	std::vector<uint8_t> visited;
	for(auto it = tree.begin(); it != tree.end(); ++it)
		visited.push_back(it->first);
	CHECK(visited == std::vector<uint8_t>{0, 5, 200});
}

TEST_CASE( "two-byte unsigned integer keys (uint16_t) sort correctly, including large values that would look negative if signed" ) {
	RadixMap<uint16_t,std::string> tree;
	tree.insert(uint16_t{60000}, "");
	tree.insert(uint16_t{5}, "");
	tree.insert(uint16_t{0}, "");

	std::vector<uint16_t> visited;
	for(auto it = tree.begin(); it != tree.end(); ++it)
		visited.push_back(it->first);
	CHECK(visited == std::vector<uint16_t>{0, 5, 60000});
}

TEST_CASE( "four-byte integer keys (int32_t) sort correctly" ) {
	RadixMap<int32_t,std::string> tree;
	tree.insert(int32_t{-2000000000}, "");
	tree.insert(int32_t{2000000000}, "");
	tree.insert(int32_t{0}, "");

	std::vector<int32_t> visited;
	for(auto it = tree.begin(); it != tree.end(); ++it)
		visited.push_back(it->first);
	CHECK(visited == std::vector<int32_t>{-2000000000, 0, 2000000000});
}

TEST_CASE( "four-byte integer keys (int32_t) sort correctly across two's-complement boundaries" ) {
	// Too wide to enumerate exhaustively (unlike int8_t/int16_t above), so
	// this densely samples around the three places a sign-flip bug would
	// actually show up: INT32_MIN, the -1/0 crossing, and INT32_MAX.
	RadixMap<int32_t,std::string> tree;
	std::vector<int32_t> values = {
		std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::min() + 1,
		-2, -1, 0, 1, 2,
		std::numeric_limits<int32_t>::max() - 1, std::numeric_limits<int32_t>::max()
	};
	for(int32_t v : values)
		tree.insert(v, "");

	std::vector<int32_t> visited;
	for(auto &&entry : tree)
		visited.push_back(entry.first);
	std::vector<int32_t> expected = values;
	std::sort(expected.begin(), expected.end());
	CHECK(visited == expected);
}

TEST_CASE( "four-byte unsigned integer keys (uint32_t) sort correctly, including large values that would look negative if signed" ) {
	RadixMap<uint32_t,std::string> tree;
	tree.insert(uint32_t{4000000000u}, "");
	tree.insert(uint32_t{5u}, "");
	tree.insert(uint32_t{0u}, "");

	std::vector<uint32_t> visited;
	for(auto it = tree.begin(); it != tree.end(); ++it)
		visited.push_back(it->first);
	CHECK(visited == std::vector<uint32_t>{0u, 5u, 4000000000u});
}

TEST_CASE( "eight-byte integer keys (int64_t) sort correctly" ) {
	RadixMap<int64_t,std::string> tree;
	tree.insert(int64_t{-9000000000000000000LL}, "");
	tree.insert(int64_t{9000000000000000000LL}, "");
	tree.insert(int64_t{0}, "");

	std::vector<int64_t> visited;
	for(auto it = tree.begin(); it != tree.end(); ++it)
		visited.push_back(it->first);
	CHECK(visited == std::vector<int64_t>{-9000000000000000000LL, 0, 9000000000000000000LL});
}

TEST_CASE( "eight-byte integer keys (int64_t) sort correctly across two's-complement boundaries" ) {
	// Same reasoning as the int32_t version above: densely samples the
	// boundary regions (INT64_MIN, the -1/0 crossing, INT64_MAX) rather than
	// enumerating the whole range.
	RadixMap<int64_t,std::string> tree;
	std::vector<int64_t> values = {
		std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::min() + 1,
		-2, -1, 0, 1, 2,
		std::numeric_limits<int64_t>::max() - 1, std::numeric_limits<int64_t>::max()
	};
	for(int64_t v : values)
		tree.insert(v, "");

	std::vector<int64_t> visited;
	for(auto &&entry : tree)
		visited.push_back(entry.first);
	std::vector<int64_t> expected = values;
	std::sort(expected.begin(), expected.end());
	CHECK(visited == expected);
}

TEST_CASE( "eight-byte unsigned integer keys (uint64_t) sort correctly, including large values that would look negative if signed" ) {
	RadixMap<uint64_t,std::string> tree;
	tree.insert(uint64_t{18000000000000000000ULL}, "");
	tree.insert(uint64_t{5ULL}, "");
	tree.insert(uint64_t{0ULL}, "");

	std::vector<uint64_t> visited;
	for(auto it = tree.begin(); it != tree.end(); ++it)
		visited.push_back(it->first);
	CHECK(visited == std::vector<uint64_t>{0ULL, 5ULL, 18000000000000000000ULL});
}

TEST_CASE( "float keys round-trip and sort correctly across negative, zero, and positive values" ) {
	RadixMap<float,std::string> tree;
	tree.insert(0.0f, "");
	tree.insert(1.5f, "");
	tree.insert(-1.5f, "");
	tree.insert(100.0f, "");
	tree.insert(-100.0f, "");
	CHECK(tree.contains(0.0f));
	CHECK(tree.contains(1.5f));
	CHECK(tree.contains(-1.5f));
	CHECK(tree.contains(100.0f));
	CHECK(tree.contains(-100.0f));
	CHECK_FALSE(tree.contains(2.5f));

	std::vector<float> visited;
	for(auto it = tree.begin(); it != tree.end(); ++it)
		visited.push_back(it->first);
	CHECK(visited == std::vector<float>{-100.0f, -1.5f, 0.0f, 1.5f, 100.0f});
}

TEST_CASE( "float keys treat positive and negative zero as distinct keys" ) {
	// Same intentional divergence from std::map<float,...> as the double case
	// above -- see "RadixMap<double,...> treats positive and negative zero as
	// distinct keys" for the reasoning (falls out of encoding by raw bits).
	RadixMap<float,std::string> tree;
	tree.insert(0.0f, "poszero");
	tree.insert(-0.0f, "negzero");
	CHECK(tree.size() == 2);
	CHECK(tree.at(0.0f) == "poszero");
	CHECK(tree.at(-0.0f) == "negzero");
}

TEST_CASE( "an empty string is a valid key, distinct from never having been inserted" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("", "empty-key-value");
	tree.insert("a", "a-value");
	CHECK(tree.contains(""));
	CHECK(tree.at("") == "empty-key-value");
	CHECK(tree.size() == 2);

	std::vector<std::string> visited;
	for(auto &&entry : tree)
		visited.push_back(entry.first);
	CHECK(visited == std::vector<std::string>{"", "a"});
}

TEST_CASE( "a string key with an embedded null byte round-trips correctly" ) {
	RadixMap<std::string,std::string> tree;
	std::string keyWithNull = std::string("ab") + '\0' + "cd";
	REQUIRE(keyWithNull.size() == 5);

	tree.insert(keyWithNull, "hasNull");
	CHECK(tree.contains(keyWithNull));
	CHECK(tree.at(keyWithNull) == "hasNull");
	CHECK_FALSE(tree.contains("ab"));
}

TEST_CASE( "find returns an iterator to the entry, or end() if not found" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("hello", "world");

	auto found = tree.find("hello");
	REQUIRE(found != tree.end());
	CHECK(found->first == "hello");
	CHECK(found->second == "world");

	CHECK(tree.find("nope") == tree.end());
}

TEST_CASE( "lower_bound/upper_bound on an empty tree return end()" ) {
	RadixMap<std::string,std::string> tree;
	CHECK(tree.lower_bound("anything") == tree.end());
	CHECK(tree.upper_bound("anything") == tree.end());
}

TEST_CASE( "lower_bound finds an exact match; upper_bound skips past it" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("b", "1");

	auto lb = tree.lower_bound("b");
	REQUIRE(lb != tree.end());
	CHECK(lb->first == "b");

	CHECK(tree.upper_bound("b") == tree.end());
}

TEST_CASE( "lower_bound/upper_bound on a key between two entries both land on the next entry" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("apple", "1");
	tree.insert("cherry", "2");

	auto lb = tree.lower_bound("banana");
	REQUIRE(lb != tree.end());
	CHECK(lb->first == "cherry");

	auto ub = tree.upper_bound("banana");
	REQUIRE(ub != tree.end());
	CHECK(ub->first == "cherry");
}

TEST_CASE( "lower_bound/upper_bound past the last entry return end()" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("apple", "1");
	tree.insert("banana", "2");

	CHECK(tree.lower_bound("zebra") == tree.end());
	CHECK(tree.upper_bound("zebra") == tree.end());
}

TEST_CASE( "lower_bound/upper_bound before the first entry both land on begin()" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("apple", "1");
	tree.insert("banana", "2");

	auto lb = tree.lower_bound("aardvark");
	REQUIRE(lb != tree.end());
	CHECK(lb->first == "apple");

	auto ub = tree.upper_bound("aardvark");
	REQUIRE(ub != tree.end());
	CHECK(ub->first == "apple");
}

TEST_CASE( "lower_bound/upper_bound distinguish a key that is a prefix of another, stored key" ) {
	// "cat" and "catalog" both hold values, so lower_bound("cat") must land
	// on "cat" itself while upper_bound("cat") must skip past it to
	// "catalog" -- exercising the exact-length-match branch in boundImpl.
	RadixMap<std::string,std::string> tree;
	tree.insert("cat", "1");
	tree.insert("catalog", "2");

	auto lb = tree.lower_bound("cat");
	REQUIRE(lb != tree.end());
	CHECK(lb->first == "cat");

	auto ub = tree.upper_bound("cat");
	REQUIRE(ub != tree.end());
	CHECK(ub->first == "catalog");
}

TEST_CASE( "lower_bound on a key shorter than any stored key, but a shared prefix, lands on the first extension" ) {
	// "cat" was never inserted (only "catalog" was), so both bounds must
	// skip past the structural branch-point node and land on the first real
	// entry underneath it.
	RadixMap<std::string,std::string> tree;
	tree.insert("catalog", "1");
	tree.insert("catapult", "2");

	auto lb = tree.lower_bound("cat");
	REQUIRE(lb != tree.end());
	CHECK(lb->first == "catalog");

	auto ub = tree.upper_bound("cat");
	REQUIRE(ub != tree.end());
	CHECK(ub->first == "catalog");
}

TEST_CASE( "lower_bound/upper_bound land correctly when the query diverges mid-branch" ) {
	// "banana"/"bandana"/"bandit" all share "ban", then "bandana"/"bandit"
	// further share "band" before diverging at 'a' vs 'i'. A query of
	// "bandage" walks down to the "bandana" leaf's own subkey ("ana", the
	// remainder after "band") and diverges from it mid-subkey ('n' vs 'g',
	// deep inside a single node rather than at a branch point), which sorts
	// "bandage" just before "bandana": banana < bandage < bandana < bandit.
	RadixMap<std::string,std::string> tree;
	tree.insert("banana", "1");
	tree.insert("bandana", "2");
	tree.insert("bandit", "3");

	auto lb = tree.lower_bound("bandage");
	REQUIRE(lb != tree.end());
	CHECK(lb->first == "bandana");
}

TEST_CASE( "lower_bound/upper_bound match std::map across random string keys" ) {
	auto strings = makeRandomStrings(2000);

	RadixMap<std::string,int> tree;
	std::map<std::string,int> reference;
	for(size_t i = 0; i < strings.size(); ++i) {
		tree.insert(strings[i], static_cast<int>(i));
		reference[strings[i]] = static_cast<int>(i);
	}

	// Query with both inserted keys and never-inserted probes so the
	// comparison exercises exact matches, in-between misses, and out-of-range
	// queries alike.
	auto probes = makeRandomStrings(500);
	for(const std::string &probe : {strings.front(), strings.back()})
		probes.push_back(probe);

	for(const std::string &probe : probes) {
		auto refLower = reference.lower_bound(probe);
		auto lower = tree.lower_bound(probe);
		if(refLower == reference.end())
			CHECK(lower == tree.end());
		else {
			REQUIRE(lower != tree.end());
			CHECK(lower->first == refLower->first);
		}

		auto refUpper = reference.upper_bound(probe);
		auto upper = tree.upper_bound(probe);
		if(refUpper == reference.end())
			CHECK(upper == tree.end());
		else {
			REQUIRE(upper != tree.end());
			CHECK(upper->first == refUpper->first);
		}
	}
}

TEST_CASE( "lower_bound/upper_bound match std::map across random double keys" ) {
	auto doubles = makeRandomDoubles(2000);

	RadixMap<double,int> tree;
	std::map<double,int> reference;
	for(size_t i = 0; i < doubles.size(); ++i) {
		tree.insert(doubles[i], static_cast<int>(i));
		reference[doubles[i]] = static_cast<int>(i);
	}

	std::mt19937 generator(2);
	std::uniform_real_distribution<double> probeValue(-1e6, 1e6);
	for(int i = 0; i < 500; ++i) {
		double probe = probeValue(generator);

		auto refLower = reference.lower_bound(probe);
		auto lower = tree.lower_bound(probe);
		if(refLower == reference.end())
			CHECK(lower == tree.end());
		else {
			REQUIRE(lower != tree.end());
			CHECK(lower->first == refLower->first);
		}

		auto refUpper = reference.upper_bound(probe);
		auto upper = tree.upper_bound(probe);
		if(refUpper == reference.end())
			CHECK(upper == tree.end());
		else {
			REQUIRE(upper != tree.end());
			CHECK(upper->first == refUpper->first);
		}
	}
}

TEST_CASE( "prefix_range on an empty tree is empty" ) {
	RadixMap<std::string,std::string> tree;
	auto range = tree.prefix_range("any");
	CHECK(range.begin() == range.end());
}

TEST_CASE( "prefix_range with an empty prefix visits every entry, same as begin()/end()" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("banana", "1");
	tree.insert("apple", "2");
	tree.insert("cherry", "3");

	std::vector<std::string> visited;
	for(auto &&entry : tree.prefix_range(""))
		visited.push_back(entry.first);
	CHECK(visited == std::vector<std::string>{"apple", "banana", "cherry"});
}

TEST_CASE( "prefix_range on a prefix that lands on a valueless structural branch-point visits both children" ) {
	// "apple"/"apply" share "appl", which was never itself inserted -- so this
	// exercises the same branch-point shape as the "find does not match a
	// structural branch-point" regression test above, but for prefix_range()
	// rather than contains().
	RadixMap<std::string,std::string> tree;
	tree.insert("apple", "1");
	tree.insert("apply", "2");
	tree.insert("banana", "3");

	std::vector<std::string> visited;
	for(auto &&entry : tree.prefix_range("appl"))
		visited.push_back(entry.first);
	CHECK(visited == std::vector<std::string>{"apple", "apply"});
}

TEST_CASE( "prefix_range on a prefix that is itself a stored key includes that key first" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("cat", "1");
	tree.insert("catalog", "2");
	tree.insert("catapult", "3");
	tree.insert("dog", "4");

	std::vector<std::string> visited;
	for(auto &&entry : tree.prefix_range("cat"))
		visited.push_back(entry.first);
	CHECK(visited == std::vector<std::string>{"cat", "catalog", "catapult"});
}

TEST_CASE( "prefix_range on a prefix shorter than any stored key still bounds the subtree correctly" ) {
	// "ban"/"band"/"bandana"/"bandit" all share "ban" as a strict prefix, and
	// "banana" ends inside that same node's subkey ("ana") rather than at a
	// branch point -- both must be included, nothing outside "ban*" may leak
	// in from a sibling subtree.
	RadixMap<std::string,std::string> tree;
	std::vector<std::string> keys = {"banana", "band", "bandana", "bandit", "can", "cannon"};
	for(const std::string &key : keys)
		tree.insert(key, key);

	std::vector<std::string> visited;
	for(auto &&entry : tree.prefix_range("ban"))
		visited.push_back(entry.first);
	CHECK(visited == std::vector<std::string>{"banana", "band", "bandana", "bandit"});
}

TEST_CASE( "prefix_range excludes a sibling subtree that only shares a shorter prefix" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("apple", "1");
	tree.insert("apply", "2");
	tree.insert("banana", "3");

	std::vector<std::string> visited;
	for(auto &&entry : tree.prefix_range("app"))
		visited.push_back(entry.first);
	CHECK(visited == std::vector<std::string>{"apple", "apply"});
}

TEST_CASE( "prefix_range diverging mid-subkey returns empty" ) {
	// "appl" is a real node (branch-point for apple/apply), but "apz" diverges
	// from it inside the subkey itself ('p' vs 'z' at the 3rd byte), not at a
	// branch -- must return empty, not fall through to some unrelated subtree.
	RadixMap<std::string,std::string> tree;
	tree.insert("apple", "1");
	tree.insert("apply", "2");

	auto range = tree.prefix_range("apz");
	CHECK(range.begin() == range.end());
}

TEST_CASE( "prefix_range longer than any matching key returns empty" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("apple", "1");
	tree.insert("apply", "2");

	auto range = tree.prefix_range("apples");
	CHECK(range.begin() == range.end());
}

TEST_CASE( "prefix_range on a prefix matching no key at all returns empty" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("apple", "1");

	auto range = tree.prefix_range("zzz");
	CHECK(range.begin() == range.end());
}

TEST_CASE( "prefix_range skips an erased key within the range but keeps others" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("apple", "1");
	tree.insert("apply", "2");
	tree.insert("banana", "3");

	tree.erase("apple");

	std::vector<std::string> visited;
	for(auto &&entry : tree.prefix_range("app"))
		visited.push_back(entry.first);
	CHECK(visited == std::vector<std::string>{"apply"});
}

TEST_CASE( "prefix_range mutating a value through the range is visible via at()" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("apple", "1");
	tree.insert("apply", "2");

	for(auto &&entry : tree.prefix_range("app"))
		entry.second += "-touched";

	CHECK(tree.at("apple") == "1-touched");
	CHECK(tree.at("apply") == "2-touched");
}

TEST_CASE( "const_iterator: prefix_range works through a const reference" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("apple", "1");
	tree.insert("apply", "2");
	tree.insert("banana", "3");

	const RadixMap<std::string,std::string> &constTree = tree;

	std::vector<std::string> visited;
	for(auto &&entry : constTree.prefix_range("app"))
		visited.push_back(entry.first);
	CHECK(visited == std::vector<std::string>{"apple", "apply"});
}

TEST_CASE( "prefix_range matches a linear starts_with scan across random string keys and prefixes" ) {
	auto strings = makeRandomStrings(2000);

	RadixMap<std::string,int> tree;
	for(size_t i = 0; i < strings.size(); ++i)
		tree.insert(strings[i], static_cast<int>(i));

	// Prefixes drawn from real keys (truncated to a random length, so they're
	// guaranteed to match at least one key) plus fresh random strings (which
	// usually won't match anything) -- exercises both the common and the
	// empty-range path.
	std::mt19937 generator(3);
	std::uniform_int_distribution<size_t> keyPick(0, strings.size() - 1);
	std::vector<std::string> prefixes = makeRandomStrings(300);
	for(int i = 0; i < 300; ++i) {
		const std::string &source = strings[keyPick(generator)];
		std::uniform_int_distribution<size_t> lengthPick(0, source.size());
		prefixes.push_back(source.substr(0, lengthPick(generator)));
	}

	for(const std::string &prefix : prefixes) {
		std::vector<std::string> expected;
		for(const std::string &key : strings)
			if(key.starts_with(prefix))
				expected.push_back(key);
		std::sort(expected.begin(), expected.end());
		expected.erase(std::unique(expected.begin(), expected.end()), expected.end());

		std::vector<std::string> actual;
		for(auto &&entry : tree.prefix_range(prefix))
			actual.push_back(entry.first);

		CHECK(actual == expected);
	}
}

TEST_CASE( "at returns the value or throws std::out_of_range" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("hello", "world");
	CHECK(tree.at("hello") == "world");
	CHECK_THROWS_AS(tree.at("nope"), std::out_of_range);

	RadixMap<std::string,std::string> empty;
	CHECK_THROWS_AS(empty.at("anything"), std::out_of_range);
}

TEST_CASE( "operator[] default-inserts a missing key and does not overwrite an existing one" ) {
	RadixMap<std::string,std::string> tree;
	tree["hello"] = "world";
	CHECK(tree.at("hello") == "world");
	CHECK(tree.size() == 1);

	// Reading a missing key through operator[] default-constructs it.
	CHECK(tree["missing"] == "");
	CHECK(tree.contains("missing"));
	CHECK(tree.size() == 2);

	// Accessing an existing key through operator[] leaves its value untouched.
	CHECK(tree["hello"] == "world");
}

TEST_CASE( "size and empty track insert count, including overwrites" ) {
	RadixMap<std::string,std::string> tree;
	CHECK(tree.empty());
	CHECK(tree.size() == 0);

	tree.insert("a", "1");
	tree.insert("b", "2");
	CHECK(tree.size() == 2);
	CHECK_FALSE(tree.empty());

	// Overwriting an existing key does not change size.
	tree.insert("a", "overwritten");
	CHECK(tree.size() == 2);
	CHECK(tree.at("a") == "overwritten");
}

TEST_CASE( "begin() == end() on an empty tree" ) {
	RadixMap<std::string,std::string> tree;
	CHECK(tree.begin() == tree.end());
}

TEST_CASE( "iteration over double keys visits every entry in ascending order" ) {
	RadixMap<double,std::string> tree;
	std::vector<double> values = {5.0, -3.0, 0.0, -1.0, 10.0, 1.0};
	for(double value : values)
		tree.insert(value, "");

	std::vector<double> visited;
	for(auto it = tree.begin(); it != tree.end(); ++it)
		visited.push_back(it->first);

	std::vector<double> sortedValues = values;
	std::sort(sortedValues.begin(), sortedValues.end());
	CHECK(visited == sortedValues);
}

TEST_CASE( "range-for iteration over string keys visits every entry in ascending order" ) {
	RadixMap<std::string,std::string> tree;
	std::vector<std::string> keys = {"banana", "apple", "cherry", "apply", "band"};
	for(const std::string &key : keys)
		tree.insert(key, key);

	std::vector<std::string> visited;
	for(const auto &entry : tree)
		visited.push_back(entry.first);

	std::vector<std::string> sortedKeys = keys;
	std::sort(sortedKeys.begin(), sortedKeys.end());
	CHECK(visited == sortedKeys);
}

TEST_CASE( "a branch byte landing in an empty slot outside the current mask's range does not break sort order" ) {
	// Regression test for a bug caught by lower_bound/upper_bound fuzzing
	// against std::map: 'a' (0x61), 'e' (0x65), 'l' (0x6C) only differ in
	// their low nibble, so after three inserts the root's branch mask
	// narrowed to 0xF (low nibble only). 't' (0x74) then arrived: its low
	// nibble (0x4) mapped to an empty slot, so insert() claimed it directly
	// without ever checking that 't's high nibble (0x7) put it outside the
	// range the mask had been derived from -- landing it between 'a' and 'e'
	// in the branch array when it actually sorts after all three. Nothing
	// afterward recomputes the mask unless a later key collides with an
	// already-occupied slot, so the corruption was permanent and silent:
	// plain forward iteration (not just lower_bound/upper_bound) visited
	// entries out of order.
	RadixMap<std::string,int> tree;
	std::vector<std::string> keys = {"lmijlf", "aqbmhngzsufsulko", "exxjrxawnjjhwzbariy", "tpkzihdgvrzdey"};
	for(size_t i = 0; i < keys.size(); ++i)
		tree.insert(keys[i], static_cast<int>(i));

	std::vector<std::string> visited;
	for(auto &&entry : tree)
		visited.push_back(entry.first);

	std::vector<std::string> sortedKeys = keys;
	std::sort(sortedKeys.begin(), sortedKeys.end());
	CHECK(visited == sortedKeys);
}

TEST_CASE( "forward iteration over many random string keys stays in ascending order, matching std::map" ) {
	auto strings = makeRandomStrings(5000);

	RadixMap<std::string,int> tree;
	std::map<std::string,int> reference;
	for(size_t i = 0; i < strings.size(); ++i) {
		tree.insert(strings[i], static_cast<int>(i));
		reference[strings[i]] = static_cast<int>(i);
	}

	std::vector<std::string> visited;
	for(auto &&entry : tree)
		visited.push_back(entry.first);

	std::vector<std::string> referenceOrder;
	for(auto &entry : reference)
		referenceOrder.push_back(entry.first);

	REQUIRE(std::is_sorted(visited.begin(), visited.end()));
	CHECK(visited == referenceOrder);
}

TEST_CASE( "forward iteration over many random double keys stays in ascending order, matching std::map" ) {
	auto doubles = makeRandomDoubles(5000);

	RadixMap<double,int> tree;
	std::map<double,int> reference;
	for(size_t i = 0; i < doubles.size(); ++i) {
		tree.insert(doubles[i], static_cast<int>(i));
		reference[doubles[i]] = static_cast<int>(i);
	}

	std::vector<double> visited;
	for(auto &&entry : tree)
		visited.push_back(entry.first);

	std::vector<double> referenceOrder;
	for(auto &entry : reference)
		referenceOrder.push_back(entry.first);

	REQUIRE(std::is_sorted(visited.begin(), visited.end()));
	CHECK(visited == referenceOrder);
}

TEST_CASE( "iteration handles a maximum-depth tree for an 8-byte fixed-length key" ) {
	// Guards the iterator's traversal-buffer capacity bound for fixed-length
	// keys. For an 8-byte key the deepest possible root-to-leaf path is 9
	// nodes (root + one node per key byte), which is exactly the capacity the
	// iterator reserves. This constructs that worst case on purpose: the keys
	// 0 and 1<<(8*i) for i in [0,8) branch at a different byte position each,
	// building an 8-deep spine down to key 0 (a 9th node). If the capacity
	// bound were even one too small, a heap-free BoundedVector traversal would
	// throw std::length_error here rather than silently pass. Uses uint64_t so
	// the encoded bytes are directly controllable (big-endian, no float
	// transform).
	RadixMap<uint64_t,int> tree;
	std::map<uint64_t,int> reference;
	tree.insert(0, 0);
	reference[0] = 0;
	for(int i = 0; i < 8; ++i) {
		uint64_t key = uint64_t{1} << (8 * i);
		tree.insert(key, i + 1);
		reference[key] = i + 1;
	}

	std::vector<uint64_t> visited;
	for(auto &&entry : tree)
		visited.push_back(entry.first);

	std::vector<uint64_t> referenceOrder;
	for(auto &entry : reference)
		referenceOrder.push_back(entry.first);

	REQUIRE(std::is_sorted(visited.begin(), visited.end()));
	CHECK(visited == referenceOrder);

	// Also exercise find()/lower_bound() landing on the deepest leaf (key 0),
	// which reconstructs a full-depth traversal stack.
	auto found = tree.find(0);
	REQUIRE(found != tree.end());
	CHECK(found->first == 0);
	auto lb = tree.lower_bound(0);
	REQUIRE(lb != tree.end());
	CHECK(lb->first == 0);
}

TEST_CASE( "checkInvariants() holds after every insert, across many random string keys" ) {
	// Calls checkInvariants() after each insert rather than just once at the
	// end, so a corrupting insert (like the empty-slot mask bug it exists to
	// catch) is caught at the exact call that caused it.
	auto strings = makeRandomStrings(1500);

	RadixMap<std::string,int> tree;
	for(size_t i = 0; i < strings.size(); ++i) {
		tree.insert(strings[i], static_cast<int>(i));
		tree.checkInvariants();
	}
}

TEST_CASE( "checkInvariants() holds after every insert, across many random double keys" ) {
	auto doubles = makeRandomDoubles(1500);

	RadixMap<double,int> tree;
	for(size_t i = 0; i < doubles.size(); ++i) {
		tree.insert(doubles[i], static_cast<int>(i));
		tree.checkInvariants();
	}
}

TEST_CASE( "checkInvariants() holds regardless of insertion order" ) {
	// RadixMap's shape (and so the invariants checkInvariants() verifies) is
	// insertion-order-dependent -- the empty-slot mask bug this checker
	// exists to catch only manifested for one specific arrival order of its
	// four keys. Re-insert the same key set under several random shuffles to
	// cover more of that order-dependent space than any one fixed dataset can.
	auto strings = makeRandomStrings(400);

	std::mt19937 shuffler(7);
	for(int trial = 0; trial < 20; ++trial) {
		std::shuffle(strings.begin(), strings.end(), shuffler);

		RadixMap<std::string,int> tree;
		for(size_t i = 0; i < strings.size(); ++i) {
			tree.insert(strings[i], static_cast<int>(i));
			tree.checkInvariants();
		}
	}
}

TEST_CASE( "mutating a value through an iterator is visible via at()" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("hello", "world");

	auto it = tree.find("hello");
	REQUIRE(it != tree.end());
	it->second = "there";

	CHECK(tree.at("hello") == "there");
}

TEST_CASE( "incrementing an iterator from find() resumes correctly" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("apple", "1");
	tree.insert("apply", "2");
	tree.insert("banana", "3");

	auto it = tree.find("apple");
	REQUIRE(it != tree.end());
	CHECK(it->first == "apple");
	++it;
	REQUIRE(it != tree.end());
	CHECK(it->first == "apply");
	++it;
	REQUIRE(it != tree.end());
	CHECK(it->first == "banana");
	++it;
	CHECK(it == tree.end());
}

TEST_CASE( "postfix increment returns the pre-increment position" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("a", "1");
	tree.insert("b", "2");

	auto it = tree.begin();
	auto previous = it++;
	CHECK(previous->first == "a");
	CHECK(it->first == "b");
}

TEST_CASE( "copied iterators advance independently (multipass guarantee)" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("a", "1");
	tree.insert("b", "2");
	tree.insert("c", "3");

	auto it1 = tree.begin();
	auto it2 = it1;
	++it1;
	CHECK(it1->first == "b");
	CHECK(it2->first == "a");
	++it2;
	CHECK(it2 == it1);
}

TEST_CASE( "const_iterator: cbegin/cend and const find() work through a const reference" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("a", "1");
	tree.insert("b", "2");

	const RadixMap<std::string,std::string> &constTree = tree;

	std::vector<std::string> visited;
	for(auto it = constTree.cbegin(); it != constTree.cend(); ++it)
		visited.push_back(it->first);
	CHECK(visited == std::vector<std::string>{"a", "b"});

	auto found = constTree.find("a");
	REQUIRE(found != constTree.end());
	CHECK(found->second == "1");
	CHECK(constTree.find("nope") == constTree.end());
}

TEST_CASE( "erase removes an existing key and updates size" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("apple", "1");
	tree.insert("apply", "2");

	CHECK(tree.erase("apple") == 1);
	CHECK_FALSE(tree.contains("apple"));
	CHECK(tree.contains("apply"));
	CHECK(tree.size() == 1);
}

TEST_CASE( "erase on a missing key returns 0 and does not change size" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("apple", "1");

	CHECK(tree.erase("nope") == 0);
	CHECK(tree.size() == 1);
}

TEST_CASE( "erase on a never-inserted-into (empty) tree returns 0" ) {
	RadixMap<std::string,std::string> tree;
	CHECK(tree.erase("nope") == 0);
	CHECK(tree.size() == 0);
}

TEST_CASE( "erase on a key that is itself a prefix of a remaining key" ) {
	// "cat" is a branch-point node with both a value of its own and a child
	// branch for "catalog" -- erasing it must clear only its own value and
	// leave "catalog" (and the tree structure connecting to it) intact.
	RadixMap<std::string,std::string> tree;
	tree.insert("cat", "1");
	tree.insert("catalog", "2");

	CHECK(tree.erase("cat") == 1);
	CHECK_FALSE(tree.contains("cat"));
	CHECK(tree.contains("catalog"));
	CHECK(tree.at("catalog") == "2");
	CHECK(tree.size() == 1);

	std::vector<std::string> visited;
	for(auto &&entry : tree)
		visited.push_back(entry.first);
	CHECK(visited == std::vector<std::string>{"catalog"});
}

TEST_CASE( "erase then re-insert the same key does not corrupt the tree" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("apple", "1");
	tree.insert("apply", "2");

	REQUIRE(tree.erase("apple") == 1);
	tree.insert("apple", "3");
	CHECK(tree.at("apple") == "3");
	CHECK(tree.at("apply") == "2");
	CHECK(tree.size() == 2);
}

TEST_CASE( "iteration skips an erased key" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("apple", "1");
	tree.insert("apply", "2");
	tree.insert("banana", "3");

	tree.erase("apply");

	std::vector<std::string> visited;
	for(const auto &entry : tree)
		visited.push_back(entry.first);
	CHECK(visited == std::vector<std::string>{"apple", "banana"});
}

TEST_CASE( "copying a RadixMap deep-clones it, so mutating the copy leaves the original untouched" ) {
	RadixMap<std::string,std::string> original;
	original.insert("apple", "1");
	original.insert("apply", "2");

	RadixMap<std::string,std::string> copy = original;
	copy.insert("apple", "mutated");
	copy.insert("banana", "3");

	CHECK(original.at("apple") == "1");
	CHECK(original.size() == 2);
	CHECK_FALSE(original.contains("banana"));

	CHECK(copy.at("apple") == "mutated");
	CHECK(copy.size() == 3);

	// Copy assignment is likewise a deep clone.
	RadixMap<std::string,std::string> assigned;
	assigned.insert("preexisting", "x");
	assigned = original;
	assigned.insert("apple", "assigned-mutation");
	CHECK(original.at("apple") == "1");
	CHECK(assigned.at("apple") == "assigned-mutation");
	CHECK_FALSE(assigned.contains("preexisting"));
}

TEST_CASE( "self copy-assignment leaves the tree intact" ) {
	RadixMap<std::string,std::string> tree;
	tree.insert("apple", "1");
	tree.insert("apply", "2");

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-assign-overloaded"
#endif
	tree = tree;
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

	CHECK(tree.size() == 2);
	CHECK(tree.at("apple") == "1");
	CHECK(tree.at("apply") == "2");
}

TEST_CASE( "moving a RadixMap transfers its contents" ) {
	RadixMap<std::string,std::string> original;
	original.insert("apple", "1");
	original.insert("apply", "2");

	RadixMap<std::string,std::string> moved = std::move(original);
	CHECK(moved.at("apple") == "1");
	CHECK(moved.at("apply") == "2");
	CHECK(moved.size() == 2);

	RadixMap<std::string,std::string> moveAssigned;
	moveAssigned.insert("preexisting", "x");
	moveAssigned = std::move(moved);
	CHECK(moveAssigned.at("apple") == "1");
	CHECK(moveAssigned.size() == 2);
}

TEST_CASE( "RadixMap vs std::map vs std::unordered_map: string key insert performance", "[.][benchmark]" ) {
	const std::vector<std::string> keys = makeRandomStrings(SampleCount);

	BENCHMARK("RadixMap insert") {
		RadixMap<std::string,std::string> tree;
		for(const std::string &key : keys)
			tree.insert(key, "");
		return tree.find(keys.back());
	};

	BENCHMARK("std::unordered_map insert") {
		std::unordered_map<std::string, std::string> map;
		for(const std::string &key : keys)
			map[key] = "";
		return map.find(keys.back()) != map.end();
	};

	BENCHMARK("std::map insert") {
		std::map<std::string, std::string> map;
		for(const std::string &key : keys)
			map[key] = "";
		return map.find(keys.back()) != map.end();
	};
}

TEST_CASE( "RadixMap vs std::map vs std::unordered_map: string key find performance", "[.][benchmark]" ) {
	const std::vector<std::string> keys = makeRandomStrings(SampleCount);

	RadixMap<std::string,std::string> radixMap;
	std::unordered_map<std::string, std::string> unorderedMap;
	std::map<std::string, std::string> orderedMap;
	for(const std::string &key : keys) {
		radixMap.insert(key, "");
		unorderedMap[key] = "";
		orderedMap[key] = "";
	}

	BENCHMARK("RadixMap find") {
		size_t found = 0;
		for(const std::string &key : keys)
			found += radixMap.contains(key);
		return found;
	};

	BENCHMARK("std::unordered_map find") {
		size_t found = 0;
		for(const std::string &key : keys)
			found += unorderedMap.find(key) != unorderedMap.end();
		return found;
	};

	BENCHMARK("std::map find") {
		size_t found = 0;
		for(const std::string &key : keys)
			found += orderedMap.find(key) != orderedMap.end();
		return found;
	};
}

TEST_CASE( "RadixMap<double,...> vs std::map vs std::unordered_map: double key insert performance", "[.][benchmark]" ) {
	const std::vector<double> keys = makeRandomDoubles(SampleCount);

	BENCHMARK("RadixMap<double,...> insert") {
		RadixMap<double,std::string> tree;
		for(double key : keys)
			tree.insert(key, "");
		return tree.find(keys.back());
	};

	BENCHMARK("std::unordered_map insert") {
		std::unordered_map<double, std::string> map;
		for(double key : keys)
			map[key] = "";
		return map.find(keys.back()) != map.end();
	};

	BENCHMARK("std::map insert") {
		std::map<double, std::string> map;
		for(double key : keys)
			map[key] = "";
		return map.find(keys.back()) != map.end();
	};
}

TEST_CASE( "RadixMap<double,...> vs std::map vs std::unordered_map: double key find performance", "[.][benchmark]" ) {
	const std::vector<double> keys = makeRandomDoubles(SampleCount);

	RadixMap<double,std::string> radixMap;
	std::unordered_map<double, std::string> unorderedMap;
	std::map<double, std::string> orderedMap;
	for(double key : keys) {
		radixMap.insert(key, "");
		unorderedMap[key] = "";
		orderedMap[key] = "";
	}

	BENCHMARK("RadixMap<double,...> find") {
		size_t found = 0;
		for(double key : keys)
			found += radixMap.contains(key);
		return found;
	};

	BENCHMARK("std::unordered_map find") {
		size_t found = 0;
		for(double key : keys)
			found += unorderedMap.find(key) != unorderedMap.end();
		return found;
	};

	BENCHMARK("std::map find") {
		size_t found = 0;
		for(double key : keys)
			found += orderedMap.find(key) != orderedMap.end();
		return found;
	};
}

TEST_CASE( "RadixMap::find()/lower_bound() string vs double keys, with BoundedVector vendored", "[.][benchmark]" ) {
	// contains()/at()/operator[] (benchmarked above) go through locate(), which
	// never allocates regardless of Key -- they don't exercise the
	// BoundedVector-backed iterator at all. find()/lower_bound()/upper_bound()
	// do: they build a resumable IteratorImpl, whose traversal-state buffers are
	// heap-free BoundedVector for a fixed-length Key (e.g. double) and
	// std::vector for a variable-length one (e.g. std::string) -- see
	// RADIXMAP_HAVE_BOUNDED_VECTOR near the top of RadixMap.hpp. This benchmark
	// isolates that difference directly, string vs double, side by side.
	const std::vector<std::string> stringKeys = makeRandomStrings(SampleCount);
	const std::vector<double> doubleKeys = makeRandomDoubles(SampleCount);

	RadixMap<std::string,std::string> stringMap;
	for(const std::string &key : stringKeys)
		stringMap.insert(key, "");

	RadixMap<double,std::string> doubleMap;
	for(double key : doubleKeys)
		doubleMap.insert(key, "");

	BENCHMARK("RadixMap<string> find()") {
		size_t found = 0;
		for(const std::string &key : stringKeys)
			found += stringMap.find(key) != stringMap.end();
		return found;
	};

	BENCHMARK("RadixMap<double> find()") {
		size_t found = 0;
		for(double key : doubleKeys)
			found += doubleMap.find(key) != doubleMap.end();
		return found;
	};

	BENCHMARK("RadixMap<string> lower_bound()") {
		size_t found = 0;
		for(const std::string &key : stringKeys)
			found += stringMap.lower_bound(key) != stringMap.end();
		return found;
	};

	BENCHMARK("RadixMap<double> lower_bound()") {
		size_t found = 0;
		for(double key : doubleKeys)
			found += doubleMap.lower_bound(key) != doubleMap.end();
		return found;
	};
}
