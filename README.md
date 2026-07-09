# RadixMap.hpp

[API docs](https://lrmoorejr.github.io/radix-map/)

A sorted associative container -- a `std::map`-like alternative for simple, fixed-shape keys
(`std::string`, or any integral/floating-point type) that's **up to 5x faster than `std::map`** for
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

That same prefix-compressed tree also answers a query `std::map` has no clean way to express: every
key that starts with a given prefix, in one bounded descent rather than a linear scan (see
[Prefix search](#prefix-search)).

```cpp
for(auto &&[key, value] : ages.prefix_range("A"))  // just "Alice"
	std::cout << key << " = " << value << "\n";
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
- Header-only -- copy `RadixMap.hpp` and [`BoundedVector.hpp`](https://github.com/lrmoorejr/bounded-vector)
  (vendored alongside it in this repo) into your project and `#include "RadixMap.hpp"`
- `Key` must have a `RadixMapKeyTraits<Key>` specialization; built in for `std::string` and any
  integral/floating-point type (see [Custom key types](#custom-key-types) to add your own)
- `Value` must be copy-constructible (for `insert()`) and default-constructible (for
  `operator[]`, matching `std::map::operator[]`'s own requirement)
- Optional: [`Ensure.hpp`](https://github.com/lrmoorejr/ensure) for its `throw_if()` helper; if
  it's not present, RadixMap falls back to an equivalent local implementation

`BoundedVector.hpp` is what makes iteration (`begin()`/`find()`/`lower_bound()`/`upper_bound()`)
over a fixed-length `Key` (any integral/floating-point type) fully heap-allocation-free -- see
[Performance](#performance).

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
| `lower_bound(key)` | Returns an iterator to the first entry `>= key`, or `end()`. |
| `upper_bound(key)` | Returns an iterator to the first entry `> key`, or `end()`. |
| `prefix_range(prefix)` | Returns a range over every entry whose key starts with `prefix`, in ascending order; empty (not an error) if none match. See [Prefix search](#prefix-search). |
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

## Prefix search

`prefix_range(prefix)` returns every entry whose key starts with `prefix`, in ascending order:

```cpp
RadixMap<std::string, Endpoint> routes;
routes.insert("/api/users/list", ...);
routes.insert("/api/users/get",  ...);
routes.insert("/api/orders/list", ...);

for(auto &&[key, endpoint] : routes.prefix_range("/api/users/"))
    handle(key, endpoint); // visits "/api/users/get", "/api/users/list" -- nothing under /api/orders/
```

`std::map` has no clean equivalent: the usual workaround is calling `lower_bound(prefix)` for the
start and hand-constructing an upper bound by incrementing `prefix`'s last byte, which has no
valid answer when `prefix` ends in `0xFF`. RadixMap doesn't need the trick -- every prefix that
was ever a branch point between two or more keys already has its own node, so `prefix_range()` is
one descent to locate it (cost bounded by `prefix`'s length, not the map's size) followed by
walking exactly that subtree.

An empty prefix matches every entry (equivalent to `begin()`/`end()`), and a prefix matching
nothing returns an empty range rather than an error.

A prefix only means something for a `Key` type where a shorter value is itself a valid `Key` --
true for `std::string`, not for a fixed-width type like `double` (there's no way to express "the
leading 3 bytes of a double" as a `Key`). `prefix_range()` still compiles and runs for those, but
degenerates to at most a single entry, the same as `find()`.

## Performance

Measured with Catch2's benchmark harness (Release build, `-O3`; see `RadixMap-test.cpp`'s hidden
`[benchmark]`-tagged test cases -- run them with `./RadixMap-test "[benchmark]"`), inserting/
finding 20,000 keys. Speedup vs `std::map`:

| Operation | RadixMap vs `std::map` |
|---|---|
| String keys, insert | 3.2x faster |
| String keys, `contains()` | 5.4x faster |
| `double` keys, insert | 2.2x faster |
| `double` keys, `contains()` | 2.3x faster |

Full numbers, plus `std::unordered_map` for reference (it isn't sorted, so it's not solving the
same problem RadixMap is):

| Variant | String keys, insert | String keys, `contains()` | `double` keys, insert | `double` keys, `contains()` |
|---|---|---|---|---|
| `RadixMap` | ~111-118 ms | ~70-73 ms | ~101-104 ms | ~46-47 ms |
| `std::map` | ~366-369 ms | ~379-407 ms | ~215-234 ms | ~100-109 ms |
| `std::unordered_map` | ~110-119 ms | ~27-29 ms | ~88-89 ms | ~6.8-7.1 ms |

`contains()`/`at()`/`operator[]` return a bare `bool`/reference from a raw pointer-chase and never
allocate, regardless of `Key`. `find()`/`lower_bound()`/`upper_bound()` do more: each returns a
resumable iterator, so it has to decode the key and build a traversal stack on the way down, not
just answer yes/no -- so they cost more than `contains()`, by design, even in the best case. If
all you need is a yes/no or a value lookup, prefer `contains()`/`at()`/`operator[]`; reach for
`find()`/`lower_bound()`/`upper_bound()` when you actually need the resulting position (e.g. to
iterate onward from it, or because you need the first-`>=`/first-`>` semantics they alone provide).

On top of that fixed cost, `find()`/`lower_bound()`/`upper_bound()`'s traversal-stack storage is
heap-free (backed by the vendored `BoundedVector`) for a fixed-length `Key` like `double`, and
`std::vector`-backed for a variable-length one like `std::string` -- shrinking, but not
eliminating, their gap over `contains()` for `double` specifically. Measured (20,000 keys):

| | `contains()` | `find()` | `lower_bound()` |
|---|---|---|---|
| String keys | ~70-73 ms | ~452-461 ms | ~595-629 ms |
| `double` keys | ~46-47 ms | ~57-60 ms | ~460-480 ms |

For `double`, `find()` is only ~1.3x the cost of `contains()` -- the heap-free traversal state
closes most of the gap. For `std::string`, which can't use it, `find()` costs ~6.4x `contains()`.
`lower_bound()` costs more than `find()` for both key types regardless of allocation, since it
does genuinely more tree-descent/branch-scanning work per call; comparing `double` to
`std::string` within each column instead (not across the `contains()`/`find()`/`lower_bound()`
divide), `double`'s `find()` is still about 7.8x faster than `std::string`'s, and its
`lower_bound()` about 1.3x faster.

**A note on measuring this yourself:** these numbers only mean anything from an optimized build.
An unoptimized (`-O0`) build disproportionately penalizes the STL comparisons (their containers
lean on inlining far more than this header's own tree-walk code does), producing a misleadingly
one-sided picture. Always benchmark via a `CMAKE_BUILD_TYPE=Release` configure.

## License

Apache License 2.0 -- see [LICENSE](https://github.com/lrmoorejr/radix-map/blob/main/LICENSE).
