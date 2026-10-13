# Plan: Jade, Astra's GUI library, based on JadeUI

Status: agreed plan, every decision settled (section 8). Work proceeds one milestone at a
time (section 7). M0, the two spikes, is done and accepted; what it found is in
[m0-findings.md](m0-findings.md) and is folded in below. M1, the language, and M2, the
standard library, are done. M3, threads and async I/O, is built; what was settled along the
way is under "As built in M3" in 5.5 (and "As built in M2" in 5.4). M3.5, parallel
sections, is built too (5.5, point 7). M4 is next.

## 1. Goal and ground rules

Give Astra a GUI library with the same API as JadeUI (`jadelib-ui`): every public function
and parameter keeps its name, order and default, and every class keeps its name without the
`Jade` prefix (`JadeCanvas` is `Canvas`), so that an app written against JadeUI in Kotlin is
ported by translating syntax, not by redesigning its UI code. The deliberate differences are
listed in section 6.

In Astra the library is called **Jade** (`import Jade`, or `import Jade.*` for bare names). "JadeUI" in this document always
means the Kotlin original. Sections that describe the Kotlin library use its names
(`JadeComponent`); sections about Astra use Jade's (`Component`).

- The library is written in Astra.
- C++ is used only for what a script cannot do: windows, input, drawing, text, images,
  clipboard, file dialogs.
- No Kotlin and no JVM anywhere in Astra. JadeUI is the specification, not a dependency.

The full list of names to match is in [jadeui-api-inventory.md](jadeui-api-inventory.md)
(machine-extracted; check a signature against the Kotlin source before relying on it). It
lists the Kotlin names; apply sections 6.1 and 6.2 to get Jade's.

## 2. What JadeUI is

Source: `C:\Users\sciss\IdeaProjects\JadeLib` (`jadelib-ui`, version 0.6.0-SNAPSHOT).

| Part | Files | Lines | Notes |
| --- | --- | --- | --- |
| Root package `com.jadelib.ui` | 26 | 9,200 (5,900 of code) | components, grid, widgets, text editing, menus, host contract |
| 16 sub-packages | 49 | 23,700 | dialogs, chart, doc/html/css, sheet, image, svg, math, map, hex, code, text, treemap, gl, icons |
| Tests | 214 | 23,000 | mostly headless: recorded draw operations and synthetic events |
| GLFW/OpenGL backend | 14 | 4,400 | a host: window, input translation, texture upload |
| JavaFX backend | 12 | 1,900 | a second host with the same policy duplicated |

Line counts include comments, which are plentiful; the 75 library files hold 22,400 lines of
code proper.

How it works, in the terms that matter for a port:

- **It draws everything itself.** A widget is a `JadeContent` (reports `sizes()`, paints
  onto a `JadeCanvas`, handles `JadeEvent`s) wrapped in a `JadeComponent` (id, bounds,
  visibility, colours, size overrides, paint cache). There are no native controls, menus or
  popups: menu bars, dropdowns, context menus, tooltips and dialogs are all Jade-drawn.
- **`JadeCanvas` is small**: colour, font, fill/draw rect, round rect, oval, line, polyline,
  polygon, `drawString`, `drawImage`, `measure`, `sub` (a translated, clipped child canvas).
  Everything richer (SVG, charts, picture editing, HTML effects) is done by drawing into an
  offscreen image with Java2D and blitting that.
- **It is headless-first.** A tree can be laid out, painted to a `RecordingCanvas` (a list
  of `DrawOp`s) and driven with synthetic events with no window. Most tests work this way.
- **The host is not thin.** About 1,200 lines of the GLFW backend are policy: press capture,
  focus rules, Tab traversal, popup-versus-press rules, context menu on right release,
  dropdown opening, tooltip timing, caret blink, hold-to-repeat, click counting, when to
  flush paint caches. The JavaFX backend duplicates it.
- **One UI thread.** The only real background thread in the core indexes lines for the
  large-text view.

Three apps depend on it today:

| App | Size | Uses |
| --- | --- | --- |
| JadeMem (gui module) | 3,400 lines | grid, split, scroll, tabs, screen, label, button, check box, combo box, table, menus, charts |
| JadeGen | 7,400 lines | the above plus text area, custom contents, markdown/HTML view, raw input, and an OpenGL view |
| Snips | 38,100 lines | nearly everything: documents, spreadsheet, code, math, SVG, charts, maps, hex, large text |

## 3. Architecture for Astra

```
app.astra
   │  same calls as the Kotlin app: grid { row { cell(label("Name:")) } }, Window(root).show()
   ▼
Jade (Astra)            components, layout, widgets, text editing, menus, dialogs,
   │                    and the host policy that both Kotlin backends duplicate, written once
   ▼
compat (Astra)          the JDK names the API exposes: Color, Font, Dimension, Rectangle, Insets,
   │                    Point, BufferedImage, File; and Kotlin's collection and string names
   ▼
native layer (C++)      windows and raw events, a 2D canvas with offscreen surfaces, text
                        measuring, images, clipboard, cursors, timers, file dialogs
```

Decisions built into this shape:

1. **The host policy moves into Astra.** The native layer delivers raw events (pointer,
   wheel, key down/up with physical key and layout letter, text, focus, resize, scale change,
   close request, file drop) and nothing else. A window is a `Window`, the one
   class that replaces JadeUI's `JadeHost`, `JadeWindow` and `WindowSpec` (section 6.1). The Kotlin
   backends' own host classes are not part of Jade under any name: nothing in Astra is called
   after GLFW or JavaFX.
2. **The native canvas mirrors `Canvas`** (JadeUI's `JadeCanvas`) and adds persistent offscreen surfaces, because
   the per-component paint cache and the retained window buffer both depend on them.
3. **One UI thread, in a runtime that has threads.** Layout, painting and input all run on
   the thread that runs the event loop, as in JadeUI. Caret blink, tooltip delay, hold-repeat
   and scheduled tasks are polled against a clock in the Kotlin frame function; here they are
   timers on that loop. Astra itself gains threads and async I/O (section 5.5), so an app
   does its slow work off the UI thread, as the Kotlin apps do, and hands the result back
   through `Jade.scheduler`. The native layer must repaint from inside the resize callback
   (Windows runs a modal loop while a window is being resized), and runs file dialogs and
   image decoding on worker threads, delivering their results on the loop.
4. **The old `gui` library is no longer supported and is removed.** Jade is Astra's only GUI
   library. The old one receives no fixes from now on, and its files leave the tree in M5,
   when Jade's native layer replaces its backend:
   - `lib/gui.astra` and `src/gui.cpp`, with the references to them in `CMakeLists.txt`,
     `src/builtins.cpp`, `src/compiler.cpp`, `src/main.cpp` and `src/vm.h`;
   - the five `examples/gui_*.astra` and `tests/gui_widgets.astra`;
   - the GUI section of the README.

   Only its own examples and test were ever written against it, so nothing is kept for
   compatibility and nothing is migrated; Jade's examples are written fresh. Its `Window`
   and `Color` names pass to Jade. Three behaviours of the old backend are worth repeating
   in the new native layer: a window shows the icon of a program built with `--icon`; an
   error raised inside an event handler is reported instead of being lost in the message
   loop; and sizes follow the display's DPI.

### Speed

Measured on the current VM: 15 to 25 million method calls a second, and laying out and
painting 2,000 simple widgets takes about 0.4 ms. Ordinary widget trees are not a concern.
Three things are: per-pixel loops (treemap shading, flood fill, blur), text layout of very
long documents, and regex-heavy parsing (CSS, Markdown). Those get native helpers rather than
script loops; the plan marks where.

## 4. The native layer (C++)

**Drawing is done with OpenGL.** Every window is an OpenGL window, and rectangles, text,
images and the widgets' cached pictures are all drawn by the graphics card. This is the
choice JadeUI's own GLFW backend points to, and it is made for the 3D view: JadeGen puts an
OpenGL view inside the window and JadeUI draws menus, popups and tooltips over it. In a
window that is OpenGL throughout, that is three layers drawn in order (the interface, the 3D
view, the popups). In a window drawn any other way the 3D view either hides whatever should
cover it or has to be copied in every frame.

How the pieces map onto OpenGL (3.3 core, the version JadeUI's backend and JadeGen use):

- **Shapes.** A small renderer of Jade's own: batched quads, with the edges of rectangles,
  rounded rectangles, ovals and lines smoothed in the shader. Colour with alpha is ordinary
  blending.
- **Text.** OpenGL has none, and Jade's comes from an engine of its own, with no
  third-party code: it reads font files, measures, and rasterises glyphs once into a texture
  from which they are drawn as quads. Described under the tiers below. Measuring works with
  no OpenGL context, because the recorded-drawing tests measure text without ever opening a
  window.
- **Offscreen pictures.** A widget's paint cache is a texture drawn into through a
  framebuffer and blitted from then on. The window keeps its last picture the same way, so a
  small change redraws a small rectangle.
- **Clipping and offsets.** The scissor rectangle and a translation. HiDPI is a scale.
- **Several windows.** One context per window, all in one share group, so cached textures
  and the glyph atlas are shared, as in the Kotlin backend.
- **No window on screen.** A hidden context drawing into a framebuffer, read back with
  `glReadPixels`.

What OpenGL does not give and has to be supplied: text (above), reading and writing image
files, and the window itself with its input.

**The window and its input are written by us, directly against Windows,** with no
third-party code: a Win32 window with a WGL context, a window procedure that turns Windows'
messages into the raw events listed in section 3, the clipboard, cursors, file drops, and
the built program's icon. It is about two thousand lines, and the old `gui` backend already
had its beginnings (a window class, the message loop, DPI handling, timers).

It sits behind a small interface: create a window, deliver events, set a cursor, read and
write the clipboard, wake the loop. That interface is the only part of Jade that is written
per system. OpenGL drawing and the text engine are the same everywhere, so running on
another system later means writing this one layer again for it and changing nothing in the
toolkit. Two cautions for when that day comes: macOS's windowing is written in Objective-C,
and Apple has deprecated OpenGL there, where it still works but is frozen at an old version.

| Tier | Contents | Needed by |
| --- | --- | --- |
| **N1** core | Windows with an OpenGL context: create hidden, show, title, size and position, min/max size, resizable, owner window, icons, bring to front, close. Raw events as listed above, plus pointer leave and the OS double-click time. Nine cursor shapes, hidden and locked pointer. One-shot wake timer and a nanosecond clock. Surfaces: the window's retained picture and offscreen textures, clear to transparent, redraw a dirty rectangle. Canvas: colour with alpha, font (family, size, bold, italic), the `Canvas` operations, a translate-and-clip stack, a scale for HiDPI, image draw scaled and with constant alpha. Text: Jade's own engine (below) and the glyph atlas; width, ascent, descent, height, per-character width; three logical families. Plain-text clipboard. Save a surface as PNG and read a pixel (for tests). | root package, dialogs, sheet, hex, code, text, map, most of doc |
| **N2** services | Async open/save/folder dialogs. Image decode (PNG, JPEG, GIF, BMP) and PNG encode. Clipboard HTML and images. Installed font families. OS dark-mode query. Request attention. | dialogs, image view, CSS fonts |
| **N3** vector stack | Paths with lines, quadratic and cubic curves, turned into triangles; stroke width, caps, joins, dashes; arcs and pie sectors; linear and radial gradients; affine transform stack; clip to a shape (stencil); image draws through a transform with nearest/bilinear/bicubic; `Clear` composite; Gaussian blur; whole-image pixel arrays. OpenGL provides none of this ready-made: it is written, or taken from a small library such as NanoVG, when M10 is reached. | svg, toolbar icons, chart (partly), math, image editing, HTML effects |
| **N4** 3D view | The layering of an app's own OpenGL drawing between the interface and its popups, with the state set up and restored around it; and OpenGL bindings for Astra, so that an app can issue the calls. The context and the layering come with the window; the bindings are the large part. | `GlView`, so JadeGen |
| **N5** parsing | A `Regex` class and an XML reader. | CSS, Markdown, HTML, code highlighting, SVG, `.ovr` overrides |
| **R** runtime | Not GUI: threads, locks, the event loop, and file, process and socket I/O (section 5.5). The window loop in N1 is this loop. | every app's background work; the large-text view; image loading |

N1 is several thousand lines of C++ in new files that take the place of `src/gui.cpp`, more
than a Direct2D version would have been, with text the largest part of the difference. In
exchange N4 shrinks to the bindings, where before it was the largest single item in the plan.

### The text engine

Text is Jade's own, in C++, with no third-party code and nothing asked of the system except
which font files are installed. The requirement is that text works the same everywhere:
measuring and positioning are reliable. It does not have to match any system's own
rendering. It is a complete engine from the start, not a subset to be extended later.

**Reading fonts**

1. **TrueType and OpenType files and collections** (`.ttf`, `.otf`, `.ttc`): which glyph a
   character maps to, each glyph's width, the font's ascent, descent and line gap.
2. **Both outline formats.** Quadratic outlines, as in the fonts Windows ships, and the
   cubic outlines of `.otf` fonts, which are a small program per glyph that has to be
   interpreted. The second kind is common on Linux and in Chinese, Japanese and Korean fonts.
3. **Variable fonts.** One file holding every weight and width, with each glyph's outline
   and width adjusted for the instance asked for. Windows 11's default interface font and
   macOS's system font are both of this kind, so Jade uses them as they are.
4. **Finding fonts.** The system is asked only for the installed font files. Jade reads
   their names to match a family, size and style, and their character tables to find another
   font when the chosen one lacks a character.

**Laying text out**

5. **Shaping.** Turning a run of characters into the right glyphs in the right places, for
   every script and not only those where one character is one glyph: the joined forms of
   Arabic, the reordering and conjuncts of the Indic scripts, combining marks placed on their
   bases, and emoji sequences that join into one picture. This means an interpreter for the
   substitution and positioning tables fonts carry, plus the rules particular to each script.
6. **Direction.** Right-to-left text, and lines that mix both directions, put into visual
   order by the Unicode bidirectional rules.
7. **Unicode character data.** Script, joining behaviour, combining class, direction and
   cluster boundaries for every character, built into the runtime as tables. This is data
   published by Unicode, not code, and it is the one outside input the engine has.

**Measuring and positioning, by one rule**

8. A glyph's width is its width in the font scaled to the size and rounded to a whole pixel,
   and a run's width is the sum of its glyphs' widths after shaping. Each glyph is placed by
   adding up those widths, so measuring and drawing use the same numbers and a caret cannot
   drift from its text. On a scaled display the rounding has to happen in device pixels, or
   spacing comes out loose (found in M0); the exact clause is settled at the start of M4.
9. Shaping applies what a script requires and leaves optional typography off: no kerning
   and no discretionary ligatures. For Latin, Cyrillic, Greek, Chinese, Japanese and Korean
   a string's width is therefore still the sum of its characters' widths, which is what
   JadeUI's caret arithmetic assumes, and that arithmetic is exact. For scripts that are
   shaped, measuring part of a string shapes that part on its own, which is how the Kotlin
   version behaves through Java's text today: carets are approximately right, as they are
   now. Making editing exact in those scripts would mean carets that move by cluster and
   selections in visual order, which is a change to the ported text widgets and not to the
   engine; it is not part of this plan.

**Drawing**

10. **Rasterising.** Each glyph's outline is turned into a small antialiased picture, once
    per font, size and instance, and kept in the atlas.
11. **Hinting.** A simple rule of Jade's own that snaps the baseline and the tops of letters
    to whole pixels, so small text has crisp horizontal edges. The programs fonts carry for
    this are not run.
12. **Colour emoji.** The layered-colour format Windows' emoji font uses, in both its
    versions (flat layers, and the newer one with gradients), drawn into a colour atlas
    beside the one for ordinary glyphs. The bitmap-based formats other systems' emoji fonts
    use are read too, which needs a PNG decoder of Jade's own.

**What to expect of it**

- Reliable metrics and positions on every system, and identical pictures wherever the same
  font file is used, so even pixel tests of text are portable.
- Small text a little softer than Windows' own on a screen at 100% scaling, since Windows
  runs the fonts' hinting programs and Jade does not. On a HiDPI screen the difference all
  but disappears. JadeUI on the JVM is drawn with Java's antialiased text today, not
  Windows', so this is close to how the apps look now.
- Each system's own default font, so numbers differ between Windows and macOS where the
  fonts differ. Shipping one font with Jade would make them equal as well; nothing in the
  design depends on that choice.

**How big this is.** Points 1, 2, 4, 8, 10 and 11 are a few thousand lines and are what the
first window needs. Variable fonts and colour emoji are each a bounded addition. Shaping,
direction and the Unicode data (points 5 to 7) are the bulk: the library the rest of the
world uses for exactly this job, HarfBuzz, is over a hundred thousand lines, and a from-
scratch engine that handles Arabic and the Indic scripts properly is a larger piece of work
than porting Jade's whole root package. It has a milestone of its own (M4), ordered so that
the parts the first window needs come first and the window work is not held up by the rest.

Testing support belongs in N1 from the start: hidden windows that render to an offscreen
surface, synthetic raw events fed through the same path as real ones, an in-memory clipboard
and scripted file-dialog answers. That is the equivalent of the hidden test mode in the
Kotlin GLFW backend, under names of Jade's own, and it is what lets the Kotlin tests be ported.

## 5. What Astra is missing

Counts are occurrences in JadeUI's main sources (22,400 lines of code without comments) and
in app code (the examples plus JadeGen, 7,500 lines), from a regex census. "Today" was checked
by running each construct through the current `astra.exe`.

### 5.1 Needed for the API to have the same shape

Without these the port cannot keep the names and call syntax; there is no workaround that
leaves app code looking the same.

| Feature | Library | Apps | Astra today | What is needed |
| --- | --- | --- | --- | --- |
| Interfaces | 56 declared, 140 classes with 2+ supertypes | every custom widget | none; single inheritance only | `interface` with abstract and default methods and properties; a class may implement several; `x is SomeInterface` works, including for marker interfaces such as `FocusTarget`. The whole capability system (`content is OverlayContent`) rests on this. |
| Enums | 22 | 2 | none | `enum Fill { none, horizontal, ... }`, with `name`, `ordinal`, `entries`, and optional fields and methods. An entry can be written by its bare name wherever its enum is expected. Described below the table. |
| Singletons | 29 `object` | 6 | none | `object Jade { ... }`: a class and its one instance, bound to the name, with fields, properties with setters, and methods. |
| Static members | 34 `companion object` | 7 | `static fun` only; static fields only by assigning after the class | A `static { ... }` block in a class, and static fields (`Font.bold`, `Color.black`). Described below the table. |
| Nested classes | throughout | 16 inner | compile error | `Event.Click(x, y)`, `x is Event.Click`, `DrawOp.Text`, `Grid.Cell`. Several nested names repeat (`Text`, `Wheel`, `Hover`), so they must be scoped, not flattened. |
| Lambda literals | 491 with parameters, 1,588 uses of `it` | 212 / 901 | `fun(x) { ... }` only | `{ a, b -> ... }` and `{ ... it ... }`, with the last expression as the result. A literal with no parameter list accepts no argument or one (`it`), since Kotlin writes the same `{ ... }` for both. `{}` is the lambda that does nothing, the usual default of a callback parameter. The parameter list may run over several lines, as in Kotlin. Told apart from a map literal by what follows the brace: an expression and `:` is a map, `{:}` the empty map, anything else a lambda. |
| Trailing lambdas | 153 | 77 | none | `button("OK") { ... }`, `grid { ... }`. The lambda goes to the function's last parameter, whatever came before it, named arguments included: `spinner(3, min = 0, max = 20) { ... }`. In the condition of `if`, `while` and `for` a `{` opens the body instead, and parentheses allow a lambda again: `if (items.any { it > 1 }) { ... }`. |
| A default before a required parameter | `grid(id = null, build)`, `block(source, label, onEdit = {}, view)` | – | an error: defaults must come last | Allowed. The parameter after the defaults is given as a trailing lambda or by name, and a call that leaves it without a value is an error. |
| Lambdas with a receiver | 3 declared, but they are the layout DSL | every screen | none | Inside `grid { ... }` and `row { ... }`, `insets = ...`, `row { }` and `cell(...)` resolve to the receiver. A variable of the block or of the function around it wins over the receiver's member, as in Kotlin; a global does not, so a Kotlin `fun main()` stays a function when ported. A block inside another reaches the outer one's object too. |
| Scope functions, and `return` in a lambda | `.let {` 529; a bare `return` in a one-line lambda 52; `return@label` 33 | 263 / 109 / 352 | none | `let`, `also`, `apply`, `run` and `takeIf` work on every value, and written with a lambda literal (`x.let { ... }`) are expanded in place by the compiler, so they cost no call. Described below the table. |
| Varargs | 4 | 1 | none | `tabs("A" to a, "B" to b)`, `lineChart(s1, s2)`, and `listOf(...)`. |
| Implicit `this` | every method | every method | `fun get() => x` fails with "undefined variable" | A bare name inside a class is looked up as a local, then a member of the object, then a static of the class, then a global. Writing `this.` stays valid. |
| Null operators | `?.` 1,418; `?:` 817; `!!` 46 | 562 / 357 / 15 | none | `?.`, `?:`, `!!`. `or` is not a substitute for `?:`: it also replaces `false`. |
| `return`, `break`, `continue`, `throw` as expressions | 299 | 953 in the three apps | statements only | `val hit = find(id) ?: return`, `val c = next() ?: continue`. It is how Kotlin code deals with a missing value, so it comes with `?:`. |
| Casts and tests | `as?` 90, `!is`, `in` / `!in` 888, `===` 83 | 71 / – / 272 / 5 | `is` only | `as?`, `as`, `!is`, `in`, `!in`, `===`, `!==`. |
| `when` | 295 | 81 | none | With a subject, `is` branches, several values per branch, `else`. |
| `finally` | 60 | 18 | none | `try { child.paintInto(c) } finally { c.dispose() }` is how every container paints. |
| Data classes | 48 | 12 | header classes compare by identity | `data Point(val x : int, val y : int)`: a class whose header fields give it a generated `==`, `copy(...)`, `toString()` and destructuring. Kotlin writes `data class`; Astra writes `data` alone and the porter deletes the word `class`. `data` is special only at the start of such a declaration, so it stays usable as a variable name. `Component` decides whether a cached image is still valid with `cacheStyle == st`, and routing uses `event.copy(x = lx, y = ly)`. |
| A property and a method with one name | `tooltip`, `background`, `foreground` on `Component` | fluent calls everywhere | not possible | `c.tooltip` reads the field and `c.tooltip("text")` calls the method. |
| Namespaces and visibility | 2,037 `private`, 182 `internal`; 75 files in 17 packages | 751; Snips has 15 packages | one global namespace, where redeclaring a class or function silently replaces it for everyone | One namespace per folder, with `private`, `internal` and public names. Described below the table. |
| Type syntax | every signature | every signature | simple names and `?` only | Accept type arguments and function types in annotations: `list<string>`, `map<string, int>`, `(int) -> bool`. Checking can stay shallow: `list`, `map`, `function`. The built-in names stay lowercase; the porter rewrites `Int`, `String`, `List`. |

**Enums.** An enum is a type with a fixed set of values:

```astra
enum Way { up, down, left, right }

var t : Way = up        // t can only be one of the four, so the name is enough
t = down
if t == left { ... }
t = Way.right           // the full name is always allowed
```

- Declared with `enum Name { ... }` and no other way. Kotlin writes `enum class`, because
  there `enum` is a modifier on a class; the porter deletes the word `class` from each
  declaration (22 in the library).
- Entries have `name` and `ordinal`, the enum has `entries`, and an enum may declare fields
  and methods, as in Kotlin.
- **A bare entry name is allowed wherever the destination is declared as that enum**, because
  there it can mean nothing else. The destinations are: a variable or field of that type,
  being initialised or assigned; a parameter of that type, receiving an argument; a function
  returning that type; and the other side of `==`, `!=` or a `when` branch against a value of
  that type.
- Where the compiler can see the destination's type, it settles this when the program is
  compiled, and a name that is not an entry of that enum is a compile error. Where it
  cannot, typically an argument to a method (`cell(c, fill = horizontal)`: which `cell` is
  only known when the call happens), the name is settled as the value arrives at the typed
  parameter, and a wrong one is an error there.
- **With no enum-typed destination the full name is required.** `val x = up`, `print(up)`
  and `items.add(up)` are errors that ask for `Way.up`. This is what keeps a misspelt or
  undeclared variable from quietly turning into an enum entry.
- The destination decides, not the name, so it does not matter that several enums share an
  entry. That matters for Jade: its 18 public enums have 180 entry names and 23 of them
  belong to more than one enum (Kotlin's `LEFT`, here `left`, is in six: `Key`, `KeyCode`, `PointerButton`,
  `FloatSide`, `ProseEdge`, `SheetAlign`).
- A bare name is tried as an entry last, after locals, the object's members, the class's
  statics and globals. A variable called `left` therefore still means the variable.

Ported Kotlin always writes the full name (`Fill.HORIZONTAL`, here `Fill.horizontal`) and is unaffected. The three apps and the
examples do so 321 times, about two thirds of them for `Anchor` and `Fill` in grid layouts, where
`cell(label("Name:"), fill = horizontal, anchor = west)` now reads as well.

**Namespaces.** Today every file shares one set of global names, and declaring a class or a
function under a name that already exists replaces it without a word, for every file. An app
that declared its own `Button` after importing a toolkit broke the toolkit's `button(...)`
factory from the inside. With the `Jade` prefix gone, Jade alone brings in about 220 short
names (`Button`, `Table`, `Event`, `Window`, `Block`). So names get the structure Kotlin
gives them:

- **A folder is a namespace**, as a package is in Kotlin. The files of one folder see each
  other's top-level names. Nothing outside the folder sees them unless it imports the folder.
- **`import Jade` makes the folder's names available as `Jade.Name`**, and nothing more:
  everything used from it is written with `Jade.` in front. **`import Jade.*` also lets them
  be written bare**, which is what a ported Kotlin file wants (its `import com.jadelib.ui.*`).
  Either way the names are the ones the importing file may see: public, plus `internal` when
  it is in the same library, never private. A sub-folder is `import Jade.Chart` (then
  `Jade.Chart.Axis`) or `import Jade.Chart.*`. A path that is not made of plain names is
  written in quotes, `import "../shared/util" as util`. An import is not passed on: importing
  a folder does not give you what that folder imported. A sub-folder is a namespace of its
  own and imports its parent like anyone else, as `com.jadelib.ui.chart` imports
  `com.jadelib.ui`.
- **Three levels of visibility**, Kotlin's own. `private` at the top level of a file is seen
  in that file only. `internal` is seen throughout its library (Jade and all its
  sub-folders) and nowhere else. Anything else is public.
- **A folder's own names win** over imported ones. JadeGen's `TextField` and Jade's then
  coexist, and Jade keeps using its own whatever the app declares.
- **A hidden name is reached through the import's name**: `Jade.Block`, or `Ui.Block` after
  `import Jade.* as Ui`. Snips needs this: it has a `Block` of its own and uses Jade's in 8
  places.
- **Two imports that export the same name** are fine until the name is used unqualified;
  that use is a compile error asking which is meant (`Jade.title` or `text.title`).
- **Declaring a name twice in one namespace is a compile error**, never a silent
  replacement.
- Built-in names (`print`, `Math`, `File`, the types) are visible everywhere.
- All of this is settled when a program is compiled; a name still costs a slot lookup at
  run time, as now.

What it gives the port: JadeUI's 17 packages become 17 folders with their `private` and
`internal` helpers intact; JadeMem and JadeGen are one folder each; Snips' 15 packages become
15 folders, which also resolves the four names it declares twice (`Tok`, `Expr`, `Stmt`,
`Closure`). A Kotlin file's list of imports becomes one line per package it uses.

How files are loaded:

- **Importing a folder loads every `.astra` file directly in it**, once, as one unit, so its
  files see each other whatever order they are in. This is what `import Jade` does.
- **Importing a file loads that file only**, into the namespace of the folder it is in. A
  script with `import helpers` beside it shares names with `helpers.astra` exactly as
  today, and the other scripts in that folder are not touched.

**Static members.** Kotlin has no `static`; what belongs to a class rather than to one object
goes in a `companion object { ... }` inside it. JadeUI and the three apps have 111 of them.
None implements an interface, has a name or is passed around as a value: every one is a plain
holder of constants and functions. Astra therefore gets statics, not companions, in a form
that takes a companion's body as it stands:

```astra
class Grid {
  static {
    val maxTrack = 100000
    fun key(row : int, col : int) : int => row * maxTrack + col
  }

  fun add(component, row, col) { ... key(row, col) ... }
}
```

- `static { ... }` holds declarations only: `val`, `var`, `fun`, `get`, `set`. Everything in
  it is a static member of the class. It is not a block of statements that runs once, which
  is what `static { }` means in Java and C#; initial values already run once, in order, when
  the class is defined.
- The prefix stays as shorthand for a single member: `static fun unit() => ...`, and now
  also `static val` and `static var`. The block and the prefix mean the same thing, and
  existing `static fun` code is unaffected.
- Static fields are typed and `val` is enforced, like any other field.
- `private static { ... }` covers a whole block; 33 of the 111 companions are private.
- From outside, a static is reached through the class: `Grid.key(1, 2)`, `Font.bold`.
- From inside the class it is reached by its bare name, both from other statics and from the
  class's instance methods, as Kotlin code does. That needs unqualified names in a class to
  be looked up as: locals, then the object's members, then the class's statics, then globals,
  which is the implicit-`this` rule in the table above.

Porting a companion is then one edit: `companion object {` becomes `static {`.

**`return` in a lambda.** Astra follows Kotlin's rule, which has three cases:

```astra
fun find(id) {
  for child in children { child.find(id)?.let { return it } }    // leaves find()
  return null
}

items.forEach { if it.hidden { return@forEach }; show(it) }      // leaves the lambda, like continue

val area = fun(w, h) { return w * h }                            // leaves the anonymous function
```

- In a lambda literal given to `let`, `also`, `apply`, `run` or `takeIf`, a bare `return`
  leaves the enclosing function. The compiler expands these five in place, so there is no
  call to return through: it is an ordinary return. Of the 161 bare returns counted in
  one-line lambdas across the library and the apps, 147 are of this kind.
- In any other lambda literal a bare `return` is a compile error that points to
  `return@label`. Kotlin permits it there only when the receiving function is `inline`,
  which Astra has no notion of; the porter turns those few (a `forEach` with an early exit)
  into a `for` loop.
- `return@label` leaves just the lambda, with a value if one is given. The label is the name
  of the function the lambda is passed to, or one written before the lambda (`name@ { ... }`).
- `fun(x) { ... }` is unchanged: `return` leaves that function, as in a Kotlin anonymous
  function.
- The five names are special in one form only: a call with a lone lambda literal,
  `x.run { ... }`. The compiler has to decide what that means without knowing what `x` is,
  so in that form it is always the scope function. Classes may still declare methods with
  these names and call them with ordinary arguments: `panel.apply(item)`, `stepper.run(block)`.
  The library and the apps declare 25 such methods (`run` and `apply`), and all but two are
  unaffected. The two take a lone lambda and are called as `x.run { ... }`, which Astra
  would read as the scope function: `FxThread.run(block)` in JadeMem and
  `SnippyRunner.run(body)` in Snips. The porter renames them or calls them with parentheses.

### 5.2 Needed for app code to port without rewriting each line

Each of these has a workaround, but the workaround touches nearly every line of a Kotlin app.

| Feature | Library | Apps | Workaround if not added |
| --- | --- | --- | --- |
| `if`, `when`, `try` as expressions | 673 `= if` | 228 | Rewrite to `? :` or a variable assigned in branches. |
| `if (cond) statement` without braces | very common (`if (!visible) return`) | very common | Add braces. |
| Destructuring | 224 | 54 | Index by hand: `val (r, c, v) = cell`, `for ((i, img) in icons.withIndex())`. |
| Property accessors with `field` | 181 | 6 | A second field plus `get x()` / `set x(value)`. |
| `override`, `open`, `abstract`, `public`, `internal` | 563 `override` | 108 | Delete them. Better: accept them, and check `override` and unimplemented interface members when a class is created, which catches porting slips. |
| `init { }` blocks, secondary constructors | 35 / 4 | 4 / 0 | Hand-written `init`. |
| Anonymous objects `object : GlRenderer { ... }` | 12 | 11 | A named local class (those already work). |
| Loop labels: `break@outer`, `continue@outer` | 3 | a few | Restructure. (`return@label` is settled: 5.1.) |
| `downTo`, `step`, `indices`, `withIndex`, `repeat` | 201 | 159 | `range(a, b, step)` and counters. (`until` and `..<` are settled: see 5.3.) |
| Raw strings `"""` | 56 (CSS, HTML) | 46 | Escape by hand. |
| Infix `to` and `Pair` / `Triple` | 159 | 53 | `Pair(a, b)`. |
| `do { } while` | 2 | 0 | `while` with a flag. |
| Extension functions | 33, among them `JadeComponent.layout`, `dispatch`, `find`, `click` | 1 | Declare them as methods of the class: the call looks the same. Only extensions on built-in types change shape; the public one is `List<JadeComponent>.nextFocus(current, forward)`. |

Decisions for this section (2026-10-12). They are built as a second part of M1.

- **`if` keeps its braces.** `if (!visible) return` is ported as `if !visible { return }`. `if` and
  `try` do become values: `val w = if wide { 200 } else { 100 }`.
- **Accessors are written Kotlin's way**, under the property, with `field` for the stored value:
  `var gap = 4` followed by `set(value) { field = value; repaint() }`.
- **Extension functions are allowed**, on our own classes and on the built-in types:
  `fun Component.layout() { ... }`, `fun list.nextFocus(current, forward) { ... }`. A member of
  the same name comes first. Unlike Kotlin's they are not scoped to the importing file.
- **`override` is required**, as in Kotlin, on a method, field or property that replaces an
  inherited one or supplies what an interface or abstract class asks for, and is an error
  where there is nothing to replace.
- Secondary constructors are left out (4 uses): default arguments or a static function do the job.
- `indices`, `withIndex`, `repeat` and the other collection helpers come with the standard
  library (M2).

### 5.3 The same text means something different

These are the dangerous ones: the Kotlin line is accepted by Astra, or nearly, and does
something else.

| Kotlin | Astra today | Consequence | Outcome |
| --- | --- | --- | --- |
| `0..n` includes `n`; `0 until n` excludes it | `0..n` excludes `n`; `0..=n` includes it | Every `..` loop ported unchanged is one short, silently. 102 in the library, 23 in apps. | **Astra changes.** `..` includes its end; `..<` and `until` exclude it. `..=` then says the same as `..` and is removed. Existing Astra code is updated in the same change. |
| `a and b`, `a or b` are bitwise (`shl`, `xor` too) | `and` / `or` are the logical operators | `flags and 0xFF` evaluates to `0xFF` instead of masking. 78 in the library, 249 in apps. | **Astra keeps its meaning.** The porter's rule: `&&` → `and`, `\|\|` → `or`, and Kotlin's `and` `or` `xor` `shl` `shr` → `&` `\|` `^` `<<` `>>`. A `shl` or `xor` nearby is the warning sign. |
| `return` in a lambda leaves the enclosing function | `return` in `fun(x) { }` leaves the lambda | Early exits inside a lambda change meaning. | **Kotlin's rule** (5.1, scope functions): the common case ports as written and the rest is a compile error, never a silent change. |
| A lambda's value is its last expression | a block body returns `null` unless it says `return` | `list.map { it * 2 }` would produce nulls. | Lambda literals give their last expression (5.1). `fun(x) { ... }` is unchanged. |
| `==` compares lists and data classes by content | lists and objects compare by identity | Cache checks and change detection never match. | **Astra changes.** `==` on lists, maps and sets compares their contents, element by element and recursively, with the depth guard that printing a self-containing list already has. `contains`, `indexOf` and `remove` on a list find an element the same way. Data classes compare by content too (5.1). `===` and `!==` compare identity. One difference remains: Kotlin's arrays compare by identity, and in Astra they are lists, so an array `==` in ported code compares contents. Jade's own tests rely on this decision: 211 of the Kotlin test assertions compare a result with an expected list. |
| `'a'` is a `Char` with arithmetic (`'A' + c`) | `'a'` is a string | 400 char literals in the library. | Stays a one-character string, with character helpers (5.4). The porter rewrites arithmetic on characters. |
| String indexes are UTF-16 units | `length`, indexing and `slice` count bytes | Caret positions, selection and word movement break on any non-ASCII text. | **Astra changes.** Strings are counted and indexed by character (code point) (5.4). That matches Kotlin for all text except characters outside the basic plane, mainly emoji, which Kotlin counts as two units and Astra as one. |
| `{}` is an empty lambda (the default of most callbacks) | `{}` is an empty map | `onClick: () -> Unit = {}` appears in almost every factory. | **Astra changes.** `{}` is the empty lambda and the empty map is written `{:}`. Map literals with entries, `{ "k": v }`, are unchanged. The five `{}` maps in existing Astra code are rewritten in the same change. |
| `Int` is 32 bits | `int` is 64 bits | `Int.MAX_VALUE` means "unbounded" in `Sizes`; packed ARGB values are negative in Kotlin. | Stays 64 bits. Named constants for the limits; colour packing is checked by hand. |
| A `Byte` runs from -128 to 127 | (no byte type) | Code that tests a byte's sign, or stores a negative one, behaves differently. | **Astra's `bytes` holds 0 to 255.** Kotlin's usual `b.toInt() and 0xFF` gives the same answer either way and can lose its mask. A sign test (`b < 0`) is rewritten by hand as `b >= 128`. |
| `x ?: y` replaces only null | `x or y` also replaces `false` | A `Boolean?` setting loses its `false`. | `?:` is added (5.1), so the Kotlin line ports as written. |

### 5.4 Standard library and runtime

| Gap | Use | Needed |
| --- | --- | --- |
| Strings | everything that edits text: 146 `substring`-style calls and 63 pieces of index arithmetic in the library, 435 and 131 in the apps | Counted and indexed by character (code point), not byte. Described below the table. Kotlin's names: `substring`, `lowercase`, `uppercase`, `isEmpty`, `isBlank`, `lines`, `take`, `drop`, `padStart`, `toInt`, `toIntOrNull`, `toDouble`, `format`. Character tests: letter, digit, whitespace. `StringBuilder` or fast `+=`. |
| Bytes | `ByteArray` 8 times in the library (hex view, clipboard pictures), 86 in the apps | A `bytes` type for binary data, now that a string is text only. Described below the table. |
| Lists and maps | 1,538 collection calls in the library, 728 in apps | Kotlin's names beside Astra's: `add`, `addAll`, `size`, `isEmpty`, `first`, `last`, `firstOrNull`, `any`, `all`, `none`, `forEach`, `forEachIndexed`, `mapNotNull`, `sumOf`, `maxOf`, `sortedBy`, `joinToString`, `indexOfFirst`, `getOrPut`, `getOrNull`, `subList`, `reversed`, `distinct`, `zip`, and so on; `listOf`, `mutableListOf`, `mapOf`, `emptyList`. |
| Sets and deques | 113 / 16 | A set type; a deque or efficient removal at the front. |
| Structural map keys | `HashMap<Pair<Int, Role>, Color>` in `Palette`, and elsewhere | Maps and sets that key a data class such as `Pair` by its contents; 17 places in the library and the apps use one. A list is a key by its contents too (see "As built in M2" below). |
| Numbers | `coerceIn`, `coerceAtLeast`, `coerceAtMost` (very common), `Int.MAX_VALUE`, `roundToInt` | Helpers and constants. |
| `Regex` | 63 | Native class (N5). |
| Files | `java.io.File` is in the public API (`onFilesDropped: (List<File>) -> Unit`, `largeTextView(file)`) | A `File(path)` object with `name`, `path`, `exists()`, `readText()`. Astra's built-in `File` is a holder of static functions under the same name, so it has to grow into this. |
| Namespaces | 75 files, 330 public classes | One per folder (5.1). `import Jade` gives the root package, `import Jade.Chart` a sub-package. |
| Threads and async I/O | 1 thread in the core; every app runs background work | Section 5.5. |

**Strings.** Today `length`, indexing, `slice` and `indexOf` count UTF-8 bytes while a `for`
loop steps by character: `"naïve café"` has a length of 12, loops 10 times, and
`slice(0, 3)` cuts `ï` in half. From now on a string is a sequence of characters, where a
character is one Unicode code point:

- `length`, `s[i]`, `slice`, `substring`, `indexOf` and every other position count
  characters. `"naïve café".length` is 10, and a slice can never cut a character in half.
  A caret is an index, and `caret + 1`, `text[i - 1]` and `text.substring(0, caret)` in
  ported code are correct as written.
- `s[i]` and a `for` loop give one character at a time, as a one-character string, so
  `for c in s` loops `s.length` times.
- **Where it differs from Kotlin:** a character outside the basic plane, mainly emoji, is
  two units to Kotlin (a surrogate pair) and one character to Astra. Ordinary index
  arithmetic is the better for it, since it can no longer land between the two halves. Code
  that handles the halves by hand is rewritten: 5 places in the library, all in the HTML
  entity decoder, and 27 in Snips.
- A character made of several code points (a letter with a separate combining accent, a
  flag) still counts as several. JadeUI does not handle those either.
- Strings stay UTF-8 inside the runtime, so the rest of the C++ code is untouched. A string
  that is all ASCII, which is most of them, indexes as fast as today; one with other
  characters gets a small table from character position to byte position, built the first
  time it is indexed.
- `upper` and `lower` (and Kotlin's `uppercase` / `lowercase`) handle all letters, not only
  ASCII.

For ASCII text nothing a program can observe changes. For other text every position does.

**Bytes.** A string no longer doubles as a buffer of bytes, so binary data gets a type of its
own, `bytes`, standing where Kotlin has `ByteArray`:

- Fixed length, changeable elements: `bytes(16)` makes sixteen zeros, `bytes([1, 2, 255])`
  copies a list, `b[i]` reads and writes, `b.size`, `b.slice(from, to)`, `+` joins two.
- To and from text, with the encoding named: `"héllo".toBytes()` (UTF-8 by default) and
  `b.decodeToString()`.
- Wherever data is not text: `File(path).readBytes()` and `writeBytes(b)`, pipes and sockets
  (5.5), picture data on the clipboard, the hex view, image encoding and decoding.
- In a type annotation it is `bytes`, lowercase like the other built-in types; the porter
  rewrites `ByteArray`.
- An element is a number from 0 to 255. Storing anything outside that range is an error.
  A Kotlin `Byte` runs from -128 to 127, so this is one more entry for 5.3.

**As built in M2 (2026-10-12).** What was settled while building it, and what is left to the
porter:

- **Where the library lives.** What Astra code can do for itself is written in Astra, as
  extension functions and classes in `src/std/*.astra`, packed into `astra.exe` and loaded
  before every program: the collection and string helpers, `Set`, `Deque`, `StringBuilder`,
  `File`. They run as fast as the native ones (a `forEach` over a million items takes what
  the native `each` takes), and unlike a native method that calls back into a lambda they
  will be able to suspend (5.5). Native are the methods called most (`add`, `first`, `get`,
  `coerceIn`), and what Astra cannot do: reading files, `format`, telling letters from digits.
- **Map keys.** A list, a range, `bytes` and a data object are keys by what they hold; any
  other object is a key as itself. That settles the question left open above: a list may be
  a key, and an equal list finds the entry. A set tells its items apart the same way.
- **A lambda after a call** goes to the last parameter and the parameters between take their
  defaults, for methods and extension functions as for functions
  (`joinToString { it.name }`). One exception lets a function parameter be followed by
  optional ones, which Kotlin does with overloads: if every parameter that has to be given
  is there but the last of them, the lambda is that one (`groupBy { key }`,
  `groupBy({ key }) { value }`).
- **Named arguments to built-in methods**, for the ones Kotlin code names:
  `contains(x, ignoreCase = true)`, `indexOf(x, startIndex = n)`, `padStart(length = 3)`.
- **`size`, `keys`, `values`, `entries`, `indices`, `lastIndex`, `first`/`last` of a range,
  `absoluteValue`, `sign` and `code`** are read without brackets. `keys`, `values` and
  `entries` are lists made on the spot, not views: `map.keys.removeAll { ... }` and
  `map.values.remove(x)` change nothing and the porter rewrites them.
- **`put` and `remove` on a map give the value that was there**, as in Kotlin (`remove` gave
  `true` or `false` before).
- **`orEmpty()` on `null` is an error**, because strings, lists and maps each have one and
  `null` does not say which is meant. The porter writes `x ?: ""` or `x ?: []`.
  `isNullOrEmpty()` is one function for all of them.
- **One whole-number type.** `Int.maxValue` is Kotlin's 2147483647 so that a limit means the
  same number, but arithmetic goes on past it instead of wrapping. `toLong` is `toInt`.
- **Constants are camelCase** here too: `Math.pi`, `Int.maxValue`.
- **`File`** reads and writes a whole file at a time, on the calling thread. Reading in
  pieces and not blocking come with 5.5.
- Upper and lower case, and which characters are letters, digits and spaces, come from
  Windows for characters outside ASCII until the text engine brings its own tables (M4).
- **The math functions stay in `Math`** (decided 2026-10-12): there is no bare `min`, `max`,
  `abs` or `sqrt`. The porter writes `Math.min(a, b)` for Kotlin's `min(a, b)`, and so on
  for the rest of `kotlin.math` (or `minOf`, `maxOf`, `absoluteValue`).
- **Left to the porter**, because the names would not be Astra's: `println` is `print` and Kotlin's `print` is `write`; arrays are lists
  (`IntArray(n)` is `List(n) { 0 }`); a `Comparator` is a function of two values;
  `filterIsInstance<T>()` is `filter { it is T }`; a sequence is a list; and the bit
  operations `shl`, `and`, `xor` are the operators `<<`, `&`, `^`.
- `Regex` is still to come (N5).
- A compiled `.astc` now carries the Astra-written library, about 85 KB, where it was a few
  hundred bytes for a small program. A built `.exe` is 0.18 MB larger than before.

### 5.5 Threads and async I/O

Astra has neither today. JadeUI itself barely needs them: one UI thread does everything and
the core starts a single background thread, to index a large text file. The apps are the
reason:

| App | Background work |
| --- | --- |
| JadeMem | Three single-thread executors (memory sampling, pool attribution, disk scan), with results handed to the UI thread. |
| JadeGen | A child Python process spoken to over pipes and a socket with reader threads, and threads for long jobs. |
| Snips | A thread accepting connections from a second launch of the app, and a thread pool running its evaluator. |

What is added, in dependency order:

1. **Execution contexts.** The VM is split into what is shared (objects, globals, interned
   strings, the collector) and what belongs to one line of execution (value stack, call
   frames, error handlers, open upvalues, native temporaries). Threads, suspending functions
   and workers all stand on this.
2. **Threads.** Real operating-system threads sharing one heap, with one of them running
   Astra code at a time. They switch at the points where the collector may already run (calls
   and loop back-edges) and whenever one blocks in native code. So:
   - objects are shared between threads as in Kotlin, and thread code ports as written;
   - a race can give a wrong answer but cannot corrupt memory or crash the VM;
   - Astra code does not run faster on several cores. The parallelism is in native work that
     runs outside the lock: I/O, system calls, image decoding.

   Surface: `Thread(function, name)` with `start()`, `join()` and `isDaemon`;
   `Thread.sleep(ms)`; a lock, with `synchronized(lock) { ... }`; a blocking queue; a small
   executor (one thread, or a pool), since two of the apps are built on them, whose tasks
   give back a future that can be waited on with a time limit and cancelled; and an
   interrupt flag a running thread can check (`Thread.isInterrupted`), which is how Snips
   stops a script that overruns. `@Volatile` is accepted and means nothing.
3. **The event loop.** One per process, and the same loop that delivers window events:
   timers, plus a `post(task)` that is safe from any thread. A console program has it too
   and ends when its script has finished, no non-daemon thread is running and the loop has
   nothing left.
4. **Suspending functions.** A function can set itself aside in the middle of a call and be
   taken up again later, where it was, without holding a thread. This is Kotlin's `suspend
   fun`, and it follows Kotlin's model, not the `async` / `await` of JavaScript and Python:
   - A suspending function is called like any other. There is no keyword at the call.
     `suspend` on a declaration is accepted and documents intent; any function may suspend
     when it runs inside a flow.
   - Underneath, each flow runs on a small stack of its own (a fiber), which is why it can
     pause several calls deep.
   - Two primitives, under Kotlin's names: start a flow (`startCoroutine`), and
     `suspendCoroutine { c -> ... }`, which sets the flow aside until `c.resume(value)` is
     called, on whichever thread calls it.

   The reason is Snips. Jade's dialogs are modeless and answer through a callback, and Snips
   wraps them so that a command asking several things reads top to bottom: 23 `suspend`
   functions, 149 calls to them, 55 flows started, built on those two primitives and nothing
   else. It ports as written. With `await` at every call it would not, and with callbacks
   alone the 55 flows become nested callbacks.
5. **I/O.** Files, child processes with their pipes and exit code, TCP sockets (connect and
   listen), and fetching a URL. There is one set of functions, written as ordinary calls:
   - Called on a background thread, a read or a write blocks that thread. That is how the
     apps do all their I/O today (JadeGen's reader threads loop on `readLine`), and it ports
     as written.
   - Called inside a flow on the UI thread, the same call sets the flow aside until the data
     is there, and the window stays live. This is the async I/O: no second set of functions
     and no callbacks.
   - Called on the UI thread outside a flow, it blocks the UI, as it would in Kotlin.

   Underneath, worker threads make the blocking calls.
6. **Isolated workers.** A worker is a second Astra VM on its own thread, with its own heap
   and its own lock. It shares nothing with the program that started it; the two exchange
   messages, which are copied. Workers are what threads are not:
   - Astra code in a worker really runs on another core, at the same time as the main
     program.
   - A worker can be killed outright, however stuck it is, because nothing else depends on
     its memory.

   A worker is started from a file or folder of code that it loads itself, not from a
   function: a closure cannot cross, since everything it refers to lives in the other heap.
   A message can hold numbers, strings, booleans, null, `bytes`, and lists and maps of those.
   Surface: `Worker(path)`, `post(message)`, a handler for messages coming back (delivered on
   the event loop), `terminate()`.

   Threads come first, since the apps' code is written for them. Workers stand on the same
   execution contexts and are added after: none of the three apps needs them to port, and
   the first real use would be Snips' evaluator, which is self-contained, takes values and
   returns a value, and wants a hard kill for a script that never reaches a checkpoint.

7. **Parallel sections** (added 2026-10-13; milestone M3.5). Several cores running Astra code
   on the same objects at the same time, for the one shape of work where that is safe: many
   items, each worked on by itself.

   ```astra
   balls.parallelStream().forEach { it.move(width, height, steps) }
   val totals = orders.parallelStream().map { priceOf(it) }.toList()
   ```

   Threads take turns and workers copy; this does neither. The items are dealt out to the
   cores, and for the length of the section the runtime holds each core to what the program
   is promising:
   - **A core owns the items it was dealt**, and whatever those items alone hold, and
     whatever it creates. It may change those.
   - **Everything else can be read and not changed**: the variables the lambda sees, other
     objects, globals. A write to something the core does not own is an error that names
     it, raised there. It is never a damaged list or a crash. So `{ it.move() }` runs on
     every core, and `{ it.move(); moved.add(it) }` is refused the first time it runs.
   - **A result comes back by being returned**: `map`, `filter`, `sumOf`, `count`, `any`,
     `all`, `minOf`, `maxOf` put the cores' answers together on the calling thread.
   - **Numbers in bulk are shared by slice.** `bytes`, and a buffer of numbers to be added
     with it, can be dealt out in pieces that do not overlap, each owned by one core. That
     is what filling the points of a picture for the graphics card needs, or painting a
     picture band by band.
   - Inside a section there is no waiting: no lock, no I/O, no setting a flow aside. Those
     are errors there. The section itself is an ordinary call that returns when every core
     has finished, with the first error if there was one.

   What it takes underneath: the interpreter's working state, which exists once today and is
   handed from thread to thread, becomes one per core; objects made in a section are kept on
   the core's own list and joined to the rest at the end, and the collector waits for the
   end; every instruction that writes to an object asks who owns it, which costs nothing
   outside a section and one comparison inside; and each built-in function is marked as
   safe to run on several cores or not, the rest being errors in a section.

   What it is not for: work that is quick for each item and few in number is faster left
   alone, since starting the cores has a cost. And it does not draw. Moving the 5000 balls
   of the example is 0.6 ms of a 26 ms frame; the rest is the old library drawing circles
   one call at a time, which no core can share. In Jade the graphics card draws them all in
   one call, and what the cores are for is working out the numbers it is given.

   **As built in M3.5 (2026-10-13).** What was settled while building it, and where it
   differs from the above:

   - **No change to the interpreter.** "One interpreter state per core" turned out not to
     need the state taken out of the VM. Each core has a VM of its own, made once and kept,
     with its own stack and calls; these VMs work on the heap of the VM that started the
     section, which waits with its lock held so that none of its threads runs. A core's VM
     has a copy of the table of variables, which it reads and never writes, and uses the
     root's table of names.
   - **Ownership is one byte in every object**: 0 for shared, otherwise the core that owns it.
     Every place that looks into an object asks: the property and index instructions, method
     calls, loops, comparing and printing, the keys of maps, and every built-in method as it
     is called, with those that change what they are called on marked as such. Outside a
     section the question costs one comparison that never fails.
   - **A core owns the item, not what the item holds.** "Whatever those items alone hold"
     cannot be told without walking the whole heap, so what an item holds is shared: it can
     be read, and replaced by something the core has made, but not changed in place. An item
     that is itself a list or a map is owned like any other.
   - **Another core's item cannot even be read**, since its owner may be half way through
     changing it. Asking whether two values are the same object is allowed, so a core can
     find its item in a list of everyone's.
   - **The same object twice in one stream is an error**, as it would be two cores' at once.
   - **The buffer of numbers is not built.** `bytes` is shared out by position
     (`parallelChunks`); a buffer of floating-point numbers is added when Jade has points to
     hand the graphics card (M5).
   - **Built-in functions that wait, or that work on what the program has one of**, are
     refused in a section by name: threads and locks, the event loop, flows, files and
     streams, workers, windows, `input` and `exit`.
   - **Measured** on a machine with 24 cores: moving 50,000 balls 7.4 ms in one loop and
     0.9 ms in a section; 64 counts of primes 2084 ms and 129 ms. Starting the cores costs
     0.04 ms, so a section pays from about 300 quick items. Dealing the items out and taking
     them back is done by the one waiting thread, which is what keeps quick items from
     gaining more.
   - **The bounce example is one line different** (`gui_bounce_parallel.astra`) and, as
     foreseen, no smoother: 37 frames a second at 5000 balls either way.
   - **The stress setting named in the milestone was a mistake**: running every `forEach`
     of the library as a section would refuse the many lambdas that change a variable, as
     it should. Instead `ASTRA_CORES` sets the number of cores, and the section tests are
     run with 1, 2, 3, 7 and all of them, under the sanitizer.
   - **What there is no tool for**: nothing on Windows finds two cores touching the same
     memory, so the rule was checked by going through every instruction and every built-in
     that writes. All writes are either checked or made to an object just created.

Rules for Jade, the same as JadeUI's:

- The tree, layout, painting and events belong to the UI thread, the one running the loop.
- Safe from any thread: every member of `Window`, `Jade.requestRepaint()`,
  `Jade.requestFrame()` and `Jade.scheduler`, which with a delay of 0 is how a
  background thread hands a result to the UI.
- Anything else called from another thread raises an error naming the thread and the call.
  Kotlin lets such a call through silently; here a porting mistake shows at once.
- Inside Jade, the large-text view keeps its indexing thread, and file dialogs and image
  decoding run on native workers.

Tested as the collector is: a stress setting that switches threads at every safe point, run
over the whole suite.

**As built in M3 (2026-10-13).** All six points: workers (point 6) were planned for M10 and
were brought forward. What was settled while building it:

- **Execution contexts.** A `Context` holds one line of execution: its value stack, its
  calls and their catch blocks. Each thread has one and each flow has one. The interpreter
  keeps working through members of the VM, as before, and a context's state is moved in and
  out when it starts and stops running, so the instruction loop is no slower for it.
- **The lock.** One thread runs Astra code at a time. A thread that has waited 5 ms asks the
  one running to give way, which it does at its next safe point; a thread that blocks lets
  go at once. `ASTRA_THREAD_STRESS=1` changes threads at every safe point. A list or a map is
  never seen half changed, and the `Atomic...` classes need no lock of their own: a method
  with no call in it runs in one piece.
- **Flows have no thread and no stack of the machine's**: a flow is a context, and setting
  it aside means leaving the instruction loop with its calls still in place. So a flow
  cannot be set aside from inside a built-in function that has called back into the
  program, as this plan foresaw. To leave few such places:
  - every method of a list, map, string or range that takes a function is now written in
    Astra, `map`, `filter`, `each`, `find`, `reduce`, `first { }` and `forEach` included;
  - `apply`, `run`, `also`, `let` and `with` set the block going as a call of the program's
    own instead of calling it from inside themselves;
  - what is left is the comparison function of `sort`, a property's getter, `toString()`
    when something is printed, `==` when a collection compares its items, `iterator()`, and
    the initial values of fields. A flow that tries to wait there gets an error saying so.
- **Kotlin's coroutine names**, and nothing above them: `startCoroutine` (as a method of a
  function, and as a function taking the lambda), `suspendCoroutine`, `Continuation`,
  `Result`, `EmptyCoroutineContext`, `runCatching`. Snips' `Flows.kt` ports line for line,
  and is in the tests in that form. `suspend` is accepted in front of `fun` and in a function
  type. Added on top: `delay(ms)` and `future.await()`.
- **Where a flow goes on.** On whichever thread resumes it, there and then, as in Kotlin
  without a dispatcher. What the runtime itself resumes (the end of a read, a `delay`, an
  awaited future) it resumes on the main thread, from the event loop.
- **One event loop**, the `EventLoop` object: `post`, `after`, `every`, `run`,
  `isLoopThread`. It starts when the script ends. In M5 the window's messages are waited for
  in the same place the loop waits now, so there is one loop and not two.
- **Errors nothing catches.** In the script or in a task of the loop: the program ends, with
  the report and exit code it had before. On another thread: that thread ends, with the
  report, and the program goes on, as in Kotlin. In a flow: the error goes to whoever was
  running it, unless it was started with a completion.
- **One set of I/O functions** (`VM::blocking`): on a thread the call blocks with the lock
  released; in a flow on the main thread a worker thread does the waiting and the flow is
  set aside. `File`'s whole-file calls go the same way, and a file can now also be read and
  written a piece at a time.
- **Java's names for threads and I/O**, since that is what the apps are written in:
  `Thread`, `synchronized`, `Executors`, `Future`, `CountDownLatch`, `AtomicInteger`,
  `ProcessBuilder`, `Process`, `Socket`, `ServerSocket`, `InputStream`, `OutputStream`,
  `IOException`, `InterruptedException`, `TimeoutException`. Where they differ:
  - `Future` is also what Kotlin code calls `CompletableFuture` (it has `complete`);
  - a stream reads and writes text itself, so `bufferedReader()` and `bufferedWriter()`
    give the stream back;
  - `wait`, `notify` and `notifyAll` are methods of `Lock`, not of every object, though any
    object can be the lock of a `synchronized`;
  - a time limit is a number and a `TimeUnit`, with its names in camelCase
    (`TimeUnit.seconds`), and milliseconds when no unit is given;
  - `LinkedBlockingQueue` is `BlockingQueue`; `ConcurrentHashMap` and the synchronized
    collections are plain maps, lists and sets, which the lock already keeps whole.
- **`@Volatile` is accepted** and means nothing. Any other annotation is an error, so that
  one with a meaning (`@Synchronized`) is not passed over in silence.
- **`return` inside `synchronized { }`** is `return@synchronized`, because it is a call and
  not expanded in place as `let` is. If the apps turn out to return from it often, it can be
  expanded in place too.
- **For a program to talk to a copy of itself**, as Snips does for a second launch:
  `Process.currentCommand()` is what started it. `flush()` sends on what has been printed,
  which matters when another program is reading it.
- **Workers** as point 6 describes them, with these particulars:
  - the script is named by a string in the call (`Worker("physics.astra")`) and compiled
    into the program there, as a module with names of its own, so that a built executable
    carries its workers; the worker loads its own copy of the whole program and runs that
    one module;
  - in the worker, the program that started it is `Worker.parent` (`onMessage`, `post`,
    `close`);
  - `terminate()` stops the worker at its next safe point, not by killing its thread: the
    two runtimes share the process's memory allocator, and a thread killed inside it would
    take the program with it. A worker stuck inside one built-in call is not stopped until
    the call returns;
  - a worker is a daemon or not, like a thread; it has no windows;
  - measured: four workers do four jobs in the time one takes, and a message of 25,000
    numbers takes about 0.3 ms each way.
- **The old `gui` library's `App.run()`** now waits with the lock released and runs this loop
  inside its own, so threads, workers and `EventLoop` work in a program with a window. That
  is the shape M5 gives Jade's loop.
- **Not built:** fetching a URL (point 5 names it; the milestone does not, and none of the
  three apps' background work in the table above needs it); the three I/O kinds are on
  Windows only.
- **What it cost:** `map` and `filter` now run Astra code instead of C++ and are slower for
  it (over a million items, `map` takes 40 ms where it took 26, `filter` 39 where it took
  31); a plain loop and everything else are as fast as before. An error inside the lambda
  shows one more line in its trace, the library function's.

### 5.6 Not needed

- **Overloading.** About 25 overloaded names, almost all "old blocking form versus new callback
  form" or "text versus model". Each merges into one Astra function that looks at its
  argument (`textField("abc")`, `textField(model)`); call sites are unchanged. The deprecated
  blocking dialog forms are dropped, as the GLFW backend already answers null for them.
- **Generics** beyond accepting the syntax, **`private` and `protected` on class members**
  beyond accepting the words, **annotations**, `inline` / `reified`, explicit-API mode, `ServiceLoader`, `swing()`.

## 6. API compatibility rules

1. Names, parameter names, order and defaults are identical to the Kotlin source, except for
   the deliberate changes in 6.1 and the dropped prefix in 6.2. Named arguments and defaults already work in Astra, so
   `cell(x, fill = Fill.HORIZONTAL, weightX = 1.0)` carries over with the constant in camelCase: `Fill.horizontal`.
2. JDK types in public signatures keep their names, provided by the compat layer with the
   members JadeUI and its apps use: `Color` (293 uses in the API), `Font` (109),
   `BufferedImage` (76, backed by a native surface), `Rectangle` (33), `Dimension` (30),
   `File` (8), `Insets` (4), `Point` (1). `Graphics2D`, `Shape` and the `java.awt.geom`
   types appear only in chart, svg, math and image; they are defined with tier N3.
3. Built-in type names stay lowercase in Astra, and the porter rewrites Kotlin's: `Int`,
   `Long` → `int`; `Double`, `Float` → `float`; `String` → `string`; `Boolean` → `bool`;
   `ByteArray` → `bytes`;
   `List<T>` → `list<T>`; `Map<K, V>` → `map<K, V>`; `Unit` → no return type. Astra does not
   accept the Kotlin spellings as aliases.
4. Known deviations, to be kept in a list beside the code: the window types, `WindowSpec`
   and the opener's interface (6.1); blocking
   dialog calls removed; `swing()` and `SwingContent` removed; the backend modules' classes
   are not ported; `JadeHostProvider` and `JadeHostSpec` removed, since there is one kind of
   window, nothing to choose between, and no provider to hand a spec to (no app uses
   either); extensions on built-in types become functions.

### 6.1 `Window` replaces `JadeHost`, `JadeWindow` and `WindowSpec`

JadeUI has two window types and two ways to open one: `JadeHost`, the app's main window,
made by constructing a host; and `JadeWindow`, the handle of a dialog or secondary window,
made with `JadeUi.windowOpener.open(WindowSpec(...))`. In Jade there is one class, `Window`,
and one way: construct it and call `show()`.

- **Making one.** `Window(root, title, initialWidth = 900, initialHeight = 640, exitOnClose,
  menuBar = null, onClose, onFilesDropped, onFilesDroppedAt, icons, onFront)`: the
  parameters of `jadeHost(...)`, with the same names and order. After them, by name, come
  the settings a `WindowSpec` carried: `owner`, `focus` (the component to focus first),
  `dialog`, `sizeToContent`, `centerOnOwner`, `resizable` and `keyFilter`. The function
  `jadeHost` and the class `WindowSpec` are not carried over.
- **`exitOnClose` defaults to `true` for a window with no owner and to `false` for one with
  an owner.** A Kotlin host exits the program when closed and a window from the opener never
  does; the default keeps both behaviours without the porter having to remember either.
- **Members**, the union of the two Kotlin types: `root`, `show()`, `close()`,
  `requestRepaint()`, `fitToContent()`, `title` as a property and `setTitle(text)`,
  `toFront()` and `bringToFront()` as two names for one action. Both spellings of the last
  two stay so that calls written against either Kotlin type still work.
- **Constructing a `Window` opens nothing.** It holds its settings. `show()` hands it to
  `Jade.windowOpener`, which realises it and gives back a handle; `toFront()`, `close()`,
  `fitToContent()` and a change of title pass through to that handle. Every window goes this
  way, the main one included.
- **`Jade.windowOpener` stays a replaceable object,** and that is the point of keeping it.
  The normal opener makes a native window; the hidden-window test mode is another opener;
  and a test can install a fake that makes nothing and records what it was asked for. The
  dialog tests work that way: 22 tests in the main dialog file put a fake opener in place
  and then type, press buttons and send Enter or Escape through it, and Snips' own tests
  replace the opener in two files.
- **The first `show()` starts the event loop**, as it does for a Kotlin host.
- **In app code** a variable declared `JadeHost?` or `JadeWindow?` is declared `Window?`.

What a port changes:

| Kotlin | Jade |
| --- | --- |
| `JavaFxHost(root, title = ..., onClose = ...)` (three places) and `jadeHost(...)` (one) | `Window(...)`, arguments untouched |
| `JadeUi.windowOpener.open(WindowSpec(title, content, width, height, focus = f, onClosed = g))` (JadeGen `HistoryWindow.kt:157`, Snips `PictureWindows.kt:75`, and the one place in the dialog code every dialog goes through) | `Window(content, title, width, height, owner = ..., focus = f, onClose = g)` followed by `show()`. Three names change: `content` is `root`, `width` and `height` are `initialWidth` and `initialHeight`, `onClosed` is `onClose`. |
| The owner is whichever window is in front, implicitly | Passed as `owner`. Jade offers the window in front for a caller that wants the old behaviour. |
| `open(...)` answers null when there is no host; Snips checks for it | `Window(...)` always gives an object. A window that cannot be realised fails at `show()`. |
| A test's fake opener receives a `WindowSpec` and returns a fake `JadeWindow` | It receives a `Window` and returns a recording handle. About six small fake classes are rewritten (four in the library's tests, two in Snips'). The tests themselves mostly stand, since they act through helpers on the fake (`w.type(...)`, `w.key(Key.ESCAPE)`, `w.press("OK")`); where one reads `w.spec.title` it reads `w.title`. |

The opener's interface changes with this: it realises a `Window` and returns a handle where
it used to take a spec and return a window. The handle is a new, small type with the four
operations that pass through; its name is still to be chosen.

### 6.2 The `Jade` prefix is dropped, and how names are cased

**Casing.** Everything we build follows one rule (settled 2026-10-12):

- **Libraries and classes are in Title case**: the library is `Jade`, a sub-folder `Jade.Chart`,
  and so is every class, enum, interface and object (`Window`, `ListBox`, `Event.Click`).
- **Methods, fields, variables and constants are in camelCase**: `openWindow`, `windowOpener`.
- **The built-in type names are lowercase** (`int`, `string`, `list`, ...). They are the one
  exception.
- **Constants and enum entries are camelCase too**, JadeUI's included: `Font.BOLD` is
  `Font.bold`, `Fill.HORIZONTAL` is `Fill.horizontal`, `MAX_TRACK` is `maxTrack`. This is the
  one place where Astra's names differ from Kotlin's by more than the dropped prefix. The
  porter's rule is mechanical (UPPER_SNAKE to camelCase) and applies to about 3,350 uses
  across JadeUI and the three apps. A keyword is a name wherever only a name can come, so
  `ImageStore.CLASS` becomes `ImageStore.class` like the rest. An entry called `NAME`
  (`SortKey.NAME`, and `Value.NAME`, `T.NAME`, `Table.NAME` in Snips) becomes `name` like the
  rest: a class tells its own name with `className`, so `name` is free on the enum, where
  `SortKey.name` is the entry and `SortKey.size.name` each entry's own name. Only `entries`
  cannot be the name of an entry. A member of an enum called `entries`, `name` or `ordinal`
  is an error unless it is written `override` and is a property of the same type as the one
  it replaces; anywhere else those three are ordinary names. Inside the enum `super.entries`,
  `super.name` and `super.ordinal` still give the originals. (`super` reaches a superclass's
  properties in general, not only its methods.)

The library and its global object are both called `Jade`. That works: `Jade.Button` is the
library's class, and a name the library does not declare, `Jade.requestRepaint()`, is looked
for on the object.


Jade is the one standard GUI library, so its classes do not carry its name. A class called
`JadeXxx` in Kotlin is `Xxx` in Astra; everything else about it is unchanged.

Of JadeUI's 227 public top-level types, 72 have the prefix. Four are the window types
already dealt with above, and two are named on their own account:

- **`JadeUi` is `Jade`.** It is the toolkit's one global object (repaint and layout
  requests, the clipboard, the scheduler, the dialogs, the frame and input hooks), named
  after the library rather than after a widget, so it takes the library's name:
  `Jade.requestRepaint()`, `Jade.inputDialog.prompt(...)`, `Jade.scheduler`. Nothing in the
  three apps or in JadeUI is called `Jade` today.
- **`JadeList` is `ListBox`.** By the rule it would be `List`, one capital letter away from
  Astra's own `list` type and the name every ported signature is being changed from.
  `ListBox` avoids that and matches the class's factory, `listBox(...)`, the way `ComboBox`
  matches `comboBox(...)`.

The other 66 are renamed by the rule:

| Package | Astra names |
| --- | --- |
| root | `Block`, `Button`, `Canvas`, `CheckBox`, `CheckItem`, `ChecklistDialog`, `ChoiceDialog`, `Clipboard`, `ComboBox`, `Component`, `Content`, `DialogHandle`, `Doc`, `Drafts`, `Event`, `FileDialog`, `FindDialog`, `FormDialog`, `Grid`, `IconButton`, `ImageDialog`, `InputDialog`, `Label`, `Menu`, `MenuBar`, `MenuBarView`, `MenuEntry`, `MenuItem`, `OptionsDialog`, `ProgressBar`, `RadioButton`, `Screen`, `Scroll`, `Separator`, `Slider`, `SourceWindow`, `Spinner`, `Split`, `Table`, `Tabs`, `TextArea`, `TextField`, `Toggle`, `ToolbarSeparator`, `Tree`, `WindowOpener`, `WrappedLabel` |
| sub-packages | `ChartView`, `CodeView`, `Dialogs`, `DocView`, `HtmlView`, `ProseView`, `FormulaBar`, `Sheet`, `GlView`, `HexView`, `ImageView`, `PictureCanvas`, `PictureEditor`, `PictureViewer`, `LargeTextView`, `MapView`, `MathBlock`, `MathView`, `SvgView` |

- **Unchanged:** the 155 types that never had the prefix (`Sizes`, `Style`, `Palette`,
  `SharedTextModel`, `RadioGroup`, `TreeNode`, `PromptSpec`, the capability interfaces, the
  enums), every function (`label(...)`, `grid { }`, `textField(...)`), every parameter, and
  every nested name. Nested types follow their owner: `JadeEvent.Click` is `Event.Click`,
  `JadeGrid.Cell` is `Grid.Cell`.
- **No two Jade types end up with the same name.** Checked against all 227.
- **A class and its factory now differ only by the first letter** for 23 widgets: `Button`
  and `button(...)`, `Grid` and `grid { }`, `TextField` and `textField(...)`. Astra is
  case-sensitive, so both exist, as `JadeButton` and `button` do in Kotlin: the class is the
  widget's content, the function returns it wrapped in a `Component`.
- **In app code the rename is mechanical.** The three apps name a prefixed type 137 times
  (JadeMem), 167 times (JadeGen) and 676 times (Snips); the most frequent are
  `JadeComponent`, `JadeMenuItem`, `JadeUi` (becoming `Jade`), `JadeEvent` and `JadeCanvas`.
- **Apps have classes of their own under these names.** With the prefix gone, 12
  declarations in the three apps share a name with a Jade class. Ten are nested inside
  another class and stay out of the way because nested names are scoped. Two are top-level:
  `TextField` in JadeGen and `Block` in Snips. Folder namespaces (5.1) keep both apart from
  Jade's: the app's own class wins inside the app, and Snips reaches Jade's `Block` through
  an alias.

## 7. Order of work

The work goes one milestone at a time. A milestone is built, tested and shown, and the next
one starts only when the result is accepted. Nothing beyond the current milestone is
committed to.

- **Built:** inside a milestone the work lands one feature at a time, each with its tests,
  as the language has been built so far. Direction can change at any point.
- **Tested:** each milestone names what it ends with. That is run and reported, together
  with the full test suite, also under the sanitizer and the stress settings.
- **Shown:** something that runs, to try by hand, with a short account of what was built,
  what turned out differently from this plan, and what that means for the milestones after.
- **Versioned:** the version of `astra` goes up with each milestone: M1 is 0.1.0, M2 is
  0.2.0, M3 is 0.3.0, M3.5 is 0.3.5, and so on. `astra --version` then says which milestone a build is.
- **Accepted, or not:** the next milestone starts on a go-ahead. If the result is not good
  enough it is reworked first; if it changes what should come next, the plan is revised
  before going on.

Later milestones are described in less detail on purpose. Each is planned properly when it
is reached, with what the earlier ones taught.

**M0. Two spikes.** *Done and accepted. What they found is in
[m0-findings.md](m0-findings.md); the code is on the branch `m0-spikes`.* Spike 1: lambda literals, trailing
lambdas and receiver blocks in the parser, to find the grammar conflicts before committing
(a `{` after a call in an `if` header, and telling a map literal from a lambda by its first
tokens), and the two loading questions left open under namespaces in 5.1. Spike 2: an OpenGL window
that draws smooth shapes and text from a glyph atlas, draws the same picture into an offscreen
framebuffer, and calls a script function per event. Text is the part to prove: the spike
reads Segoe UI, rasterises it with Jade's own hinting rule at 12 and 13 pixels, and the result
is judged by eye beside the same text drawn by Windows.

**M1. Language, first tranche.** Everything in 5.1, and the changes settled in 5.3. The two
changes to what current programs mean go in first, each on its own with the existing tests,
examples and README updated in the same change: the new meaning of `..`, `{}` becoming the
empty lambda with `{:}` for the empty map, and `==` comparing lists and maps by content.
Each feature lands with golden tests, as the language has so far.

The M0 spike already has lambda literals, trailing lambdas, receiver blocks, the `{}` change
and the type syntax working in the real parser, compiler and VM, as prototypes. M1 turns
those into finished features and adds what the spike left out: the rule for `return` in a
lambda, the scope functions expanded in place, a lambda's parameter list wrapping over
several lines, destructuring parameters, an instruction of its own for calling a receiver's
method by its bare name, a direct path for trailing lambdas, and proper errors.

**M2. Standard library.** 5.4: character-indexed strings, the `bytes` type, the Kotlin
collection and string names, sets, `Pair`, number helpers, `File` objects. The string change
comes first, since the new string functions are defined in terms of it.

**M3. Threads and async I/O.** Section 5.5, in its order: execution contexts, threads, the
event loop, suspending functions, then files, processes and sockets. It comes before any window code so that
the window loop is built on the runtime's loop rather than beside it. Ends with a console
program that runs a child process and reads its output without blocking while a second thread
updates shared state, and a flow that asks two questions in turn through callbacks, all
passing under thread-switch stress. Isolated workers (5.5, point 6) are
not part of this milestone as planned: the execution contexts are built so that a second VM
can run beside the first, and workers themselves were scheduled with M10, where Snips is the
first to want them. (They were built with M3 after all; see "As built in M3" in 5.5.)

**M3.5. Parallel sections.** Section 5.5, point 7: `parallelStream()` on lists and ranges
with `forEach`, `map`, `filter` and the reductions; ownership checked on every write;
`bytes` dealt out in slices. In this order: one interpreter state per core; allocation and
collection around a section; the ownership check; the built-in functions sorted into safe
and not; the stream methods. Ends with the bounce example moving its balls with one line
changed and nothing else; a heavier piece of work (each item taking milliseconds) shown to
go faster by about the number of cores; a lambda that writes to a shared list getting the
error; and the whole suite under the sanitizer and the stress settings, with a new setting
that runs every `forEach`, `map` and `filter` of the standard library as a parallel section.

**M4. Text engine.** Section 4, "The text engine". It needs no window: it reads font files
and produces numbers and pictures, so it is built and tested on its own, with its output
saved as images and compared. In two parts, so that the window work is not held up:

- **Part one, which M5 depends on:** both outline formats, variable fonts, font matching
  and fallback, measuring and positioning by the one rule, rasterising, hinting, colour
  emoji. Ends with a page of text in several fonts and sizes rendered to an image, measured
  widths matching drawn widths exactly.
- **Part two, alongside M5 to M8:** shaping, direction and the Unicode data. Ends with
  Arabic, Hebrew, Devanagari, combining marks and emoji sequences rendered correctly against
  reference pictures. The engine is not finished until this is.

**M5. Native tier N1 and the compat types.** The old `gui` library is deleted here (section
3, point 4). Ends with a script that opens a window, draws text and shapes, echoes raw
events, and renders the same picture to a PNG with no window.

**M6. Core, part one.** `Jade.kt` (types, events, capability interfaces, `Palette`,
`elide`), `Canvas.kt` (`Canvas`, `RecordingCanvas`, `DrawOp`), `JadeComponent.kt`
(`Component`: cache, layout, dispatch, focus helpers), `JadeGrid.kt` (`Grid`), `Widgets.kt`
(label, button), `RenderLoop.kt`, the `Jade` object, and `Window`. File names here are the
Kotlin sources being ported. The root package refers to `doc`, `gl`,
`sheet` and `dialogs`, so those get stubs for now. Ends with a grid of labels and buttons in
a real window and the matching Kotlin tests passing.

**M7. Core, part two.** `Text.kt`, `TextNav.kt`, `TextArea.kt` (text model, field, area,
undo, selection), `Scroll`, `HScrollBar`, `Split`, `Tabs`, `Controls` (check box, radio,
slider, progress, list, toggle, spinner), `Collections` (tree, table), `Combo`, `Screen`
(menu bar, popups, context menus, tooltips, accelerators), `Chrome` types, `Drafts`,
clipboard. Ends with the examples' `Launcher.kt` ported with its widget tabs, close to line
for line; that file is the acceptance test for "painless".

**M8. Owned windows and dialogs.** The dialog-side settings of `Window` (owner, first focus,
size to content, centre on owner, key filter), `DialogSession` and the eight
dialogs, async file dialogs (N2), then `chart`. Ends with **JadeMem's gui module ported**,
its three background samplers running as threads: it is the smallest real app and uses
exactly this much.

**M9. Self-contained packages.** `code`, `hex`, `text`, `treemap`, `map`, and `Regex` (N5).

**M10. Documents.** N3, then `svg` and `icons`, `math`, `css` / `html` / `doc`, `sheet`,
`image`. This is more than half of the library by line count and is what Snips needs.
Isolated workers (5.5) are built here too, ahead of Snips' evaluator.

**M11. 3D view.** N4 (mostly the OpenGL bindings for Astra) and `gl`, when JadeGen is to be
ported. The window is already OpenGL, so the view's place between the interface and its
popups is there from M5.

Port the tests with the code. The root package alone has 71 test files, and most assert on
recorded draw operations, which carry over. Tests that hard-code numbers derived from Java's
font metrics will need new numbers: no other font engine measures text exactly as AWT does.

## 8. Decisions

### Settled

| | Decision | Outcome |
| --- | --- | --- |
| | The library's name in Astra. | `Jade`, in Title case like every library and class we build; methods, fields, constants and enum entries are camelCase, JadeUI's upper-case constants included (`Fill.horizontal`); the built-in type names stay lowercase (6.2). |
| | What `import` brings in. | `import Jade` makes its names available as `Jade.Name` only; `import Jade.*` also lets them be written bare (5.1, namespaces). |
| | Classes named after the Kotlin backends. | None. No GLFW or JavaFX names; `JadeHostProvider` and `JadeHostSpec` are dropped. |
| | The window types. | One class, `Window`, made with `Window(...)` and opened with `show()`, replacing `JadeHost`, `JadeWindow`, `WindowSpec` and `jadeHost(...)` (6.1). |
| | The old `gui` library. | Unsupported and removed (section 3). |
| | Threads and async I/O. | Added to Astra (5.5). The thread model is D10 and the way code waits is D11, both below. |
| | The `Jade` prefix on class names. | Dropped. The global object is `Jade`; `JadeList` is `ListBox` (6.2). |
| | Static members. | A `static { ... }` block, with the `static` prefix as shorthand for one member. No companion objects (5.1). |
| | Enums. | Declared with `enum Name { ... }`, without Kotlin's `class`. An entry may be written by its bare name wherever the destination is declared as that enum; elsewhere the full name is required (5.1). |
| | Data classes. | Declared with `data Name(...)`, without Kotlin's `class` (5.1). |
| D1 | Lambda literals, trailing lambdas, receiver blocks, implicit `this`. | All four are added (5.1). |
| D2 | The meaning of `a..b`. | `..` includes its end, as in Kotlin; `..<` and `until` exclude it; `..=` is removed (5.3). |
| D3 | The native drawing backend. | OpenGL: every window is an OpenGL window and all drawing goes through it, chosen so that an app's 3D view layers cleanly under the popups (section 4). |
| D4 | How strings are counted and indexed. | By character (code point), not byte. The same as Kotlin except for emoji and other characters outside the basic plane, which are one character here. Binary data gets its own type, `bytes` (5.4). |
| D5 | Kotlin's type names (`Int`, `String`, `Boolean`, `Double`) as aliases. | No. The built-in names stay lowercase and the porter rewrites them (section 6, rule 3). |
| D6 | `and` / `or` versus Kotlin's bitwise infix words. | Astra keeps its meaning; the porter applies the rule in 5.3. |
| D7 | `return` inside a lambda literal. | Kotlin's rule. In a lambda given to `let`, `also`, `apply`, `run` or `takeIf` a bare `return` leaves the enclosing function; in any other lambda literal it is a compile error; `return@label` leaves just the lambda (5.1). |
| D8 | `{}` in an expression. | The empty lambda, as in Kotlin. The empty map is written `{:}`; map literals with entries are unchanged (5.3). |
| D9 | `WindowSpec` and `Jade.windowOpener`. | `WindowSpec` is folded into `Window(...)`, the one way to make a window. `Jade.windowOpener` stays as the replaceable object that `show()` uses to realise a window, so tests can still put a fake in its place. `exitOnClose` defaults to false for a window with an owner (6.1). |
| D10 | The thread model. | Both of two kinds. Threads share one heap, with one running Astra code at a time, so thread code ports as written. Isolated workers, each a separate VM exchanging copied messages, give real parallelism and a hard kill; they are added after threads (5.5). Threads that run fully in parallel on one heap are not planned; parallel sections (5.5, point 7) give several cores to work on separate items, with ownership checked, and that is as far as it goes (revised 2026-10-13). |
| D11 | How code waits without blocking. | Kotlin's model: suspending functions, called like any other, running on fibers. No `async` or `await` keywords. One set of I/O functions that block on a background thread and suspend inside a flow (5.5). |
| D12 | How far to commit. | One milestone at a time: build it, test it, and move to the next only when the result is accepted (section 7). |
| D13 | Namespaces, and an app class with a Jade name. | One namespace per folder, as with Kotlin packages. A folder's own names win over imported ones, and a library never sees the names of the program that imports it (5.1). |
| D14 | `==` on two lists, maps or sets. | Compares their contents, as in Kotlin. `===` compares identity (5.3). |
| D15 | The value of an element of `bytes`. | 0 to 255, not Kotlin's -128 to 127. Code that tests a byte's sign is rewritten by hand (5.3). |
| D16 | Where text comes from. | An engine of Jade's own, with no third-party code, complete from the start: both outline formats, variable fonts, shaping for every script, text direction and colour emoji, measuring by one rule on every system (section 4). |
| D17 | Where the window and its input come from. | Written by us, directly against Windows (a Win32 window with a WGL context), with no third-party code. It sits behind a small interface so that another system needs one new file and no change to the toolkit (section 4). |
| | How files are loaded under namespaces. | Importing a folder loads every `.astra` file directly in it, as one unit. Importing a file loads that file only, into its folder's namespace (5.1). |
| | A lambda's parameter list over several lines. | Allowed, as in Kotlin (5.1). |

### Open

None. Two questions are deliberately left for the milestone that raises them:

- **The measuring rule on scaled displays** (M4). M0 found that giving every character a
  whole number of logical pixels leaves spacing a little loose at 2x. The rule needs a
  clause: round each width in device pixels and report the total in logical ones.
- **How JadeMem's real sampling is done** (M8): on sample data, or through a way for Astra
  to call Windows system functions, which this plan does not size.

## 9. Risks

- **The language work is larger than the library's first milestone.** Section 5.1 is about
  fifteen features; several (interfaces, lambdas with receivers, implicit `this`, nested
  classes) touch the compiler, the VM and the bytecode format.
- **Grammar conflicts.** Astra writes `if cond { ... }` without parentheses; with trailing
  lambdas, `if ready() { ... }` is ambiguous. Swift has the same problem and forbids trailing
  closures in conditions. Spike 1 exists to settle this early.
- **Namespaces change how every existing program is put together.** Imports stop being
  global, so current multi-file Astra code, the tests under `tests/modules` among it, has to
  be revisited, and the bytecode file and the standalone-executable bundling both carry
  module information that changes shape.
- **Receiver lookups cost a run-time check** per unqualified name inside a builder block,
  and Kotlin lets a receiver's member hide a top-level function of the same name (`label`).
- **OpenGL and a text engine of Jade's own make the native layer much bigger.** Smooth
  shapes, a font reader, a rasteriser, hinting, the glyph atlas and font lookup all have to
  exist before the first label appears, where a ready-made 2D library would have supplied
  them.
- **The text engine is now one of the largest pieces of the whole plan.** Shaping for every
  script, text direction and the Unicode data are what a hundred-thousand-line library
  exists to do, and writing them from scratch is more work than porting Jade's root
  package. It is also work that none of the three apps needs in order to run: their text is
  Latin. It is in the plan because the engine is meant to be complete, and it is ordered
  (M4, part two) so that it cannot delay the first window.
- **Small text looking good enough** was the question the M0 spike was built to answer, and
  it does: our rasteriser draws Segoe UI at 11 to 28 pixels more evenly than Windows'
  greyscale rendering and close to ClearType, a little lighter at 11 and 12 pixels. What is
  left is tuning (a slight darkening of thin strokes) and the rule for scaled displays. The
  fallback of letting Windows rasterise the glyphs is unlikely to be needed.
- **OpenGL is weakest where desktop apps often run.** Over Remote Desktop, in virtual
  machines and on old integrated graphics, OpenGL 3.3 can be missing or slow, and there is
  no automatic software fallback as there is for Windows' own drawing. If that turns out to
  matter, the remedy is shipping a software OpenGL (Mesa) or a translation layer (ANGLE)
  beside the program.
- **"Same names" is not "same behaviour".** Text metrics, font fallback and antialiasing will
  differ from Java2D, so layouts will be a pixel or two different from the Kotlin app.
- **Threads reach into the centre of the VM.** Splitting execution state from the heap
  touches the interpreter loop, the collector, error handling and every native that keeps
  temporaries. Races in the runtime itself are hard to find: ThreadSanitizer does not run on
  Windows, so the safeguards are the single lock around all script-visible state and a stress
  mode that switches threads at every safe point.
- **A flow cannot pause inside a C++ function.** If `forEach`, `map` or `sort` is native and
  calls back into Astra, a suspending call made from that callback has a C++ frame in its
  way. Kotlin allows it there, and Snips will do it. So the collection functions that take a
  lambda are written in Astra or expanded in place, and what remains native reports a clear
  error instead of misbehaving.
- **App logic outside the UI is only partly covered.** Threads, processes and sockets (5.5)
  give the apps what their background work needs. JadeMem's kernel-memory queries through
  native calls, JadeGen's 3D rendering and Snips' evaluator are still theirs to port.
- **No IME.** JadeUI has no input-method composition in either backend; the port inherits
  that unless it is added natively.
