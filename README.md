# RadixMap.hpp

[API docs](https://lrmoorejr.github.io/radix-map/)

A sorted associative container -- a `std::map`-like alternative for simple, fixed-shape keys
(`std::string`, or any integral/floating-point type) that's up to 5x faster than `std::map` for
both insert and lookup while staying just as sorted -- see [Performance](#performance). Built on
a PATRICIA/radix-tree structure that compresses shared key prefixes instead of storing every
key's bytes in full at every node.

```cpp
#include "RadixMap.hpp"

RadixMap<std::string, int> ages;
ages["Alice"] = 30;
ages.insert("Bob", 25);

ages.contains("Alice");          // true
ages.at("Bob");                  // 25
ages["Carol"];                   // 0 (default-inserted)

for(auto &&entry : ages)         // iterates in ascending key order
	std::cout << entry.first << " = " << entry.second << "\n";

ages.erase("Alice");
```

## Why not just std::map?

`std::map` compares whole keys at every node on the way down a red-black tree. RadixMap instead
stores each node's key bytes only once -- a node holding `"http://"` is shared by every key that
starts with it -- and picks branches by an adaptively-sized bit-mask over the next byte, rather
than a full 256-way fan-out per node. The result is still fully sorted (iteration, like
`std::map`, visits keys in ascending order) but faster for both insert and lookup on the kinds of
keys it's built for. See [Performance](#performance) for real numbers.

## Requirements

- C++20 or later
- Header-only -- copy `RadixMap.hpp` into your project and `#include` it
- `Key` must have a `RadixMapKeyTraits<Key>` specialization; built in for `std::string` and any
  integral/floating-point type (see [Custom key types](#custom-key-types) to add your own)
- `Value` must be copy-constructible (for `insert()`) and default-constructible (for
  `operator[]`, matching `std::map::operator[]`'s own requirement)
- Optional: [`Ensure.hpp`](https://github.com/lrmoorejr/ensure) for its `throw_if()` helper; if
  it's not present, RadixMap falls back to an equivalent local implementation

## API

| Call | Behavior |
|---|---|
| `RadixMap<Key, Value>()` | Default-constructs, empty. |
| `insert(key, value)` | Inserts, or overwrites the existing value if `key` is already present. |
| `contains(key)` | True if `key` has been inserted (and not since erased). |
| `at(key)` | Bounds-checked access; throws `std::out_of_range` if `key` isn't present. |
| `operator[](key)` | Returns a reference to `key`'s value, default-constructing and inserting it first if missing -- matches `std::map::operator[]`. |
| `erase(key)` | Removes `key` if present; returns 1 if removed, 0 if not found. See [erase() is a tombstone](#erase-is-a-tombstone). |
| `size()`, `empty()` | Current entry count, and whether it's 0. |
| `begin()`, `end()`, `cbegin()`, `cend()` | Forward iteration in ascending key order. See [Iterating](#iterating) for a range-for gotcha. |
| `find(key)` | Returns an iterator to `key`'s entry, or `end()`. |
| Copy/move construction and assignment | Copying deep-clones the whole tree; moving is cheap (pointer swap). |

## Custom key types

`RadixMapKeyTraits<Key>` is the customization point that turns a `Key` into a byte string RadixMap
can compare with `memcmp` in a way that agrees with `Key`'s own natural ordering. Built-in
specializations cover `std::string` (a zero-copy view over the string's own bytes) and any
integral/floating-point type (encoded to a big-endian, byte-comparable bit pattern -- signed
integers and floats each need different bit-twiddling to sort correctly; see the header for the
reasoning). To support another `Key` type, specialize it yourself:

```cpp
template<>
struct RadixMapKeyTraits<MyKeyType> {
	using Encoded = /* a type with .data() (const unsigned char*) and .size() (size_t) */;
	static Encoded encode(const MyKeyType& key) noexcept;
	static MyKeyType decode(const unsigned char* bytes, size_t length);
};
```

Using a `Key` with no specialization is a clear compile error (a `static_assert`), not a wall of
unrelated template errors.

One consequence of encoding by raw bits: `0.0` and `-0.0` encode to different byte patterns and
are treated as two distinct keys, unlike `std::map<double,...>` (whose default `std::less`
treats them as equivalent). This is intentional, not a bug -- it falls directly out of the
encoding scheme.

## erase() is a tombstone

`erase()` clears the entry's value in place but leaves the tree's internal structure (the
compressed prefixes and branches) untouched -- it doesn't reclaim the compression a full
structural merge would. This is a deliberate simplification, not an oversight: real node-merging
on erase is a separate, harder feature. In practice this means a RadixMap that erases many more
entries than it currently holds will use more memory than the current entry count alone would
suggest, until (if ever) a future `compact()`-style pass is added.

## Iterating

`operator*()` returns each entry as a small proxy (`Entry{ Key first; Value& second; }`), not a
reference into a stored `(Key, Value)` pair -- RadixMap doesn't actually store pairs, only path
bytes, and reconstructs `Key` on the fly as it walks. That means range-for needs `auto &&` or
plain `auto`, not `auto &`:

```cpp
for(auto &&entry : ages) { ... }   // OK
for(auto entry : ages) { ... }     // also OK -- entry.second is itself a reference
for(auto &entry : ages) { ... }    // does not compile: a non-const lvalue reference
                                    // can't bind to a prvalue
```

`entry.second` is a real reference either way, so mutating it (e.g. `entry.second = newValue;`)
works identically regardless of which of the first two forms you use.

Iteration is forward-only -- there's no `operator--`/`rbegin()`/`rend()` -- since the underlying
tree has no parent pointers to walk backwards through.

## Performance

Measured with Catch2's benchmark harness (Release build, `-O3`; see `RadixMap-test.cpp`'s hidden
`[benchmark]`-tagged test cases -- run them with `./RadixMap-test "[benchmark]"`), inserting/
finding 20,000 keys:

| Variant | String keys, insert | String keys, find | `double` keys, insert | `double` keys, find |
|---|---|---|---|---|
| `RadixMap` | ~216-226 ms | ~69.5-70 ms | ~193-195 ms | ~46.5-48 ms |
| `std::map` | ~361-365 ms | ~373-395 ms | ~208-218 ms | ~103-106 ms |
| `std::unordered_map` | ~108-109 ms | ~29-34 ms | ~89 ms | ~6.3-6.8 ms |

RadixMap consistently beats `std::map` by roughly 1.1x (double insert) to 5.4x (string find).
`std::unordered_map` numbers are included too, but only for reference -- it isn't sorted, so
it's not solving the same problem RadixMap is.

**A note on measuring this yourself:** these numbers only mean anything from an optimized build.
An unoptimized (`-O0`) build disproportionately penalizes the STL comparisons (their containers
lean on inlining far more than this header's own tree-walk code does), producing a misleadingly
one-sided picture. Always benchmark via a `CMAKE_BUILD_TYPE=Release` configure.

## License

Apache License 2.0 -- see [LICENSE](https://github.com/lrmoorejr/radix-map/blob/main/LICENSE).
