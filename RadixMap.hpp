#pragma once

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
#include <cassert>
#include <cstdint>
#include <memory>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string.h>
#include <type_traits>
#include <utility>
#include <vector>

// Ensure.hpp is an optional dependency: if it's available (either as part of this
// checkout or vendored alongside this header), use its ensure()/throw_if() for a
// formatted diagnostic on failure; otherwise fall back to equivalent local
// implementations so this header still works standalone.
#if __has_include("commons/Ensure.hpp")
	#include "commons/Ensure.hpp"
#elif __has_include("Ensure.hpp")
	#include "Ensure.hpp"
#else
	// Guard against Ensure.hpp having already been included under a path our
	// __has_include checks above don't know about (e.g. vendored elsewhere as
	// "3rdparty/Ensure.hpp") -- COMMONS_ENSURE_HPP is defined by Ensure.hpp
	// itself, so this still catches that case even under an unknown filename.
	// (throw_if is a function template, not a macro, so #ifndef can't guard
	// it directly -- redefining it without this check would be a hard error.)
	#if !defined(COMMONS_ENSURE_HPP) && !defined(ensure)
		#define ensure(condition, ...) assert((condition))
	#endif
	// A second guard (distinct from COMMONS_ENSURE_HPP) covers the case where two
	// headers using this same standalone fallback are included together.
	#if !defined(COMMONS_ENSURE_HPP) && !defined(COMMONS_THROW_IF_FALLBACK_DEFINED)
	#define COMMONS_THROW_IF_FALLBACK_DEFINED
	template<class T, class... Args>
	constexpr inline void throw_if(bool condition, Args&&... args) {
		if (condition)
			throw T(std::forward<Args>(args)...);
	}
	#endif
#endif

/// @cond INTERNAL

// This class represents a node in the tree.  It is similar in many respects
// to a "digial tree," except this tree is optimized for space (which helps
// with performance, too).  Each node hold a portion of the key (Subkey) that
// is common to all of it's children.  This can be empty, if there is no common
// section, but you can imagine URL keys like "http://whatever" would all
// begin with "http://," so that would wind up in the root node, stored a single
// time.  This results in a natural data compression.
//
// In addition to the commmon subkey, this node also contains branches to its
// children organized in an array that is indexed by the masked / shifted version
// of the next byte in the key.  For example:

// Next byte: 0B01100101
// Shift: 2
// Mask: 12
// Index = (0B01100101 & 3) >> 2 = 1

// The shift / mask scheme minimizes the branch space required (a real digital
// tree would require 256 * sizeof(void*) for EVERY node), alleviates the
// need for any searching for the child node, and maintains sorted order.
//
// RadixMapNode is templated only on Value -- it operates purely on raw key
// bytes (see RadixMapKeyTraits below for how a Key gets turned into bytes) so
// the byte-shift/subkey-compression algorithm above is completely independent
// of both Key and Value.
//
// RadixMapNode is an internal implementation detail of RadixMap (below) --
// nothing outside RadixMap.hpp is expected to name it directly, which is why
// this whole class, and the byte-encoding plumbing that follows it, are
// excluded from generated documentation.
template<class Value>
class RadixMapNode final {
	public:
		void* operator new(size_t sz, size_t subkeySize) {
			// sz already accounts for one byte via subkey[1], so only the bytes
			// beyond that first one need to be added. subkeySize is unsigned, so
			// guard the subkeySize == 0 (empty key) case explicitly rather than
			// computing subkeySize - 1, which would underflow and wrap back
			// around to the same answer only by coincidence of struct padding.
			size_t extra = subkeySize > 1 ? subkeySize - 1 : 0;
			void* p = ::operator new(sz + extra);
			return p;
		}
		void operator delete(void*mem) {
			::operator delete(mem);
		}

		// Private constructors, used internally during the insertion process to build the tree
		RadixMapNode(const unsigned char* key, const size_t length) noexcept;
		RadixMapNode(const unsigned char* key, const size_t length, const Value& value) noexcept;

		// Intentionally not virtual for performance reasons
		~RadixMapNode();

		// Raw owning pointers (branches, value) with no reference counting --
		// an implicit copy would double-free/shallow-copy. Use clone() for a
		// real deep copy instead.
		RadixMapNode(const RadixMapNode&) = delete;
		RadixMapNode& operator=(const RadixMapNode&) = delete;
		RadixMapNode(RadixMapNode&&) = delete;
		RadixMapNode& operator=(RadixMapNode&&) = delete;

		// Returns a deep copy of this node and its entire subtree.
		RadixMapNode* clone() const;

		// RadixMapNode is an internal implementation detail of RadixMap -- these
		// members are public only so RadixMap and its nested iterator (which
		// needs direct access to branches/value/subkey to walk the tree) can use
		// them; nothing outside RadixMap.hpp is expected to name RadixMapNode
		// directly.

		// Here is how we insert / search the tree for key / values. Returns the
		// node holding an exact match for the given key, or nullptr if the key
		// was never inserted (including if it only matches a structural
		// branch-point node that has no value of its own).
		RadixMapNode* locate(const unsigned char* ks, size_t keyLength) noexcept;

		// For visual debugging, to make sure the tree is constructed properly.
		void dump() const noexcept;

		// For optimization purposes, I was interested in seeing the distribution
		// of subkey space allocated in the tree.
		void dumpSubkeySizes() const noexcept;

		// Internal routines used for insertion.  They don't really need to be
		// seperate methods-- it is just an organizational strategy to avoid
		// one enormous insert method.
		// Returns true if this was a fresh insert (the key had no prior value),
		// false if it overwrote an existing value -- lets RadixMap track size()
		// in O(1) without a separate lookup.
		bool insert(const unsigned char *key, const int length, const Value &value) noexcept;
		void expandBranches(const unsigned char *key, const int keyLength, const Value& value) noexcept;
		void splitNode(const unsigned char *ss, const int subkeyIndex, const unsigned char *ks, const int keyIndex, const int keyLength, const Value &value) noexcept;

		// Field order and widths are chosen to minimize the node header:
		// pointers and subkeyLength first, then the four small fields packed
		// into 6 bytes (shift <= 7, mask <= 0xFF, and branchCount/branchSpace
		// <= 256 -- one slot per possible byte value -- so they never need
		// more than the widths below), and the flexible-array subkey last.
		// This makes the header 30 bytes instead of 40, which drops a node
		// with a short subkey (e.g. any 8-byte arithmetic key) into a smaller
		// malloc size class and lets header + subkey share one cache line --
		// find() is dominated by pointer-chasing, so node footprint is speed.
		RadixMapNode **branches = nullptr;

		// The value associated with the key formed by the parents' subkeys
		// concatenated with this one.  Any node in the tree may or may not
		// be a leaf, so this can legitimately be a nullptr.  I use lazy
		// allocation for performance reasons.
		Value *value = nullptr;

		// The portion of the key that this node manages.  The prefix(es) are
		// managed by parent nodes, suffix(es) by children.
		size_t subkeyLength = 0;

		// Branch information
		uint16_t branchCount = 0;
		uint16_t branchSpace = 0;
		uint8_t shift = 0;
		uint8_t mask = 0;

		unsigned char subkey[1];
};

// Non-owning byte view used as the "encoded key" for std::string, whose bytes
// already compare correctly in lexicographic (byte) order with no transformation.
struct RadixMapByteView {
	const unsigned char* bytes;
	size_t length;
	const unsigned char* data() const noexcept { return bytes; }
	size_t size() const noexcept { return length; }
};

// Owning fixed-size byte buffer used as the "encoded key" for arithmetic types,
// which need their bits transformed before they're byte-comparable (see
// RadixMapKeyTraits below).
template<size_t N>
struct RadixMapByteBuffer {
	unsigned char bytes[N];
	const unsigned char* data() const noexcept { return bytes; }
	size_t size() const noexcept { return N; }
};

// Byte-swaps an unsigned integer of any of the standard widths, reversing its
// byte order. Arithmetic keys need this so memcmp (which only ever compares
// byte-by-byte in ascending address order) agrees with their numeric order.
template<class Unsigned>
constexpr Unsigned radixMapByteSwap(Unsigned value) noexcept {
	if constexpr (sizeof(Unsigned) == 1)
		return value;
	else if constexpr (sizeof(Unsigned) == 2)
		return __builtin_bswap16(value);
	else if constexpr (sizeof(Unsigned) == 4)
		return __builtin_bswap32(value);
	else
		return __builtin_bswap64(value);
}

/// @endcond

/**
 * @brief Customization point mapping a Key type to a byte string that compares
 * correctly via `memcmp`/lexicographic byte order, matching Key's own natural
 * ordering.
 *
 * Built-in specializations below cover `std::string` and any integral or
 * floating-point type, so `RadixMap<Key,Value>` works out of the box for
 * those. To support another Key type, specialize `RadixMapKeyTraits<Key>`
 * yourself; a specialization must provide:
 *  - `Encoded`: a type with `.data()` (returning `const unsigned char*`) and
 *    `.size()` (returning `size_t`) member functions exposing the encoded
 *    byte string. It may be a non-owning view (like the built-in
 *    `std::string` specialization) or an owning buffer (like the built-in
 *    arithmetic specialization) -- RadixMap only ever reads from it once,
 *    immediately after calling `encode()`.
 *  - `static Encoded encode(const Key&) noexcept`: encodes a key to bytes.
 *  - `static Key decode(const unsigned char* bytes, size_t length)`: the
 *    inverse of `encode()`, used to reconstruct a Key during iteration.
 *
 * @tparam Key The key type to encode/decode. The primary template is
 * intentionally left undefined (via a `static_assert` on instantiation) for
 * any Key without a specialization, so using an unsupported Key type is a
 * clear compile error rather than a wall of unrelated template errors.
 */
template<class Key>
struct RadixMapKeyTraits {
	static_assert(sizeof(Key) == 0,
		"RadixMapKeyTraits<Key> has no specialization for this Key type -- "
		"supported out of the box: std::string and arithmetic types "
		"(integral/floating point). Specialize RadixMapKeyTraits<Key> "
		"yourself to add support for another Key type.");
};

/**
 * @brief Built-in RadixMapKeyTraits specialization for `std::string` keys.
 *
 * A zero-copy view over the string's own bytes, which already compare
 * correctly in lexicographic order with no transformation needed.
 */
template<>
struct RadixMapKeyTraits<std::string> {
	/// The encoded-key type: a zero-copy view over the original string's bytes.
	using Encoded = RadixMapByteView;

	/**
	 * @brief Encodes key as a view over its own existing bytes; no copy.
	 *
	 * @param key String to encode.
	 * @return View over key's bytes.
	 */
	static Encoded encode(const std::string& key) noexcept {
		return Encoded{reinterpret_cast<const unsigned char*>(key.data()), key.size()};
	}

	/**
	 * @brief Reconstructs the original string from its encoded bytes.
	 *
	 * @param bytes Pointer to the encoded byte string.
	 * @param length Number of bytes at bytes.
	 * @return The decoded string.
	 */
	static std::string decode(const unsigned char* bytes, size_t length) {
		return std::string(reinterpret_cast<const char*>(bytes), length);
	}
};

/// @brief True for any integral or floating-point Key type; constrains the
/// arithmetic RadixMapKeyTraits specialization below.
template<class Key>
concept RadixMapArithmeticKey = std::is_integral_v<Key> || std::is_floating_point_v<Key>;

/**
 * @brief Built-in RadixMapKeyTraits specialization for any integral or
 * floating-point Key type.
 *
 * Encodes to a big-endian, byte-comparable bit pattern: unsigned integers via
 * a byteswap only, signed integers via a sign-bit flip then byteswap (the
 * standard signed-radix-sort trick), and floating-point types via a
 * sign-bit flip plus a full bit inversion for negative values (IEEE-754
 * sign-magnitude negatives otherwise sort backwards relative to their
 * magnitude) -- see encode()'s body for the reasoning behind each case.
 *
 * @note `0.0` and `-0.0` encode to different byte patterns and are therefore
 * treated as two distinct keys, unlike `std::map<double,...>` (whose default
 * `std::less` treats them as equivalent). This falls directly out of encoding
 * by raw bits and is intentional, not a bug.
 */
template<RadixMapArithmeticKey Key>
struct RadixMapKeyTraits<Key> {
	/// Unsigned integer type of the same width as Key, used as the working type for the bit manipulation in encode()/decode().
	using Unsigned = std::conditional_t<sizeof(Key) == 1, unsigned char,
	                 std::conditional_t<sizeof(Key) == 2, uint16_t,
	                 std::conditional_t<sizeof(Key) == 4, uint32_t, uint64_t>>>;
	/// The encoded-key type: an owning byte buffer exactly sizeof(Key) bytes wide.
	using Encoded = RadixMapByteBuffer<sizeof(Key)>;

	/**
	 * @brief Encodes key to a big-endian, byte-comparable bit pattern (see
	 * this specialization's class documentation for the per-category
	 * transform).
	 *
	 * @param key Value to encode.
	 * @return The encoded bytes.
	 */
	static Encoded encode(Key key) noexcept {
		Unsigned bits;
		memcpy(&bits, &key, sizeof(Key));

		constexpr Unsigned signBit = Unsigned(1) << (sizeof(Unsigned) * 8 - 1);
		if constexpr (std::is_floating_point_v<Key>) {
			// IEEE-754: flip the sign bit, and if the value was negative (sign bit
			// now cleared), invert every other bit too -- sign-magnitude negatives
			// otherwise sort backwards relative to their magnitude.
			bits ^= signBit;
			if(!(bits & signBit))
				bits ^= static_cast<Unsigned>(~signBit);
		} else if constexpr (std::is_signed_v<Key>) {
			// Two's complement: flipping just the sign bit turns the native
			// ordering directly into unsigned/byte-comparable ordering.
			bits ^= signBit;
		}
		// Unsigned integral keys need no bit manipulation at all.

		Unsigned big = radixMapByteSwap(bits);
		Encoded result;
		memcpy(result.bytes, &big, sizeof(Key));
		return result;
	}

	/**
	 * @brief Reconstructs the original value from its encoded bytes; the
	 * inverse of encode().
	 *
	 * @param bytes Pointer to the encoded byte string.
	 * @return The decoded value.
	 */
	static Key decode(const unsigned char* bytes, size_t) noexcept {
		Unsigned big;
		memcpy(&big, bytes, sizeof(Key));
		Unsigned bits = radixMapByteSwap(big);

		constexpr Unsigned signBit = Unsigned(1) << (sizeof(Unsigned) * 8 - 1);
		if constexpr (std::is_floating_point_v<Key>) {
			if(!(bits & signBit))
				bits ^= static_cast<Unsigned>(~signBit);
			bits ^= signBit;
		} else if constexpr (std::is_signed_v<Key>) {
			bits ^= signBit;
		}

		Key key;
		memcpy(&key, &bits, sizeof(Key));
		return key;
	}
};

/**
 * @brief A sorted associative container -- a fast alternative to std::map for
 * simple, fixed-shape keys -- with a std::map-like surface.
 *
 * Built on a PATRICIA/radix-tree-style structure that compresses shared key
 * prefixes and uses an adaptive mask/shift branch scheme instead of a full
 * 256-way fan-out per node (an internal implementation detail; not part of
 * this class's public surface). A Key is turned into a byte string that
 * compares correctly via `memcmp` through RadixMapKeyTraits<Key>, which has
 * built-in specializations for `std::string` and any integral/floating-point
 * type; specialize RadixMapKeyTraits<Key> yourself to support another Key
 * type.
 *
 * Not supported: a comparator/allocator template parameter, thread-safety,
 * bidirectional iteration, or structural re-compression after erase() (erase
 * is a tombstone -- the entry's value is cleared but the tree's shape is not
 * rebalanced).
 *
 * @tparam Key Key type. Must have a RadixMapKeyTraits<Key> specialization
 * (built in for `std::string` and arithmetic types).
 * @tparam Value Mapped type. Must be copy-constructible for insert(), and
 * default-constructible for operator[] (matching std::map::operator[]'s own
 * requirement).
 */
template<class Key, class Value>
class RadixMap final {
	public:
		/**
		 * @brief Constructs an empty RadixMap.
		 */
		inline RadixMap() = default;

		/**
		 * @brief Deep-copies other, including its entire tree structure.
		 *
		 * @param other RadixMap to copy from.
		 */
		inline RadixMap(const RadixMap& other) : count_(other.count_) {
			if(other.root)
				root.reset(other.root->clone());
		}

		/**
		 * @brief Deep-copies other, replacing this RadixMap's current contents.
		 * Safe for self-assignment.
		 *
		 * @param other RadixMap to copy from.
		 * @return `*this`.
		 */
		inline RadixMap& operator=(const RadixMap& other) {
			if(this != &other) {
				root.reset(other.root ? other.root->clone() : nullptr);
				count_ = other.count_;
			}
			return *this;
		}

		/**
		 * @brief Move-constructs from other, leaving it empty.
		 */
		inline RadixMap(RadixMap&&) noexcept = default;

		/**
		 * @brief Move-assigns from other, leaving it empty.
		 *
		 * @return `*this`.
		 */
		inline RadixMap& operator=(RadixMap&&) noexcept = default;

		/**
		 * @brief Inserts key with value, or overwrites the existing value if key
		 * is already present.
		 *
		 * @param key Key to insert or overwrite.
		 * @param value Value to associate with key.
		 */
		inline void insert(const Key& key, const Value& value) noexcept {
			auto encoded = RadixMapKeyTraits<Key>::encode(key);
			if(root == nullptr) {
				root.reset(new(encoded.size()) RadixMapNode<Value>(encoded.data(), encoded.size(), value));
				++count_;
			} else if(root->insert(encoded.data(), static_cast<int>(encoded.size()), value)) {
				++count_;
			}
		}

		/**
		 * @brief True if key has been inserted and not since erased.
		 *
		 * @param key Key to check.
		 * @return True if key is present.
		 */
		inline bool contains(const Key& key) const noexcept {
			return locate(key) != nullptr;
		}

		/**
		 * @brief Bounds-checked value access, matching std::map::at().
		 *
		 * @param key Key to look up.
		 * @return Reference to key's value.
		 * @throws std::out_of_range If key was never inserted (or was erased).
		 */
		inline Value& at(const Key& key) const {
			RadixMapNode<Value>* node = locate(key);
			throw_if<std::out_of_range>(node == nullptr, "RadixMap::at: key not found");
			return *node->value;
		}

		/**
		 * @brief Returns a reference to key's value, default-constructing
		 * `Value{}` and inserting it first if key isn't already present --
		 * matching std::map::operator[].
		 *
		 * @param key Key to look up or default-insert.
		 * @return Reference to key's (possibly newly default-constructed) value.
		 */
		inline Value& operator[](const Key& key) noexcept {
			if(RadixMapNode<Value>* node = locate(key))
				return *node->value;
			insert(key, Value{});
			return *locate(key)->value;
		}

		/**
		 * @brief Current number of entries.
		 *
		 * @return Current number of entries.
		 */
		inline size_t size() const noexcept { return count_; }

		/**
		 * @brief True if size() == 0.
		 *
		 * @return True if size() == 0.
		 */
		inline bool empty() const noexcept { return count_ == 0; }

		/**
		 * @brief Removes key if present.
		 *
		 * Tombstone only: clears the entry's value in place but leaves the
		 * tree's subkeys/branches structurally untouched, so it doesn't reclaim
		 * the compression a full structural merge would -- a deliberate
		 * tradeoff (real node-merging is a separate, harder feature), not an
		 * oversight.
		 *
		 * @param key Key to remove.
		 * @return 1 if a value was removed, 0 if key wasn't found.
		 */
		inline size_t erase(const Key& key) noexcept {
			RadixMapNode<Value>* node = locate(key);
			if(node == nullptr)
				return 0;
			delete node->value;
			node->value = nullptr;
			--count_;
			return 1;
		}

		/**
		 * @brief Prints the tree's internal node structure to stdout, for
		 * debugging.
		 */
		inline void dump() const noexcept {
			if(root)
				root->dump();
		}

		/**
		 * @brief Prints each node's subkey length to stdout, for studying the
		 * distribution of subkey space allocated in the tree.
		 */
		inline void dumpSubkeySizes() const noexcept {
			if(root)
				root->dumpSubkeySizes();
		}

		/**
		 * @brief Forward iterator over a RadixMap's entries in ascending key
		 * order.
		 *
		 * Accessed via RadixMap::iterator / RadixMap::const_iterator; not named
		 * directly. The tree stores no actual `(Key,Value)` pairs -- only path
		 * bytes -- so dereferencing reconstructs Key on the fly via
		 * `RadixMapKeyTraits<Key>::decode()` as the iterator walks.
		 *
		 * operator*() therefore returns Entry by value (a proxy, not a
		 * reference to a stored pair), so range-for must bind it as
		 * `for(auto &&entry : map)` or `for(auto entry : map)` --
		 * `for(auto &entry : map)` does not compile, since a non-const lvalue
		 * reference cannot bind to a prvalue. Entry::second is itself a
		 * reference, so mutating `entry.second` works the same regardless of
		 * which of those two forms you use.
		 *
		 * Forward-only: the underlying tree has no parent pointers, so there's
		 * no cheap way to go backwards.
		 *
		 * @tparam IsConst True for RadixMap::const_iterator (Entry::second is
		 * `const Value&`), false for RadixMap::iterator (Entry::second is
		 * `Value&`, mutable).
		 */
		template<bool IsConst>
		class IteratorImpl {
			public:
				/// `const Value&` for const_iterator, `Value&` for iterator.
				using ValueRef = std::conditional_t<IsConst, const Value&, Value&>;

				/**
				 * @brief The `(Key, Value&)` pair returned by dereferencing an
				 * iterator.
				 *
				 * A proxy, not a reference into stored state -- see
				 * IteratorImpl's documentation for what that means for
				 * range-for.
				 */
				struct Entry {
					Key first;       ///< The entry's key, decoded fresh on each dereference.
					ValueRef second; ///< Reference to the entry's stored value.
				};

				/// @cond INTERNAL
				// Return type of operator->(); users never name this directly --
				// it only exists so `it->first`/`it->second` has something to
				// take the address of, since operator*() has nothing real to
				// point to (see Entry's documentation).
				struct ArrowProxy {
					Entry entry;
					Entry* operator->() noexcept { return &entry; }
				};
				/// @endcond

				/// Standard iterator trait: this is a forward iterator (single-pass reset via multiple copies, no `operator--`).
				using iterator_category = std::forward_iterator_tag;
				/// Standard iterator trait: the type produced by dereferencing.
				using value_type = Entry;
				/// Standard iterator trait: type used to express distance between iterators.
				using difference_type = std::ptrdiff_t;

				/**
				 * @brief Constructs an end() iterator.
				 */
				IteratorImpl() noexcept = default;

				/**
				 * @brief Dereferences to the current `(Key, Value&)` pair.
				 *
				 * @return An Entry by value; see IteratorImpl's documentation
				 * for why range-for needs `auto &&`/`auto` rather than `auto &`.
				 */
				Entry operator*() const noexcept {
					return Entry{RadixMapKeyTraits<Key>::decode(pathBytes.data(), pathBytes.size()), *current->value};
				}

				/**
				 * @brief Arrow access to the current entry, e.g.
				 * `it->first`/`it->second`.
				 *
				 * @return A proxy through which `->first`/`->second` reach the
				 * current Entry; see Entry's documentation for why this isn't
				 * just a plain pointer.
				 */
				ArrowProxy operator->() const noexcept {
					return ArrowProxy{*(*this)};
				}

				/**
				 * @brief Advances to the next entry in ascending key order.
				 *
				 * @return `*this`, now referring to the next entry (or end()).
				 */
				IteratorImpl& operator++() noexcept {
					current = stepToNextValue();
					return *this;
				}

				/**
				 * @brief Postfix increment.
				 *
				 * @return A copy of `*this` as it was before advancing.
				 */
				IteratorImpl operator++(int) noexcept {
					IteratorImpl copy = *this;
					++(*this);
					return copy;
				}

				/**
				 * @brief True if both iterators refer to the same entry, or both
				 * are end().
				 */
				friend bool operator==(const IteratorImpl& a, const IteratorImpl& b) noexcept {
					return a.current == b.current;
				}

				/**
				 * @brief Negation of operator==.
				 */
				friend bool operator!=(const IteratorImpl& a, const IteratorImpl& b) noexcept {
					return !(a == b);
				}

			private:
				friend class RadixMap;

				struct Frame {
					RadixMapNode<Value>* node;
					int nextStep; // -1 = haven't checked node's own value yet; 0.. = next branch index to try
					size_t depth; // pathBytes.size() before node's subkey was appended; restored on backtrack
				};

				// Pushes node onto the traversal stack, extending pathBytes with its
				// subkey. Used both for the initial descent from root and for
				// descending into a branch.
				void pushNode(RadixMapNode<Value>* node) noexcept {
					size_t depthBefore = pathBytes.size();
					pathBytes.insert(pathBytes.end(), node->subkey, node->subkey + node->subkeyLength);
					stack.push_back(Frame{node, -1, depthBefore});
				}

				// Advances the cursor to the next node with a value, in ascending key
				// order (pre-order over the tree: a node's own value -- if any -- sorts
				// before all of its branches, and branch index order already matches
				// ascending byte order per RadixMapNode's own mask/shift scheme).
				// Returns nullptr once the traversal is exhausted.
				RadixMapNode<Value>* stepToNextValue() noexcept {
					while(!stack.empty()) {
						Frame &top = stack.back();
						if(top.nextStep == -1) {
							top.nextStep = 0;
							if(top.node->value != nullptr)
								return top.node;
							continue;
						}

						bool descended = false;
						while(top.nextStep < top.node->branchSpace) {
							int index = top.nextStep++;
							RadixMapNode<Value>* child = top.node->branches[index];
							if(child != nullptr) {
								pushNode(child); // invalidates `top` -- don't touch it again this iteration
								descended = true;
								break;
							}
						}
						if(descended)
							continue;

						pathBytes.resize(top.depth);
						stack.pop_back();
					}
					return nullptr;
				}

				std::vector<Frame> stack;
				std::vector<unsigned char> pathBytes;
				RadixMapNode<Value>* current = nullptr;
		};

		/// Mutable iterator; `Entry::second` is `Value&`. See IteratorImpl.
		using iterator = IteratorImpl<false>;
		/// Read-only iterator; `Entry::second` is `const Value&`. See IteratorImpl.
		using const_iterator = IteratorImpl<true>;

		/**
		 * @brief Iterator to the first entry in ascending key order, or end()
		 * if empty.
		 *
		 * @return Mutable iterator to the first entry, or end().
		 */
		inline iterator begin() noexcept { return beginImpl<false>(); }

		/**
		 * @brief Iterator one past the last entry.
		 *
		 * @return Mutable end() iterator.
		 */
		inline iterator end() noexcept { return iterator{}; }

		/**
		 * @brief const overload of begin().
		 *
		 * @return Read-only iterator to the first entry, or end().
		 */
		inline const_iterator begin() const noexcept { return beginImpl<true>(); }

		/**
		 * @brief const overload of end().
		 *
		 * @return Read-only end() iterator.
		 */
		inline const_iterator end() const noexcept { return const_iterator{}; }

		/**
		 * @brief Explicitly const-qualified begin(), for a const_iterator on a
		 * non-const RadixMap.
		 *
		 * @return Read-only iterator to the first entry, or cend().
		 */
		inline const_iterator cbegin() const noexcept { return beginImpl<true>(); }

		/**
		 * @brief Explicitly const-qualified end(), for a const_iterator on a
		 * non-const RadixMap.
		 *
		 * @return Read-only end() iterator.
		 */
		inline const_iterator cend() const noexcept { return const_iterator{}; }

		/**
		 * @brief Finds key, returning an iterator to it.
		 *
		 * Incrementing the returned iterator continues in ascending key order,
		 * the same as a begin()-driven traversal that had stepped its way
		 * there.
		 *
		 * @param key Key to look up.
		 * @return Mutable iterator to key's entry, or end() if key was never
		 * inserted.
		 */
		inline iterator find(const Key& key) noexcept { return findImpl<false>(key); }

		/**
		 * @brief const overload of find().
		 *
		 * @param key Key to look up.
		 * @return Read-only iterator to key's entry, or end() if key was never
		 * inserted.
		 */
		inline const_iterator find(const Key& key) const noexcept { return findImpl<true>(key); }

	private:
		// Shared by contains()/at()/operator[], which just need a value pointer,
		// not a full resumable iterator.
		inline RadixMapNode<Value>* locate(const Key& key) const noexcept {
			if(root == nullptr)
				return nullptr;
			auto encoded = RadixMapKeyTraits<Key>::encode(key);
			return root->locate(encoded.data(), encoded.size());
		}

		template<bool IsConst>
		IteratorImpl<IsConst> beginImpl() const noexcept {
			IteratorImpl<IsConst> it;
			if(root) {
				it.pushNode(root.get());
				it.current = it.stepToNextValue();
			}
			return it;
		}

		// Walks from root towards key, building the same traversal stack begin()
		// would have left behind had it stepped its way to this exact node --
		// so that incrementing the returned iterator resumes correctly.
		template<bool IsConst>
		IteratorImpl<IsConst> findImpl(const Key& key) const noexcept {
			if(root == nullptr)
				return IteratorImpl<IsConst>{};

			auto encoded = RadixMapKeyTraits<Key>::encode(key);
			const unsigned char* ks = encoded.data();
			size_t keyLength = encoded.size();

			IteratorImpl<IsConst> it;
			RadixMapNode<Value>* n = root.get();
			it.pushNode(n);

			while(true) {
				size_t subkeyLen = n->subkeyLength;
				if(subkeyLen > keyLength || memcmp(n->subkey, ks, subkeyLen) != 0)
					return IteratorImpl<IsConst>{};

				ks += subkeyLen;
				keyLength -= subkeyLen;
				it.stack.back().nextStep = 0;

				if(keyLength == 0) {
					if(n->value == nullptr)
						return IteratorImpl<IsConst>{};
					it.current = n;
					return it;
				}

				if(n->branchCount == 0)
					return IteratorImpl<IsConst>{};

				int index = (*ks & n->mask) >> n->shift;
				RadixMapNode<Value>* child = n->branches[index];
				if(!child)
					return IteratorImpl<IsConst>{};

				it.stack.back().nextStep = index + 1;
				n = child;
				it.pushNode(n);
			}
		}

		std::unique_ptr<RadixMapNode<Value>> root;
		size_t count_ = 0;
};

/// @cond INTERNAL

// --- RadixMapNode implementation -------------------------------------------

// Lookup tables for shift, mask, and branchSpace, keyed by the "relevant bits"
// (the XOR of every sibling branch's first byte). One instance per process,
// regardless of how many translation units include this header.
class RadixMapAttributeTables final {
public:
	RadixMapAttributeTables() {
		for(int relevantBits = 1; relevantBits < 256; relevantBits++) {
			// Figure out the highest bit we are interested in
			int highestSetBit = 7;
			int bitMask = 1 << highestSetBit;
			while(!(relevantBits & bitMask)) {
				bitMask >>= 1;
				highestSetBit--;
			}

			// Figure out the lowest bit we are interested in
			int lowestSetBit = 0;
			bitMask = 1;
			while(!(relevantBits & bitMask)) {
				lowestSetBit++;
				bitMask <<= 1;
			}

			// Set all the bits between the lowestSetBit and the highestSetBit
			// This can then be our mask.  (If we don't set all the bits, the
			// mask will effectively filter out some branches even though we have
			// allocated memory for them.)
			bitMask <<= 1;
			int relevantRange = relevantBits;
			for(int bit = lowestSetBit + 1; bit < highestSetBit; ++bit, bitMask <<= 1)
				relevantRange |= bitMask;
			mask[relevantBits] = relevantRange;

			// If we wanted to minimize space usage, we could try increasing
			// amounts of shift with the hopes that we could drop a few bits.
			// (...each bit dropping memory requirements in half.)
			// However, that would take time, and there's a good chance we
			// might just have to add the bits back in later (since we
			// already know there is variation in them), so let's just
			// use all of them.  Here we are trading memory for performance.
			// (and I don't have to write a bunch of obscure code)
			branchSpace[relevantBits] = 2 << (highestSetBit - lowestSetBit);
			shift[relevantBits] = lowestSetBit;
		}
	}

	int getShift(int relevantBits) { return(shift[relevantBits]); }
	int getMask(int relevantBits) { return(mask[relevantBits]); }
	int getBranchSpace(int relevantBits) { return(branchSpace[relevantBits]); }

private:
	int shift[256];
	int mask[256];
	int branchSpace[256];
};

inline RadixMapAttributeTables& radixMapAttributeTables() {
	static RadixMapAttributeTables tables;
	return tables;
}

// Constructor for RadixMapNode with an initial key
template<class Value>
inline RadixMapNode<Value>::RadixMapNode(const unsigned char* key, const size_t length) noexcept {
	memcpy(subkey, key, length);
	subkeyLength = length;
	branchCount = branchSpace = shift = mask = 0;
}

// Constructor for a leaf RadixMapNode with a value
template<class Value>
inline RadixMapNode<Value>::RadixMapNode(const unsigned char* key, const size_t length, const Value& value) noexcept
	: RadixMapNode(key, length) {
	this->value = new Value(value);
}

// Destructor frees memory and all children
template<class Value>
inline RadixMapNode<Value>::~RadixMapNode() {
	if(branches) {
		for(int i = 0; i < branchSpace; ++i)
			if(branches[i])
				delete branches[i];
	}
	free(branches);
	if(value)
		delete value;
}

// Returns a deep copy of this node and its entire subtree, built with the same
// flexible-array-member placement-new the rest of the tree uses.
template<class Value>
inline RadixMapNode<Value>* RadixMapNode<Value>::clone() const {
	RadixMapNode* copy = value != nullptr
		? new(subkeyLength) RadixMapNode(subkey, subkeyLength, *value)
		: new(subkeyLength) RadixMapNode(subkey, subkeyLength);

	copy->branchCount = branchCount;
	copy->branchSpace = branchSpace;
	copy->shift = shift;
	copy->mask = mask;

	if(branchSpace > 0) {
		copy->branches = static_cast<RadixMapNode**>(calloc(sizeof(RadixMapNode*), branchSpace));
		for(int i = 0; i < branchSpace; ++i)
			if(branches[i] != nullptr)
				copy->branches[i] = branches[i]->clone();
	}

	return copy;
}

// Protected insert() method does the real work of inserting a new key / value pair
template<class Value>
inline bool RadixMapNode<Value>::insert(const unsigned char *key, const int keyLength, const Value& value) noexcept {
	RadixMapNode* n = const_cast<RadixMapNode*>(this);
	const unsigned char *keyPosition = key;
	const unsigned char *subkeyPosition = subkey;
	int subkeyIndex = 0;
	int keyIndex = 0;
	int subkeyLength = static_cast<int>(this->subkeyLength);

tryNextNode:
	while(subkeyIndex < subkeyLength && keyIndex < keyLength && subkeyPosition[subkeyIndex] == keyPosition[keyIndex]) {
		++subkeyIndex;
		++keyIndex;
	}

	if(subkeyIndex < subkeyLength) {
		// Our subkey and key differ in the middle, so we need to split this node vertically.
		// A diverging subkey means this exact key was never stored at this node before.
		n->splitNode(subkeyPosition, subkeyIndex, keyPosition, keyIndex, keyLength, value);
		return true;
	} else if(keyIndex < keyLength) {
		// Go to branch node
		if(n->branchCount == 0) {
			// We don't currently have any branches
			// We need to break this node into 2 with a parent / chld relationship
			RadixMapNode* newNode = new(keyLength - keyIndex) RadixMapNode(keyPosition + keyIndex, keyLength - keyIndex, value);

			// Create a single branch
			n->branchCount = 1;
			n->branchSpace = 1;

			// Add our branch. Nothing to delete since count used to be 0
			n->branches = static_cast<RadixMapNode**>(calloc(sizeof(RadixMapNode*), 1));
			n->branches[0] = newNode;
			return true;
		} else {
			// We have breanches, so see if the one we need exists
			int index = (keyPosition[keyIndex] & n->mask) >> n->shift;
			RadixMapNode *childNode = n->branches[index];
			if(!childNode) {
				// The slot is empty, so claim it. It's fine if this key's
				// unmasked bits differ from what the mask was derived from:
				// a future colliding key will fail the subkey comparison
				// below and trigger expandBranches, which recomputes the
				// mask over every child's first byte. (This condition once
				// also compared the unmasked key bits to themselves --
				// always true -- so it has only ever checked for an empty
				// slot.)
				n->branches[index] = new(keyLength - keyIndex) RadixMapNode(keyPosition + keyIndex, keyLength - keyIndex, value);
				return true;
			} else if(*childNode->subkey != keyPosition[keyIndex]) {
				// Expand the range of the keys
				n->expandBranches(keyPosition + keyIndex, keyLength - keyIndex, value);
				return true;
			} else {
				// Move to the branch and repeat.
				n = childNode;
				subkeyPosition = n->subkey;
				subkeyLength = static_cast<int>(n->subkeyLength);
				subkeyIndex = 0;

				// Note: I don't usually use gotos, but this is an exceptional circumstance.
				// The code actually wound up looking a lot cleaner this way, and is more performant.
				goto tryNextNode;
			}
		}
	} else {
		// Perfect key match-- we belong in this node
		// It seems to be faster to do an assignment vs. delete + new
		bool wasEmpty = (n->value == nullptr);
		if(n->value)
			*n->value = value;
		else
			n->value = new Value(value);
		return wasEmpty;
	}
}

// Called when a node needs to split vertically (i.e. the subkey is getting split)
template<class Value>
inline void RadixMapNode<Value>::splitNode(const unsigned char *ss, const int subkeyIndex, const unsigned char *ks, const int keyIndex, const int keyLength, const Value &value) noexcept {
	// Create a new child that contains most of our current into, minus some of the subkey
	RadixMapNode* newChild1 = new(subkeyLength - subkeyIndex) RadixMapNode(ss + subkeyIndex, subkeyLength - subkeyIndex);
	newChild1->branchCount = branchCount;
	newChild1->branchSpace = branchSpace;
	newChild1->shift = shift;
	newChild1->mask = mask;
	newChild1->branches = branches;
	newChild1->value = this->value;

	// Break the subkey apart
	subkeyLength = subkeyIndex;

	if(keyIndex < keyLength) {
		// The subkey and key differ somewhere in the middle
		// We need to break this subkey apart, and make two children
		RadixMapNode *newChild2 = new(keyLength - keyIndex) RadixMapNode(ks + keyIndex, keyLength - keyIndex, value);

		branchCount = 2;
		branchSpace = 2;
		this->value = nullptr;

		// Figure out which bit we are going to use as the hash value
		shift = 7;
		mask = 0x80;
		int hash1 = newChild1->subkey[0];
		int hash2 = newChild2->subkey[0];
		while((hash1 & mask) >> shift == (hash2 & mask) >> shift) {
			shift--;
			mask >>= 1;
		}

		// Re-initialize branches, since we transferred ours to our new child
		branches = static_cast<RadixMapNode**>(calloc(sizeof(RadixMapNode*), 2));
		branches[(hash1 & mask) >> shift] = newChild1;
		branches[(hash2 & mask) >> shift] = newChild2;
	} else {
		// Our subkey is bigger than the provided key.
		// We need to break this node into 2 with a parent / chld relationship
		branchCount = 1;
		branchSpace = 1;

		// There is only one branch
		shift = 0;
		mask = 0;

		// Re-initialize branches, since we transferred ours to our new child
		branches = static_cast<RadixMapNode**>(calloc(sizeof(RadixMapNode*), 1));
		branches[0] = newChild1;

		// Child already has a pointer to our old value, so just clobber it
		// with a new one.  Don't worry about delete.
		this->value = new Value(value);
	}
}

// Called when an existing RadixMapNode needs more branches to accomodate a new child
template<class Value>
inline void RadixMapNode<Value>::expandBranches(const unsigned char *key, const int keyLength, const Value& value) noexcept {
	// Compute the logical and + logical or of all the hash values
	int ands = key[0];
	int ors = ands;
	std::for_each(branches, branches + branchSpace, [&ands, &ors](RadixMapNode* &branch){
		if(branch != nullptr) {
			int branchHash = branch->subkey[0];
			ors |= branchHash;
			ands &= branchHash;
		}
	});

	// This will have all the bits we are interested in set
	int relevantBits = (~ands) & ors;

	RadixMapAttributeTables &tables = radixMapAttributeTables();
	shift = tables.getShift(relevantBits);
	mask = tables.getMask(relevantBits);

	int oldBranchSpace = branchSpace;
	branchSpace = tables.getBranchSpace(relevantBits);
	branchCount++;

	// Rehash the branches
	RadixMapNode **newBranches = static_cast<RadixMapNode**>(calloc(sizeof(RadixMapNode*), branchSpace));
	std::for_each(branches, branches + oldBranchSpace, [this, newBranches](RadixMapNode* &branch) {
		if(branch != nullptr) {
			int branchHash = branch->subkey[0];
			int newBranchIndex = (branchHash & mask) >> shift;
			newBranches[newBranchIndex] = branch;
		}
	});
	free(branches);
	branches = newBranches;

	// Lastly, add our new branch that we just made room for
	int newBranchIndex = (key[0] & mask) >> shift;
	ensure(branches[newBranchIndex] == nullptr, "expandBranches: branch slot already occupied");
	branches[newBranchIndex] = new(keyLength) RadixMapNode(key, keyLength, value);
}

// Locates a key and returns the node holding it, or nullptr if the key was
// never inserted. Only nodes with a non-null value represent a key that was
// actually inserted -- a structural match against a branch-only node's subkey
// (i.e. a prefix shared by more than one inserted key, but never itself
// inserted) must not count as found.
template<class Value>
inline RadixMapNode<Value>* RadixMapNode<Value>::locate(const unsigned char* ks, size_t keyLength) noexcept {
	RadixMapNode* n = this;
	const unsigned char *ss = subkey;

	size_t subkeyLength = n->subkeyLength;
	while(subkeyLength <= keyLength) {
		if(memcmp(ss, ks, subkeyLength))
			return nullptr;

		// Advance the section of the key we are considering
		ks += subkeyLength;
		keyLength -= subkeyLength;
		if(keyLength == 0)
			return n->value != nullptr ? n : nullptr;

		// We expect more in a branch
		if(n->branchCount == 0)
			return nullptr;

		int index = (*ks & n->mask) >> n->shift;
		n = n->branches[index];
		if(!n)
			return nullptr;

		ss = n->subkey;
		subkeyLength = n->subkeyLength;
	}

	return nullptr;
}

// Dumps out the tree contents for debugging purposes
template<class Value>
inline void RadixMapNode<Value>::dump() const noexcept {
	std::cout << "RadixMapNode " << std::hex << this << std::endl;
	std::cout << " subkey " << subkey << std::endl;
	std::cout << " branchCount " << std::dec << branchCount << std::endl;
	std::cout << " branchSpace " << std::dec << branchSpace << std::endl;
	std::cout << " mask " << std::hex << static_cast<int>(mask) << std::endl;
	std::cout << " shift " << static_cast<int>(shift) << std::endl;
	for(int i = 0; i < branchSpace; ++i)
		if(branches[i] == nullptr)
			std::cout << " " << i << " branch " << "null" << std::endl;
		else
			std::cout << " " << i << " branch " << std::hex << branches[i]->subkey << std::endl;
	std::cout << std::endl;

	// Dump out the children recursively
	for(int i = 0; i < branchSpace; ++i)
		if(branches[i] != nullptr)
			branches[i]->dump();
}

// Dumps out subkey sizes-- useful for studying the allocation dynamics
template<class Value>
inline void RadixMapNode<Value>::dumpSubkeySizes() const noexcept {
	std::cout << subkeyLength << std::endl;
	for(int i = 0; i < branchSpace; ++i)
		if(branches[i] != nullptr)
			branches[i]->dumpSubkeySizes();
}

/// @endcond
