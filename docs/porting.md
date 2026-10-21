# Porting Kotlin to Astra

What to change when Kotlin code (JadeUI, the apps) is brought over, and what to look at that
compiles but means something else. Grouped as M6.6's second half builds the language choices
([language-choices.html](language-choices.html), [gui-plan.md](gui-plan.md) D19 to D153); each
group adds its section.

## Numbers and strings (M6.6 part 8, group 1)

### What to write instead

| Kotlin | Astra | |
| --- | --- | --- |
| `Int`, `Long` | `int` | 64 bits, signed (D19) |
| `Short`, `Int` in a large array or a binary format | `int16`, `int32`, `uint16`, `uint32` | storage widths: compact where kept, read as `int` (D148) |
| `Double`, `Float` | `float` (`float32` to keep it in 4 bytes) | |
| `Byte` | `byte` | **0 to 255**, not -128 to 127 (D19) |
| `Char` | `char` | one code point, not a UTF-16 unit (D24) |
| `Int.MAX_VALUE`, `Long.MIN_VALUE` | `int.maxValue`, `int.minValue` | the error names what to write (D76) |
| `Double.MAX_VALUE`, `Double.MIN_VALUE` | `float.maxValue`, `float.minPositive` | Kotlin's `MIN_VALUE` is the smallest above 0; `float.minValue` is the most negative |
| `Double.NaN`, `Double.POSITIVE_INFINITY` | `float.nan`, `float.positiveInfinity` | |
| `x ushr n` | `x >>> n` | (D44) |
| `x shl n`, `x shr n`, `and`, `or`, `xor`, `inv()` | `<<`, `>>`, `&`, `\|`, `^`, `~` | |
| `n.toChar()`, `Char(n)` | `n.char` | (D142) |
| `c.code`, `c.toInt()` | `c.code` | |
| `c.uppercaseChar()`, `c.uppercase()` | `c.upper()` | a char, the char itself where the upper case is longer (D74) |
| `c.digitToInt()`, `c.isLetter()` | the same | |
| `n.toByte()` | `n.toByteWrapped()` | Kotlin's keeps the low 8 bits; Astra's `toByte()` is an error past 0 to 255 |
| `b.toInt() and 0xFF` | `b.toInt()` | a byte is already 0 to 255 |
| `buf.getInt(at)`, `toUInt()` on 4 bytes | `buf.int32(at)`, `buf.uint32(at)` | least significant byte first unless `bigEndian = true` |
| `'text'` in a string literal | `"text"` | single quotes hold one character only (D25) |
| `"""cost: ${'$'}5"""` | `$$"""cost: $5"""` | `$$name` read, a lone `$` text (D112) |
| `Objects.hash(a, b)`, `31 * h + x.hashCode()` | `hash(a, b)` | wraps at 32 bits inside (D148) |

### What compiles but means something else

- **Overflow is an error.** Kotlin's `Int` wraps silently past 2,147,483,647; Astra's `int` is 64
  bits and an answer past its range is an error (D27). Code that counts on wrapping says so:
  `a &+ b`, `a &- b`, `a &* b`, and `.wrap32()` for Kotlin's 32-bit result. Look for:
  - hash loops, `h = 31 * h + c`: write `h = (31 &* h &+ c.code).wrap32()`, or use `hash(...)`;
  - random number generators and checksums multiplying large numbers;
  - `ushr` on a negative number, which in Kotlin works on 32 bits: `x ushr 16` of a negative
    `Int` is `(x & 0xFFFFFFFF) >>> 16` in Astra, or `x.toUint32Wrapped() >>> 16`;
  - masks written for 32 bits (`and 0xFFFFFFFF`), and `inv()` of a 32-bit value;
  - rotations: `Integer.rotateLeft(x, n)` is `x.rotateLeft32(n)`.
- **A byte is 0 to 255.** Kotlin's `Byte` is signed: a test such as `b < 0` to find the high
  bit becomes `b >= 128`, and `b.toInt()` gives 0 to 255 where Kotlin gave -128 to 127. Byte
  arithmetic gives an int, so `bytes[i] = bytes[i] + 1` needs `.toByte()`, as in Kotlin.
- **A string's positions count characters, not UTF-16 units.** An emoji or another character
  past U+FFFF is one char in Astra and two in Kotlin, so `size`, `s[i]` and slices
  agree with Kotlin on text below U+10000 only.
- **A char is not a string**, but `'c' == "c"` is true, and they are one map key (D25).
  `c + c` is a string; `c + 1` the next char; `c - 'a'` an int.

### What the compiler says

A ported line that uses a name Astra does not have is a compile error naming what to write:
`Int.MAX_VALUE`, `toChar()`, `char(n)`, `chr(n)`, `ord(c)`, `'ab'`, a constant whose arithmetic
overflows, a char compared with a string literal of another length.

## Equality and collections (M6.6 part 8, group 2)

### What to write instead

| Kotlin | Astra | |
| --- | --- | --- |
| `List<T>` | `List<T>` | read-only through that name (D32) |
| `MutableList<T>`, `MutableMap<K, V>`, `MutableSet<T>` | `open List<T>`, `open Map<K, V>`, `open Set<T>` | the compiler says so; a list made with `[...]` or `listOf` is open |
| `MutableList<in Button>` | `open List<accepts Button>` | (D34) |
| `toMutableList()`, `toMutableMap()`, `toMutableSet()` | `toOpenList()`, `toOpenMap()`, `toOpenSet()` | (D46) |
| `s.length`, `list.count()` as a size | `s.size` | every count of things is `size` (D130); `File.length()` is `size()` |
| `s.substring(a, b)`, `list.subList(a, b)`, `slice(a..b)` | `s[a..<b]`, `list[a..<b]`, `list[a..b]` | a slice, a new string or list (D147) |
| `s.substring(a)`, `list.drop(a)` as a slice | `s[a..]` | |
| `list.last()`, `list[list.size - 2]` | `list[^1]`, `list[^2]` | `^k` counts from the end (D146) |
| `map.containsKey(k)`, `k in map` | `map.keys.contains(k)`, `k in map.keys` | `in` a map asks for a whole entry (D140) |
| `map.containsValue(v)` | `map.values.contains(v)` | |
| `for ((k, v) in map)` | `for (k, v) in map` | a `for` over a map gives its entries (D64) |
| `joinToString(...)` | `join(...)` | the same arguments (D74) |
| `uppercase()`, `lowercase()` | `upper()`, `lower()` | (D74) |
| `toDouble()`, `toLong()`, `roundToLong()` | `toFloat()`, `toInt()`, `roundToInt()` | (D74) |
| `override fun equals(other: Any?)` | `op ==(other : any?) : bool` | with `fun hashCode() : int` beside it, both or neither (D26) |
| `listOf(1, 2)` as a map key | `listOf(1, 2).frozen()` | a key cannot change (D26) |

### What compiles but means something else

- **A name written `List<T>` may only be read.** A parameter, field or local written
  `List<T>`, `Map<K, V>` or `Set<T>` that the code adds to or removes from is a compile error
  until it is written `open`. What is made in the function (`[...]`, `listOf`, `filter`, `map`,
  `sorted`, `toList`, slices) is open already.
- **A position is never negative.** `items[-1]` is an error, not the last item; that is
  `items[^1]`. A slice past the end is an error too; `take(n)` and `drop(n)` take as many as
  there are.
- **A class's own `==` and `hashCode` decide map keys.** An object of such a class finds the
  entry of an equal one, where before it was a key as itself (Jade's `Color(255, 0, 0)` finds
  the entry of `Color.red`).
- **A key cannot change.** A mutable list, map, set or `Bytes` used as a key, a data class with
  a `var`, or a class whose `==` or `hashCode` reads a `var` field, is refused: by the compiler
  where the type says so, and when the program runs otherwise. Freeze the key, or key by a
  value that cannot change (an id, a string).
- **A map's `keys` and `entries` are live**: they show the map as it is, later changes too, and
  cannot be changed through. Copy with `toList()` or `toSet()` to keep them as they were.
- **A `for` over a set sees it as it is**: an item added while it runs is reached, where Kotlin
  throws `ConcurrentModificationException`.
- **`Pair`, `Triple`, map entries and `IndexedValue` are values**, copied where they go and equal
  by what they hold (D73); `===` on them is a compile error, since a value has no identity.

### What the compiler says

A ported line using a name that went is an error naming what to write: `length`, `substring`,
`subList`, `slice`, `containsKey`, `containsValue`, `has`, `joinToString`, `uppercase`,
`lowercase`, `toDouble`, `toLong`, `toMutableList`, `push`, `pop`, `each`, `len`, `str`, `keys()`,
`values()`, `entries()`, `MutableList`; and so is a change through a read-only name, a key that
can change, and an `==` without a `hashCode`.

## Types and declarations (M6.6 part 8, group 3)

### What to write instead

| Kotlin | Astra | |
| --- | --- | --- |
| `sealed class Event` | `enum class Event` | its kinds extend it, in its file (D39) |
| `sealed interface Shape` | `enum interface Shape` | |
| `enum class Color { RED, GREEN }` | `enum Color { red, green }` | the error says so |
| `companion object { ... }` | `static { ... }` | one block a class (D67) |
| `val x get() = ...` and `var x ... set(v) { }` under the property | the same | `get x()` as a member of its own is an error (D68) |
| `lateinit var x : T` | the same | not for `int`, `bool` or a value type (D36) |
| `val x by lazy { ... }` | `lazy val x = ...` | also `lazy var`; `by lazy` is not accepted (D37) |
| `typealias Grid = List<List<Int>>` | `type Grid = List<List<int>>` | (D41) |
| `object Registry { }` | the same | made when first used, as in Kotlin (D66) |
| `class Car : Weighed` where `Car` is someone else's | `extend Car : Weighed { ... }` | getters bridge names that differ (D33) |
| `data class Point3(...) : Point(...)` | `data Point3(val x, val y, val z) : Point(x, y)` | only a data class extends one, its header starting with the parent's fields (D65) |
| `super.x = value` in an overriding setter | the same | |

### What compiles but means something else

- **A `when` covers every kind.** Over an enum, an enum class or interface, a bool or a union,
  a `when` that leaves a kind out is an error even as a statement; Kotlin asks that only of a
  `when` that gives a value. Add the kinds or an `else`.
- **Two values of unrelated types make a union.** `flag ? 1 : "one"` is an `int | string`, and
  so is the item type of `[1, "one"]`: what only one of them has needs an `is` first. Code that
  used such lists as bags of anything writes `List<any>` and casts, as Kotlin's `List<Any>`.
- **A property overridden in part.** A subclass that changed only the setter of a property now
  writes the property whole, with `get() => super.name` for the half it keeps.
- **A property that could only be set** has no Kotlin form; it becomes a function, or a
  property with a getter too.
- **Extension functions follow imports.** One declared in another folder is in reach only of
  files importing that folder.
