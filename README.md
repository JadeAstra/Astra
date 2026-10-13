# Astra

Astra is a small object-oriented scripting language. Source code is compiled to bytecode and
run on a stack-based virtual machine with a garbage collector. You can run a script directly,
or compile it ahead of time to a bytecode file and run that.

The implementation is C++20 with no dependencies beyond the standard library.

```astra
class Animal {
  fun init(name) {
    this.name = name
  }
  fun speak() => "${this.name} makes a sound"
}

class Dog : Animal {
  override fun speak() => super.speak() + " - woof!"
}

for pet in [Animal("Generic"), Dog("Rex")] {
  print(pet.speak())
}
```

## Building

Windows with Visual Studio 2022 (C++ workload):

```
build.bat
```

This finds the MSVC toolchain, configures with the CMake and Ninja that ship with Visual
Studio, and produces `build\astra.exe` with a copy of `lib` beside it. Any other CMake setup
works too (`cmake -S . -B build && cmake --build build`); GCC and Clang flags are in
`CMakeLists.txt` but only the MSVC build has been tested.

## Usage

```
astra                                 start the interactive prompt
astra <file> [args...]                run a source (.astra) or bytecode (.astc) file
astra build <file> [-o <out.astc>]    compile a source file to a bytecode file
astra build <file> --exe [-o <app.exe>] [--windowed] [--icon <file.ico>]
                                      compile it into a program of its own
astra dis <file>                      list the bytecode of a source or bytecode file
```

`astra build` bundles the file and everything it imports into one `.astc`, which runs without
the sources. Exit codes: 0 success, 65 compile error, 66 file not found, 70 uncaught runtime
error, or whatever the script passes to `exit()`.

### Standalone programs

`astra build --exe` turns a script into an executable of its own:

```
astra build examples\bank.astra --exe                        makes examples\bank.exe
astra build examples\gui_todo.astra --windowed -o Todo.exe   a GUI app with no console window
astra build app.astra --windowed --icon app.ico              app.exe, with an icon of its own
```

The result is one file holding the Astra runtime and the compiled script with everything it
imports, the `gui` library included. It needs neither `astra.exe`, the sources nor `lib` to
run, so it can be copied to another machine as it is.

A built program runs as its own process, named after its file: `Todo.exe` is listed as
`Todo.exe` in Task Manager, never as `astra.exe`, so two Astra apps running side by side are
told apart by name. Rename the file and the process name follows.

- Every command-line argument goes to the script and shows up in `args()`; a built program has
  no `build`, `dis` or other astra commands.
- `--windowed` is for GUI apps: starting the program opens no console window. `print` output
  then goes nowhere unless it is redirected, and an uncaught error is shown in a message box
  instead of being printed.
- Without `--windowed` the program is a console program, with `print`, `input()` and exit
  codes working as they do under `astra`.
- `--icon app.ico` gives the program an icon: Explorer, the taskbar and Task Manager show it
  for the file, and the program's windows carry it in their title bars. It takes an `.ico`
  file, which can hold the picture in several sizes (16, 32, 48 and 256 pixels cover what
  Windows asks for). Without it the program has the plain default icon.
- Asking for a name ending in `.exe` with `-o`, or for an icon, implies `--exe`.

## The language

### Basics

```astra
val limit = 10           // immutable: assigning to it again is a compile error
var count = 3            // mutable
var nothing              // null

// Types: null, bool, int (64-bit), float, string, list, map, range, function, class, instance
print(7 / 2, 7 / 2.0, 7 % 3)        // 3 3.5 1   (int / int truncates)
print("Hi $count, or ${count + 1}") // string interpolation
print(1 == 1.0, "a" + 1)            // true a1
```

### Strings

Strings use double or single quotes and build text from code with `$`:

```astra
"hello $name"                        // a variable
"hello ${car.name}"                  // any expression goes in braces
"hello ${session.getUser().name}"
"total: ${price * quantity}"
```

`$name` takes just the variable name, so `"$car.name"` is the car followed by the text
`.name`; use `${car.name}` for anything longer than one name. Values appear as `print` shows
them, and an object's `toString()` is used if it has one. A `$` that is not followed by a
name or `{` is an ordinary dollar sign (`"$5"`); write `\$` to keep one in front of a name
(`"\$name"`).

A string in three quotes is raw: it runs to the next three quotes, over as many lines as it
likes, and quotes and backslashes in it are just text. `$name` and `${...}` still work.
`trimIndent()` takes away the indentation its lines share and `trimMargin()` everything up to
a `|` at the start of each line, so that the text can be indented with the code around it:

```astra
val path = """C:\temp\new "file".txt"""
val css = """
    .button {
        color: $colour;
    }
""".trimIndent()
```

**A string is text**: a sequence of characters, where a character is one Unicode code point.
Every position counts characters, never bytes, so a string can be cut anywhere without
cutting a character in half:

```astra
val s = "naïve café"
s.length                 // 10
s[2]                     // "ï": one character, as a string of length one
s.substring(6)           // "café"
s.indexOf("é")           // 9
s.uppercase()            // "NAÏVE CAFÉ": all letters, not only the ASCII ones
for c in s { ... }       // loops 10 times
"é".code                 // 233, the number of the character; 233.toChar() goes back
```

There is no separate character type: `"a"` and `'a'` are both strings of length one, and
`isLetter()`, `isDigit()`, `isWhitespace()`, `isUpperCase()` and `isLowerCase()` are asked of
a string. A character outside the basic plane, such as an emoji, is one character. One made
of several code points (a letter followed by a combining accent, a flag) counts as several.

**`bytes` is for data that is not text**: a fixed number of bytes, each a number from 0 to
255, that can be changed in place.

```astra
val b = bytes(16)                    // sixteen zeros
val c = bytes([1, 2, 255])           // from a list; bytes(b) copies
c[0] = 7                             // read and written by position
c.size        c + b        c.slice(1, 3)        c.toList()        c.fill(0)
"héllo".toBytes()                    // the text as UTF-8: 6 bytes
c.decodeToString()                   // and back
for byte in c { ... }
File("photo.png").readBytes()        // files give and take bytes (see File)
```

Two `bytes` are `==` if they hold the same bytes. In a type it is written `bytes`.

### Keywords as names

A keyword is only a keyword where it could be one. Where nothing but a name can come, it is
a name: after a dot, as a field, method, property or enum entry being declared, as the name
of an argument, and as a key in a map.

```astra
enum Key { if, else, in, class }
class Token(val class : string, var in = 0) {
  fun is(other) => this.class == other.class
}

Key.if        token.class        token.is(other)        Token(class = "word")        {in: 1}
```

On its own a keyword keeps its meaning, so inside the class above the field is written
`this.class`. `op`, `static`, `get`, `set`, `data`, `enum`, `interface`, `object`, `vararg`,
`as`, `until`, `private`, `internal` and `finally` are not keywords at all: each means
something only in the one place it belongs, and is an ordinary name everywhere else.

### Variables and types

Variables are declared with `val` (immutable) or `var` (mutable). A type may follow the name:

```astra
val name : string = "Astra"
var total : int = 0
var ratio : number = 1       // int or float
var owner : Person? = null   // "?" also allows null
var pet : Animal = Dog()     // a class type accepts its subclasses

total = "many"               // error: cannot assign a value of type string to 'total',
                             //        which is declared as int
```

The type is optional; a variable without one (or declared `any`) holds any value. A typed
variable keeps its type for life: every assignment is checked, including `+=` and
assignments made from other functions or closures. A literal of the wrong type is rejected
when the program is compiled; everything else is checked as the program runs and raises a
catchable `Error`.

Type names: `int`, `float`, `number`, `string`, `bool`, `list`, `map`, `range`, `function`,
`any`, or the name of a class, interface or enum (`Event.Click` for one declared inside
another class). Without `?`, a typed variable must be initialized and can never be null.

A type can say more, for the reader's sake: what a list or map holds, and what a function
takes and gives back.

```astra
val names : list<string> = []
var index : map<string, list<int>> = {:}
fun each(action : (int) -> unit) { ... }           // unit: gives nothing back
var onChange : ((Settings) -> unit)? = null        // a function, or null
fun build(block : Grid.() -> unit) { ... }         // a function run on a Grid
```

Only the outer type is checked: that `names` is a list, that `action` is a function.

A `val` fixes the variable, not the object: a list held in a `val` can still be changed.

### Statements and expressions

Statements end at the end of the line; `;` separates statements on one line. A line break
inside `( )` or `[ ]` is ignored, and a line may continue with any operator except `-`, `(`
or `[`, which start a new statement. Comments are `// ...` and `/* ... */`.

Only `null` and `false` are falsy. Logical and/or are the words `and` and `or`; they stop as
soon as the answer is known and give back one of their operands (`name or "unknown"`).
`!x` is logical not, and `cond ? a : b` is the conditional expression.

Operators, from the ones that bind tightest:

| Operators | |
| --- | --- |
| `f()` `a[i]` `a.b` `a?.b` `a!!` | after their operand |
| `-x` `!x` `~x` | before their operand; `~` flips every bit of an int |
| `as` `as?` | `value as Type` |
| `*` `/` `%` | |
| `+` `-` | |
| `<<` `>>` | shift the bits of an int left or right |
| `&` | bitwise and |
| `^` | bitwise exclusive or |
| `\|` | bitwise or |
| `..` `..<` `until` | ranges: `a..b` includes `b`; `a..<b` and `a until b` stop before it |
| `?:` | `a ?: b` is `a`, or `b` when `a` is null |
| `<` `<=` `>` `>=` `is` `!is` `in` `!in` | |
| `==` `!=` `===` `!==` | equal in value; `===` is "the very same object" |
| `and` | |
| `or` | |
| `? :` | |
| `=` `+=` `-=` `*=` `/=` `%=` `<<=` `>>=` `&=` `\|=` `^=` | assignment |

The bit operators work on ints. They bind more tightly than comparisons, so `flags & mask != 0`
tests the masked bits, with no parentheses needed. `>>` keeps the sign of a negative number,
and shifting by a negative amount is an error.

`++` and `--` add or subtract one from a variable, a field or an element:

```astra
count++
this.total--
items[i]++
```

They are statements, not expressions: each stands on its own and has no value, so
`val n = count++` and `list[i++]` are compile errors, and there is no `++count`. Change the
target on one line and use it on the next. The operator goes on the same line as its target.

`is` tests what a value is, against a class or one of the built-in type names, and `in` asks
a list, string, map (its keys) or range whether it holds a value:

```astra
shape is Circle       shape !is Circle      // Circle or a subclass of it
x is int              x is string           // also float, number, bool, list, map, range, function
x is Shape?                                 // a Shape, or null
3 in [1, 2, 3]        "ell" in "hello"      "key" in settings       n !in 1..10
```

`a in b` calls `b.contains(a)`, so a class joins in by having a `contains` method. `as`
insists that a value has a type and gives the value back; `as?` gives `null` when it does not:

```astra
val circle = shape as Circle                // runtime error unless shape is a Circle
val radius = (shape as? Circle)?.radius ?: 0
```

Three operators deal with `null`:

```astra
user?.name                  // the name, or null when user is null
user?.rename("Sam")         // not called, and its arguments not worked out, when user is null
user?.address?.city         // each step that may be null needs its own ?.
user?.visits += 1           // assignments are skipped the same way
title ?: "Untitled"         // title, unless it is null
user!!.name                 // insists: a runtime error if user is null
```

`?:` only looks for `null`: `0 ?: 1` is `0` and `"" ?: "x"` is `""`, where `or` would move on
to the right-hand side for `false` as well.

`return`, `throw`, `break` and `continue` can be written where a value is expected, which with
`?:` gives the usual way to stop when something is missing:

```astra
val user = find(id) ?: return           // leave the function
val port = config["port"] ?: throw Error("no port configured")
for line in lines {
  val entry = parse(line) ?: continue   // on to the next line
  ...
}
```

### Control flow

```astra
if n < 0 { ... } else if n == 0 { ... } else { ... }
while i < 10 { i += 1 }
for x in [1, 2, 3] { ... }      // also strings (characters), maps (keys), ranges
for i in 1..10 { ... }          // 1 to 10: a range includes its end
for i in 0..<10 { ... }         // 0 to 9;  `0 until 10` says the same;  range(10, 0, -2) steps
break    continue
```

`when` picks one branch out of several. With a subject, a branch lists the values it takes,
or tests the subject with `is` or `in`; without one, each branch is a condition:

```astra
when (key) {                              // the parentheses are optional, as for if
  "q", "x" -> quit()                      // either value
  in "0123456789" -> digit(key)           // also !in
  is Shortcut -> key.run()                // also !is
  else -> print("unknown key")
}

val size = when {
  n == 0 -> "none"
  n < 10 -> "a few"
  else -> "many"
}
```

Values are compared with `==`. A `when` has a value, that of the branch taken, so it can be
assigned, returned or used inside a larger expression; with no `else` and no branch taken the
value is `null`. A branch is one statement, or after the arrow a `{ }` block with variables of
its own, whose last expression is the branch's value. `when (val n = text.length) { ... }`
gives the subject a name for the branches to use.

`is`, `!is`, `in` and `!in` are the operators that cannot begin a continuation line: at the
start of a line they open a new branch.

`if` and `try` have a value too, that of the block that ran, which is the block's last
expression. The braces stay:

```astra
val width = if wide { 200 } else { 100 }
val port = try { int(text) } catch (e) { 8080 }
val label = if count == 0 {
  "nothing"
} else {
  val rest = count - 1
  "one and $rest more"
}
```

An `if` with no `else` gives `null` when its condition is false. A `finally` block still
runs, and does not change the value.

More about loops:

```astra
for i in 10 downTo 1 { ... }             // counting down, 1 included
for i in 0..100 step 10 { ... }          // every tenth; also after ..<, until and downTo
do { line = read() } while line != null  // the block runs before the first test

rows@ for row in grid {                  // a name for the loop
  for cell in row {
    if cell == null { continue@rows }    // on to the next row
    if cell == wanted { break@rows }     // out of both loops
  }
}
```

### Functions

```astra
fun add(a, b) { return a + b }
fun square(x) => x * x                    // expression body
val twice = fun(f) => fun(v) => f(f(v))    // anonymous functions and closures
```

Functions are values, capture variables from enclosing scopes, and must be called with
exactly as many arguments as they declare.

A function may refer to the variable it is being stored in, which is how a lambda calls itself
and how a callback names the thing it was registered as:

```astra
val factorial = fun(n) => n <= 1 ? 1 : n * factorial(n - 1)
val timer = App.every(1000, fun() { App.cancel(timer) })
```

That is the one way a variable can appear in its own initial value: `val x = x + 1` is a
compile error, and a function that runs before the variable has its value fails with
`'x' is used before it has a value`.

Parameters and the return value can be typed, with the same types as variables:

```astra
fun area(width : number, height : number) : number => width * height
fun find(name : string, pets : list) : Animal? { ... }     // may return null
val double = fun(n : int) : int => n * 2                   // lambdas too

area(3, "4")     // error: argument 'height' of area() must be number, got string
```

Arguments can be passed by name, in any order, after any positional ones:

```astra
fun connect(host : string, port : int, secure : bool) { ... }

connect("example.org", secure = true, port = 443)
```

This works for functions, lambdas, methods and constructors, but not for built-in functions.
Because `name = value` inside a call now means a named argument, an assignment used as an
argument needs its own parentheses: `f((x = 1))`.

A parameter can have a default value, which makes that argument optional:

```astra
fun greet(name : string, greeting : string = "Hello") : string => "$greeting, $name!"

greet("Ann")                      // Hello, Ann!
greet("Ann", "Hi")
greet(greeting = "Hi", name = "Ann")
```

A default is computed on each call that needs it and may use the parameters before it. A
parameter without a default may follow one with: it is then given by name, and a call that
leaves it out is an error.

A `vararg` parameter takes any number of arguments and receives them as a list. In a call,
`*list` passes the elements of a list as arguments of their own, to any function:

```astra
fun total(vararg numbers : int) : int {     // the type is that of each argument
  var sum = 0
  for n in numbers { sum += n }
  return sum
}

total()                           // 0
total(1, 2, 3)                    // 6
val more = [4, 5]
total(1, *more, 6)                // 16
print(*more)                      // 4 5

fun chart(vararg series, title = "") { ... }
chart(a, b, title = "Sales")      // what comes after a vararg parameter is passed by name
```

A function has at most one `vararg` parameter. Passed by name it takes the list itself:
`total(numbers = more)`.

Each type is optional and independent, so `fun log(level : int, message)` is fine.

 Arguments
are checked when the function is entered and the result when it returns; a function declared
to return a non-nullable type that reaches its end without returning a value raises an
error. Returning a literal of the wrong type is a compile error. Methods, static methods,
initializers and operators take types the same way (an initializer has no return type).

### Lambdas

A function can be written in braces where a value is expected. Its value is its last
expression:

```astra
val add = { a, b -> a + b }
val double = { it * 2 }              // with no parameter list, the one argument is called `it`
val nothing = {}                     // does nothing; the empty map is written {:}

[1, 2, 3].map { it * 2 }             // after a call, the lambda is its last argument
button("OK") { save() }              // after the other arguments, too
spinner(3, max = 20) { update(it) }
pairs.map { (name, score) -> "$name: $score" }    // (a, b) takes an argument apart
```

A lambda written after a call goes to the function's last parameter, whatever came before it,
and the parameters in between take their defaults: `names.joinToString { it.upper() }` leaves
the separator alone. This holds for functions, methods and extension functions alike. One
case is different, so that a function can have optional parameters after a function
parameter: when every parameter that has to be given is there but the last of them, the
lambda is that one.

```astra
fun list.groupBy(keySelector, valueTransform = null) { ... }
words.groupBy { it.length }                      // the lambda is keySelector
words.groupBy({ it.length }) { it.upper() }      // keySelector, then valueTransform
```

In the condition of an `if`, `while` or `for` a `{` opens the body, so a call there that takes
a lambda goes in parentheses: `if (items.any { it > 1 }) { ... }`. A parameter list may be
broken after a comma. What tells a lambda from a map is what follows the brace: a key and a
`:` make a map (`{"a": 1}`), `{:}` is the empty map, anything else is a lambda.

**A lambda can be run on an object**, whose members are then in reach by their bare names.
This is what builds a layout or fills in an object:

```astra
val panel = Panel().apply {          // apply runs the lambda on the panel and gives the panel
  title = "Settings"
  gap = 4
  row("first")                       // a method of the panel
}
with(panel) { row("second") }
```

A name in such a lambda is looked for among its own variables and those of the functions
around it first, then on the object, then (in a lambda inside another) on the objects of the
lambdas around it, and last among the globals.

Every value has `let`, `also`, `apply`, `run` and `takeIf`:

| | the lambda gets the value as | the result is |
| --- | --- | --- |
| `x.let { ... }` | `it` | the lambda's value |
| `x.also { ... }` | `it` | `x` |
| `x.takeIf { ... }` | `it` | `x` if the lambda gives true, else `null` |
| `x.run { ... }` | the object it runs on | the lambda's value |
| `x.apply { ... }` | the object it runs on | `x` |

With `?.` they are the usual way to do something only when a value is there:
`user?.let { greet(it) }`.

**`return` in a lambda.** Written with a lambda, `let`, `also` and `takeIf` are compiled in
place, as part of the function they are in. A `return` there leaves that function, and
`break` and `continue` reach a loop around them:

```astra
fun find(node, name) {
  for child in node.children { child.find(name)?.let { return it } }   // leaves find()
  return null
}
```

In any other lambda a plain `return` is a compile error, because it could not leave the
function it is written in. `return@name` leaves the lambda itself, with a value if one is
given; the name is that of the function the lambda is passed to, or one written before the
lambda:

```astra
items.each { if it.hidden { return@each }; show(it) }       // on to the next item
val pick = larger@ { a, b -> if a > b { return@larger a }; b }
```

`fun(x) { ... }` is unchanged: it is a function, and `return` returns from it.

### Classes

```astra
class Shape(val name : string) {             // header: a field, set when the object is made
  var visible : bool = true                  // field with an initial value

  fun area() : number { return 0 }
  fun describe() : string => "${this.name}: ${this.area()}"
  static fun unit() : Shape => Square(1)     // called as Shape.unit()
}

class Square(val side : number) : Shape("square") {    // single inheritance
  override fun area() : number => this.side * this.side
}

val s = Square(3)
print(s.describe(), s is Shape, type(s)) // square: 9 true Square
```

#### Class headers

The parentheses after the class name list what an object is made from:

```astra
class Student(var name : string, var grade : int)      // a complete class; no body needed

class Vec2(val x : number, val y : number) {           // the same as declaring x and y in the body
  fun length() : float => Math.sqrt(this.x * this.x + this.y * this.y)
}

class Dog(name : string, val breed : string) : Animal(name, 4)

val ann = Student("Ann", 9)
```

- An entry with `val` or `var` declares a field, exactly as in the class body.
- An entry without them (`name` in `Dog`) is only a parameter, typically one to hand on to the
  superclass.
- `: Animal(name, 4)` passes arguments to the superclass initializer; they can be any
  expressions over the header's names.
- A class that does not write its own `init` gets one that takes the header's entries in
  order, initializes the superclass, and assigns the fields. Types are checked as usual.

An entry can have a default value, which makes that argument optional:

```astra
class Config(val host : string = "localhost", var port : int = 8080)

Config()                      // localhost, 8080
Config("example.org")         // example.org, 8080
Config("example.org", 443)
```

Entries with defaults come after those without. A default is computed afresh for each object
that needs it, and may use the entries before it (`val height : number = width`). To skip an
earlier entry, pass the later one by name: `Config(port = 21)`.

A class that needs more than that writes `fun init(...)`. The header's fields are then plain
declarations that `init` has to assign (a default there is just the field's initial value),
and `init` calls `super.init(...)` itself; the header cannot have bare parameters or
superclass arguments in that case.

#### Fields

Fields are declared in the class body with `val` or `var`, like variables: the type and the
initial value are both optional.

```astra
class Account {
  val owner : string            // no initial value: init must set it
  var balance : number = 0
  val history : list = []       // a fresh list for every instance
  var manager : Person? = null  // class types may name a class declared later
  var note                      // untyped, starts as null
}

account.balance = "lots"   // error: cannot assign a value of type string to field 'balance'
                           //        of Account, which is declared as number
account.owner = "eve"      // error: cannot assign to val field 'owner' of Account
```

- Initial values are computed for each new instance before `init` runs, superclass fields
  first. They can use `this` and fields declared above them.
- A typed field is checked on every assignment, wherever it is made.
- A `val` field can be assigned only once: by its initial value, or else in `init`.
- A field with no initial value that is a `val`, or whose type excludes null, must be assigned
  by `init` or be listed in the class header; leaving such a field unset is an error.
- A subclass cannot redeclare a field of its superclass.

A class that declares fields has exactly those fields. Assigning to any other name is an
error (`Account has no field 'balanse'`), which catches misspellings; inside the class's own
methods it is reported when the program is compiled. Subclasses inherit the restriction and
add fields by declaring them.

A class with no field declarations, and none in its superclasses, stays open: assigning
`this.x = 1` in `init` simply creates the field. The built-in `Error` class declares
`message`, so an error class that carries more data declares it:

```astra
class HttpError : Error {
  val status : int
  fun init(status : int, message : string) {
    super.init(message)
    this.status = status
  }
}
```

Methods are declared with `fun`, exactly like functions. A hand-written initializer is the method named
`init`, and a static method is written `static fun`. An operator is declared with `op` alone
(`op +(other)`), without `fun`.

A `toString()` method controls how an instance prints. Classes can overload operators:

```astra
class Vec {
  fun init(x, y) { this.x = x; this.y = y }
  op +(o) => Vec(this.x + o.x, this.y + o.y)
  op ==(o) => o is Vec and this.x == o.x and this.y == o.y
  op -() => Vec(-this.x, -this.y)             // unary minus
  op [](i) => i == 0 ? this.x : this.y
}
```

Overloadable: `+ - * / %`, `<< >>`, `& | ^`, unary `-` and `~`, `==`, `< <= > >=`, `[]`, `[]=`.
`!=` is the negation of `==`, and `++` and `--` use `+` and `-`. Without an `==` operator,
an instance is equal only to itself.

`==` compares by value. Two lists are equal if they hold equal items in the same order, two
maps if they have the same keys with equal values, and that goes all the way down, using an
object's own `==` where its class has one. `contains`, `indexOf` and `remove` on a list find
an item the same way. `===` and `!==` ask a different question, whether two names refer to
the very same object:

```astra
val a = [1, 2]
val b = [1, 2]
print(a == b, a === b, a === a)     // true false true
```

Numbers, booleans and strings have no identity apart from their value, so for them `===` is
the same as `==`.

#### Properties

A property looks like a field from outside but runs code: `get name()` when it is read,
`set name(value)` when it is assigned.

```astra
class Temperature(var celsius : number) {
  get fahrenheit() : number => this.celsius * 9 / 5 + 32
  set fahrenheit(value : number) { this.celsius = (value - 32) * 5 / 9 }
}

val t = Temperature(100)
print(t.fahrenheit)      // 212
t.fahrenheit = 32        // runs the setter; t.celsius is now 0
t.fahrenheit += 18       // reads, adds, assigns
```

A property can have just a getter (assigning it is then an error) or just a setter. Getters
and setters take types like any function, are inherited, and a subclass can replace either
one. A setter is the place to check a value or react to a change before storing it; the value
itself lives in an ordinary field with a different name. `get` and `set` are only special at
the start of a class member, so they remain usable as names (`map.get(key)`).

#### Names inside a class

Inside a class, `this.` can be left off. A bare name is looked for among the function's own
variables, then the members of the object, then the static members of the class, then the
globals:

```astra
class Account(val owner : string, var balance : int = 0) {
  val history = []

  fun deposit(amount) {
    balance += amount                   // this.balance
    history.push(amount)                // this.history
    return describe()                   // this.describe()
  }
  fun describe() => "$owner has $balance"
  fun rename(owner) { this.owner = owner }   // the parameter hides the field: say this.
}
```

A function or a lambda written inside a method reaches the object the same way. In a lambda
that is run on another object (`panel.apply { ... }`) that object's members come first, then
those of the object the method belongs to.

A field's initial value can use the entries of the class header and the fields above it:

```astra
class Box(val width : int, val height : int) {
  val area = width * height
  val label = "$area square"
}
```

That holds for a class whose initializer is the one made from its header. With a hand-written
`init`, initial values are given before `init` runs, so they cannot depend on what it sets.

A field or property and a method may share a name. Reading the name gives the value and
calling it runs the method, which is how a setter that can be chained is written:

```astra
class Component {
  var tooltip : string? = null
  fun tooltip(text : string) {
    tooltip = text
    return this
  }
}
Component().tooltip("Save")       // the method
component.tooltip                 // the field
```

#### Accessors under a property

A getter and a setter can also be written under the field they belong to. In them `field` is
the stored value:

```astra
class Panel {
  var gap : int = 4
    set(value) {
      field = value                        // store it
      repaint()                            // and react
    }

  var title = "untitled"
    get() => field.upper()

  val area : int                           // nothing stored: worked out each time
    get() => width * height
}
```

Reading and assigning `panel.gap` go through the accessors, from inside the class as well. A
property has a stored value when it is given an initial one; an accessor that is not written
is then the plain one. The accessors take the property's type.

#### Abstract classes, override and init blocks

```astra
abstract class Shape(val name : string) {
  abstract fun area()                      // for a subclass to write
  abstract val sides : int
  open fun describe() => "$name, area ${area()}"
}

class Square(val side : int) : Shape("square") {
  override fun area() => side * side
  override val sides = 4

  init {                                   // runs when an object is made
    register(this)
  }
}
```

- An abstract class is never created itself, and a class that extends it has to supply what
  it left `abstract`, or be abstract too. `sealed class` is read the same way.
- `override` says that a member takes the place of one the class would otherwise have: a
  method, field or property of its superclass, one that comes with an interface, or one that
  an interface or abstract class asks for. The word is required for all of these, and it is
  an error where there is nothing of that name to replace. So a method can neither replace
  another by accident nor miss the one it was meant to replace through a misspelt name.
  `toString` and `==` can always be written with it, and `init` never needs it.
- `init { }` blocks run when an object is made, after the header's entries are set, in order
  with the initial values of the fields around them. A class has either such blocks or a
  hand-written `fun init`, not both.
- `open`, `final`, `public`, `protected` and `private` on a member are accepted and change
  nothing.

#### Static members

A static member belongs to the class rather than to each object, and is reached through the
class. `static` goes in front of a single member, or a `static { }` block holds several:

```astra
class Color(val r : int, val g : int, val b : int) {
  static val black = Color(0, 0, 0)        // may be an instance of the class itself
  static var created : int = 0
  static fun gray(level) => Color(level, level, level)

  static {                                  // the same, without the word on each line
    val max = 255
    fun clamp(n) => n < 0 ? 0 : n > max ? max : n
    get count() => Color.created            // a static property
  }
}

Color.black        Color.gray(128)        Color.created += 1
```

A static field gets its value once, when the class is declared, in the order the fields are
written. A `static val` cannot be assigned again and a typed static field only takes values
of its type. Inside the class a static member is reached by its bare name (`clamp(n)`,
`max`), from methods and from other static members alike; elsewhere through the class. A
subclass sees its superclass's static members through its own name (`Sub.max`), though not
by a bare name. The block holds declarations
only (`val`, `var`, `fun`, `get`, `set`); it is not code that runs.

#### Objects

`object` declares a class together with its one instance, and the name stands for that
instance:

```astra
object Settings {
  var volume = 5
  get loud() => this.volume > 7
  fun raise(by = 1) { this.volume += by }
}

Settings.raise(3)
print(Settings.volume, Settings.loud)     // 8 true
```

An object has fields, properties and methods like any class, may extend one
(`object Origin : Shape("point") { ... }`) and may have an `init`, which takes no arguments.
It is created where it is declared.

An object can also be made on the spot, with no name, where a value is expected. It sees the
variables around it, and inside a method the members of that method's object as well:

```astra
button.onClick(object : Listener {
  override fun clicked(x, y) { count += 1 }
})
val origin = object { val x = 0; val y = 0 }
```

#### Interfaces

An interface lists what a class promises to have. A method written without a body, or a
`val` / `var`, is for the class to supply; a method or property with a body comes along with
the interface:

```astra
interface Shape {
  fun area()                                   // the class writes this
  val name                                     // and has a field or getter called name
  fun describe() => "$name of area ${area()}"  // every Shape gets this
}

class Circle(val r) : Shape {
  override val name = "circle"
  override fun area() => 3.14159 * r * r
}

Circle(1).describe()
Circle(1) is Shape                             // true; also as, when, and declared types
```

After the `:` a class lists at most one superclass and any number of interfaces, in any
order: `class Button(text) : Component(text), Clickable, Focusable`. A class that lacks
something an interface requires is an error where the class is declared. A method the class
or its superclass has takes the place of one that comes with an interface. Interfaces extend
other interfaces (`interface Control : Clickable, Focusable`), may be empty (a marker to test
for with `is`), may have static members, and are implemented by objects and enums too. An
interface holds no values and is never created.

#### Classes inside classes

A class, enum, interface or object can be declared in the body of a class. It is a static
member of that class and is reached through it:

```astra
class Event(val source) {
  class Click(source, val x, val y) : Event(source)   // may extend the class it is in
  class Key(source, val code) : Event(source)
  enum Phase { capture, target, bubble }
}

val e = Event.Click("button", 3, 4)
e is Event.Click                       // also in declared types: fun route(e : Event.Click)
Event.Phase.bubble
```

Inside the outer class the short name is enough (`Click(...)`, `Phase`), and a class declared
inside sees the outer class's static members and its other inner classes by their bare names.
It does not belong to any one object of the outer class: an object it needs is passed to it.
Two classes can each have an inner class of the same name (`DrawOp.Text`, `Doc.Text`).

#### Data classes

`data` declares a class that is all about the fields in its header:

```astra
data Point(val x : int, val y : int)

val p = Point(1, 2)
print(p)                     // Point(x=1, y=2)
p == Point(1, 2)             // true: equal when the header fields are (=== still tells them apart)
p.copy(y = 9)                // Point(x=1, y=9): a new object with some fields changed
val (x, y) = p               // comes apart into its header fields, in order
```

Every entry of the header is a field (`val` or `var`). Otherwise it is an ordinary class: it
can have a body, default values, a superclass, interfaces and static members, and an `==`,
`toString` or `copy` written in the body replaces the one it would be given. Fields declared
in the body take no part in equality or printing. Objects of an ordinary class are only equal
to themselves.

#### Taking values apart

`val (a, b) = value` declares a variable for each part of a list or of a data object, and
`_` skips a part. The same works for the variable of a `for` loop:

```astra
val (low, high) = minMax(readings)          // a function returning [low, high]
val (_, second) = ["a", "b", "c"]
for (name, score) in [["Ann", 3], ["Bo", 5]] { ... }
for (key, value) in settings.entries() { ... }
```

Asking for more parts than there are is an error.

`a to b` makes a `Pair`, two values that travel together; `Triple(a, b, c)` holds three. Both
are data classes, with `first`, `second` (and `third`), and come apart like any other:

```astra
val entry = "width" to 200               // Pair("width", 200)
val (key, value) = entry
tabs("General" to general, "Advanced" to advanced)
```

#### Enums

An enum is a type with a fixed set of values, its entries:

```astra
enum Way { up, down, left, right }

Way.left.name            // "left"
Way.className            // "Way": any class tells its own name this way
Way.left.ordinal         // 2: its place in the list
Way.entries              // [up, down, left, right]
```

An entry prints as its name, entries are compared with `==`, and no others can be made. An
entry can have any name but `entries`, even `name` (`SortKey.name` is then that entry, and
`SortKey.size.name` still the name of the entry `size`).

`entries`, `name` and `ordinal` are only spoken for inside an enum. A member of an enum with
one of those names clashes with what the enum is given and is an error, unless it says
`override` and keeps the shape of what it replaces, a property of the same type:

```astra
enum Level {
  low, mid, high, hidden
  override static get entries() : list => super.entries.filter { it != hidden }
  override get name() : string => "level " + super.name
  static fun all() => super.entries
}
```

Inside the enum, `super.entries`, `super.name` and `super.ordinal` are the ones every enum is
given, whatever has been put in their place. From outside there is only `Level.entries`: what
the enum chooses to show. Like
a class, an enum can have a header, fields, methods, properties and static members; the
entries come first, with their arguments:

```astra
enum Planet(val mass : float, val radius : float) {
  mercury(3.303e23, 2.4397e6),
  earth(5.976e24, 6.37814e6);

  get gravity() => 6.673e-11 * mass / (radius * radius)
  fun heavierThan(other) => mass > other.mass
}
```

**Where the destination is declared as the enum, an entry can be written by its bare name**,
because there it can mean nothing else:

```astra
var t : Way = up                    // a variable or field of that type, declared or assigned
t = down
if t == left { ... }                // the other side of == or !=
fun turn(way : Way) : Way {         // a parameter, and a function's result
  return when (way) {               // the branches of a when on such a value
    up -> right
    else -> up
  }
}
turn(left)
cell(label, fill = horizontal)      // by name too: the parameter says which enum
```

With no such destination the full name is needed: `val x = up`, `print(up)` and
`items.push(up)` are errors, since nothing says which enum is meant. A name that already means
something (a variable, a member of the class, a global) keeps that meaning, so the bare name
of an entry is only ever tried last. The destination decides, so two enums may share entry
names. For an argument, the name is settled as it reaches the parameter, and a name that is
not an entry of the parameter's enum is an error there.

### Extension functions

A function can be added to a type from outside, to a class of your own or to a built-in
type. It is declared with the type in front of its name, and called like a method:

```astra
fun Shape.describe() => "$name of area ${area()}"    // bare names are the shape's members
fun string.shout() => upper() + "!"
fun int.isEven() => this % 2 == 0
fun list.second() => this[1]
fun string?.orEmpty() => this == null ? "" : this    // with ?, null takes it too
fun any.tag() => "<" + this + ">"                    // every value

Shape("dot", 1, 1).describe()        "hi".shout()        4.isEven()        null.orEmpty()
```

Inside, `this` is the value the function was called on. The types are a class, an interface,
an enum, or `int`, `float`, `number`, `string`, `bool`, `list`, `map`, `range`, `function`
and `any`. A member of the same name always comes first, so an extension cannot change what
a type already does. An extension function belongs to the type, not to a file: once the file
that declares it has been loaded, every value of that type has it.

### Errors

```astra
class NotFound : Error {
  fun init(what) { super.init(what + " not found") }
}

try {
  throw NotFound("key")
} catch (e) {
  print(e.message, e is NotFound)
}
```

Any value can be thrown. Runtime errors (division by zero, undefined variables, a stack
overflow, ...) are `Error` instances and can be caught the same way. An uncaught error
prints a stack trace and exits with code 70.

A `catch` can be for one class of error, and several can follow a `try`; an error that none
of them is for carries on outwards. A `finally` block runs however the `try` is left: at its
end, by an error, or by `return`, `break` or `continue`.

```astra
fun load(path) {
  val file = open(path)
  try {
    return parse(file)
  } catch (e : NotFound) {
    return null
  } catch (e) {                 // any other error; has to be the last catch
    log("cannot read $path: $e")
    throw e
  } finally {
    file.close()                // runs in every case, after the value to return is worked out
  }
}
```

### Modules

**A folder is a namespace.** The files of one folder share their top-level names: a function,
class or variable declared in one is in reach of the others, with no import. Other folders
see nothing of it until they import it:

```astra
import Shapes              // the folder Shapes: its names are written Shapes.Circle
import Shapes.*            // the same, and its names can be written bare as well: Circle
import Jade.Chart          // the folder Jade/Chart: Jade.Chart.Axis
import Jade as Ui          // under a name of your own: Ui.Button
import Shapes.Circle       // a file, Shapes/Circle.astra: loads that one file
import "../shared/util" as Util    // a path that is not made of plain names goes in quotes
```

- **`import Shapes` makes you say `Shapes.`** in front of everything you use from it. That
  keeps a file's own names and the library's apart, and shows where each name comes from.
- **`import Shapes.*` brings in every name the file may see**: the public ones, and the
  `internal` ones if the file belongs to the same library. Private names never. The name
  `Shapes.Circle` still works, for the cases below.
- An import counts for the file that writes it, and only that file. It is not passed on: what
  the folder itself imported stays its own business.
- A file's own folder comes first. A name declared there is the one meant, even if a `.*`
  import declares it too; the import's is then written with its name in front.
- When two `.*` imports declare the same name, its bare use is an error that asks which is
  meant: `Shapes.title` or `Text.title`.
- A name with an import in front works in types too (`fun show(b : Ui.Block)`).
- A library may hold an object called like itself. `Jade.Button` is then the library's class,
  and what the library has no name for, `Jade.requestRepaint()`, is looked for on the object.
- A name can be declared only once in a folder.

**How names are written.** Libraries (folders) and classes, enums, interfaces and objects
are in Title case: `Jade`, `Jade.Chart`, `ListBox`. Functions, methods, fields, variables and
constants and enum entries are in camelCase: `openWindow`, `windowOpener`, `Color.black`,
`Fill.horizontal`. The built-in type names (`int`,
`string`, `list`, ...) are lowercase, and are the only exception. The language does not
enforce this; it is how everything that comes with it is named.
- Built-in names (`print`, `Error`, the types) are in reach everywhere.

Three words say who may see a top-level declaration:

```astra
private fun helper() { ... }      // this file only
internal val cache = {:}          // this library: its folder and those under it
fun open() { ... }                // everyone who imports the folder
```

A library is a folder directly inside `lib`, with everything below it, so `Jade` and
`Jade/Chart` can share `internal` names that programs using them cannot see. The folders of
a program count as one library.

An import is looked for next to the importing file, then in the `lib` folder beside
`astra.exe`, and nowhere else. That is where bundled libraries such as `gui` live, so
`astra.exe` and its `lib` folder belong together when astra is copied somewhere.

**Order.** Within a file, and across the files of a folder that is imported as a whole, the
order of declarations does not matter. Before anything else runs, the imports are loaded, the
functions defined and the classes set up, each after the classes it extends. A class's static
fields and an enum's entries are worked out the first time one of them is asked for, or when
the program reaches the class, whichever comes first. Everything else, top-level variables
and statements, runs in the order written, and for a folder file by file in alphabetical
order: a top-level variable can be used by a function at any time, but not by a statement
that runs before the variable's own line.

## GUI

`import gui.*` gives a program windows, buttons, text boxes, lists and a canvas to draw on.
It uses the native controls of Windows and works only there.

```astra
import gui.*

val window = Window("Counter")
var count = 0

val label = window.label("Count: 0", width = 240)
val buttons = window.row()
buttons.button("Add one", fun() {
  count += 1
  label.text = "Count: $count"
})
buttons.button("Reset", fun() {
  count = 0
  label.text = "Count: 0"
})

window.show()
App.run()          // handles events until the window is closed
```

**Layout.** Nothing is positioned by hand. A `Window` is a column: each widget you add goes
below the last. `row()` and `column()` add a nested row or column, and return it so that you
can add widgets to it. A widget made with `fill = true` takes the spare space of its row or
column (a list that should grow with the window, a text box that should use the rest of a
line), and `spacer()` is empty space that does the same. Layout is redone when the window is
resized. `Window("Title", width, height)` fixes the starting size; without a size the window
fits its contents.

**Widgets.** Each of these is a method of a window, row or column, and returns the widget:

| Method | Widget | Notable members |
| --- | --- | --- |
| `label(text)` | `Label` | `text` |
| `button(text, onClick)` | `Button` | `onClick`, `click()`, `makeDefault()` (Enter clicks it) |
| `checkBox(text, checked, onChange)` | `CheckBox` | `checked`, `onChange(on)` |
| `textBox(text, onChange)` | `TextBox` | `text`, `onChange(text)` |
| `textArea(text, onChange)` | `TextArea` | the same, for several lines |
| `listBox(items, onSelect)` | `ListBox` | `add(item)`, `setItems(list)`, `remove(index)`, `clear()`, `selectedIndex`, `selected`, `count`, `onSelect(index, item)` |
| `canvas(width, height)` | `Canvas` | see below |

Names without parentheses are properties, read and assigned like fields: `label.text = "Done"`,
`if box.checked { ... }`, `list.selectedIndex = 0`. All widgets have `text`, `enable(on)`,
`disable()` and `focus()`, and take
`width =` (and where it makes sense `height =` and `fill =`) as named arguments. Handlers are
ordinary functions; pass one when creating the widget or assign it later
(`button.onClick = fun() { ... }`).

**Windows.** `show()`, `close()`, the `title` property, `size()`, `setSize(width, height)`,
`saveImage(path)`, and the handlers `onClose` (return `false` to keep the window open),
`onResize(width, height)` and `onEscape`.

**Canvas.** A surface that keeps what is drawn on it:

```astra
val canvas = window.canvas(400, 300)
canvas.clear(Color.white)
canvas.fillRect(10, 10, 80, 50, Color.blue)
canvas.circle(200, 150, 40, Color.red, 3)          // centre, radius, colour, line width
canvas.line(0, 0, 400, 300)
canvas.drawText(10, 270, "hello", Color.black, 16)

canvas.onMouseDown = fun(x, y, button) { canvas.fillCircle(x, y, 5, Color.green) }
canvas.onKeyDown = fun(key) { if key == "Space" { canvas.clear() } }
```

Drawing methods: `clear`, `line`, `rect`, `fillRect`, `ellipse`, `fillEllipse`, `circle`,
`fillCircle`, `drawText`, plus `size()` and `saveImage(path)`. Colours are `0xRRGGBB` numbers;
`Color` has the common ones (`Color.red`) and `Color.rgb(r, g, b)`. Handlers: `onMouseDown`,
`onMouseUp`, `onMouseMove`, `onKeyDown`, `onKeyUp`, `onResize`. A canvas receives keys once it
has been clicked or given `focus()`.

**App.** `App.run()` runs the program's event loop and returns when every window is closed.
`App.quit()` closes them all. `App.after(ms, action)` runs a function once, `App.every(ms,
action)` runs it repeatedly (use it for animation), and `App.cancel(timer)` stops either.
`App.alert(message)` and `App.confirm(message)` show message boxes.

The examples `gui_counter`, `gui_temperature`, `gui_todo`, `gui_paint` and `gui_bounce` show
each of these in a complete program. The library itself is `lib/gui.astra`, written in Astra on
top of a small native backend, so it is also the place to look for how something works.

## Standard library

The names are Kotlin's where Kotlin has one, so that code ported from it reads the same;
Astra's own shorter names (`push`, `upper`, `each`, `has`) are still there beside them. Part
of the library is built into the runtime and the rest is written in Astra itself
(`src/std/*.astra`), mostly as extension functions, and carried inside `astra.exe`.
`examples\words.astra` puts a good part of it to work on a piece of text.

**Functions:** `print(...)`, `write(...)` (no newline), `input(prompt?)`, `len(x)`, `str(x)`,
`int(x)`, `float(x)`, `type(x)`, `range(end)` / `range(start, end, step?)`,
`assert(cond, message?)`, `clock()`, `args()`, `exit(code?)`, `chr(n)`, `ord(s)`,
`bytes(size | list)`, `with(object, lambda)`, `repeat(times) { index -> }`, `flush()`

**Checks:** `require(condition) { message }`, `check(condition) { message }`,
`requireNotNull(value)`, `checkNotNull(value)` and `error(message)` throw an `Error`. The
message is a lambda, so it is only worked out when the check fails, and may be left out.

**Math:** `pi`, `e`, `sqrt`, `abs`, `floor`, `ceil`, `round`, `trunc`, `sign`, `min`, `max`,
`pow`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2(y, x)`, `hypot(x, y)`, `log`,
`log10`, `log2`, `exp`, `random()`, `randomInt(lo, hi)`, `seed(n)`

### Numbers

| | |
| --- | --- |
| `n.coerceIn(low, high)`, `n.coerceIn(range)` | `n` if it lies between the two, else the nearer one |
| `n.coerceAtLeast(low)`, `n.coerceAtMost(high)` | the same with one bound |
| `x.roundToInt()` | the nearest whole number; a half goes up |
| `x.toInt()`, `n.toDouble()` | drop what is after the point; make a fraction of a whole number |
| `n.absoluteValue`, `n.sign` | read without brackets |
| `n.pow(e)`, `n.mod(d)` | `mod` is never negative for a positive divisor: `(-1).mod(5)` is 4 |
| `n.toString()`, `n.toString(16)`, `n.toChar()` | as text, in another base, as the character with that code |
| `x.isNaN()`, `x.isInfinite()`, `x.isFinite()` | |
| `compareValues(a, b)`, `maxOf(a, b, ...)`, `minOf(a, b, ...)` | order numbers or strings; `null` comes first |

Astra has one type for whole numbers (64 bits) and one for fractions, so `toLong`,
`roundToLong` and `toFloat` are the same as `toInt`, `roundToInt` and `toDouble`. The limits
Kotlin's types have are there under its names: `Int.maxValue` (2147483647), `Int.minValue`,
`Long.maxValue`, `Long.minValue`, `Double.maxValue`, `Double.minValue`,
`Double.positiveInfinity`, `Double.negativeInfinity`, `Double.nan`, and the same for `Float`.

### Strings

`length` (also `size`), `lastIndex`, `indices` and `code` are read without brackets.

| | |
| --- | --- |
| Parts | `substring(start, end?)`, `substring(range)`, `slice(start, end?)`, `take(n)`, `takeLast(n)`, `drop(n)`, `dropLast(n)`, `takeWhile { }`, `dropWhile { }`, `substringBefore(d)`, `substringAfter(d)`, `substringBeforeLast(d)`, `substringAfterLast(d)`, `removePrefix(p)`, `removeSuffix(s)`, `removeSurrounding(p, s?)`, `chunked(n)`, `lines()`, `split(sep?)` |
| Finding | `contains(s)`, `startsWith(s)`, `endsWith(s)`, `indexOf(s, startIndex?)`, `lastIndexOf(s)`, `indexOfFirst { }`, `indexOfLast { }`, `equals(s)`, `compareTo(s)`; all of these but the two with lambdas take `ignoreCase = true` |
| Changing | `uppercase()`, `lowercase()` (`upper`, `lower`), `trim()`, `trimStart()`, `trimEnd()`, `trimIndent()`, `trimMargin()`, `replace(old, new, ignoreCase?)`, `replaceFirst(old, new)`, `replaceFirstChar { }`, `padStart(n, char?)`, `padEnd(n, char?)`, `repeat(n)`, `reversed()` |
| Asking | `isEmpty()`, `isNotEmpty()`, `isBlank()`, `isNotBlank()`, `isNullOrEmpty()`, `isNullOrBlank()`, `ifEmpty { }`, `ifBlank { }`, `orEmpty()` |
| Characters | `first()`, `last()`, `firstOrNull()`, `lastOrNull()`, `get(i)`, `getOrNull(i)`, `isLetter()`, `isDigit()`, `isLetterOrDigit()`, `isWhitespace()`, `isUpperCase()`, `isLowerCase()`, `digitToInt()`, `forEach { }`, `map { }`, `filter { }`, `any { }`, `all { }`, `none { }`, `count { }`, `toList()` |
| Converting | `toInt(radix?)`, `toIntOrNull(radix?)`, `toDouble()`, `toDoubleOrNull()`, `toBoolean()`, `toBytes()`, `format(...)` |

`substring` and `get` are errors for a position outside the string; `slice` and `s[i]` count
a negative position from the end and `slice` clamps. `toInt()` is an error for text that is
not a number, and `toIntOrNull()` gives `null`.

`format` fills a pattern in, with the items of Kotlin's `String.format`:

```astra
"%d items at %.2f each".format(3, 1.5)     // 3 items at 1.50 each
"%5d|%-5d|%05d".format(42, 42, 42)         //    42|42   |00042
"%,d bytes, %x, %s".format(1234567, 255, [1, 2])   // 1,234,567 bytes, ff, [1, 2]
```

`%d` whole number, `%x %X %o` in another base, `%f %e %g` fraction, `%s` anything as `print`
shows it, `%c` character, `%b` true or false, `%%` a percent sign, `%n` a line end. Between
the `%` and the letter may come `-` (left-justify), `0` (pad with zeros), `+`, `,` (thousands
separators), a width, `.` and a precision, and in front of those `2$` to pick a value by its
position.

**`StringBuilder`** puts a long text together a piece at a time, which `+=` on a string does
slowly because each one copies the string:

```astra
val out = StringBuilder()
for row in rows { out.append(row.name).append(": ").appendLine(row.total) }
print(out.toString())

val line = buildString { append("x = "); append(x) }    // the same in one expression
```

`append`, `appendLine`, `insert(i, x)`, `deleteAt(i)`, `deleteRange(from, to)`, `setLength(n)`,
`reverse()`, `clear()`, `length`, `isEmpty()`, `toString()`.

### Lists

`size` (also `length`), `lastIndex` and `indices` are read without brackets.

| | |
| --- | --- |
| Making | `[1, 2]`, `listOf(...)`, `mutableListOf(...)`, `emptyList()`, `listOfNotNull(...)`, `List(n) { index -> }`, `buildList { add(1) }` |
| Adding and removing | `add(x)`, `add(i, x)`, `addAll(items)`, `addFirst(x)`, `remove(x)`, `removeAt(i)`, `removeFirst()`, `removeLast()`, `removeFirstOrNull()`, `removeLastOrNull()`, `removeAll { }`, `removeAll(items)`, `retainAll(...)`, `clear()`, `set(i, x)`, `fill(x)`; and `push(...)`, `pop()`, `insert(i, x)` |
| Reading | `get(i)`, `getOrNull(i)`, `getOrElse(i) { }`, `first()`, `last()`, `firstOrNull()`, `lastOrNull()`, `single()`, `singleOrNull()`, each of those six also with a condition (`first { it > 2 }`), `find { }`, `findLast { }`, `contains(x)`, `containsAll(items)`, `indexOf(x)`, `lastIndexOf(x)`, `indexOfFirst { }`, `indexOfLast { }` |
| Asking | `isEmpty()`, `isNotEmpty()`, `isNullOrEmpty()`, `any { }`, `all { }`, `none { }`, `count { }`, `ifEmpty { }` |
| Going through | `forEach { }`, `forEachIndexed { index, item -> }`, `onEach { }`, `each { }`, `withIndex()` |
| Another list | `map { }`, `mapIndexed { }`, `mapNotNull { }`, `flatMap { }`, `flatten()`, `filter { }`, `filterNot { }`, `filterNotNull()`, `filterIndexed { }`, `partition { }`, `take(n)`, `takeLast(n)`, `drop(n)`, `dropLast(n)`, `takeWhile { }`, `dropWhile { }`, `subList(from, to)`, `slice(start?, end?)`, `chunked(n)`, `windowed(n, step?)`, `zip(other)`, `zipWithNext()`, `distinct()`, `distinctBy { }`, `plus(x)`, `minus(x)`, `reversed()`, `toList()`, `toMutableList()` |
| Order | `sorted()`, `sortedDescending()`, `sortedBy { }`, `sortedByDescending { }`, `sortedWith(comparator)` give a new list; `sort(comparator?)`, `sortBy { }`, `sortByDescending { }`, `sortWith(comparator)`, `sortDescending()`, `reverse()`, `shuffle()` change the list; `shuffled()`, `random()` |
| A value | `sum()`, `sumOf { }`, `average()`, `max()`, `min()`, `maxOrNull()`, `minOrNull()`, `maxOf { }`, `minOf { }`, `maxBy { }`, `minBy { }`, `maxByOrNull { }`, `minByOrNull { }`, `fold(initial) { acc, item -> }`, `reduce { acc, item -> }`, `joinToString(separator?, prefix?, postfix?, limit?) { }`, `join(sep?)` |
| A map or set | `associate { key to value }`, `associateBy { }`, `associateWith { }`, `groupBy { }`, `toMap()` (of pairs), `toSet()` |

`get`, `set` and `subList` are errors for a position outside the list, and `getOrNull` gives
`null` for one, also for a negative one; `xs[i]` and `slice` count a negative position from
the end. `first()` and `max()` are errors on an empty list where `firstOrNull()` and
`maxOrNull()` give `null`. `sort` is stable.

A **comparator** is a function of two values that gives a negative number, zero or a positive
one. `compareBy({ it.size }, { it.name })` orders by the first selector and then the second,
`compareByDescending { }`, `naturalOrder()` and `reverseOrder()` make others, and a comparator
has `thenBy { }`, `thenByDescending { }`, `then(other)` and `reversed()`.

### Maps

`size` (also `length`), `keys`, `values` and `entries` are read without brackets. `keys` and
`values` are lists, made afresh each time; an entry has a `key` and a `value`, and can be
taken apart: `for (name, age) in ages.entries { ... }`.

| | |
| --- | --- |
| Making | `{"a": 1}`, `{:}`, `mapOf("a" to 1, ...)`, `mutableMapOf(...)`, `emptyMap()`, `buildMap { put(k, v) }` |
| Reading | `map[key]`, `get(key, default?)`, `getValue(key)` (an error if it is not there), `getOrDefault(key, d)`, `getOrElse(key) { }`, `containsKey(key)`, `key in map`, `has(key)`, `containsValue(v)` |
| Writing | `map[key] = v`, `put(key, v)` and `remove(key)` (both give what was there before), `set(key, v)`, `putAll(map or pairs)`, `getOrPut(key) { }`, `clear()` |
| Asking | `isEmpty()`, `isNotEmpty()`, `isNullOrEmpty()`, `any { }`, `all { }`, `none { }`, `count { }` |
| Going through | `forEach { entry -> }`, `forEach { (key, value) -> }`, `forEach { key, value -> }` |
| A list | `map { }`, `mapNotNull { }`, `flatMap { }`, `toList()` (of pairs) |
| Another map | `filter { }`, `filterNot { }`, `filterKeys { }`, `filterValues { }`, `mapValues { }`, `mapKeys { }`, `plus(pair or map)`, `minus(key)`, `toMap()`, `toMutableMap()` |

Maps keep the order their keys were first added in; a missing key reads as `null`.

**What may be a key.** Any value. Two keys are the same key when they are `==`: a number, a
string or a boolean by its value (`1` and `1.0` are one key), and a list, a range, `bytes` or
a data object by what it holds, so that an equal one finds the entry. Any other object is a
key as itself.

```astra
data Cell(val row, val column)
val colours = {:}
colours[Cell(1, 2)] = "red"
colours[Cell(1, 2)]                  // "red": an equal Cell is the same key
colours[1 to "header"] = "grey"      // a Pair is a data object
```

A key should not be changed while it is in a map: a list that is added to after being used as
a key is no longer found.

### Sets and deques

A **`Set`** holds each value once, in the order the values were first added, and tells values
apart as a map tells keys apart.

```astra
val seen = Set()                     // or setOf(1, 2, 3), mutableSetOf(), items.toSet()
if (seen.add(id)) { ... }            // add gives true if it was not there before
id in seen        seen.remove(id)        seen.size
a + b        a - b        a.union(b)        a.intersect(b)        a.subtract(b)
setOf(1, 2) == setOf(2, 1)           // true: the same items, in any order
```

`add`, `addAll`, `remove`, `removeAll`, `retainAll`, `clear`, `contains`, `containsAll`,
`isEmpty`, `isNotEmpty`, `size`, `first`, `last`, `toList`, `toSet`, and from a list's methods
those that read: `forEach`, `map`, `filter`, `any`, `all`, `none`, `count`, `find`, `sum`,
`sumOf`, `maxOf`, `minOf`, `sorted`, `sortedBy`, `joinToString`, `groupBy` and the like, each
giving a list where a list's gives one. A `for` loop goes through the items as they were when
it began, so the set may be changed inside it. A set prints as `{1, 2, 3}`.

A **`Deque`** is a queue open at both ends. Adding and removing are quick at the front as
well as the back, where a list is quick only at the back.

```astra
val queue = Deque()                  // or Deque(items)
queue.addLast(job)
val next = queue.removeFirst()       // removeFirstOrNull() when it may be empty
queue.addFirst(urgent)        queue.removeLast()        queue.first()        queue.last()
queue[0]        queue.size        queue.isEmpty()        for job in queue { ... }
```

### Ranges

`start`, `end`, `step`, `first`, `last` and `size` are read without brackets. A range has
`contains(n)`, `isEmpty()`, `toList()`, `reversed()`, `random()`, and the methods of a list
that go through it: `forEach`, `map`, `filter`, `any`, `all`, `none`, `count`, `sum`, `sumOf`,
`firstOrNull`, `joinToString`.

### Files

A **`File`** stands for a file or folder by its path. Making one touches nothing; the disk is
looked at when something is asked of it.

```astra
val notes = File("notes.txt")
if (notes.exists()) { print(notes.readText()) }
notes.writeText("first\n")
notes.appendText("second\n")
for line in notes.readLines() { ... }

val folder = File("out")
folder.mkdirs()
File(folder, "data.bin").writeBytes(bytes([1, 2, 3]))    // a file inside a folder
for file in folder.listFiles() { print(file.name, file.length()) }
```

| | |
| --- | --- |
| The path | `path`, `name`, `extension`, `nameWithoutExtension`, `parent`, `parentFile`, `absolutePath`, `absoluteFile`, `resolve(child)` |
| On disk | `exists()`, `isFile`, `isDirectory`, `length()`, `lastModified()`, `list()`, `listFiles()` |
| Text | `readText()`, `readLines()`, `forEachLine { }`, `writeText(s)`, `appendText(s)` |
| Bytes | `readBytes()`, `writeBytes(b)`, `appendBytes(b)` |
| Changing | `delete()`, `deleteRecursively()`, `mkdirs()`, `renameTo(file)`, `copyTo(file, overwrite?)` |

Text is read and written as UTF-8, and a path may hold any characters. Each call reads or
writes the whole file. Two Files are `==` if their paths are the same text. The shorter
`File.read(path)`, `File.write(path, text)`, `File.append(path, text)` and
`File.exists(path)` work on a path without making a File.

### Every value

`toString()`, `equals(other)`, `compareTo(other)`, and the scope functions `let`, `also`,
`apply`, `run`, `takeIf` and `takeUnless`. A function has `invoke(...)`, so that
`callback?.invoke(x)` calls one that may be `null`.

## Doing several things at once

A program has four ways of having more than one thing in hand: **threads**, which share
everything and take turns; the **event loop**, which runs small tasks one after another on
the main thread; **flows**, functions that can stop in the middle to wait for something and
go on later without holding a thread; and **workers**, which share nothing and run on other
cores. Reading and writing are written the same way in all three.
`examples\watch.astra` and `examples\questions.astra` use them together.

**When a program ends.** A program is over when its script has run to the end, every thread
that is not a daemon has finished, and the event loop has nothing left to wait for: no task,
no timer that has not been cancelled, no reading or writing still going on. An error that
nothing catches ends the program if it happens in the script or in a task of the event loop;
on another thread it ends that thread, with a report, and the program goes on.

### Threads

```astra
val worker = Thread({ load(file) }, "loader")    // or Thread("loader") { load(file) }
worker.isDaemon = true            // a daemon does not keep the program alive
worker.start()
worker.join()                     // waits for it; join(500) for half a second at most

thread(name = "saver") { save() }                 // makes one and starts it
Thread.sleep(250)
Thread.currentThread().name
```

Threads share everything: a variable or an object is the same one on every thread. One
thread runs Astra code at a time, and they change places at calls and at the ends of loop
bodies. So a thread never sees a list or a map half changed, and a race between threads can
give a wrong answer but cannot damage the program; but two steps that belong together are
not done as one unless a lock makes them so. Astra code does not get faster on several cores.
What runs side by side is the waiting: a thread that is reading, sleeping or waiting for a
lock leaves the others free.

| | |
| --- | --- |
| `Thread` | `Thread(task, name?)`, `start()`, `join(millis?)`, `isAlive`, `isDaemon`, `name`, `interrupt()`, `isInterrupted`; a class may extend it and override `run()` |
| on the running thread | `Thread.currentThread()`, `Thread.sleep(ms)`, `Thread.interrupted()`, `Thread.yield()` |
| `synchronized(lock) { }` | runs the block holding `lock`, which may be any object; no other thread is inside a `synchronized` on the same object meanwhile |
| `Lock` | `lock()`, `unlock()`, `tryLock()`, `withLock { }`, and `wait(millis?)`, `notify()`, `notifyAll()` for waiting until another thread has changed something |
| `AtomicInteger`, `AtomicBoolean`, `AtomicReference` | `get()`, `set(v)`, `incrementAndGet()`, `getAndAdd(n)`, `compareAndSet(expected, v)` and the like, each done in one piece |
| `BlockingQueue(capacity?)` | `put(x)`, `take()` (waits for an item), `offer(x)`, `poll(timeout?)`, `peek()`, `size` |
| `CountDownLatch(n)` | `countDown()`, `await(timeout?, unit?)` |

```astra
val jobs = BlockingQueue()
val results = []
val lock = Lock()
val workers = (1..3).map { thread { while (true) { val job = jobs.take(); if (job == null) { break }
                                                     val done = job(); synchronized(lock) { results.add(done) } } } }
```

**Interrupting.** `thread.interrupt()` marks a thread. If it is waiting (in `sleep`, `join`,
`wait`, a queue), the wait ends with an `InterruptedException`; if it is working, it finds out
when it asks `Thread.currentThread().isInterrupted`. That is how a task that runs too long is
stopped: it checks now and then.

**Executors** keep threads that take tasks from a queue. A task gives a `Future`:

```astra
val pool = Executors.newFixedThreadPool(4)        // or newSingleThreadExecutor(), newCachedThreadPool()
val total = pool.submit { sum(numbers) }
total.get()                               // waits for the result
total.get(2, TimeUnit.seconds)            // TimeoutException if it takes longer
total.cancel(true)                        // and interrupt the thread working on it
pool.shutdown()                           // its threads keep the program alive until then
```

`get()` throws `ExecutionException` (with the task's error as `cause`) if the task failed and
`CancellationException` if it was cancelled. A `Future()` can also be made and completed by
hand (`complete(value)`, `completeExceptionally(error)`), `whenComplete { value, error -> }`
is told when it is done, and `Future.supplyAsync { ... }` runs a function on a thread of its
own. A time limit is a number and a `TimeUnit` (`milliseconds`, `seconds`, `minutes`...), and
milliseconds if no unit is given. Each of the factory functions takes a function that makes
the thread for a task, for naming them or making them daemons:
`Executors.newSingleThreadExecutor { work -> Thread(work, "saver").apply { isDaemon = true } }`.

`return` inside the block of `synchronized` or `withLock` is `return@synchronized value`, as
in any lambda that is not `let`, `also` or `takeIf`.

### The event loop

```astra
EventLoop.post { refresh() }                  // run this on the main thread, soon; from any thread
val tick = EventLoop.every(1000) { print("tick") }
EventLoop.after(5500) { tick.cancel() }       // once, after a time
EventLoop.isLoopThread
```

There is one loop, on the main thread. It starts when the script has run to its end, and
runs the posted tasks and the timers one at a time. `post` is how a thread hands a result to
the main thread, which is where a window's contents will have to be changed from. A script
that has something to do after the loop is idle calls `EventLoop.run()` itself.

### Flows

A flow is a function that can set itself aside in the middle of a call and be taken up again
later, where it was. Code that asks one thing and then another, each answered through a
callback, reads from top to bottom:

```astra
// Something that answers later: prompt returns at once, and calls back with the answer.
suspend fun ask(question) => suspendCoroutine { continuation ->
  dialog.prompt(question) { answer -> continuation.resume(answer) }
}

startCoroutine {
  val name = ask("Name?")                 // the flow is set aside here until the answer comes
  val colour = ask("Favourite colour, $name?")
  save(name, colour)
}
```

- `startCoroutine { ... }` starts a flow. It runs at once, as far as the first place it has
  to wait, and then `startCoroutine` returns.
- `suspendCoroutine { continuation -> ... }` sets the flow aside and hands the block a
  continuation. Calling `continuation.resume(value)` gives the flow its answer and takes it up
  again, there and then; `resumeWithException(error)` raises an error in it instead. A
  continuation is resumed once. If it is resumed before the block has returned, the flow just
  goes on.
- A function that may set a flow aside is called like any other: there is no `await` at the
  call. `suspend` in front of `fun` says so to the reader and changes nothing.
- A flow holds no thread while it waits, and is not tied to one: it goes on, on whichever
  thread resumes it. `delay(ms)` waits without holding the main thread, and
  `future.await()` waits for work done on a thread.
- These are Kotlin's names, and code written on `kotlin.coroutines` ports as it is:
  `block.startCoroutine(Continuation { result -> ... })` tells a completion how the flow
  turned out, as a `Result` (`isSuccess`, `getOrNull()`, `exceptionOrNull()`, `getOrThrow()`);
  `runCatching { ... }` makes one. Without a completion, an error the flow does not catch
  goes to whoever was running it: the caller of `startCoroutine`, or of the `resume` that
  took it up.

A flow can be set aside anywhere in the program's own code, however many calls deep,
including inside a lambda given to `map`, `forEach`, `apply`, `with` and the rest of the
library. It cannot be set aside inside a built-in function that is calling back into the
program, of which few are left: the comparison function of `sort`, a property's getter, an
object's `toString()`. Trying is an error that says so. On a thread that is not running a
flow, a suspending function simply waits on that thread; on the main thread outside a flow
it is an error, because the main thread waiting would stop the loop that brings the answer.

### Workers

A worker is a second copy of the runtime, on a thread of its own, running a script of its
own. Unlike a thread it runs at the same time as the program that started it, on another
core: four workers do four jobs in the time one takes. The price is that a worker shares
nothing with the program, not a variable and not an object. The two exchange messages, and
a message is copied.

```astra
// In the program:
val worker = Worker("primes.astra")              // the script, next to this file
worker.onMessage = { count -> print("the worker found", count) }
worker.post({"upTo": 100000})

// In primes.astra. Worker.parent is the program that started it.
Worker.parent.onMessage = { job -> Worker.parent.post(countPrimes(job["upTo"])) }
```

- **The script** is named by a string written in the call itself. It is compiled into the
  program there, so a program built into an executable takes its workers with it. Its names
  are its own: it does not see the program's functions and variables, nor clash with them.
  It can `import` what it needs, as any file can.
- **A message** can hold numbers, strings, booleans, `null`, `bytes`, and lists and maps of
  those. Anything else is refused where it is sent. What arrives is a copy: changing it does
  nothing to the original.
- **Messages arrive on the event loop** of the side they are sent to, one at a time, in the
  order they were sent. A worker has an event loop, threads and flows of its own.
- `worker.terminate()` stops a worker whatever it is doing, at its next call or loop round.
  In the worker, `Worker.parent.close()` says it is finished. `worker.isAlive` asks.
- **An error** that nothing catches in a worker ends that worker. The program is told through
  `worker.onError = { description -> ... }`, and without one the error is reported as an
  uncaught error is.
- A worker keeps the program alive until it ends, unless it was made as
  `Worker("script.astra", isDaemon = true)`.
- A worker has no windows: what belongs to the user interface stays with the main program.

Copying has a cost, which decides what a worker is good for. Work that takes much longer
than its data takes to copy gains a core. Work that is quick for each item, on many items,
does not: moving five thousand balls takes 0.6 ms, and so does copying them to a worker and
back.

### Streams, processes and sockets

Anything here that waits is an ordinary call, and the same one everywhere:

- on a thread it blocks that thread, and the others go on;
- in a flow on the main thread it sets the flow aside, and the event loop goes on;
- on the main thread outside a flow it blocks the main thread.

That holds for a `File`'s `readText`, `writeText` and the like as well.

**Streams** read and write a piece at a time. Text is UTF-8.

```astra
File("log.txt").inputStream().use { input ->          // use closes it afterwards
  while (true) { val line = input.readLine(); if (line == null) { break }; handle(line) }
}
File("out.txt").outputStream(append = true).use { it.println("one more line") }
```

| | |
| --- | --- |
| `InputStream` | `readLine()` (null at the end), `readLines()`, `forEachLine { }`, `readText()`, `read(count?)` (bytes; null at the end), `readBytes()`, `close()`, `use { }` |
| `OutputStream` | `write(text or bytes)`, `print(x)`, `println(x)`, `newLine()`, `flush()`, `close()`, `use { }` |
| `File` | `inputStream()`, `outputStream(append?)`, and under Kotlin's other names `bufferedReader()`, `bufferedWriter()`, `printWriter()` |

`print` to another program or to a file is sent on in pieces; `flush()` sends what has been
printed so far.

**A process** is another program started from this one:

```astra
val git = ProcessBuilder("git", "status", "--short").directory(project).start()
for line in git.inputStream.readLines() { print(line) }     // what it writes
print("exit code", git.waitFor())

val python = ProcessBuilder("python", "-u", "server.py").redirectErrorStream().start()
python.outputStream.println("{\"cmd\": \"ping\"}")          // what it reads
print(python.inputStream.readLine())
python.destroy()
```

`inputStream` is what the program writes, `errorStream` what it writes as errors, and
`outputStream` what it reads. Before `start()`: `directory(folder)`, `environment()` (a map of
changes), `redirectErrorStream()` (errors go with the ordinary output), and
`redirectInput`, `redirectOutput`, `redirectError`, each taking a `File` or
`ProcessBuilder.pipe`, `.inherit` or `.discard`; `inheritIO()` for all three. After:
`waitFor()` (the exit code), `waitFor(timeout, unit)` (true if it ended in time), `isAlive`,
`exitValue()`, `pid()`, `destroy()`. `Process.currentCommand()` is what started this program,
so that it can start another of itself.

**Sockets** are TCP connections:

```astra
val socket = Socket("example.org", 80)                     // or Socket().connect(host, port, timeoutMs)
socket.outputStream.write("GET / HTTP/1.0\r\nHost: example.org\r\n\r\n")
print(socket.inputStream.readLine())
socket.close()

val server = ServerSocket(8080, bindAddress = "127.0.0.1")   // port 0: the system picks one, see localPort
while (true) {
  val client = server.accept()                             // a Socket
  thread { serve(client) }
}
```

`inputStream`, `outputStream`, `close()`, `isClosed`, `localPort`, `remoteAddress`, `port`,
`soTimeout` (how long a read waits, in milliseconds), `shutdownOutput()`. Closing a socket or
a stream that a thread is waiting on gets that thread out, with an `IOException`, which is
what every failure to read, write, connect or start a program is.

## How it works

```
source ──lexer──▶ tokens ──parser──▶ AST ──compiler──▶ bytecode ──▶ VM
                                                          │
                                                          └── astra build ──▶ .astc file
                                                                             or an .exe
```

| File | Role |
| --- | --- |
| `src/lexer.*` | Tokens, including string interpolation and significant line breaks |
| `src/parser.*`, `src/ast.h` | Recursive-descent parser producing the syntax tree |
| `src/compiler.*` | Scope resolution, closures and code generation; module loading |
| `src/bytecode.*` | Instruction set, bytecode verifier, disassembler, `.astc` reader/writer |
| `src/value.*` | Values and heap objects (strings, lists, maps, classes, closures...) |
| `src/vm.*` | Interpreter loop, calls, exceptions, mark-and-sweep garbage collector |
| `src/builtins.cpp`, `src/stdlib.cpp` | Standard library, the part built into the runtime |
| `src/threads.cpp` | Threads and the lock they share, locks for programs, the event loop, flows |
| `src/io.cpp` | Streams, processes and sockets |
| `src/worker.cpp` | Workers: a second runtime on its own thread, and the copying of messages |
| `src/std/*.astra` | Standard library, the part written in Astra; packed into `astra.exe` by the build (`cmake/embed.cmake`) and loaded before every program |
| `src/gui.cpp` | Native backend of the GUI library (Windows) |
| `lib/gui.astra` | The GUI library: widgets, layout and events, written in Astra |
| `src/main.cpp` | Command line, REPL and building standalone programs |

Every function is checked by the verifier before it can run, whether it came from the
compiler or from a `.astc` file: operands must be in range and the stack depth consistent on
every path. That is also where each function's stack requirement is computed.

A standalone program is a copy of `astra.exe` with the compiled bytecode attached to the end
of the file, followed by its size and a marker. On startup astra looks at the last bytes of
its own file; when the marker is there it runs the attached program instead of reading the
command line. `astra.exe` is linked with the static C++ runtime so that such a copy has no
DLLs to bring along. An icon is written into the copy's resources before the bytecode is
attached.

## Tests

```
powershell -ExecutionPolicy Bypass -File tests\run.ps1
```

Each `tests\*.astra` file states its expected output in `// expect:` comments. The runner
executes every test three ways (from source, from a compiled `.astc`, and built into an
`.exe`) and expects the same result from each. A last check builds a program with `--icon`
and looks for the icon in the result.

`// expect error output:` is for a program that finishes normally having reported an error
(one that ended a thread, say).

Setting `ASTRA_THREAD_STRESS=1` makes the threads of a program change places at every
opportunity, which is how the code that threads share is tested.

Setting the environment variable `ASTRA_GC_STRESS=1` makes the VM collect garbage at every
opportunity, which is slow but useful for finding memory bugs when changing the VM.

## Limitations

- The GUI library is Windows-only, and covers the basic widgets: there are no menus, images,
  combo boxes, sliders or tabs yet.
- Types are checked while the program runs (only literals are checked at compile time), so
  a mismatch shows up when the offending line executes. Typed calls cost a little speed.
- Loop variables cannot be typed, and the element types in `list<int>` or `(int) -> bool` are
  read but not checked.
- Which characters are letters, digits or spaces, and what their upper and lower case are,
  is asked of Windows for characters outside ASCII. There are no regular expressions yet.
- `keys`, `values` and `entries` of a map are lists made when they are asked for: changing
  one does not change the map.
- `orEmpty()` cannot be called on `null` itself, since strings, lists and maps each have one
  and `null` does not say which is meant; write `text ?: ""` or `items ?: []`.
- A compiled `.astc` file carries the Astra-written part of the standard library, about 85 KB.
- `private` and the other access words are enforced for top-level declarations. On the members
  of a class they are accepted and change nothing yet.
- A class declared inside another does not belong to one object of the outer class (there is
  no `inner`), and an inherited static member needs the class name in front (`Base.max`).
- `if` always needs its braces, also as a value: `if wide { 200 } else { 100 }`.
- Extension functions are not scoped to the file that imports them, and there are no extension
  properties. A class has one initializer: there are no secondary constructors.
- `apply`, `run`, `synchronized` and `withLock` are ordinary calls, so a plain `return` inside
  their lambda is an error; it works in `let`, `also` and `takeIf`.
- Streams, processes and sockets are there on Windows only. There is no way yet to fetch a URL.
- Astra code that shares its objects does not run on several cores at once: threads take
  turns, and workers, which do run side by side, share nothing.
- The old `gui` library draws one shape at a call, which is what limits an animation of
  thousands of shapes: no thread or worker can draw for it. `examples\gui_bounce.astra` and
  `examples\gui_bounce_workers.astra` show it, the second with the moving done on other cores.
- An `object` is created where it is written, and top-level variables get their values in
  order (file by file for a folder), so one cannot be used by a statement that runs earlier.
- Integer arithmetic wraps on overflow.
- Bytecode files are verified when loaded, but that is a consistency check rather than a
  security sandbox. Run only `.astc` files you trust.
- Standalone programs are about 1.4 MB each and have only been tried on Windows. They carry
  no version information, and since they are unsigned Windows may ask for confirmation the
  first time one downloaded from the internet is run. The script inside is stored as
  bytecode, not encrypted.
- `--icon` takes `.ico` files only, not PNG or other pictures. Explorer remembers icons, so a
  program rebuilt with a different icon can show its old one there for a while.
