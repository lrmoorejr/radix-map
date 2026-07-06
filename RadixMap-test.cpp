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

#include <algorithm>
#include <cstdint>
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
