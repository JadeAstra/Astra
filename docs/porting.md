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
  past U+FFFF is one char in Astra and two in Kotlin, so `length`, `s[i]` and `substring`
  agree with Kotlin on text below U+10000 only.
- **A char is not a string**, but `'c' == "c"` is true, and they are one map key (D25).
  `c + c` is a string; `c + 1` the next char; `c - 'a'` an int.

### What the compiler says

A ported line that uses a name Astra does not have is a compile error naming what to write:
`Int.MAX_VALUE`, `toChar()`, `char(n)`, `chr(n)`, `ord(c)`, `'ab'`, a constant whose arithmetic
overflows, a char compared with a string literal of another length.
