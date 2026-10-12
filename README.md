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
  fun speak() => super.speak() + " - woof!"
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
astra build <file> --exe [-o <app.exe>] [--windowed]
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
- Asking for a name ending in `.exe` with `-o` implies `--exe`.

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
`any`, or the name of a class. Without `?`, a typed variable must be initialized and can
never be null.

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
| `f()` `a[i]` `a.b` | after their operand |
| `-x` `!x` `~x` | before their operand; `~` flips every bit of an int |
| `*` `/` `%` | |
| `+` `-` | |
| `<<` `>>` | shift the bits of an int left or right |
| `&` | bitwise and |
| `^` | bitwise exclusive or |
| `\|` | bitwise or |
| `..` `..=` | ranges |
| `<` `<=` `>` `>=` `is` | |
| `==` `!=` | |
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

### Control flow

```astra
if n < 0 { ... } else if n == 0 { ... } else { ... }
while i < 10 { i += 1 }
for x in [1, 2, 3] { ... }      // also strings (characters), maps (keys), ranges
for i in 0..10 { ... }          // 0 to 9;  0..=10 includes 10;  range(10, 0, -2) steps
break    continue
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

Parameters with defaults come last. A default is computed on each call that needs it and may
use the parameters before it.

Each type is optional and independent, so `fun log(level : int, message)` is fine.

 Arguments
are checked when the function is entered and the result when it returns; a function declared
to return a non-nullable type that reaches its end without returning a value raises an
error. Returning a literal of the wrong type is a compile error. Methods, static methods,
initializers and operators take types the same way (an initializer has no return type).

### Classes

```astra
class Shape(val name : string) {             // header: a field, set when the object is made
  var visible : bool = true                  // field with an initial value

  fun area() : number { return 0 }
  fun describe() : string => "${this.name}: ${this.area()}"
  static fun unit() : Shape => Square(1)     // called as Shape.unit()
}

class Square(val side : number) : Shape("square") {    // single inheritance
  fun area() : number => this.side * this.side         // overrides
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
instances compare by identity.

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

### Modules

```astra
import "lib/shapes"      // path relative to this file; ".astra" is optional
```

An imported file runs once. All files share one global namespace, so its top-level
functions, classes and variables are simply available afterwards.

A file that is not found next to the importing one is looked for in the `lib` folder beside
`astra.exe`, and nowhere else. That is where bundled libraries such as `gui` live, so
`astra.exe` and its `lib` folder belong together when astra is copied somewhere.

## GUI

`import "gui"` gives a program windows, buttons, text boxes, lists and a canvas to draw on.
It uses the native controls of Windows and works only there.

```astra
import "gui"

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

**Functions:** `print(...)`, `write(...)` (no newline), `input(prompt?)`, `len(x)`, `str(x)`,
`int(x)`, `float(x)`, `type(x)`, `range(end)` / `range(start, end, step?)`,
`assert(cond, message?)`, `clock()`, `args()`, `exit(code?)`, `chr(n)`, `ord(s)`

**Math:** `PI`, `E`, `sqrt`, `abs`, `floor`, `ceil`, `round`, `min`, `max`, `pow`, `sin`,
`cos`, `tan`, `log`, `exp`, `random()`, `randomInt(lo, hi)`, `seed(n)`

**File:** `File.read(path)`, `File.write(path, text)`, `File.append(path, text)`,
`File.exists(path)`

| Type | Members |
| --- | --- |
| string | `length`, `upper`, `lower`, `trim`, `split(sep?)`, `contains`, `startsWith`, `endsWith`, `indexOf`, `slice(start, end?)`, `replace(from, to)`, `repeat(n)` |
| list | `length`, `push(...)`, `pop`, `insert(i, x)`, `removeAt(i)`, `remove(x)`, `clear`, `contains`, `indexOf`, `slice(start?, end?)`, `join(sep?)`, `reverse`, `sort(compare?)`, `map(f)`, `filter(f)`, `reduce(f, initial)`, `each(f)`, `find(f)` |
| map | `length`, `keys`, `values`, `entries`, `has(k)`, `get(k, default?)`, `remove(k)`, `clear` |
| range | `start`, `end`, `step`, `length`, `toList`, `contains(n)` |

Lists and strings accept negative indexes (`xs[-1]`). Maps keep insertion order; a missing
key reads as `null`. `sort` is stable; a comparator returns a negative, zero or positive
number.

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
| `src/builtins.cpp` | Standard library |
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
DLLs to bring along.

## Tests

```
powershell -ExecutionPolicy Bypass -File tests\run.ps1
```

Each `tests\*.astra` file states its expected output in `// expect:` comments. The runner
executes every test three ways (from source, from a compiled `.astc`, and built into an
`.exe`) and expects the same result from each.

Setting the environment variable `ASTRA_GC_STRESS=1` makes the VM collect garbage at every
opportunity, which is slow but useful for finding memory bugs when changing the VM.

## Limitations

- No `finally` or variadic parameters.
- The GUI library is Windows-only, and covers the basic widgets: there are no menus, images,
  combo boxes, sliders or tabs yet.
- Types are checked while the program runs (only literals are checked at compile time), so
  a mismatch shows up when the offending line executes. Typed calls cost a little speed.
- Static fields, loop variables and `catch` variables cannot be typed, and there are no element
  types such as "list of int".
- Strings are UTF-8 byte strings: `length`, indexing and `slice` count bytes, while `for`
  and `split("")` step through whole characters. `upper`/`lower` only change ASCII letters.
- Imports share one global namespace; there are no private module members.
- Integer arithmetic wraps on overflow.
- Bytecode files are verified when loaded, but that is a consistency check rather than a
  security sandbox. Run only `.astc` files you trust.
- Standalone programs are about 0.9 MB each and have only been tried on Windows. They have
  the default program icon and no version information, and since they are unsigned Windows
  may ask for confirmation the first time one downloaded from the internet is run. The script
  inside is stored as bytecode, not encrypted.
