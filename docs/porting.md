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

## Functions (M6.6 part 8, group 4)

### What to write instead

| Kotlin | Astra | |
| --- | --- | --- |
| `fun interface Action { ... }` | `interface Action { ... }` | any interface with one abstract method takes a lambda (D28) |
| `tailrec fun walk(...)` | `tail fun walk(...)` | or no word: a call of itself as the last step is a loop anyway (D29) |
| `inline fun f(noinline a : () -> Unit)` | `inline fun f(a : () -> unit)` | no `noinline` or `crossinline`: worked out from the body (D59) |
| `infix fun Int.over(x : Int)` | `infix fun int.over(x : int)` | (D72) |
| `this@Outer`, `this@apply` | the same | (D57) |
| `block(obj)`, `obj.block()` for `T.() -> R` | the same | `with(obj, block)` still works (D58) |
| `1 until 5` | `1..<5` | `until` is an error saying so |

### What compiles but means something else

- **`this` in a block run on an object is that object**, in a method too, as in Kotlin: inside
  `other.run { this.name }` it is `other`'s name. The method's object is `this@Class` (D58).
- **A bare `return` in `forEach`, `map` and the other lambda functions of the standard
  library** leaves the function the call is in, as Kotlin's inline functions do; `return@forEach`
  goes on to the next item. Before, it was an error everywhere but `let`, `also` and `takeIf`.
- **A function calling itself as its last step does not run out of stack**: such recursion is a
  loop, and a test that relied on a stack overflow needs the call to do something after it.
- **A block given a type that takes nothing has no `it`.** `items.forEach { later { print(it) } }`
  prints the item, where `it` was null before (D55); `it` inside another lambda's `it` is an
  error asking for a name.
- **`to`, `downTo` and `step` are infix functions**, with infix precedence: `a downTo b step c`
  is `(a downTo b) step c`, below arithmetic and above comparisons, as in Kotlin.

## Errors (M6.6 part 8, group 5)

### What to write instead

| Kotlin | Astra | |
| --- | --- | --- |
| `throw Exception("...")` meaning a mistake | `throw IllegalStateException("...")` or another `RuntimeException` | a plain `Exception` is checked (D35) |
| `catch (e: Exception)` | `catch (e)` or the same | a bare catch takes every Exception, no Error (D151) |
| `catch (e: Throwable)` | the same | the only way to catch an Error besides naming it |
| `@Throws(IOException::class)` | `throws IOException` after the result type | required on a library's public functions (D54) |
| `inputStream.use { ... }` | the same | for an `AutoCloseable`; `extend` fits another class to it (D113) |
| `e.message` as a `String` | `e.message` as a `string?` | as in Kotlin, it may be null |

### What compiles but means something else

- **Checked exceptions are checked.** Code that does I/O in a callback (`button.onClick = {
  file.writeText(...) }`), a thread, a task or a timer catches the `IOException` inside it; the
  compiler names the place. What only calls its lambda (`forEach`, `map`, `use`, `useKey`)
  passes it on.
- **An override throws no more than what it replaces.** An interface whose implementations read
  files says so: `fun load() : string throws IOException`.
- **A bare `catch` lets an Error through**: a stack overflow, running out of memory, `assert`
  failing. Name it to catch it.
- **The VM's errors have Java's classes**: an index out of range is an
  `IndexOutOfBoundsException`, `!!` on null a `NullPointerException`, and so on; code that caught
  `Error` for them catches those, or a bare `catch`.

## Reflection, annotations and tests (M6.6 part 8, group 6)

### What to write instead

| Kotlin | Astra | |
| --- | --- | --- |
| `x::class`, `Box::class` | `x#`, `Box#` | a `Class`; `Box<int>#` with type arguments (D48) |
| `x::class.simpleName` | `x#.name` | `className` is gone too |
| `Box::class.memberProperties` | `Box#.fields` | a property is a field; `get(obj)`, `set(obj, v)` |
| `Box::class.memberFunctions` | `Box#.methods` | `call(obj, ...)` |
| `Box::class.constructors` | `Box#.constructors` | `call(...)` |
| `Box::class.supertypes` | `Box#.superclass`, `Box#.interfaces` | |
| `Shape::class.sealedSubclasses` | `Shape#.kinds` | of an enum class (D39) |
| `Class.forName(name)` | `Class.namedFrom(name, [A#, B#])` | among the classes the program lists (D50) |
| `prop.findAnnotation<Json>()` | `field.annotation<Json>()` | also on a class, a method, a parameter |
| `annotation class Json(val name: String)` | `annotation Json(val name : string)` | no `class` (D153) |
| `@Volatile`, `@Suppress(...)`, `@JvmField`, `@JvmName(...)` | delete it | an error saying so |
| `@Synchronized fun f()` | `fun f() = lock.useKey { ... }` | |
| `@JvmStatic` in a companion | the member in `static { }` | |
| `kotlin.test`: `@Test`, `assertEquals` | the same, run by `astra test` | `@BeforeTest`, `@AfterTest`, `@BeforeAll`, `@AfterAll`, `@Timeout`, `@TempDir` |

### What compiles but means something else

- **A call by name wraps what it throws** in an `InvocationTargetException`, which is checked:
  `m.call(obj)` needs a `try` or a `throws InvocationTargetException`, as Java's
  `Method.invoke` does, and so do a field's `get` and `set`, which may run a getter or setter.
  An Error (a stack overflow, an `AssertionError`) is not wrapped.
- **Only what the code may use is listed**: a class's private fields are not among its
  `fields` except in its own code, where Kotlin's reflection lists them all.
- **A field's type is a `Type`, a class is a `Class`**: `field.type == int#.type`, not
  `== Int::class`.
- **`astra test` runs a file as a program first**: what its top level does runs before the
  tests.

## Concurrency (M6.6 part 8, group 7)

### What to write instead

| Kotlin | Astra | |
| --- | --- | --- |
| `launch { }`, `async { }`, `startCoroutine` | `task { }` | gives a `Job<T>` of what the block returns (D81) |
| `suspendCoroutine { c -> ... c.resume(v) }` | `suspendTask { resume -> ... resume(v) }` | `resume.fail(e)` for `resumeWithException` |
| `delay(16)` | `delay(16.milliseconds)` | every time is a `Duration` (D90) |
| `Thread.sleep(100)`, `join(500)` | `Thread.sleep(100.milliseconds)`, `join(timeout = 500.milliseconds)` | `kotlin.time`'s units: `.seconds`, `.minutes`... |
| `TimeUnit.SECONDS.toMillis(2)` | `2.seconds.inWholeMilliseconds` | |
| `thread { }` | the same | gives a `Job` of what the block returns (D117) |
| `Future`, `CompletableFuture`, `Deferred` | `Job<T>` | `get()`, `get(timeout)`, `then { }`, `onError { }`, `complete`, `fail`, `cancel` (D89) |
| `CompletableFuture.allOf(...)`, `awaitAll()` | `Job.all(jobs)` | a Job of all the results, in order |
| `synchronized(x) { }`, `ReentrantLock`, `withLock { }` | `val lock = Lock()`, `lock.useKey { }` | data reached only with the key: `LockBox(value)` (D86) |
| `lock.wait()`, `notifyAll()`, `Condition` | `waitUntil { condition }` inside `useKey` | checked again whenever a key is given back |
| `BlockingQueue`, `kotlinx.coroutines.channels.Channel` | `Channel<T>(capacity = n)`, `Channel<T>.unbounded()` | `send`, `receive`, a `for` over it (D87) |
| `Executors.newFixedThreadPool(n)` | `ThreadPool(threads = n)` | `submit`, `after`, `every`, `close` (D88) |
| `Executors.newSingleThreadExecutor()` | `JobQueue(name)` | |
| `ScheduledExecutorService.scheduleAtFixedRate` | `pool.every(period) { }` | |
| `CountDownLatch(n)` | `Countdown(n)` | `countDown()`, `await(timeout?)`, `count` (D118) |
| `AtomicInteger`, `AtomicReference`, `AtomicBoolean` | `Atomic(value)` | `incrementAndGet()` is `increment()`, `getAndSet` is `exchange` (D85) |
| `@Volatile var count` changed by several threads | `atomic var count` | the field changes in one step in a parallel section (D92) |
| `ConcurrentHashMap`, `ConcurrentHashMap.newKeySet()` | `SharedMap`, `SharedSet` | for parallel sections and threads (D94) |
| `list.parallelStream().collect(...)` | `gather(into, each, merge)` | each core fills a container of its own (D93) |
| `ForkJoinPool`, `RecursiveTask` | `parallel({ a }, { b })`, `list.splitAt(mid) { l, r -> }` | views of a list, each one function's (D96) |
| `Arrays.parallelSort(a)` | `list.parallelSort()` | |
| `sequence { yield(x) }`, `generateSequence`, `asSequence()` | the same | (D98) |

### What compiles but means something else

- **An error in a task is kept in its Job.** `task { }` is Kotlin's `async`, not `launch`: an
  error it does not catch fails the job, and is seen through `get()` or `onError { }`. A
  `thread { }` also reports it, as Java does for a thread.
- **`Job.all` and `Job.any` give Jobs**, not lists: `Job.all(jobs).get()` waits for them all.
- **Threads take turns.** One thread runs Astra code at a time, so threads overlap waiting,
  not computing. Work that is to use the cores goes to a parallel section or to workers.
- **`fold` on a parallel stream goes over the items in order, on one core**: what it combines
  need not be associative. `reduce` is dealt out to the cores.
- **A parallel section changes only its own items.** Code that adds to a shared list from
  `forEach` gets an error the first time it tries; it returns its results (`map`, `gather`)
  or uses `Atomic`, `atomic var`, `SharedMap` or `SharedSet`.
- **A sequence's block waits only at `yield`**: a `delay` or another suspend call there is a
  compile error, where Kotlin's `@RestrictsSuspension` gives a less direct one.
- **Frozen data sent to a worker is shared**, not copied: `table.frozen()` sent to eight
  workers is one copy. `post(bytes, handOver = true)` leaves the sender's buffer empty.
- **A `Thread` made with a lambda takes it in parentheses**: `Thread({ work() }, "name")`.

### What the compiler says

`Future`, `CompletableFuture`, `Executors`, `ExecutorService`, the `BlockingQueue` family,
`CountDownLatch`, the `Atomic*` classes, `TimeUnit`, `synchronized`, `ReentrantLock`,
`startCoroutine`, `suspendCoroutine`, `Continuation` and `EmptyCoroutineContext` are each an
error that names what Astra has in their place. A wait given a bare number (`Thread.sleep(100)`)
says to write `100.milliseconds`; a `Channel<T>()` without a capacity says to choose one; and
`lock.lock()` or `lock.withLock { }` says to use `useKey`.

## Files and input and output (M6.6 part 8, group 8)

### What to write instead

| Kotlin | Astra | |
| --- | --- | --- |
| `File(p)`, `Paths.get(p)`, `Path.of(p)` | `Path(p)` | one type for a path (D107) |
| `File(dir, name)`, `dir.resolve(name)` | `Path(dir, name)`, `dir.resolve(name)` | |
| `Files.readAllBytes(p)`, `Files.exists(p)` and the rest | `p.readBytes()`, `p.exists()` | `Files.x(p)` is `p.x()` (D131) |
| `file.path`, `file.toString()` | `path.text`, `"$path"` | (D137) |
| `file.parentFile`, `file.parent` as text | `path.parent`, `path.parent?.text` | null at the top (D128) |
| `file.absolutePath`, `absoluteFile` | `path.absolute.text`, `path.absolute` | (D135) |
| `file.canonicalFile`, `toRealPath()` | `path.canonical()` | an `IOException` if it is not there |
| `file.isFile`, `isDirectory` | `path.isFile()`, `path.isDirectory()` | what asks the disk is a function (D150) |
| `file.length()` | `path.size()` | |
| `file.list()`, `listFiles()` | `dir.list()` | gives `Path`s; `.map { it.name }` for names (D138) |
| `walkTopDown()`, `Files.walk(p)` | `dir.walk()` | a sequence of every path below |
| `mkdirs()`, `Files.createDirectories(p)` | `mkdir()` | `mkdir(parents = false)` for just one (D126) |
| `renameTo(b)`, `Files.move(a, b)` | `a.moveTo(b)` | throws where it cannot (D129) |
| `Files.createTempDirectory(prefix)` | `Path.createTempDirectory(prefix)` | and `createTempFile(prefix, suffix)` |
| `file.inputStream()`, `bufferedReader()` | `path.reader()` | a `Reader`, bytes and text (D119) |
| `file.outputStream()`, `bufferedWriter()`, `printWriter()` | `path.writer(append = ...)` | a `Writer` |
| `InputStream`, `OutputStream` | `Reader`, `Writer` | |
| `Charsets.UTF_8`, `StandardCharsets.ISO_8859_1` | `Charset.utf8`, `Charset.latin1` | also `ascii`, `utf16le`, `utf16be` (D127) |
| `s.toByteArray(cs)`, `String(bytes, cs)` | `s.toBytes(cs)`, `bytes.decodeToString(cs)` | |
| `ProcessBuilder(cmd).directory(d).start()` | `Process(cmd..., directory = d)` | `input`, `output`, `error` as named arguments (D91) |
| `redirectErrorStream(true)` | `error = toOutput` | |
| `redirectOutput(file)` | `output = path` | |
| `process.inputStream`, `outputStream`, `errorStream` | `process.output`, `process.input`, `process.error` | named for what the program does |
| `process.destroy()`, `exitValue()` | `process.kill()`, `process.exitCode` | |
| run, read the output, wait | `Process.run(cmd...)` | gives `exitCode`, `output` and `error` as text |
| `socket.getInputStream()`, `getOutputStream()` | `socket.reader`, `socket.writer` | (D120) |
| `socket.soTimeout = 5000` | `socket.readTimeout = 5.seconds` | past it a `SocketTimeoutException` |

### What compiles but means something else

- **What asks the disk is a function**: `path.isDirectory()`, `path.size()`, so a property never
  throws or touches the disk. Ported `file.isDirectory` without parentheses is the function, an
  error wherever a bool is wanted.
- **Failing throws.** `mkdir`, `delete`, `moveTo` and `list` throw an `IOException` saying why,
  where Java's `File` gave false or null; `deleteIfExists()` gives whether it was there.
- **`list()` gives Paths**, not names.
- **Text is UTF-8 unless a Charset says otherwise**, and a charset argument does what it says:
  ported `Charsets.UTF_8` is deleted or written `Charset.utf8`, and `ISO_8859_1` really reads
  Latin-1.
- **A Reader and a Writer carry bytes and text alike**, unlike Java's, and read ahead already.
- **A line read lazily** (`lineSequence()`, `useLines`) that fails throws an
  `UncheckedIOException`, as Java's `Files.lines` does.
- **`Process.run` gives the program nothing to read** unless `input` says otherwise.
- **A socket's reads block their thread**, even in a task: a server serves each connection from
  a thread or a pool's job.

### What the compiler says

`File`, `Files`, `Paths`, `InputStream`, `OutputStream`, `BufferedReader`, `BufferedWriter`,
`PrintWriter`, `ProcessBuilder`, `Charsets` and `StandardCharsets` are each an error naming what
to write, and so are Java's members of a file, a stream, a process and a socket that Astra
names otherwise (`parentFile`, `absolutePath`, `listFiles`, `mkdirs`, `renameTo`, `toFile`,
`inputStream`, `destroy`, `soTimeout` and the rest) and `toByteArray`.

## Graphics and Jade (M6.6 part 8, group 9)

### What to write instead

| Kotlin and JadeUI | Astra | |
| --- | --- | --- |
| `Dimension(w, h)` | `Size(w, h)` | a value type (D102) |
| `Rectangle(x, y, w, h)` | `Rect(x, y, w, h)` | a value type; `Rectangle(w, h)` is `Rect(0, 0, w, h)` (D104) |
| `d.width = w` on a shared `Dimension` | the same, on the place that holds it | a value is copied where Java's object was shared |
| `color.red` (an int), `color.getRed()` | `color.red` | a byte, which goes where an int does |
| `color.rgb`, `getRGB()` | `color.argb` | (D99) |
| `Color(r, g, b)` with a part over 255 | the same, in range | an `IllegalArgumentException`, as Java's |
| `Font("Arial", Font.BOLD, 14)` | `Font("Arial", 14, bold = true)` | the style said by name (D144) |
| `font.deriveFont(Font.ITALIC, 18f)` | `font.copy(size = 18, italic = true)` | |
| `FontMetrics`, `fm.stringWidth(s)`, `fm.height` | `font.width(s)`, `font.lineHeight` | the font measures itself (D101) |
| `fm.charWidth(c)` | `font.width(c)` | |
| `FontMetrics(font, scale)`, `deviceWidth(s)` | `font.width(s, scale)` | in a dense screen's pixels |
| `BufferedImage(w, h, TYPE_INT_ARGB)` | `Image(w, h)` | four bytes a pixel, always (D106, D122) |
| `image.getRGB(x, y)` | `image[x, y]`, `image[x, y].argb` | a `Color` (D141) |
| `image.setRGB(x, y, argb)` | `image[x, y] = Color(argb, hasAlpha = true)` | |
| `image.getSubimage(x, y, w, h)` | `image.crop(x, y, w, h)` | a new image, not a view |
| `image.createGraphics()` | `image.canvas()` | drawn in memory (D123) |
| `ImageIO.read(file)`, `ImageIO.write(img, "png", file)` | `Image.load(path)`, `image.save(path)` | PNG |
| `g.drawImage(img, x, y, null)` | `canvas.draw(img, x, y)` | and `draw(img, x, y, w, h, alpha = ...)` |
| `g.fill(shape)`, `g.draw(shape)` | `canvas.fill(Rect(...))`, `canvas.draw(Oval(...))` | `Rect`, `Oval`, `Line` |
| JadeUI `canvas.style`, `sub(x, y, w, h, style)` | `owner!!.paintStyle`, `sub(x, y, w, h)` | the style is on the component (D132) |
| JadeUI `OwnedContent` | `Content`, which has `var owner` | every content has one |
| JadeUI `Grid` (the layout) | `GridPane` | `grid { }` is unchanged |
| `surface.toPicture()`, `Picture` | `surface.toImage()`, `Image` | one in-memory picture type (D122) |
| a 2D array of cells | `Grid<T>(w, h)`, `grid[x, y]` | packed by `T` (D155) |
| `x = x.trim()` | `x .= trim()` | (D156) |
| several named arguments passed along together | `params(...)` and `...bundle` | (D143) |
| Kotlin's secondary `constructor(...) : this(...)` | the same | a class or value type with a header |

### What compiles but means something else

- **A colour's parts are bytes**, 0 to 255, and go where an int does: `color.red + color.green`
  is an int.
- **The geometry is copied, not shared**: `val r = comp.bounds; r.translate(1, 1)` changes `r`,
  not the component, as with every value type.
- **An `Image` is drawn on in memory**, on the processor, and only copied to the graphics card
  when it is drawn there (the part that changed). A `Surface` is the picture for heavy drawing
  off the screen.
- **`Font` has no scale**: a canvas draws it at its own, and `font.width(text)` is at scale 1.
  On a dense screen, `font.width(text, scale)` gives the screen's pixels.
- **`image.pixels` may only be read**; `editPixels { pixels -> }` changes them.
- **`Grid.bytes` is a copy** of the cells' storage.
- **A grid's rows and columns are added by making a new grid**: `g.appendRows(2)` alone does
  nothing to `g` (a compile error); `g .= appendRows(2)`.

### What the compiler says

`Dimension`, `Rectangle`, `BufferedImage`, `Picture`, `FontMetrics`, `Bitmap`, `Graphics` and
`Graphics2D` are each an error naming what to write, and so are Java's members of a colour, a
font, a canvas, an image, a size, a rectangle and a point that Astra names otherwise (`getRed`,
`rgb`, `deriveFont`, `Font.bold`, `stringWidth`, `textFont`, `drawImage`, `getFontMetrics`,
`getRGB`, `setRGB`, `getSubimage`, `setSize`, `getBounds` and the rest), Java's
`Font(name, style, size)`, and a grid's new rows left unused.
