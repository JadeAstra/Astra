# M0: what the two spikes found

M0 was two throwaway prototypes, built to test the riskiest designs in
[the plan](gui-plan.md) before committing to them: the parser changes for Kotlin-style
lambdas, and an OpenGL window with a text engine of our own. Both work. This is what was
built, what it showed, and what it changes in the plan.

M0 was accepted on 2026-10-12, and the changes it suggested are now in the plan.

The code is on the branch `m0-spikes`, and the file paths below are in that branch. It is
prototype code: it proves the designs and is not the form the features will finally take.

## Spike 1: lambda literals, trailing lambdas, receiver blocks

### What was built

In the real lexer, parser, compiler and VM:

- `{ a, b -> ... }` and `{ ... it ... }` lambda literals, whose value is their last expression.
- A lambda with no parameter list accepts no argument or one.
- `{}` as the empty lambda and `{:}` as the empty map.
- Trailing lambdas: `button("OK") { ... }`, `grid { ... }`, `items.filter { ... }`.
- Blocks that run on an object, so that `grid { hgap = 8; row { cell(...) } }` works, with
  `with(obj) { }`, `apply`, `also`, `let` and `run`.
- Type annotations with type arguments and function types: `list<int>`,
  `map<string, list<int>>`, `(int) -> int`, `Grid.() -> Unit`.

Three scripts exercise it, each passing from source, from a compiled `.astc` and as a built
executable, and under the sanitizer with collection stress:
`spikes/m0/lambdas.astra`, `spikes/m0/receivers.astra`, and a table of corner cases,
`spikes/m0/grammar_probes.ps1`.
`receivers.astra` ends with the layout block from the examples' `Launcher.kt`, written as it
would be ported, running against stand-in classes.

The existing test suite passes 94 of 94 with these changes in place.

### The grammar conflicts, and how each was settled

| Conflict | Outcome |
| --- | --- |
| `if ready() { ... }`: body, or a lambda passed to `ready()`? | In the condition of `if`, `while` and `for` a `{` always opens the body. Parentheses switch that off for what they enclose, so `if (items.any { it > 1 }) { ... }` works. This is Swift's rule. No existing test needed changing. |
| `if items.any { it > 1 } { ... }`, written without the parentheses | Was silently misread: the first block became the body. It is now a compile error at the second `{` that says to put the condition in parentheses. |
| `{` starting a map or a lambda | A key followed by `:` is a map; names followed by `->` on the first line are a lambda's parameters; `{:}` is the empty map; anything else is a lambda. Map keys were already restricted to a name, a string, a number or `[expr]`, so one or two tokens of lookahead decide it. |
| `{ ready() ? it : 0 }`, a ternary inside a lambda | Reads correctly as a lambda. |
| `{ x : int -> x * 2 }`, a typed parameter that looks like a map entry | Reads correctly: the `->` on the first line wins. |
| A `{` on the line after a call | Starts a block, as today. A trailing lambda has to begin on the line the call ends on, as in Kotlin. |
| `map<string, list<int>>` ending in `>>` | The parser splits the shift token. |

The whole change to existing Astra code was seven empty-map literals, two of them in the old
`gui` library.

### Things the plan did not have

1. **A parameter with a default before one without.** Kotlin's
   `grid(id: String? = null, build: JadeGrid.() -> Unit)` is an error in Astra today, where
   defaults must come last. The spike allows it. Such a parameter is filled by a lambda after
   the call or by name, and a call that leaves it without a value is an error.
2. **A trailing lambda goes to the last parameter**, whatever came before it, including named
   arguments: `spinner(3, min = 0, max = 20) { ... }` is in `Launcher.kt`. Only the function
   being called knows where its last parameter is, so this is settled at the call.
3. **A top-level variable is not a local.** In a receiver block, a variable of the enclosing
   function wins over a member of the object, as in Kotlin, but a global does not. A Kotlin
   `fun main() { val children = ...; grid { ... children ... } }` relies on the first. Ported
   as a `fun main()` it behaves the same; with its body moved to the top level of a script it
   would not, if the object happened to have a member of that name.
4. **A lambda's parameter list has to fit on its first line** in the spike. Kotlin allows it
   to wrap. Decided: Astra allows it too, in M1.
5. **Destructuring parameters, `{ (a, b) -> ... }`,** come with the destructuring feature and
   are not in the spike.

### What it costs at run time

A name inside a lambda that is not one of its variables may belong to the object the lambda
runs on, which is known only when it runs. Measured over two million iterations:

| Access | Time each |
| --- | --- |
| A global from an ordinary function | 21 ns |
| The same from a lambda running on nothing | 25 ns |
| The same from a lambda running on an object that lacks the name | 34 ns |
| A field of the object by its bare name | 33 ns |
| The same field as `this.total` in a method | 28 ns |
| A method of the object by its bare name | 111 ns |
| The same as `this.bump()` in a method | 42 ns |

So a bare name costs a few nanoseconds more than a resolved one, which is negligible for
layout code. Calling a method by its bare name is the exception, at about two and a half
times the cost: the spike builds a bound-method object for every such call. M1 gives it an
instruction of its own.

### Not done in the spike, for M1

- The agreed rule for `return` in a lambda (D7). Today a `return` there leaves only the lambda.
- The scope functions expanded in place by the compiler. Here they are ordinary calls.
- Calling a receiver-typed parameter as `g.build()`. The spike's library code writes
  `g.apply(build)`, which is also valid Kotlin.
- A fast path for trailing lambdas, which now go through the named-argument call.
- A proper error object for a missing argument; the spike throws text.

### The two namespace questions

These concern how files are loaded once a folder is a namespace. They cannot be tested until
namespaces exist. Both recommendations were accepted:

1. **Importing a folder loads every `.astra` file directly in it**, once, as one unit, so that
   its files see each other whatever order they are in. This is what a Kotlin package is, and
   it is what `import "jade"` has to mean.
2. **Importing a file loads that file only**, into the namespace of the folder it is in. A
   script with `import "helpers"` beside it therefore shares names with `helpers.astra`
   exactly as today, and the other scripts in that folder are not touched.

## Spike 2: an OpenGL window and a text engine of our own

### What was built

About 1,500 lines of C++ in `src/spike`, with no third-party code:

- **font.cpp**: reads TrueType fonts and collections, maps characters to glyphs, measures by
  the one rule, rasterises outlines with exact-area antialiasing, and hints with a rule of
  our own (baseline, x-height and capital height snapped to whole pixels).
- **glwin.cpp**: a Win32 window with an OpenGL 3.3 core context; the 28 OpenGL functions it
  needs fetched by name; one shader that draws fills, rounded rectangles, ellipses, lines and
  glyphs with smoothed edges; a glyph atlas; drawing into an offscreen framebuffer and reading
  it back; raw events delivered to a script function.
- **png.cpp**: writes a PNG, for the pictures below.

It is reachable from scripts as a class `Spike`, used by `spikes/m0/window.astra` and
`spikes/m0/text_compare.astra`.

### Text: the part to prove

Running `text_compare.astra` draws Segoe UI at 11 to 28 pixels, our engine beside Windows'
own, into `spikes/m0/out` (`text_compare.png`, `text_compare_dark.png`, and
`text_compare_zoom.png` for the 12 and 13 pixel rows at three times the size). The pictures
are not kept in the repository; the script makes them again.

- **It reads and draws correctly** at every size tried, composite glyphs included.
- **Against Windows' greyscale rendering it is the more even of the two.** Windows' is
  visibly irregular at 13 to 16 pixels (some letters and all the digits come out heavier).
- **Against ClearType it is slightly lighter** at 11 and 12 pixels and otherwise close. A
  small darkening of thin strokes is the obvious adjustment; the engine has a setting for it.
- **Our hinting rule makes little visible difference** at these sizes. It is cheap and stays,
  but the quality comes from the rasteriser, not from the hinting.
- **Measured widths equal Windows' exactly from 13 pixels up** (573, 605, 688, 859 and 1,216
  pixels for the test line). At 11 and 12 Windows' are wider (497 against 472, 517 against
  515), because at small sizes it uses adjusted widths of its own. Ours are the font's.
- **Measuring and drawing agree.** In the test scene each line has a rule drawn under it at
  its measured width, and at 1x every rule ends where the text ends.

**One problem found, at HiDPI.** The rule gives every character a whole number of *logical*
pixels. At 2x the glyphs are drawn at twice the size but still placed on that grid, so
spacing is a little loose and the measured width runs slightly past the text. The rule needs a
clause for scaled displays: round each width in device pixels and report the total in logical
ones. To settle at the start of M4.

**Every font on this machine.** Of the 486 font files in `C:\Windows\Fonts`, 463 are read and
rasterised without error, also under the sanitizer. The other 23 are the two cases the plan
already has for M4: 13 use the second outline format, and 10 are symbol fonts (Wingdings and
the like) with a character map of their own.

### The window

- An OpenGL 3.3 core context on a plain Win32 window works with no library (tested on an
  NVIDIA card).
- The scene is drawn into an offscreen framebuffer and read back from a window that is never
  shown, at 1x and 2x: `scene.png` and `scene_2x.png`. That is the hidden mode tests need.
- Shapes are smooth at any angle and one-pixel rules are crisp.
- Raw events reach the script: a second script shows the window, posts mouse, key and
  character messages to it, and the script counts them and quits on Escape.

### Not tested

- Any graphics driver other than this machine's. Intel graphics, virtual machines and Remote
  Desktop are the cases the plan's risk list names.
- The window redrawing while it is being resized. The code draws from inside the resize
  message, but nobody has dragged its edge.
- Per-monitor DPI. The spike process is not DPI-aware, so Windows would stretch it.
- More than one window, and sharing textures between contexts.

## What changed in the plan

| Section | Change |
| --- | --- |
| 5.1, lambda literals | A parameter list may run over several lines, as in Kotlin. |
| 5.1, new | A parameter with a default may precede one without; a trailing lambda is the last parameter. |
| 5.1, receiver blocks | A global does not shadow a receiver's member; a `fun main()` should stay a function when ported. |
| 5.1, namespaces | The two loading rules above. |
| 4, the text engine | The measuring rule gets a clause for scaled displays. Stroke darkening is a tuning setting. |
| 9, risks | "Small text may not look good enough" is much reduced: it already looks good. The fallback of letting Windows rasterise is unlikely to be needed. |
