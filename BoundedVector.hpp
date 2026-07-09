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

#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <utility>

// Ensure.hpp is an optional dependency: if it's available (either as part of this
// checkout or vendored alongside this header), use its throw_if() for input
// validation; otherwise fall back to an equivalent local implementation so this
// header still works standalone. Unlike ensure() (this codebase's usual choice for
// "should never happen" internal invariants), every check in this header validates
// caller-supplied input (an index, a requested size) that a caller may legitimately
// want to catch and recover from -- matching std::vector::at()'s own throwing
// contract -- so throw_if() rather than ensure() is used throughout.
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

/**
 * @brief A fixed-capacity, non-heap-allocating vector-like container.
 *
 * Capacity is a compile-time constant (@p M): the backing storage is a plain array embedded
 * directly in the object, so constructing, copying, and destroying a BoundedVector never
 * allocates. Element access, iteration, and mutation are modeled closely on std::vector, and
 * BoundedVector can often be used as a drop-in replacement for the common subset of that API --
 * see the README for the full rationale and a performance comparison.
 *
 * @tparam T Element type. Must be default-constructible; insert_at()/erase_at() (and therefore
 * the iterator-based insert()/erase(), which delegate to them) additionally require T to be
 * move/copy-assignable, and use a bulk memmove() instead of a per-element loop when T is also
 * trivially copyable.
 * @tparam M Capacity: the maximum number of elements this BoundedVector can ever hold, fixed at
 * compile time. Exceeding it throws std::length_error rather than reallocating.
 */
template<typename T, short M>
class BoundedVector {
	// components (the full M-slot array) is default-initialized on entry to every constructor
	// that doesn't explicitly list it in its member-initializer list -- which is all of them
	// except the default constructor below -- so T must be default-constructible to construct
	// a BoundedVector at all, not just to use the default/sized constructors or resize(). Asserted
	// here so that requirement is a clear, intentional message instead of a confusing compiler
	// error blaming whichever constructor happens to be instantiated first.
	static_assert(std::is_default_constructible_v<T>, "BoundedVector requires a default-constructible T");

public:
	/// Plain T*; there's no separate iterator type, since the backing storage is already a
	/// contiguous array.
	using iterator = T*;
	/// Plain const T*; see iterator.
	using const_iterator = const T*;

	/**
	 * @brief Constructs an empty BoundedVector.
	 */
	constexpr BoundedVector() : components{}, count(0) {}

	/**
	 * @brief Constructs with count value-initialized elements.
	 *
	 * explicit, like std::vector(size_type): otherwise a bare int/short implicitly converts to
	 * a whole BoundedVector wherever one is expected, which is surprising on its own and, worse,
	 * can silently steal overload resolution from other constructors (e.g. a braced-init-list
	 * of ints binding as "one BoundedVector per int" instead of "one BoundedVector holding these
	 * ints" when inserting into a container of BoundedVector).
	 *
	 * @param count Number of elements to value-initialize.
	 * @throws std::length_error If @p count exceeds capacity M.
	 */
	explicit constexpr BoundedVector(short count) : count(count) {
		throw_if<std::length_error>(count > M, "BoundedVector overflow");
		for(short index = 0; index < count; ++index)
			components[index] = T{};
	}

	/**
	 * @brief Constructs with count copies of fillValue.
	 *
	 * explicit for the same reason as BoundedVector(short) above.
	 *
	 * @param count Number of copies to construct.
	 * @param fillValue Value each element is copied from.
	 * @throws std::length_error If @p count exceeds capacity M.
	 */
	explicit constexpr BoundedVector(short count, T fillValue) : count(count) {
		throw_if<std::length_error>(count > M, "BoundedVector overflow");
		for(short index = 0; index < count; ++index)
			components[index] = fillValue;
	}

	/**
	 * @brief Constructs from a brace-init list, e.g. `BoundedVector<int, 4>{1, 2, 3}`.
	 *
	 * @param values Elements to copy in, in order.
	 * @throws std::length_error If @p values has more elements than capacity M.
	 */
	constexpr BoundedVector(std::initializer_list<T> values) {
		reconfigure(std::begin(values), std::end(values));
	}

	/**
	 * @brief Replaces the contents with the range [begin, end), discarding whatever was there.
	 *
	 * @tparam It Input iterator type.
	 * @param begin Start of the replacement range.
	 * @param end End of the replacement range.
	 * @throws std::length_error If the range has more elements than capacity M.
	 */
	template<class It>
	void reconfigure(It begin, It end) {
		count = 0;
		for(auto iterator = begin; iterator != end; ++iterator)
			push_back(*iterator);
	}

	/**
	 * @brief Appends a copy of item.
	 *
	 * @param item Value to copy in.
	 * @throws std::length_error If already at capacity M.
	 */
	constexpr inline void push_back(const T& item) {
		throw_if<std::length_error>(count >= M, "BoundedVector overflow");
		components[count++] = item;
	}

	/**
	 * @brief Constructs an element in place at the end and returns a reference to it.
	 *
	 * Constructs T from args (or default-constructs it, given none), matching
	 * std::vector::emplace_back().
	 *
	 * @tparam Args Constructor argument types.
	 * @param args Arguments forwarded to T's constructor.
	 * @return Reference to the newly constructed element.
	 * @throws std::length_error If already at capacity M.
	 */
	template<class... Args>
	constexpr inline T& emplace_back(Args&&... args) {
		throw_if<std::length_error>(count >= M, "BoundedVector overflow");
		components[count] = T(std::forward<Args>(args)...);
		return components[count++];
	}

	/**
	 * @brief Removes the last element.
	 *
	 * @throws std::out_of_range If the vector is empty.
	 */
	constexpr inline void pop_back() {
		throw_if<std::out_of_range>(count == 0, "BoundedVector pop_back on empty vector");
		--count;
	}

	/**
	 * @brief Removes all elements, leaving size() == 0.
	 */
	constexpr inline void clear() {
		count = 0;
	}

	/**
	 * @brief Grows or shrinks to newCount elements.
	 *
	 * Elements added by growing are value-initialized, same as std::vector::resize(). Shrinking,
	 * like erase()/erase_at()/pop_back(), just lowers count -- it doesn't clear the now-unused
	 * tail.
	 *
	 * @param newCount Desired element count.
	 * @throws std::length_error If @p newCount exceeds capacity M.
	 */
	constexpr inline void resize(short newCount) {
		throw_if<std::length_error>(newCount > M, "BoundedVector overflow");
		for(short index = count; index < newCount; ++index)
			components[index] = T{};
		count = newCount;
	}

	/**
	 * @brief Inserts item at index, shifting later elements over.
	 *
	 * Takes item by value rather than by reference: the shift below moves the backing storage,
	 * which would silently invalidate a reference into this same vector (e.g. `insert_at(i,
	 * vector[j])`) before it gets read.
	 *
	 * Named insert_at() rather than overloading insert() on (unsigned int, T): a literal 0 is
	 * simultaneously a valid index and a valid null-pointer-constant const_iterator, so an
	 * (unsigned int, T) overload and the (const_iterator, const T&) overload below would make
	 * `insert(0, item)` ambiguous.
	 *
	 * @param index Position to insert at, in [0, size()].
	 * @param item Value to insert.
	 * @throws std::out_of_range If @p index > size().
	 * @throws std::length_error If already at capacity M and @p index != size().
	 */
	constexpr inline void insert_at(unsigned int index, T item) {
		throw_if<std::out_of_range>(index > static_cast<unsigned int>(count), "BoundedVector insert index out of range");
		if(index == static_cast<unsigned int>(count))
			push_back(item);
		else {
			throw_if<std::length_error>(count >= M, "BoundedVector overflow");
			// memmove is a bulk byte copy -- only safe when T is trivially copyable. For
			// anything else (e.g. a type owning a resource), fall back to shifting one element
			// at a time via move-assignment, which respects T's real move semantics.
			if constexpr (std::is_trivially_copyable_v<T>)
				memmove(components + index + 1, components + index, (count - index) * sizeof(T));
			else
				for(unsigned int i = count; i > index; --i)
					components[i] = std::move(components[i - 1]);
			components[index] = std::move(item);
			count++;
		}
	}

	/**
	 * @brief Removes the element at index, shifting later elements down.
	 *
	 * Note for non-trivial T: like pop_back(), this only lowers count -- it doesn't destroy the
	 * now-unreachable last element, so any resources it holds aren't released until that slot is
	 * next overwritten (by push_back()/insert_at()/resize() growing back into it) or the vector
	 * itself is destroyed.
	 *
	 * Named erase_at() to match insert_at() -- see its doc comment for why it isn't an overload
	 * of erase() taking an index.
	 *
	 * @param index Position to remove, in [0, size()).
	 * @throws std::out_of_range If @p index >= size().
	 */
	constexpr inline void erase_at(unsigned int index) {
		throw_if<std::out_of_range>(index >= static_cast<unsigned int>(count), "BoundedVector erase index out of range");
		if(index != static_cast<unsigned int>(count) - 1) {
			if constexpr (std::is_trivially_copyable_v<T>)
				memmove(components + index, components + index + 1, (count - index - 1) * sizeof(T));
			else
				for(unsigned int i = index; i < static_cast<unsigned int>(count) - 1; ++i)
					components[i] = std::move(components[i + 1]);
		}
		count--;
	}

	/**
	 * @brief Inserts item before pos, matching std::vector::insert().
	 *
	 * Cheap to support since iterator is already a plain pointer into components: `pos -
	 * begin()` recovers the index insert_at() needs, and the result is just `begin() + that
	 * same index`.
	 *
	 * @param pos Position to insert before.
	 * @param item Value to insert.
	 * @return Iterator to the newly inserted element.
	 * @throws std::out_of_range If @p pos is out of range.
	 * @throws std::length_error If already at capacity M and @p pos != end().
	 */
	constexpr inline iterator insert(const_iterator pos, const T& item) {
		auto index = static_cast<unsigned int>(pos - begin());
		insert_at(index, item);
		return begin() + index;
	}

	/**
	 * @brief Removes the element at pos, matching std::vector::erase().
	 *
	 * @param pos Position to remove.
	 * @return Iterator to the element that followed the removed one (== end() if the last
	 * element was removed).
	 * @throws std::out_of_range If @p pos is out of range.
	 */
	constexpr inline iterator erase(const_iterator pos) {
		auto index = static_cast<unsigned int>(pos - begin());
		erase_at(index);
		return begin() + index;
	}

	/**
	 * @brief Iterator to the first element equal to value, or end() if none.
	 *
	 * std::vector itself has no find() member -- callers normally reach for the std::find()
	 * algorithm instead -- but BoundedVector has no sorted/hashed structure to search any faster
	 * than that same linear scan would, so it's exposed directly as a member for convenience,
	 * matching contains()/find() as found on std::basic_string and the associative containers.
	 *
	 * @param value Value to search for.
	 * @return Iterator to the first matching element, or end() if none is found.
	 */
	constexpr iterator find(const T& value) {
		for(short index = 0; index < count; ++index)
			if(components[index] == value)
				return components + index;
		return end();
	}

	/**
	 * @brief const overload of find().
	 */
	constexpr const_iterator find(const T& value) const {
		for(short index = 0; index < count; ++index)
			if(components[index] == value)
				return components + index;
		return end();
	}

	/**
	 * @brief True if any element equals value.
	 */
	constexpr bool contains(const T& value) const {
		return find(value) != end();
	}

	/**
	 * @brief Unchecked element access, matching std::vector::operator[]. Out-of-range @p index
	 * is undefined behavior.
	 *
	 * @param index Position to access, expected to be in [0, size()).
	 * @return Reference to the element at @p index.
	 */
	constexpr T& operator[](unsigned int index) {
		return components[index];
	}

	/**
	 * @brief const overload of operator[].
	 */
	constexpr const T& operator[](unsigned int index) const {
		return components[index];
	}

	/**
	 * @brief Bounds-checked element access, matching std::vector::at().
	 *
	 * @param index Position to access.
	 * @return Reference to the element at @p index.
	 * @throws std::out_of_range If @p index >= size().
	 */
	constexpr T& at(unsigned int index) {
		throw_if<std::out_of_range>(index >= static_cast<unsigned int>(count), "BoundedVector at index out of range");
		return components[index];
	}

	/**
	 * @brief const overload of at().
	 *
	 * @throws std::out_of_range If @p index >= size().
	 */
	constexpr const T& at(unsigned int index) const {
		throw_if<std::out_of_range>(index >= static_cast<unsigned int>(count), "BoundedVector at index out of range");
		return components[index];
	}

	/**
	 * @brief Iterator to the first element (== end() if empty).
	 */
	const constexpr T* begin() const { return components; }

	/**
	 * @brief Iterator one past the last element.
	 */
	const constexpr T* end() const { return components + count; }

	/**
	 * @brief Non-const overload of begin() const.
	 */
	constexpr T* begin() { return components; }

	/**
	 * @brief Non-const overload of end() const.
	 */
	constexpr T* end() { return components + count; }

	/**
	 * @brief Pointer to the underlying contiguous storage.
	 *
	 * `data()[0]` through `data()[size() - 1]` are valid; nothing is guaranteed about the
	 * remaining capacity() - size() slots.
	 */
	const constexpr T* data() const { return components; }

	/**
	 * @brief Non-const overload of data() const.
	 */
	constexpr T* data() { return components; }

	/**
	 * @brief True if size() == 0.
	 */
	constexpr bool empty() const { return count == 0; }

	/**
	 * @brief Current number of elements.
	 */
	constexpr short size() const { return count; }

	/**
	 * @brief Maximum number of elements this BoundedVector can ever hold -- the template
	 * parameter M.
	 */
	constexpr short capacity() const { return M; }

	/**
	 * @brief First element. Undefined behavior if empty, matching std::vector::front().
	 */
	constexpr const T& front() const { return components[0]; }

	/**
	 * @brief Non-const overload of front() const.
	 */
	constexpr T& front() { return components[0]; }

	/**
	 * @brief Last element. Undefined behavior if empty, matching std::vector::back().
	 */
	constexpr const T& back() const {
		return components[count - 1];
	}

	/**
	 * @brief Non-const overload of back() const.
	 */
	constexpr T& back() {
		return components[count - 1];
	}

	/**
	 * @brief Hash functor for use as a key in std::unordered_map/std::unordered_set.
	 *
	 * Only combines the first size() elements -- consistent with Equal, which only compares
	 * that same range, regardless of capacity M or whatever's left in the unused tail.
	 */
	struct Hash {
		size_t operator()(const BoundedVector& other) const {
			size_t hash = 0;
			for(short index = 0; index < other.count; ++index)
				hash += 5 * hash + other.components[index];
			return hash;
		}
	};

	/**
	 * @brief Equality functor for use alongside Hash as a key in
	 * std::unordered_map/std::unordered_set.
	 *
	 * Compares size() and then the first size() elements of each side; matches operator==.
	 */
	struct Equal {
		bool operator()(const BoundedVector& lhs, const BoundedVector& rhs) const {
			if(lhs.count != rhs.count)
				return false;
			for(short index = 0; index < lhs.count; ++index)
				if(!(lhs.components[index] == rhs.components[index]))
					return false;
			return true;
		}
	};

	/**
	 * @brief True if both vectors have the same size() and equal elements in [0, size()).
	 */
	constexpr bool operator==(const BoundedVector& other) const { return Equal{}(*this, other); }

	/**
	 * @brief Negation of operator==.
	 */
	constexpr bool operator!=(const BoundedVector& other) const { return !(*this == other); }

private:
	T components[M];
	short count = 0;
};
