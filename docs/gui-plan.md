# Plan: Jade, Astra's GUI library, based on JadeUI

Status: agreed plan, every decision settled (section 8). Work proceeds one milestone at a
time (section 7). M0, the two spikes, is done and accepted; what it found is in
[m0-findings.md](m0-findings.md) and is folded in below. M1, the language, and M2, the
standard library, are done. M3, threads and async I/O, is built; what was settled along the
way is under "As built in M3" in 5.5 (and "As built in M2" in 5.4). M3.5, parallel
sections, is built too (5.5, point 7). M4, the text engine, has its first part built
("As built in M4, part one" in section 4); its second part goes alongside M5 to M8. M5,
the native layer and the types from the JDK, is built ("As built in M5" in section 4), and
the old `gui` library is gone. M6, the first part of the root package (components, the
grid, labels and buttons, and `Window`), is built ("As built in M6", 6.3). M6.5,
static types (5.7), M6.6, value types, lists of values and the language choices, and M6.7, a faster
VM, were added after it; M6.5 is next. M12 (networking), M13 (JadeGate) and M14 (native code)
were added at the end.

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
- Whatever is done in C++ is open to every program (the user, 2026-10-17): each piece has an
  Astra API of its own, as public as any library's, so programs and other libraries get the
  same speed the library that needed it gets. The text engine, PNG, the software drawing
  engine, cryptography, and M13's physics, spatial grid and vector maths are each usable on
  their own, never only from inside the one library that asked for them. Raw built-ins
  (`__text`, `__jade`) stay internal; the public face is ordinary Astra types. As long as
  opening it does not cost the library that uses it its speed: the public API adds no copy,
  check or extra call on the path Astra's own libraries take (they may use the same API, or a
  faster internal one where the public one must check more), and a piece whose fast path
  depends on staying inside (the text engine's glyph caches, the renderer's batches) shows
  what it can without giving that up.
- No Kotlin and no JVM anywhere in Astra. JadeUI is the specification, not a dependency.
- Astra being a good language comes before porting Kotlin easily (the user, 2026-10-17). Porting is done
  once, but Astra's speed, stability and readability matter for every program ever written in it,
  so they come first. The porting question is whether Astra can do
  what the code does as well or better, not whether the code compiles unchanged.
- Astra stays self-contained (the user, 2026-10-17): `astra something.astra` runs with nothing else
  installed, and a built program needs no `astra.exe`, sources or `lib`. No milestone may require
  more; M14's native code stays an option beside built programs that need no C++ compiler.

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
   spacing comes out loose (found in M0). The clause, as settled in M4: a font is made for a
   size and a scale, and every number it gives is in device pixels, each glyph's width
   rounded there. A size in logical pixels is the device number divided by the scale, taken
   of a whole run and never of one glyph at a time; where the toolkit wants a whole number
   of logical pixels for a run (`FontMetrics.stringWidth`), it rounds that quotient up, so
   the text always fits the room measured for it. At a scale of 1 the two are the same.
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

#### As built in M4, part one

Points 1 to 4, 8 and 10 to 12 are built, in `src/text` (about 4,500 lines) with PNG in
`src/image`. It needs no window and is in the runtime for every program, not only for Jade (D124).

**The parts.** `face` reads a font file: the table directory and collections, names, the
character map (formats 4, 6, 12, 13, and 0 as a last resort; symbol fonts), widths, the
line's measurements, and quadratic outlines with composites. `cff` runs the per-glyph
programs of cubic outlines, of the plain kind (`CFF `) and the variable (`CFF2`, with its
blends). `variation` reads `fvar`, `avar`, `gvar`, `HVAR` and `MVAR`:
outlines, widths and line measurements for any place on a font's axes. `color` paints
`COLR` version 0 and version 1 (every paint: solid, the three gradients with their three
ways of extending, transforms, glyph clips, layers, and the composite modes) with `CPAL`;
`bitmaps` reads the picture formats (`CBLC`/`CBDT`, `sbix`) and scales them. `raster` is
the exact-area rasteriser from the M0 spike, for quadratic and cubic curves. `fonts` is the
list of installed fonts with matching and fallback, `engine` a font at a size, and
`system.cpp` the one file written for each system. A font file is mapped into memory, not
read, and a font file once used stays mapped.

**What is asked of the system** is the list of installed font files (on Windows, the two
font folders) and nothing else: not the default font, not metrics, not a single glyph.
`system.cpp` also holds two short lists of names per system: which families "SansSerif",
"Serif" and "Monospaced" mean (Segoe UI, Times New Roman and Consolas on Windows; Segoe UI
Variable is used when asked for by name), and which families to look in first for a
character the chosen font lacks, before every other installed font is tried.

**Matching** is by family, then width, then upright or italic, then the nearest weight in
the direction asked for (the order CSS uses). A family is also found by its four-style
name ("Segoe UI Semibold") and by a full name. A variable font answers for every weight in
its range. A bold or italic the family lacks is made up, by thickening and by slanting.

**Measuring** is the rule of point 8, with the clause for scaled displays as given there.
Characters that only steer other characters (joiners, direction marks, variation
selectors) and control characters take no room. There is no kerning and there are no
ligatures, so a text's width is the sum of its characters' widths in every font.

**Hinting** is the M0 rule unchanged: the x-height and the capital height go to the
nearest whole pixel and what lies between is stretched to fit; nothing moves sideways and
nothing below the baseline moves. It is on unless a font is made without it.

**To Astra** the engine is `Fonts`, `Typeface`, `TextFont` and `Picture` (README, "Fonts,
text and pictures"), names of the engine's own. Jade's `Font`, `FontMetrics`, `Color` and
`BufferedImage` are written on them in M5, and M5's canvas takes glyph pictures from the
same engine into its atlas. A `Picture`'s pixels are an ordinary `bytes` value.

**Checked.** Every glyph of every font installed on the development machine reads without
a damaged result: 508 fonts, 1.8 million glyphs, 4 seconds; the variable ones at both ends
of their weights too. The same for the 332 fonts that other programs on the machine carry,
300 of them with cubic outlines (the installed ones have 12). All of it also under the
address sanitizer. Glyph widths of a variable font at 200 pixels agree with the ones GDI
gives. The PNG reader agrees with Windows' own on 26 pictures of every kind the format
has, written by Windows, and Windows reads the pictures written here. The tests hold
measuring to drawing: a text drawn whole is the same picture as its characters drawn one at
a time where measuring puts them, in five fonts, with fallback characters and at a scale
of 1.5.

**Not built, or built and not tried.**

- Cubic outlines in a variable font (`CFF2`) are built, and have met one font: no such font
  is installed here, and the one found on the machine is a test font of a single letter
  that the fonttools project carries. Its outlines at the places that project's own tests
  name come out to the coordinates those tests expect, exactly, as do those of the
  TrueType variable font beside it (13 axes). Neither font is in Astra's tests.
- `sbix`, Apple's picture format, is written to the specification and has not met a font:
  none is on this machine. `CBDT` was tried against Noto Color Emoji, which a program
  installed here carries, but that font is not in the tests since Windows does not have it.
- The styles a variable font names along axes other than weight ("Sitka Display",
  "Bahnschrift Condensed") are not families of their own. The weight is set as asked, and
  the cut for the size where a font has one (its "optical size" axis).
- Emoji stored as SVG are not read; no system's emoji font needs it.
- The slight darkening of thin strokes that M0 suggested is left as tuning for when text
  is on a screen (M5).
- Reading the names of all installed fonts takes about a tenth of a second the first time
  a program uses a font. If that shows at start-up, the answer is a small cache file.

#### As built in M5

Tier N1 is built, in `src/jade` (about 2,700 lines of C++) and `lib/Jade` (three files of
Astra), and the old `gui` library is gone with its backend, examples and test (section 3,
point 4).

**The parts.** `platform.h` is the interface that is written once for each system: a window
with an OpenGL context, the events it receives, cursors, the clipboard, waiting. `platform_win.cpp`
is the one for Windows, written directly against Win32 and WGL. `render` draws: one shader
program, shapes sent to the card as one small record each (76 bytes) and drawn instanced,
the two glyph atlases (coverage, and colour for emoji), and surfaces. `api.cpp` is the
built-in `__jade`, where layout units become device pixels. In `lib/Jade`: `Native.astra`
(`NativeWindow`, `Surface`, `NativeCanvas`, `Native`), `Input.astra` (`RawInput`, `KeyCode`,
`PointerButton`, `Modifiers`, `CursorKind`, `CursorMode`, ported from JadeUI's own types)
and `Awt.astra` (the JDK's names).

**Where it differs from what this section said.**

- **One OpenGL context, not one for each window.** A single context is made current on
  whichever window is being shown; every window has the same pixel format. The effect is
  the one the share group was for (textures and the glyph atlas are common to all windows)
  with less to keep in step, and the framebuffers and the vertex state, which a share group
  does not share, exist once.
- **Everything is drawn into surfaces; a window is shown by copying its surface to the
  screen.** Nothing is drawn straight to a window. That is what makes the picture with no
  window the same picture, and the window's retained picture comes for free.
- **Clipping is done in the shader**, from a clip rectangle that travels with each shape,
  not with the scissor rectangle. A thousand widgets each with a clip of its own are still
  one batch; the scissor would have ended the batch at every widget.
- **Colours are kept multiplied by their alpha** on every surface, which is what lets one
  surface be drawn onto another correctly. They are divided again when a surface is read.
- **Polygons are rasterised by the text engine** into the glyph atlas and kept by shape, so
  the arrows and marks a toolkit draws again and again cost one quad each after the first.
- **Layout units become pixels at one place**, `api.cpp`: a position is multiplied by the
  scale and rounded, so that at 150% the edges of rectangles and straight lines are still
  on pixel edges and neighbours still meet. Text is drawn with the font made for that
  scale, by the rule of section 4, point 8.
- **The window's events come through the runtime's own event loop** (`VM::loopPump`,
  `loopWait`): there is no second loop, a showing window keeps the program going the way a
  pending timer does, and while Windows runs its own loop (an edge being dragged) a timer
  keeps the program's tasks going.
- **A surface is collected.** A new kind of value, a resource, tells the native layer when
  the collector finds it unreachable; the surface is then deleted from the drawing thread.
  What a surface takes on the card counts towards the next collection.
- **`BufferedImage` is in two places**, memory and the card, each brought up to date from
  the other when it is asked for after the other changed. Its `createGraphics()` comes with
  tier N3; until then `canvas()` gives the toolkit's canvas on it.
- **The libraries that come with Astra may use the hidden built-ins** (`__jade`, `__text`):
  the rule that kept them to the standard library now lets in the `lib` folder beside
  `astra.exe`, and nothing else.

**To be known when porting.** As the key of a map, a `Color`, a `Font` or a `Rectangle` is
itself and not its value: the one such map in JadeUI's root package, the cache of font
metrics, is keyed by a string here. `Font.deriveFont(12f)` is written `deriveFont(12.0)`:
a whole number is a style and a decimal a size, which is how Java tells them apart too.

**Checked.** Every operation of the canvas is drawn on a surface and read back pixel by
pixel in the tests, the rectangles, lines and text also at scales of 1.5 and 2: what `fillRect` and `drawRect` cover, both
ends of a line, the corners of a rounded rectangle, the area of an oval and of a triangle,
clipping through two levels of sub-canvas, and text, which comes out of the card as the
same pixels the engine draws by itself to within 3 in 255. Windows are tested without
showing one: size, scale, the picture, and every kind of event sent through the path real
ones take, each key by name. A window on the screen was tried by a script that posts it
mouse and key messages, resizes it, and asks Windows for its picture: it draws, redraws at
the new size, echoes the events and closes. Fifty thousand balls are moved and drawn at 41
frames a second in one loop, 57 with the moving in a parallel section; the old library drew
five thousand at 35.

**Not tried, or not there.**

- A second screen of another density, the locked pointer under a real mouse, and files
  dropped from Explorer: the code is there and nobody has done it.
- Dragging a window by hand was the first thing tried once the examples were built, and it
  hung them: while Windows runs its own loop for a drag, the program's timers are run from
  a timer inside it, and that ran "whatever is due", which for an animation whose frame
  takes longer than its period is for ever. It now runs for a moment at a time
  (`VM::runEventLoop` with a budget) and gives Windows its turn back. A test puts a window
  into that state with a frame always due and requires a close request to arrive
  meanwhile; it fails with the old code. The test and the scripts here say the drag has
  begun by posting the message Windows sends; the drag loop itself is entered only by a
  real mouse.
- Any graphics driver but this machine's (an NVIDIA card). Intel graphics, virtual machines
  and Remote Desktop are the risk section 9 names, and still are.
- Scripted answers for file dialogs wait for the dialogs themselves (tier N2).
- No test shows a window: the suite runs unattended, three ways, under the sanitizer, and
  should not put windows on the screen of whoever is at the machine.

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
  before every program: the collection and string helpers, `Set`, `Deck` (`Deque` until D183), `StringBuilder`,
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
   stops a script that overruns. `@Volatile` is an error saying it is not needed (L47, D153).
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
- **`@Volatile` is an error** saying it is not needed (L47), as are Kotlin's other annotations
  for the JVM, each saying what to write (`@Synchronized` a lock, D153).
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

### 5.7 Static types (M6.5)

Added after M6, at the user's decision (2026-10-13 and 14): Astra becomes statically typed,
as Kotlin is. Until now a variable or parameter without a written type held anything, a
written type was checked only when the line ran, and `obj.name` was looked up by name with
nothing known in advance. The point is to find mistakes when compiling (a wrong argument,
a member that does not exist, `box.secret` from outside `Box`), and the Kotlin apps being
ported carry every type already.

**Settled by the user.**

- A variable takes its type from its initial value: `var a = 5` is an int, `val box = Box()`
  a `Box`. A written type still wins (`var n : number = 0`).
- `var sum = 0` then `sum += 0.5` is an error, as in Kotlin. `number` stays, for a value
  that may be either.
- Two integer types, `byte` (0 to 255) and `int` (64 bits), decided as D19 (L0 in
  language-choices.html).
- A variable or field that starts as `null` needs its type written: `var parent : Component? = null`.
- Every parameter has a written type, by class or interface: `fun adopt(child : Component)`.
  A parameter without one is an error. The same goes for a class header's entries.
- `any` is the type of anything; its fields and methods cannot be used until it is cast
  (`x as Box`), or tested (`if (x is Box) { x.secret }`).
- Generics: typed collections (`list<int>`, `map<string, Component>`, `set<T>`), function
  types (`(int) -> bool`, `Grid.() -> unit`), and the program's own generic classes and
  functions (`class Box<T>(val item : T)`, `fun <T> first(items : list<T>) : T`).
- No `final`. The compiler sees the whole program and every override, which is all `final`
  would tell it; `override` already keeps a method from being replaced by accident. `open`
  and `final` are not keywords: either one in front of a declaration is an error.
- The name of a function is never given another value (2026-10-18, D159), as in Kotlin:
  `area = { ... }` for a `fun area` is a compile error saying to keep a function that changes
  in a `var` of a function type. A call of a function then gives what the function declares,
  which lets the compiler leave out checks it has proved.
- Generic types are kept when the program runs, as in C# and unlike Kotlin and Java (which
  drop them for compatibility with code compiled before generics, a reason Astra does not
  have). A collection knows what it was made to hold (`items.itemType`, `map.keyType`,
  `map.valueType`), so does an object of a generic class of the program's own, and
  `x is list<int>` and `x as list<int>` are checked in full. A generic function is given
  its type arguments as hidden arguments, which is the one cost: about that of one more
  argument per call. A collection made where its type is known costs nothing more, and
  reading, iterating and adding re-check nothing. (Kotlin code that makes a wrong unchecked
  cast fails at the cast here, where on the JVM it fails later or not at all.)

**Proposed here, to be confirmed or changed when M6.5 is shown.**

- **Return types** are worked out from the body when not written, from `=> value` and from
  the `return`s of a block. A function that calls itself writes its return type. (Kotlin
  takes a block without one to return nothing; this asks less of ported code and of the
  library.)
- **Lambdas** take their parameter types from where they are passed: `items.map { it.name }`
  needs nothing written. A lambda with nowhere to take them from writes them,
  `{ x : int -> x + 1 }`.
- **Narrowing:** after `x is Box`, `x != null`, `x ?: return` and the like, a local
  variable, a parameter, or a `val` of `this` has the narrower type for as long as the test
  holds, as Kotlin's smart casts.
- **Collections are read-only unless marked `open`** (D32, built in M6.6; M6.5 has one list type): a `list<Button>` can be passed as a
  `list<Component>`, which may only be read through that name; an `open list<Component>` may
  also be changed and takes only a list of exactly that type, since a `Label` could otherwise
  be added to the buttons. Maps and sets alike; `frozenList`, `frozenMap` and `frozenSet` are
  what nobody may change (D26). A list literal takes its
  element type from where it goes (`show([ok, cancel])` with `show(items : list<Component>)`)
  or, with nowhere, from what its items have in common.
- **Overloading** by parameter types comes with it. Section 5.6 merged JadeUI's overloaded
  names into one function that looks at its argument; with every parameter typed that
  function would have to take `any`, so overloads are allowed instead, chosen when
  compiling, and API names stay as they are.
- **Built-in functions** (written in C++) get their types from declarations written in Astra
  (`external fun print(vararg items : any)`, `external fun string.substring(start : int,
  end : int) : string`), kept with the standard library.
- **Types are checked when compiling.** The checks the VM makes today for written types are left out wherever the compiler has
  already proved them, so running costs nothing more and typed code runs a little faster.
  Compiling takes longer, which matters only when running from source.

**Built in five parts,** each tested, the language working throughout. The checker first
runs beside the old rules and is switched on for good at the end, when every line of Astra
in the repository has its types.

1. **The checker's core.** Written types read in full (type arguments, function types,
   receivers); a type checker as a pass of its own between parsing and compiling, which
   knows every class, interface, field and method of the whole program; inference of
   variables; operators; calls to declared functions, methods and constructors (count,
   names, defaults, types); members on known types; `any` and casts; `null` and `?.`,
   `?:`, `!!`. The type `nothing` for jumps and functions that never return (D61), and `and`,
   `or`, `!` and conditions taking only `bool` (D62). Run with `astra check`, and by the tests, without stopping other programs.
2. **Generics and lambdas.** Type parameters on classes and functions, with bounds
   (`<T : Comparable>`); inference of type arguments; function types and lambdas with and
   without receivers; extension functions; narrowing; `when`; destructuring; overloads.
3. **Everything typed.** The case of type names first (D154): `List`, `Map`, `Set`, `Bytes`, `Sequence` and the frozen types Title case, a fixed set lowercase, so every type below is written once. Declarations for the built-in functions, with their parameter names so that any argument can be named (D116); the standard library
   (3,683 lines) with generics, its functions that relied on a trailing lambda filling the last
   required parameter made overloads (D56); Jade (3,757 lines), its types taken from JadeUI's Kotlin;
   the examples; the 117 test files (9,698 lines); the README. Then the rule is switched on:
   a parameter without a type, or code the checker refuses, is a compile error. (On since
   step 5: what the checker finds stops the program, all of it listed; a variable or field
   with neither a type nor a value is an error too, as D18 implies and Kotlin does.)
4. **Using the types when running.** The run-time checks the compiler has proved go;
   member access on a known class looked up by position where that is safe. An anonymous object's
   class compiled once instead of on every evaluation, its captured `val`s copied into hidden
   fields of the object and each captured `var` kept in a small shared box, as Kotlin does on
   the JVM (today each `object : Shape { ... }` rebuilds its class and method closures: about
   560 ns against 165 for a `data` object). Inline caches where a member
   cannot be found by position (calls through an interface, on a union or an `any`): each call
   site remembers where the member was last time. (Instructions chosen by type moved to M6.6 on
   2026-10-18, to come with generic types kept when running: until then a value of another type
   can still arrive through an erased generic or a built-in function, and the test is what
   catches it; leaving the tests out of the numeric loop measured only 1.3% faster.) The aim, measured before
   and after on a fixed set of benchmarks (a numeric loop, calls, field access, a JadeGate-style
   simulation slice): two to five times faster on typed numeric code.
5. **Smaller built programs.** (a) The M0 spike (`src/spike`, the `Spike` class) out of the
   build; the scripts in `spikes/m0` stay on the branch `m0-spikes`. (b) Code a program cannot
   reach left out of its compiled form, worked out from the types: functions, classes and
   methods never called (overrides of a called method in a reachable class kept, and what is
   found by name when running). Reflection keeps what D50 says. (c) Built programs made from
   `astra.exe` itself with what a running program never needs left out (decided 2026-10-18,
   D161): the code of the compiler, checker, parser, lexer, the cache and the shell is kept in
   a section of the executable of its own, and the standard library's source text in another,
   and `astra build` writes a copy of `astra.exe` without those sections' bytes (the space they
   took kept, empty, so that every other address stays as it was), then the program. One file,
   nothing in it twice, and the base never out of step with `astra.exe`. What both sides use
   (the inline functions and templates of the headers) stays in the common code: the section
   is chosen after a file's includes. Proved, not assumed: the build links what is kept once
   more on its own, which fails if it names anything left out, workers included; checks on
   the link maps that every function that link needs is in a section kept (a template the
   C++ library's own code uses may otherwise be made from a copy left out); checks that
   nothing left out runs as the program starts (a global of such a file is made on first use);
   and the tests run as programs built this way. (d) Jade's window, drawing and the text engine in a third such section,
   left out of a program that reaches neither Jade nor the text engine (fonts and images are
   in the standard library since D124, so not importing Jade is not enough). A built program
   reads only its own end to find its code, not the whole file. When part 5 began,
   `print("hi")` built was 2.98 MB (the executable, with 215 KB of compiled program) and ran
   in 11.0 ms, 4.5 of them the process itself; the widgets example 3.23 MB, with 472 KB of
   program. Sizes and start times measured before and after.

M6.5 ends with every test and example passing under the checker, a set of tests of the
checker's own errors, the suite under the sanitizer and stress settings, the timings
of part 4 and the sizes of part 5. Version 0.6.5.

### 5.6 Not needed

- **Overloading.** (Revised by 5.7: with every parameter typed, overloads are allowed after
  all.) About 25 overloaded names, almost all "old blocking form versus new callback
  form" or "text versus model". Each merges into one Astra function that looks at its
  argument (`textField("abc")`, `textField(model)`); call sites are unchanged. The deprecated
  blocking dialog forms are dropped, as the GLFW backend already answers null for them.
- **Annotations** (generics and the access words on members were listed here; generics come
  with 5.7, and `private`, `protected`, `internal` and `private set` are enforced since M6), `inline` / `reified`, explicit-API mode, `ServiceLoader`, `swing()`.

## 6. API compatibility rules

1. (Changed by D83: Jade may improve on JadeUI's names and shapes, but each change is put to the user first and made only when agreed; until then JadeUI's form is kept.) Names, parameter names, order and defaults are identical to the Kotlin source, except for
   the deliberate changes in 6.1 and the dropped prefix in 6.2. Named arguments and defaults already work in Astra, so
   `cell(x, fill = Fill.HORIZONTAL, weightX = 1.0)` carries over with the constant in camelCase: `Fill.horizontal`.
2. (Changed by D82: these types take names of Astra's own where they read better, decided
   type by type in L79 to L87 of language-choices.html.) JDK types in public signatures keep their names, provided by the compat layer with the
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
   either); extensions on built-in types become functions; a `Color`, `Font` or `Rectangle`
   as the key of a map is itself, not its value (as built in M5).

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
operations that pass through; it is called `WindowHandle` (6.3).

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

### 6.3 As built in M6

The first part of the root package is ported, in `lib/Jade`:

| Astra | From | Holds |
| --- | --- | --- |
| `Types.astra` | `Jade.kt` | `Sizes`, `Style`, `Fill`, `Anchor`, `Key`, `EditCommand`, `Event`, `Content`, the capability interfaces, `Palette`, `elide`, `metrics` |
| `Canvas.astra` | `Canvas.kt` | `Canvas`, `TextMetrics`, `DrawOp`, `RecordingCanvas` (`NativeCanvas` of M5 is now a `Canvas`) |
| `Component.astra` | `JadeComponent.kt` | `Component` with its kept picture, layout, dispatch, finding and focus helpers; `label`, `button`, `grid` |
| `Grid.astra` | `JadeGrid.kt` | `Grid`, `Grid.Cell`, `Grid.RowScope` |
| `Widgets.astra` | `Widgets.kt` | `Label`, `WrappedLabel`, `Button`, `IconButton`, `ToolbarSeparator` |
| `RenderLoop.astra` | `RenderLoop.kt` | `RenderLoop` |
| `Jade.astra` | the object `JadeUi` | `Jade`: repaint and layout hooks, frame hooks, the opener, focus |
| `Window.astra` | `Host.kt`, and `JadeGlSurface.kt` of the GLFW backend | `Window`, `WindowHandle`, `WindowOpener`, and `WindowHost`, the policy |

**The names left open in 6.1.** The handle an opener gives back is a `WindowHandle`
(`setTitle`, `toFront`, `close`, `fitToContent`, `requestRepaint`, `rootChanged`), and an
opener is a `WindowOpener` with the one method `open(window)`. `NativeWindowOpener` is the
usual one, `HiddenWindowOpener` the test mode (its windows are real and never shown), and a
test's own fake is a class of a few lines. A shown window's handle is its `WindowHost`,
which a test drives with `send(event)` and looks at with `snapshot()`.

**The policy** (`WindowHost`) is the Kotlin surface's, without what later milestones bring.
In: press capture for both buttons, drag and release to what was pressed, click counting by
the system's double-click time, hover with the leaving widget told, the pointer's shape,
the wheel with its Shift rule, who has the keyboard after a press, `FocusAware`, Tab and
Shift+Tab, named keys, typed text, the edit commands by the layout's letter, the key
filter, the back and forward buttons, files dropped, and when the kept pictures are
thrown away. Waiting for M7, with the things they act on: the screen (menu bar, popups,
drop-downs, context menus, accelerators), tooltips, the blinking caret, hold-to-repeat,
and raw input for 3D views.

**How a window is drawn.** A request for a repaint marks the window's loop dirty and posts
one task to the event loop, however many requests come before it runs; that task plans the
frame (`RenderLoop`), lays out if it must, and paints either everything or the damaged
rectangle through a clipped canvas onto the window's kept picture. A component's kept
picture is a `Surface` at the window's scale, drawn back pixel for pixel.

**Text is measured for the screen it is drawn on.** JadeUI measures with a scratch image
and lets Java scale the drawing. With the rule of section 4, point 8, a line is a few
units wider on a dense screen than the same line measured at a scale of 1 (up to 8 for a
long one at 2x), so layout that measured at 1 would cut text short. `Jade.layoutScale` is
the scale of the window being laid out or painted, set by its host, and `metrics(font)`
measures at it.

**Differences from the Kotlin, to be known when porting.**

- JadeUI's extension functions on `JadeComponent` (`layout`, `dispatch`, `record`,
  `renderToImage`, `componentAt`, `find`, ...) are methods of `Component`: a call reads the
  same. The one on a list, `nextFocus`, is a function: `nextFocus(list, current, forward)`.
- `filterIsInstance<T>()` is `filter { it is T }`.
- `renderInto` takes a `BufferedImage` as in Kotlin; `renderOnto(canvas, ...)` beside it
  paints on any canvas, which is what a window uses.
- A window's default title is "Jade", not "jadelib-ui".
- An error in a frame hook is reported like any other; JadeUI swallows it.
- A button is lit while the pointer is over it (asked for when M6 was shown): its edge is
  in `Palette.accent` and its body takes a little of that colour. JadeUI's button looks
  the same under the pointer as away from it; only its icon button changes.
- When the pointer leaves the window, the widget it was over is sent the same
  `Event.Hover(-1, -1)` as when the pointer goes to a neighbour. JadeUI only repaints it
  then, so an icon button at a window's edge can stay lit after the pointer has gone.
- A grid's layout written outside the program (`.ovr` files, and the `commands` file) is
  not read yet: it needs the XML reader of tier N5. `viewId` and `Jade.register` are there
  for it. `OverrideTest`, `CommandsTest` and `RobustnessTest` wait with it.
- `Jade` has what M6 uses. The dialogs, the clipboard and drafts come with their
  milestones.

**A gap in the language, found here and closed.** `private` on a member was accepted and
changed nothing, and inside a block that runs on an object (`grid { }`, `row { }`) a bare
name is looked for among the object's members before the program's top-level names.
`RowScope` keeps its grid in a private field called `grid`, which hid the function
`grid()` from every `row { }` block: a grid in a grid failed. Now the access words are
enforced for members: `private`, `protected`, `internal`, and the one in front of `set`
under a property. A member the code may not use is not what a bare name means in a block
written outside its class, so a builder can keep what it likes to itself (`Grid` has
`place`, `tracks`, `effective`, `occupied`) without hiding a name of the program's; and
`object.name` on such a member is an error.

How it is done: the compiler gives every class and every library a number and marks each
function with the numbers of the classes it is written inside of and of its library. A
class is told what it declares of each member (`Op::MemberAccess`, `Op::StaticAccess`):
the name then leads to the numbers the code using it has to carry one of. For a private
member that is the class that declared it, and any subclass that declared it again with
`private override`; for a protected one the class and, added as each is made, its
subclasses down to the object's own; for an internal one the library. Assigning has a
mark of its own beside it, for `private set`. Subclasses inherit the marks. Reading,
assigning or calling a member by name, with an object in front or bare in a block, is
refused when the function doing it carries none of the numbers.

The cost is kept off ordinary code. A name that no class restricts is passed with one
test. A member a class declares itself, reached through `this` by its own code, needs no
check at all, and the compiler writes an instruction that makes none (`GetOwn`, `SetOwn`,
`InvokeOwn`). Code written in the class of the object it works on, and an object whose
class restricts no name like the one asked for, are passed without a lookup. Four loops
of nothing but member access (no restricted members; a class's own private members; a
subclass using what it inherits; a public member whose name is private in another class)
run level with the build that checks nothing, within the 2% the timings scatter by.
Tests: `tests/member_access.astra`, and for a library's side `tests/jade_grid.astra`.

In Jade the words are as in JadeUI where a program cannot tell the difference:
`Component.parent`, `overlayDescendants` and `Jade.renderGeneration` have `private set`,
and `adopt`, `release`, `invalidateSizes`, `overlayDescendants` and `effectiveComponents`
are internal. `WrappedLabel.wrap`, `Jade.register` and `Jade.registeredGrids`, internal
in JadeUI, are left open: JadeUI's tests are part of its module and may use them, and
the tests carried over here are a program outside the library.

**Tests carried over** from JadeUI, with their names: `JadeGridLayoutTest` (4),
`GridHiddenTest` (1), `ComponentAtTest` (2), `RecordingCanvasTest` (5), `PaintStyleTest`
(4), `PaintCacheTest` (3), `RenderIntoTest` (2), `RenderLoopTest` (10), `PaletteThemeTest`
(3 of 5; two are about a list and a code view) and `WrappedLabelTest` (10): 44 tests, all
passing, in `tests/jade_grid.astra`, `jade_paint.astra` and `jade_loop.astra`. The host's
policy has a test of its own, `jade_host.astra`, since JadeUI's are written against GLFW.
`examples/widgets.astra` is the grid of labels and buttons in a real window; a click
posted to it by a script pressed its button.

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
  widths matching drawn widths exactly. (Built: `examples/text_page.astra` is that page;
  see "As built in M4, part one" in section 4.)
- **Part two, alongside M5 to M8:** shaping, direction and the Unicode data. Ends with
  Arabic, Hebrew, Devanagari, combining marks and emoji sequences rendered correctly against
  reference pictures. The engine is not finished until this is.

**M5. Native tier N1 and the compat types.** The old `gui` library is deleted here (section
3, point 4). Ends with a script that opens a window, draws text and shapes, echoes raw
events, and renders the same picture to a PNG with no window. (Built: `examples/window.astra`
is that script; see "As built in M5" in section 4.)

**M6. Core, part one.** `Jade.kt` (types, events, capability interfaces, `Palette`,
`elide`), `Canvas.kt` (`Canvas`, `RecordingCanvas`, `DrawOp`), `JadeComponent.kt`
(`Component`: cache, layout, dispatch, focus helpers), `JadeGrid.kt` (`Grid`), `Widgets.kt`
(label, button), `RenderLoop.kt`, the `Jade` object, and `Window`. File names here are the
Kotlin sources being ported. The root package refers to `doc`, `gl`,
`sheet` and `dialogs`, so those get stubs for now. Ends with a grid of labels and buttons in
a real window and the matching Kotlin tests passing. (Built: `examples/widgets.astra`, and
44 of JadeUI's tests carried over; see 6.3.)

**M6.5. Static types.** Section 5.7, in its five parts: the checker's core, generics and
lambdas, everything in the repository typed and the rule switched on, the types used
when running, and smaller built programs. Ends with every test and example passing under the checker, tests of the
checker's own errors, the suite under the sanitizer and the stress settings, timings and sizes.
Beside the checker, building only what changed (D145): files compiled once and cached, each
declaration's summary deciding what else must be compiled again, the standard library and Jade
compiled into `astra.exe`, a script's compiled program kept per machine for its next run, and a
starting budget measured on a ported app. (Built, version 0.6.5: the suite's 576 checks, every
test, the 18 examples and the standard library passing the checker, 13 files of the checker's own
errors, under the sanitizer and the GC and thread stress settings. On 19,400 lines a first run in
105 ms, a second in 16 ms, after a one-line change 66 ms (budgets 500, 20 on top of start, 100).
Part 4 against the baseline: numeric 3.2, fields 2.5, calls 2.0, simulation 1.5 times as fast;
an anonymous object 164 ns from 600. Part 5: `print("hi")` built 1.67 MB from 2.98 and 8.2 ms
from 11.1; a Jade app 2.20 MB from 3.23. Moved on: typed instructions to M6.6, with generic
types kept when running; how the VM goes from one instruction to the next to M6.7, D160.)

**M6.6. Value types, lists of values, and the language choices.** In two halves. The first, as
designed in [value-lists.html](value-lists.html):
value types (`value`), kept one after another in lists whose items are values, in place when
small, inside their objects when fields, in boxes otherwise; reads in place where the
compiler can show nothing changes the list meanwhile; results built where they go; `var`
fields changed through their place, and `mutating` methods; large items sorted through their
positions; generic list functions copied per layout. Generic types kept when the program
runs, as 5.7 settles (`items.itemType`, `x as List<int>` checked in full), and what the C++
functions return checked against their declarations, so that a type the compiler knows holds
while running; with that, instructions chosen by type, adding two `float`s with no test of
their kinds (moved from M6.5 part 4 on 2026-10-18). And where the compiler knows a
slot's type (an `int`, a `float`, a small value type, in a local or a field), the slot keeps only
its 8-byte payload, not the kind beside it, which only `any`, unions and generic parameters
need; that halves memory for typed code. (Fitting values in 8 bytes by NaN-boxing was set
aside: it leaves too few bits for Astra's 64-bit `int`.) Ends with the bounce example on every
core moving its balls in a list of values with `move` unchanged and nothing allocated in a
frame, and Jade's layout with `bounds` and `Sizes` inside their components, each measured
against today, and the suite under the sanitizer and the stress settings, with a new setting
that makes every read from a list of values a copy.

Part 1 (built 2026-10-18): generic types kept when the program runs and the C++ functions'
results checked, as D162 to D164 propose. Left for later, each known and tested around:
- A generic local function written inside another generic function sees the outer one's type
  parameters as `any` (its frame has its own type arguments, not the outer's).
- `object : Holder<T> { }`, an anonymous object with a supertype that has type arguments, does
  not parse yet: the look-ahead after `object :` does not skip `<...>`. When it does, the
  anonymous class, made once, would keep the type arguments of the first call that made it;
  a type parameter of the code around must make the class be made every time, as a variable
  of the code around does.
- A type parameter passed on (`isA<T>(x)` inside a generic function) has its type arguments
  looked up in the table of types on every call, under a lock on the cores. Cheap, but it can
  be cut (passing the frame's own when they are the same) if it shows in timings.
- Code placement moves the `fields` benchmark by 10 to 20% under MSVC with the same loop:
  compare against a baseline built the same way, and count instructions, before calling a
  change slower.

Part 2 (built 2026-10-19): value types as boxes, as D165 proposes. `bench/layout_values.astra`
against `layout_classes.astra`, Jade's grid layout with its geometry made new at every step:
84 ms against 94, a `Sizes` of three `Dimension`s one allocation of 88 bytes against four of
336. Left for later, each known:
- A value held in another and read on its own (`val pref = sizes.pref`) is copied into a box
  of its own. Read with one of its fields in the same expression (`sizes.pref.width`) it is
  read in place: the VM does the two reads as one (VM::readThrough). References into a value
  that need no copy come with the analyses (part 4).
- Jade's `Point`, `Dimension`, `Insets` and `Rectangle` are still classes: they are changed
  where they are (`setSize`, `translate`, `setLocation`), so they become values only with
  changing through a place (part 4), and that changes Jade's API, for the user to decide.
- A generic value type keeps no type arguments when the program runs: a `Two<int, string>` is
  any `Two` to `is` and `as`.
- A field of a class whose type is a value type holds a box until values inside their objects
  (part 5).

Part 3 (built 2026-10-19): lists of values, as D166 proposes. A list made to hold values keeps
them packed in one block (a packed list's storage apart from the list, so other lists grow by
one pointer); every list operation works on the block. Measured with `bench/points_values.astra`
against `points_classes.astra`, a million points: built 46 ms against 171, summed 34 against 40,
sorted by `x` 127 against 70, walked 77 against 55, 284 ms in all against 336, at most 146 MB in
use against 183 (the list itself 16 MB against some 112). `sortBy` and `sortByDescending`, for
every list, work out each key once and move each item once (C++, beside `compareValues`): the
million points sorted by `it.x` in 1.5 s before, now as above. Left for later, each known:
- An item read on its own, a loop's item and a lambda's `it` are copies in a box of their own
  (`for p in pts` costs a small allocation per item); read with one of its fields in the same
  expression, `pts[i].x`, it is read in place. References into a list come with the analyses
  (part 4); until then nothing on a stack points into a list, so there is nothing for the scan
  at a thread switch to do.
- Made where it goes (section 7, rule b) only for `pts.add(Point(...))` of a value type made
  from its header alone; `pts[i] = Point(...)`, `insert` and `copy(...)` make a box first.
- A worker is sent a list of values item by item, not as one block.
- Changing items in a parallel section waits for changing through a place (part 4): the items
  a core is given are copies, and a section only reads them.

Part 4 (built 2026-10-19): the analyses, results built where they go, and changing a value
through its place, as D167 to D170 propose. A value read where it is kept is a reference into
the list or box (on the stack only: anything that keeps it longer takes a copy), wherever the
checker shows that nothing which may change a list of that value type runs while it is in use;
a loop is judged by its body, a call that may wait counting as one that changes every list.
`pts[i] = pts[i].moved(1)`, `return Point(...)` and `replaceAll { it.moved() }` build the new
value in the item. A parallel section over a list of values gives each core references into
its own part of the list. Measured with `bench/bounce_classes.astra`, `bounce_values.astra`
(a mutating `move`) and `bounce_moved.astra` (`replaceAll { it.moved() }`), 200,000 balls
moved 200 times: 0.78 s, 0.46 and 0.59 on the 32 cores here, and 4.2 s, 6.1 and 7.6 on one;
`bench/particles.astra`, a million particles moved on every core and drawn by one loop for 100
frames, makes no box (five million every five frames before loops were judged by their
bodies), moving taking 0.93 s and drawing 8.0. Against part 3 built the same way, with Clang
and with MSVC: `points_values` 17 and 26% faster, `layout_values` 2 and 12%, `layout_classes` 2
and 11% (a `val` field given its value in `init` is now remembered by the instruction, as a
`var` field was); `calls`, `fields` and `simulation` 4 to 6% slower (19% for `calls` under
MSVC, where code placement alone moves it). Left for later, each known:
- A variable changed through its place is given a box of its own at every change (`p.x = 1`
  in a loop makes a box each time), since another variable may hold the box it had. Values in
  variables (D149, part 6) leave no box to copy.
- A field read through a list item finds the item again each time (the list, its items, the
  item), so on one core a mutating method on an item is some 1.4 times slower than on an
  object. Reading `this`'s fields at a known place is part 6's typed instructions.
- A thread that gives up the lock keeps its references into lists. Another thread reordering
  a list meanwhile is a race, which gives a wrong item, never damage: every reference is
  checked against the list's length and its type of item when it is used. So the scan at a
  thread switch has nothing it must do; a flow, which runs other code at the points where it
  waits, is handled by the checker (above).
- `pts[i]` read whole is judged by the whole function, not the statement; a loop by its body.
  (Closed 2026-10-24, G10: an item used at once by its statement, one kept in a variable by its
  for or while loop.)
- A list of values that may hold null is gone through by copies in a parallel section.
- In a parallel section, a built-in function given the list the section is over is refused
  (it would read every core's items), but for `isEmpty` and `isNotEmpty`.
- Calls, field access and the simulation slice are 4 to 6% slower than in part 3, the same
  instructions run: the checks for references, spread over the instructions, and what they do to
  how the compilers lay out the loop. The VM's loop rewritten for D160 (M6.7) is where to win
  it back.

Part 5 (built 2026-10-19): small values in the value itself, values inside their objects, and
fields and results read where they are, as D171 to D174 propose; generic functions copied per
layout measured and not built (D175). A value type of at most 8 bytes that holds no object is
kept in the value itself, with no box, wherever a value is. A field of a value type, not null
and larger, is kept inside its object, the field's slot a reference into the object once it is
given a value; `comp.bounds = Rect(...)` and `cached = measure()` build the value there. A field
read, and what a function gives back, are read where they are wherever nothing that may change
values of their type runs while they are in use, and always when read at once (`n.sizes().w`,
`return cached`). `list.add` copies an item it is given where it is, with no box made for it.
Measured, against part 4 built the same way: `bench/colors_values.astra`, 100 frames of a
320 by 240 picture blended with a palette of `Rgba(val argb : int)`, 24 to 31% faster with no
box made (15.4 million before); `bench/layout_cached_values.astra`, Jade's layout with each
component's `Sizes` kept inside it, 1,215 boxes made where part 4 made 57,126, its time 4 to 7%
more (the reads through references, below); the other benchmarks within 5 either way. Values
in variables, arguments and results (D149) are left for part 6, with the slots that keep only
a payload: they need the same change to how a frame's slots are laid out. Left for later, each
known:
- A value of a value type larger than 8 bytes in a variable, an argument or a result is still a
  box, so Jade's layout without kept sizes (`bench/layout_values.astra`) makes as many boxes as
  before, 470,042 in 200 passes. D149 (part 6).
- Through a reference into an object, a field is read by finding which of the object's values
  the reference is in, and reading on through a value held in it (`node.sizes().pref.width`)
  looks each field up by name; both are what typed instructions (part 6) make direct.
- Generic list functions that hand each item to a lambda (`filter`, `any`, `count` and the
  like) copy each item of a list of values, since the lambda may change the list: `filter` on a
  million points 98 ms, against 43 written out by hand. Inline functions (D59, the second half)
  remove it; copies per layout would not (D175).
- A field is kept inside its object when its type is written, or when it is given a value made
  by its type's name (`var cached = Size(0, 0)`); a field whose type is only worked out by the
  checker from anything else keeps a box. A field whose type names a class that does not exist
  when the first object is made keeps a box too.
- A value type of up to 8 bytes kept in the value itself holds ints, floats and bools only:
  storage widths (D148, the second half) let more fit.

Part 6 (built 2026-10-19): typed instructions and payload-only fields in objects, as D176 and
D177 propose; payload-only slots in frames, instructions chosen by type for arithmetic, and
values in variables (D149) moved to M6.7 by the user the same day (D178). A number's conversions
and Math's commonest functions, where the checker knows the call is the standard library's, are
one instruction (Numeric) in place of a call. An int, a float or a bool field whose type is
known, written or worked out from what it is first given, is kept as its 8-byte payload alone
inside its object, before the object's slots, so that each is at the same place in every object
of its class, and read and written there by the instructions that remember where a field is.
Measured against part 5 built the same way:
`bench/particles.astra` draws a million particles a frame 34 to 40% faster (`toInt()` an
instruction); a million objects of three `float`s take 112 MB at most, against 142 (72 bytes an
object, against 96); `numeric` 3 to 8% faster; the rest within 4 either way, with Clang and with
MSVC. Left for later, each known:
- A frame's slots still keep a value's kind beside its payload, and arithmetic tests its
  operands' kinds: both are M6.7's, with values in variables (D149), on the rewritten loop.
- A payload with no value yet reads as 0, 0.0 or false, where a slot reads as an error; the
  vals, and the fields an init must set, are still told apart, each by a bit of its own.
- A field whose type the checker works out is kept as a payload only when it is an int, a float
  or a bool; one whose type is a value type only when it is made by its type's name (part 5).
- Through a reference into an object, a field read on through a value held in it still looks
  each field up by name (part 5); M6.7's instructions that name a frame's slots are where a
  field's place, known when compiling, goes into the instruction.
- On one core a mutating method on the items of a list is still some 1.3 times slower than on
  objects (`bench/bounce_values.astra` 6.2 s against 4.8 with Clang): each field read through an
  item finds the item again. Keeping where a list's items are beside the list, one load in
  place of three, was tried and measured no faster, so it was left out; the item's place held
  in a frame's slot for the whole method is M6.7's.

Part 7 (2026-10-19): the first half's checkpoint. Jade's `Point`, `Dimension`, `Insets` and
`Rectangle`, and its `Sizes`, became value types, as the user chose the same day (D179): a
component's `bounds` and the sizes it keeps are inside it, changed through their places, and
`sizes = content.sizes()` builds the new ones there; `Rectangle` takes all four of its numbers
(D180). `examples/bounce_values.astra` is the bounce example on every core with `Ball` a value
type and `move`, its body unchanged, marked mutating. Measured against M6.5 (16200bd) built
the same way, MSVC and Clang:
- `bench/bounce_values.astra`, 200,000 balls moved 200 times on the 32 cores here: 0.59 s and
  0.53, against 0.86 and 1.06 for the class version on M6.5; in a frame, two objects are made
  (the parallel stream and the lambda), none for a ball, where the class version made its
  balls as objects.
- `points_values` against M6.5's `points_classes`: 5.7 to 7.4 times as fast; `layout_values`
  against `layout_classes` 1.13 to 1.26; `layout_cached_values` and `colors_values` level with
  their class versions on M6.5 (1.03 to 1.07), with no box made in a frame.
- Programs written with classes: `points_classes` 4 to 5 times as fast (sorting through
  positions), `layout_classes` 7 to 10% faster, `layout_cached_classes` 14 to 18%; `calls` 5
  to 21% slower, `simulation` 4 to 13%, `colors_classes` 8 to 9% (the checks for references
  and values spread over the instructions, part 4); `fields` 35% faster with Clang and slower
  with MSVC, where its loop's placement moves it.
- Jade's own layout (`bench/jade_layout.astra`, a tree of grids laid out 100 times): 3 to 6%
  slower than M6.5. The sizes pass between functions in variables and results, which keep a
  box until values in variables (D149, M6.7); and a widget not yet painted asks its content for
  its sizes again at each layout, so the sizes kept inside it are written more than read.
The suite (617 checks) passes as it is, with every read from a list of values a copy, under the
GC and thread stress settings, with the C++ results checked, and under the sanitizer, with
and without the GC stress setting.

The second half builds, on M6.5's checker, the language choices decided in
[language-choices.html](language-choices.html) (section 8): `byte`, with unsigned reads from `bytes` (D19); `>>>` and the unsigned helpers (D44); strings kept at
flexible width (D20); `char` and how it adds to strings, `'c' == "c"` (D24, D25); `op ==` with
`hashCode`, keys that cannot change, `frozenList`, `frozenMap` and `frozenSet` (D26); overflow
trapped, with `&+`, `&-` and `&*` (D27); a lambda for a one-method interface (D28); a function
calling itself last made a loop, and `tail` (D29); overloads that differ in type arguments
(D31); collections read-only unless `open`, `accepts`, and new collections made `open`, `toOpenList()`, read-only maps and sets passed by key, value and item type (D32, D34, D45, D46, D47); `extend` (D33);
`Throwable` and checked exceptions worked out by the compiler (D35, D51 to D54); `lateinit`, `lazy val`
and `lazy var` (D36, D37); full reflection (D38, D48, D49, D50); `enum class`, `enum interface` and every
`when` covering every kind (D39); union types and names for types (D40, D41); the nested `it` error (D55); `this@Name` (D57); `this` in blocks and more functions compiled in place (D58); `inline fun` (D59); a `for` over a map giving its entries (D64); data classes extended only by data classes (D65); an `object` made when first used (D66); statics only in one `static { }` block per class (D67); accessors in Kotlin's form only (D68); extension functions following imports (D70); `infix fun`, with `to`, `downTo` and `step` as ones (D72); `Pair`, `Triple`, map entries and indexed values as value types (D73); one name for each thing in the standard library (D74); limits on `int` and `float` at their real values, with sizes clamped at the native layer (D76); a map's keys, values and entries as read-only live views (D77); `println` and `print` as in Kotlin (D78); "task" for what was a flow, `task { }` and `suspendTask { resume -> }` (D81); one `Atomic<T>`, changeable in parallel sections (D85); `Lock`, `LockBox<T>` and `useKey { }` (D86); `Channel<T>`, its size always chosen (D87); `ThreadPool`, `JobQueue`, `after` and `every` (D88); `Job<T>` for a result to come, `task { }` giving one (D89); `Duration` wherever a time is meant (D90); `Process(...)` and `Process.run` (D91); `atomic var` fields changeable in parallel sections (D92); `gather(into, each, merge)` on a parallel stream (D93); `SharedMap` and `SharedSet` (D94); the error for another core's item pointing to `indices.parallelStream()` (D95); `parallel(...)`, nested sections, `parallelChunks`, `parallelSort` and `splitAt` (D96); frozen data shared with workers and `bytes` handed over (D97); sequences and generators (D98); `Color` as a value type (D99); a font's measurements asked of `Font` (D101); `Size` in place of `Dimension` (D102); `Point` as a value type (D103); `Rect` in place of `Rectangle` (D104); `Insets` as a value type (D105); `Drawable`, `Image` and `Surface`, drawn with `canvas.draw` (D106); `Path` in place of `File` (D107); `$$"""` raw strings (D112); `use { }` (D113); `thread { }` giving a `Job` (D117); `Countdown` (D118); `Reader` and `Writer` (D119); sockets with `readTimeout` (D120); `Picture` merged into `Image` (D122); images drawn in memory, surfaces and windows on the graphics card, one `Canvas` (D123). With D123, the canvas, image and surface work found on 2026-10-16: `Canvas` takes every call Jade's painting uses (`clearRect`, `clipped`, `drawImagePart`, `alpha` on drawing a picture, the font metrics; `withStyle` gone, the style being on the component (D132)), on every kind of canvas, the recording one too; an internal base class holds the origin, clip, scale, colour, font, `visibleBounds`, `sub` and `clipped`, so each engine (software, graphics card, recording) gives only the shapes, text and pictures in its own pixels; the default `fillPolygon`, which draws an outline, fills; `drawString` gives the width on every canvas; an image keeps a change number and the rectangle changed, and Jade its card copy (D124), in place of the two-way flags and the call on every draw; `crop` and `inkBox` in C++; `Image.load` and `save` take a `Path` (D107); `Surface.canvas` loses its `owner`, and `toPicture` becomes `toImage`, and `Surface.of` and `upload` take an `Image` (D122); an image's canvas works on any thread, so workers can draw thumbnails; and, to be asked about first (L57), an `opaque` flag on `Image` in place of `BufferedImage`'s type numbers. `Image` and `Font` in Astra's standard library beside the text engine (D124). Annotations as checked data, and the test runner `astra test` (D153). Each with tests of its own, the porting notes and the
README brought up to date, and the costs measured where a decision gave an estimate (the
overflow trap's 2 to 5%, string storage).

Part 8 (from 2026-10-20) builds the second half's language choices in nine groups, as the user
chose the same day: numbers and strings; equality and collections; types and declarations;
functions; errors; reflection; concurrency; files and input and output; graphics and Jade.

Group 1, numbers and strings (built 2026-10-20):
- Overflow (D27): an int `+`, `-`, `*`, `/`, negation, `pow` or `Math.abs` whose answer does not
  fit is an error saying so and naming `&+`, `&-` or `&*`, which wrap; constants fold the same
  way, so `const n = int.maxValue + 1` is a compile error. Measured with Clang against part 7,
  `numeric` 4% slower (with MSVC 7% faster: placement); the other benchmarks level.
- `>>>` and `>>>=`, and `toUnsignedString`, `compareUnsigned`, `divideUnsigned`,
  `remainderUnsigned` and `parseUnsigned` (D44).
- The limits (D76): `int.maxValue` and the rest, constants written in place; Kotlin's `Int`,
  `Long`, `Double` and `Float` holders are errors naming what to write. Jade's `maxInt` went for
  `int.maxValue`; the native layer clamps sizes to 32 bits (`VM::toInt32`).
- `$$"""` raw strings (D112): `$$name` and `$${...}` read, a lone `$` text; each more `$` in front
  lets one more in a row be text.
- Strings at flexible width (D20): 1, 2 or 4 bytes a character, the narrowest that holds the
  widest; the UTF-8 form made when first asked for and kept, as Python does (the UTF-8 a string
  is made from is not kept); the hash taken over the characters, so equal text hashes alike at
  any width. Joining keeps the wider part's width and goes on from the first part's hash;
  searching, slicing, splitting and ordering work on the characters. `bench/strings.astra`
  against part 7 (Clang): building text 5 to 12% faster, searching 7% slower, map keys 5%,
  sorting 4%; text with an emoji (4 bytes a character) 9 to 20% slower, as the decision foresaw.
- `char` (D24, D25, D142): a kind of value of its own; single quotes for one character only
  (`'ab'` is a compile error saying to use double quotes); D25's arithmetic, a result that is not
  a character an error; `'c' == "c"` and one key; comparing a char with a constant string of
  another length a compile error; `n.char` and `c.code`; `char(n)`, `chr`, `ord` and `toChar()`
  errors saying what to write. The standard library's string functions pass and give chars;
  Kotlin's overloads taking a `Char` are there (`split(',')`, `indexOf('x')`, `contains`,
  `startsWith`, `endsWith`, `replace`, `padStart(n, '0')`, `padEnd`). A char's own: `isLetter()`
  and the other questions, `upper()`, `lower()`, `digitToInt(radix)`, `digitToIntOrNull`.
- `byte` (D19): a kind of value of its own, what an item of `Bytes` is; arithmetic gives an int;
  `toByte()` (an error past 0 to 255) and `toByteWrapped()`; an int literal that fits, or
  arithmetic on literals, needs no conversion, as in Kotlin, while a name, even a constant's, is
  the int it is. `Bytes` reads and writes 16- and 32-bit numbers: `int16(at)`, `uint16`,
  `int32`, `uint32`, `float32`, and `putInt16(at, value)` and the rest, least significant byte
  first unless `bigEndian = true`.
- Storage widths (D148): `int16`, `int32`, `uint16`, `uint32` and `float32`, with `byte` and
  `char`, kept narrow in packed lists (`List<int16>` at 2 bytes an item), in value types' fields
  (a value type of up to 8 bytes of them is kept in the value itself, with no box) and in
  objects' fields (after the 8-byte payloads); read as an int or a float; a number that does
  not fit is an error where it is stored, naming the `toInt16Wrapped()` and the like that keep
  the low bits; locals and parameters declared so are checked the same way. `wrap32()`,
  `rotateLeft32`, `rotateRight32` and `hash(a, b, ...)` for 32-bit work.
Choices made where the decisions left room, for the user to confirm (M6.6-gaps.html): a byte becomes
an int by `toInt()`, as an int becomes a float only by `toFloat()` (comparing and `==` across
them work); a char goes where a string is asked for only through Kotlin's overloads; a map
with string keys indexed by a char is a compile error, though a `Map<any, V>` finds 'c' and
"c" as one key; the byte order is a `bigEndian` flag; `wrap32()` and `toInt32Wrapped()` both
exist, as D148 names both; `hash(...)` combines Astra's own hashes until classes' `hashCode`
is called when running (D26); a char's `upper()` and `lower()` give a char (D74's names;
Kotlin's `uppercase()` on a `Char` gives a string); the UTF-8 a string is made from is not kept,
as Python does, only what is asked for later. Left: an object's narrow field is read and
written by the interpreter's general path, not its quick one, so it is slower to reach than an
int field. Tests: `numbers.astra`, `chars.astra`, `strings_wide.astra`,
`check/numbers.astra`, `error_char_quotes.astra`; the suite, 635 checks, passes as it is,
with every read from a list of values a copy, under the GC and thread stress settings, with
the C++ results checked, and under the sanitizer, with and without the GC stress setting.

Group 2, equality and collections (built 2026-10-20):
- One name for each thing (D74) and `size` for every count (D130): the names that went are
  compile errors saying what to write (`length`, `push`, `pop`, `each`, `has`, `containsKey`,
  `containsValue`, `joinToString`, `uppercase`, `lowercase`, `toDouble`, `toLong`,
  `roundToLong`, `substring`, `subList`, `slice`, `len`, `str`, `int`, `float`, `keys()`,
  `values()`, `entries()`, `toMutableList`); `join` takes `joinToString`'s arguments;
  `File.length()` became `size()`. The standard library, Jade, the examples, the tests and the
  benchmarks were rewritten to them.
- Positions (D146) and slices (D147): `items[^1]`, `^k` a value of type `Index`;
  `items[1..<3]`, `items[2..]`, `items[..^2]`, a new list, string or bytes. A position or slice
  outside what is there is an error; a negative literal position is a compile error naming
  `[^1]`.
- Maps (D64, D77, D140): a `for` over a map gives its entries, taken apart with
  `for (k, v) in map`; `keys` and `entries` are live views, sets that cannot be changed through;
  `contains` and `in` on a map take a whole entry. Sets are built in, printed `{1, 2}`, with
  `+` and `-`. `Pair`, `Triple`, `MapEntry` and `IndexedValue` are value types (D73).
- Keys (D26): `op ==` and `hashCode` both or neither; a class's own decide map and set keys;
  every value has `hashCode()`, and `hash(...)` calls a class's own. A key cannot change: a
  list, map, set or `Bytes` only frozen, no data class with a `var`, no class whose `==` or
  `hashCode` reads a `var` (followed into the methods and getters they call): a compile error
  where the type shows it, and an error at each new key when running otherwise, the class
  marked when compiled. `frozen()` makes a copy nobody may change, `FrozenList`, `FrozenMap`
  and `FrozenSet`, known when running (`is FrozenList`); a change to one is an error.
- Read-only and open collections (D32, D34, D45, D46, D47): a name written `List<T>` only
  reads, `open List<T>` changes and takes only its exact type, `open List<accepts Button>`
  takes a list of `Button` or a type above it; what is made (`[...]`, `listOf`, `filter`,
  `map`, slices, `+`) is open; `toOpenList()`, `toOpenMap()`, `toOpenSet()` copy; `as open` and
  `is open` on a read-only name are errors; read-only maps and sets go where wider types are
  read. The mark is the checker's alone.
Choices made where the decisions left room, for the user to confirm (M6.6-gaps.html, 8.2a to 8.2l):
the names `FrozenList` and `open List` after the naming rule; `Bytes` no key, with no frozen
form; what the checker follows to tell a key that can change; `hashCode()` for every value and
`hash` calling a class's own; how deep `frozen()` goes; which results are open; `open` the
checker's alone; `mutableListOf` and the like kept; loops over a changing set seeing it as it
is; messages naming `open`; `any?` read through `accepts`; `^k` a value, a negative position an
error. Left: a map's `values` is a list made afresh; `items[..]` does not parse; `type(x)` until
reflection; a generic function's collections have `itemType` any; the checker knows nothing of
the keys of classes in files whose code comes from the cache; nothing is collected while a
key's `==` or `hashCode` runs; `frozen()` does not go into
values. Tests: `keys.astra`, `open_lists.astra`, `maps_sets.astra`, `positions.astra`,
`check/keys.astra`, `check/open.astra`, `check/names.astra`; the suite, 658 checks, passes as
it is, with every read from a list of values a copy, under the GC and thread stress settings,
with the C++ results checked, and under the sanitizer, with and without the GC stress setting.
Measured with Clang against group 1, best of five: no benchmark slower beyond the noise;
`strings` 1% slower, its map keys asking whether a key's class has an `==` of its own (7% in its
map-keys part before that question was put after the quick cases); `numeric` 14% and
`simulation` 11% faster, which nothing in this group aimed at, so code placement.

Group 3, types and declarations (built 2026-10-20):
- Statics in one `static { }` block (D67) and accessors in Kotlin's form only (D68): a `static`
  prefix, a second block, `companion object` and a `get x()` member are compile errors naming
  what to write. The standard library's, Jade's, the examples' and the tests' 163 prefixed
  statics and 190 old accessors moved over by a tool; a static property is written in the block
  in Kotlin's form; `super.name = value` reaches the setter above.
- `lateinit var` (D36); `lazy val` and `lazy var` for fields, statics and top-level values
  (D37), kept as the "no value yet" mark every read already tests, with what works the value
  out beside it, so a read after the first costs nothing more; a starting value that fails is
  tried again, one that needs itself is an error, threads wait for the one value. A named object
  is made when first used (D66).
- `enum class` and `enum interface` (D39): kinds in the same file, bare kind names where one is
  expected, every `when` over one (or an enum, a bool, a union) covering every kind; Kotlin's
  `enum class` with entries an error naming `enum`.
- Union types (D40) and names for types (D41): `string | TextModel`, narrowed by `is`, a `when`
  covering each type, inference giving the union where two types have nothing above both;
  `type Grid = ...` with type parameters, referring to itself inside another type.
- Data classes extended only by data classes, the child's header starting with the parent's
  fields (D65). `extend Person : Weighed { val name get() => ... }` (D33) and extension
  functions in reach as imports say (D70), for the checker.
Choices made where the decisions left room, for the user to confirm (M6.6-gaps.html, 8.3a to 8.3k):
what may be lateinit; how lazy is kept; which objects are made when first used; where a kind is
written bare; when inference gives a union; a type name referring to itself; Kotlin's accessors
and what they take; what goes in the static block; how a data class passes its fields up; what
extend covers; which extension function a call finds. Left: extend of a built-in type;
extension functions and extends hold for every file once loaded, when running; two imported
extensions of one name not reported as ambiguous; a thread waiting for a lazy value hands the
lock over again and again. Tests: `lazy.astra`, `enum_classes.astra`, `unions.astra`,
`data_extend.astra`, `extend.astra` (with the folder `ExtLib`), `check/enum_classes.astra`,
`check/unions.astra`, `check/declarations.astra`, `check/extend.astra`, and error tests for
each parse error; the suite, 705 checks, passes as it is, with every read from a list of
values a copy, under the GC and thread stress settings, with the C++ results checked, and
under the sanitizer, with and without the GC stress setting.
Measured with Clang against group 2, best of five: nothing slower beyond the noise (at most 1%);
`fields` 11% and `numeric` 10% faster, which nothing in this group aimed at, so code placement.

Group 4, functions (built 2026-10-21):
- Infix functions (D72): `infix fun`, a member or extension of one parameter, called `a name b`
  at Kotlin's precedence; `to` an inline infix function of the standard library, `downTo` and
  `step` infix functions written in place by the compiler, a `for` over them counted through.
- Inline functions (D59): copied into each call with the lambdas they only call or pass on, in a
  scope of their own (their file's and class's names, their own locals); a bare `return` in such
  a lambda leaves the caller, break and continue reach its loops, `return@name` leaves the lambda;
  no recursion, no overriding, none declared inside a function; their rights kept by giving them
  to the function copied into. The standard library's lambda functions are inline (D58): the
  scope functions, `with`, `repeat`, the builders, and about a hundred functions of lists, maps and
  sets; `let`, `also` and `takeIf` lost their special place in the compiler and checker.
- `this` in a block run on an object is that object (D58); `this@Name` for a class around the
  code, an extension's receiver or a block given to a function (D57); `block(obj)` and
  `obj.block()` call a function value run on an object.
- A lambda where an interface with one abstract method is expected (D28), and `Action { }`.
- A call of itself as the last step a loop (D29), with `tail fun` asking for it and traces
  counting the repeats. `it` decided by the type a lambda is given, a nested `it` an error (D55).
  Overloads by type arguments (D31) were there since M6.5 part 3.
Choices made where the decisions left room, for the user to confirm (M6.6-gaps.html, 8.4a to 8.4k):
where an infix call may break; what a lambda given as an interface is; which calls of itself are
a loop; what `tail` asks; when a nested `it` is an error; what `this@Name` names; how an inline
function keeps its rights; what is copied in; which of the standard library's functions are
inline; the scope functions on null; how `downTo` and `step` are written. Left: tail calls of
local and generic functions and inside a try; inline calls with spread arguments, of static
members or with a vararg; no reads in place inside copied code; the rights given to the whole
function copied into; `this` in a block whose type the checker cannot tell.
Tests: `infix.astra`, `inline.astra`, `this_labels.astra`, `sam.astra`, `tail_calls.astra`,
`check/functions.astra`, and error tests for each new parse and compile error; the suite, 743
checks, passes as it is, with every read from a list of values a copy, under the GC and thread
stress settings, with the C++ results checked, and under the sanitizer, with and without the GC
stress setting.
Measured with Clang against group 3, best of five: nothing slower beyond the noise (at most 2%);
`jade_layout` 10% faster, and a loop of `map`, `filter`, `count`, `forEach` and `sumOf` with
lambdas 18% faster, now copied in. Checking the standard library's inline bodies adds about 1 ms
to every start (checking 0.6 to 1.6 ms of the 8).

Group 5, errors (built 2026-10-21):
- Java's tree of throwables (D35) in the standard library (`errors.astra`): `Throwable` with a
  `message` that may be null and a `cause`; `Error` for what the VM cannot go on from;
  `Exception`, with `RuntimeException` and its classes unchecked and every other Exception
  checked. The VM's own errors are of their classes, chosen from each message; `throw` takes
  only a Throwable, checked when compiling and when running. A bare `catch (e)` takes every
  Exception and no Error (D151); clauses are tried in order, one that an earlier one takes an
  error. `require` throws IllegalArgumentException, `check` and `error()` IllegalStateException,
  `assert` an AssertionError, `TODO()` a NotImplementedError.
- Checked exceptions worked out by the compiler over the whole program (D35): each function's,
  lambda's and class's construction's, through calls, tries and catch clauses, a function that
  only calls a lambda throwing what the lambda throws, `throw e` of a catch clause's variable
  only what the try could throw; an error where one reaches what cannot pass it on: a kept
  lambda, an override or interface implementation (D51, D53), a static or lazy starting value,
  a function used as a value (D52), a library's public function without `throws`. `throws A | B`
  on functions and function types (D52, D54), a check where not required. What a kept file's
  functions throw is in its facts (D145).
- The standard library and Jade say what they throw: IOException on what reads, writes,
  connects or starts a program, ExecutionException and TimeoutException on `Future.get`.
- `use { }` for an `AutoCloseable` (D113), an inline function, in place of each stream's own.
- Fixed on the way: `astra dis` ended before the cache it was writing was written, and could
  crash.
Choices made where the decisions left room, for the user to confirm (M6.6-gaps.html, 8.5a to 8.5k):
the classes of the VM's own errors; what a Throwable holds; where InterruptedException and
CancellationException stand; what assert and TODO throw; what counts as kept; which functions are
a library's public ones; `throw e` in a catch clause; what the standard library says it throws;
what `use` takes; catch clauses in order; where `throws` is written. Left: `throws` on getters and
setters; an extend's bridges not checked against the interface; the C++ functions known by a
table; an error from `close()` replacing the block's in `use`; half a millisecond more at start.
Tests: `exceptions.astra`, `check/exceptions.astra`, `error_catch_type.astra`, and the tests that
threw `Error` or plain values brought to the tree; the suite, 751 checks, passes as it is, with
every read from a list of values a copy, under the GC and thread stress settings, with the C++
results checked, and under the sanitizer, with and without the GC stress setting.
Measured with Clang against group 4, best of five: `numeric` 7% and `fields` 8% slower and
`bounce_moved` 11% faster, with the same instructions run (158,300,448 against 158,300,185 for
`numeric`, the difference in setting up): code placement. Checking: half a millisecond more at
every start (2.2 ms against 1.7), and Jade checked whole in 13.5 ms against 10.8.

Group 6, reflection (built 2026-10-21):
- `#` gives a class (D48): `Image#` the class named Image, `Box<int>#` with type arguments,
  `int#` a built-in type, `x#` the class of the object x, binding as tightly as a dot. A `Class`
  (`reflect.astra`) has `name`, `superclass`, `interfaces`, `typeArguments`, `kinds`, `fields`,
  `methods`, `constructors`, `annotations`, `field(name)`, `method(name)`, `annotation<T>()` and
  `isInstance(x)`; a field `get` and `set`, a method and a constructor `call`. A class lists
  only what the code where `#` is written may use (D38). `className` and `x.class` are gone,
  each an error saying what to write, and `class` may be a member's name; Kotlin's `::class`
  says to write `#`.
- What a call by name throws arrives in a checked `InvocationTargetException`, the original in
  `cause` (D49); a call the member cannot take is an `IllegalArgumentException` of its own.
- Each class has two descriptions, compiled with it from what the checker knows: its fields,
  annotations and kinds; its methods and constructors. A built program keeps them only where
  reflection is used, the methods' only where they are asked for (D50); there is no lookup by a
  name alone, `Class.namedFrom(name, classes)` in its place.
- Annotations (D153): declared `annotation Name(val a : T)`, written on classes, functions,
  fields and parameters, checked as calls given constants, read through reflection. Kotlin's
  annotations for the JVM are errors saying what to write, `@Volatile` among them (L47).
- `astra test` (D153): runs the functions and methods written `@Test`, with `@BeforeTest`,
  `@AfterTest`, `@BeforeAll`, `@AfterAll`, `@Timeout` (a test stopped at a safe point once past
  it) and `@TempDir`; kotlin.test's assertions in `test.astra`.
Choices made where the decisions left room, for the user to confirm (M6.6-gaps.html, 8.6a to 8.6k):
what `#` names; what a Class is; which members are listed; who may see a member; what is
wrapped; types that cannot be named; what a built program keeps; what an annotation is; which of
Kotlin's annotations are errors; how astra test runs; `type(x)` kept. Left: D50's flow of
objects done by a simpler rule that keeps more; anonymous objects describe nothing; static
members not listed; a test held in a wait not stopped until it ends; `@Deprecated` (1.0); the
cost at start.
Tests: `reflection.astra`, `check/reflection.astra`, `testing/basics.astra` (run by `astra test`),
`shell/reflection.shell`, `annotation_jvm.astra`, `error_double_colon.astra`, and the tests that used
`className` or `@Volatile` brought to `#` and none; the suite, 763 checks,
passes as it is, with every read from a list of values a copy, under the thread stress setting
and with the C++ results checked, under the GC stress setting (739 there), and under the
sanitizer, with and without the GC stress setting.
Measured with Clang against group 5, best of five: every benchmark within 5% (`colors_classes`
5% faster, `points_values` 3% slower), the same check values. At start: 0.7 ms more for a
script printing one line (9.3 ms against 8.6), half of it the descriptions of the standard
library's classes; a program using Jade compiled afresh 1.1 ms more (7.3 ms compiling against 6.2).

Group 7, concurrency (built 2026-10-21):
- Tasks (D81): "flow" became "task" in code, messages, the README and the examples
  (`task.astra`, `bounce_tasks.astra`); `task { }` gives a `Job` (D89), `suspendTask { resume ->
  }` sets a task aside, `resume.fail(e)` raises an error in it, `delay(time)`. Kotlin's
  `startCoroutine`, `suspendCoroutine`, `Continuation` and `EmptyCoroutineContext` are errors
  naming what to write. A thread waiting for a lazy value another thread is working out now
  sleeps on the value until it is told, in place of handing the lock over again and again.
- `Job<T>` (D89): `complete`, `fail`, `cancel(interrupt)`, `get`, `get(timeout)`, `getOrNull`,
  `failed`, `then`, `onError`, `Job.all`, `Job.any`. `thread { }` gives one (D117); `Thread`
  takes `join(timeout)` and `sleep(time)` as Durations.
- `Lock` with `useKey { }`, `LockBox(value)` and `waitUntil { }` (D86): a thread keeps a stack of
  the keys it holds; `waitUntil` gives back the innermost, and a task on the main thread is set
  aside there instead of blocking the loop. `Channel<T>` (D87), `ThreadPool` and `JobQueue`
  (D88), `Countdown` (D118) and one `Atomic<T>` (D85), all in `thread.astra`; Java's names for
  each are errors naming what to write.
- `Duration` (D90), a value type of whole nanoseconds, in `time.astra`, made by extension
  properties (`val int.milliseconds : Duration get() => ...`, new in the language); every call
  that waits or times takes one, and a plain number there is an error saying to write a unit.
- `atomic var` fields (D92): `+=`, `-=`, `= max(...)`, `= min(...)` and `compareAndSet` are each
  one step (`ATOMIC_FIELD`, on the processor's atomic operations for ints and floats);
  anything else assigning one in a parallel section is a compile error.
- Parallel sections rebuilt on a pool of cores with a queue each and stealing (D96): a section
  is cut into a piece per core, and pieces carry owner numbers in place of core numbers, so a
  section may start sections of its own and `parallel(...)` runs functions at once. `gather`
  (D93), `flatMap`, `reduce` and `fold` on a parallel stream; `SharedMap` and `SharedSet` (D94);
  `parallelChunks` on any list, `parallelSort`, and `splitAt` handing out `ListView`s (D96); the
  error for another core's item names `indices.parallelStream()` (D95).
- Frozen data sent to workers is kept once outside the heaps, counted, and read in place;
  `post(bytes, handOver = true)` moves a buffer (D97).
- Sequences (D98, `sequence.astra`): `Sequence<T>` with lazy operations, `generateSequence`,
  `asSequence()`, and `sequence { yield(x) }` run on a generator, a task whose stack is reserved
  whole and committed a page at a time as it grows. In a sequence's block only `yield`,
  `yieldAll` and suspend functions of `SequenceScope`'s may wait.
- The `for` loop takes any object with `hasNext` and `next`, or an `iterator()` giving one.
Choices made where the decisions left room, for the user to confirm (M6.6-gaps.html, 8.7a to 8.7m):
where a task's error goes; what `Job.all` and `Job.any` give; `fold` on a parallel stream; a
`Thread` made with a lambda; `@Timeout` and Jade's `scheduler` left in milliseconds; how Atomic
and the shared collections keep a change whole; how a section is divided; `parallelChunks` and
views; frozen data shared with workers; a `for` over a channel and waking at a Lock; how a
sequence's block runs; what an atomic field is; what a Duration is. Left: a message never read
keeps its hold on shared data; a shared collection locked whole; a sequence's 4,000 calls;
`yieldAll`'s two forms; `@Timeout` and `scheduler` in milliseconds.
Tests: `tasks.astra` (was `flows.astra`) and `threads.astra` rewritten, `sequences.astra`,
`parallel_tools.astra`, `durations.astra`, `workers_sharing.astra`, `check/concurrency.astra`,
`error_atomic_field.astra`, and every test and example that used the old names brought to the
new ones; the suite, 786 checks, passes as it is, with every read from a list of values a
copy, under the thread stress setting and with the C++ results checked, under the GC stress
setting (762 there), and under the sanitizer, with and without the GC stress setting.
Measured with Clang against group 6, best of five: every benchmark within 3% but `jade_layout`
(4%) and `layout_cached_classes`, which with twelve runs each are within 1% and 3% (the latter
a 15 ms benchmark), the same check values. At start: no change (12.0 ms for a script printing
one line with both, measured while a sanitizer run took a core; a program using Jade compiled
afresh 34.8 ms against 35.0).

Group 8, files and input and output (built 2026-10-21):
- `Path` in place of `File` (D107, `file.astra`): what is in the path a property (`text`,
  `name`, `extension`, `parent` as a Path, `absolute`), what asks the disk a function
  (`exists()`, `isFile()`, `size()`, `canonical()`, `list()` giving Paths, `walk()` a sequence)
  (D128, D135, D137, D138, D150); `mkdir(parents)`, `moveTo`, `copyTo`, `delete()`,
  `deleteIfExists()`, the permission calls, `setLastModified` and the temporary directories
  and files (D126, D129, D131); failing throws an IOException saying why. `File.read` and the
  other static shortcuts are gone (D136).
- `Reader` and `Writer` in place of the streams (D119), and `Charset` (D127): UTF-8, Latin-1,
  ASCII and UTF-16 both ways, in `charset.cpp`, on every call that reads or writes text and on
  `toBytes` and `decodeToString`; a reader's and a writer's set may change as they go.
  `useLines` and `lineSequence()` read as they go.
- `Process(command..., directory, environment, input, output, error)` and `Process.run(...)`
  (D91), with a `Stream` enum written bare: a union parameter holding one enum now takes its
  entries by their bare names (`SETTLE_ENUM`). `Process.run` reads output and errors at once.
- Sockets with `reader`, `writer` and `readTimeout`, a Duration (D120); a read past it throws a
  `SocketTimeoutException`.
- The C++ under these is declared as `external object __file` and `__io`, with types and what
  it throws; the checker's table of them is gone (gap 8.5f). Every Java and Kotlin name for
  these is an error naming what to write.
- Jade: a window's dropped files are Paths; pictures, fonts and surfaces load and save Paths.
Choices made where the decisions left room, for the user to confirm (M6.6-gaps.html, 8.8a to 8.8k):
what `list()` and `walk()` give; deleting and what fails; permissions; what a Charset is; a
line read lazily that fails; what a read past `readTimeout` throws; a started program's
members; what `Process.run` does; bare entries for a union parameter; the C++ declared; Paths
in Jade. Left: five character sets; times as milliseconds; `walk()` past unreadable
directories; processes and sockets on Windows only.
Tests: `std_files.astra` and `io_streams.astra` rewritten, `check/files.astra`, and the tests
that used `File` brought to `Path`; the suite, 787 checks, passes as it is, with every read from
a list of values a copy, under the thread stress setting and with the C++ results checked, under
the GC stress setting (763 there), and under the sanitizer, with and without the GC stress setting.
Measured with Clang against group 7, best of five, and best of ten taken in turns for the three
that differed at first: every benchmark within 2%, the same check values. At start: 0.2 ms more
for a script printing one line (12.6 ms against 12.4, while a sanitizer run took a core), and
0.7 ms more checking a program using Jade compiled afresh (14.1 ms against 13.4), the C++
functions of files and streams being declared and checked now.

Group 9, graphics and Jade (built 2026-10-22):
- The standard library draws, with no import (D124): `graphics.astra` has `Color`, a value type
  of four bytes with its other forms as secondary constructors (D99), `Point`, `Size`, `Insets`
  and `Rect` as value types (D102 to D105, D152), the shapes `Rect`, `Oval` and `Line`, and
  `Drawable`; `image.astra` has `Canvas`, `Image` (pixels in memory, a `Grid<Color>`, read
  freely and changed in `editPixels`, D122, D133, D134, D141) and its canvas, drawn by a
  software engine in C++ on the text engine's rasterizer (D123), shapes cut finer than glyphs.
  `Font` is one type, free of scale, measured at a scale where pixels are wanted (D101, D125,
  D144); `TextFont` is internal.
- `Grid<T>` (D155, `grid.astra`, `grid.cpp`), packed by its type, with parts by ranges, new
  grids for rows and columns added or taken away, and work by rows across cores.
- Language: `.=` (D156), indexes of several parts (`op [](x, y)`), bundles of named arguments
  with a type of their own, `params(size : int, bold : bool)` (D143), secondary constructors
  (the user's choice, partly reversing D180), and a byte going where an int does.
- Jade on top: `Surface` gives and takes `Image`s; an `Image` drawn on the graphics card is
  copied there once and then only where it changed, by a change number (D124); `NativeCanvas`
  and `RecordingCanvas` are the standard `Canvas`; the style is on the component, every content
  has an `owner` (D132); `Grid` the layout is `GridPane`; `size` on components, windows and
  surfaces (D152). Every old name (`Dimension`, `Rectangle`, `BufferedImage`, `Picture`,
  `FontMetrics`, `drawImage`, `getRGB`, `Font(name, style, size)`, `deriveFont` and the rest) is
  an error naming what to write.
- Found and fixed on the way: a copied list of `float32`, `int16` or another narrow kind read
  back nulls (since part 3); the type arguments of a call about to be made were the VM's, not
  the thread's, so a thread switch could give an object another's (since part 1); a name some
  class keeps from being assigned (`private set`) made every read of it through a value inside
  a value copy that value (2.9 times slower).
Choices made where the decisions left room, for the user to confirm (M6.6-gaps.html, 8.9a to 8.9l):
secondary constructors; an exact fit beating a narrowed constant; a byte where an int goes;
what `Color` takes; what a bundle is when the program runs; details of `Grid`; a font's
measurements at a scale; the style handed to a component; pictures of a component and where
the card copy is kept; how finely curves are drawn; old static names; and the `opaque` flag
for an image (L57), left for the user.
Tests: `graphics`, `image`, `grid`, `bundles`, `dot_assign`, `index_many`,
`secondary_constructors`, `byte_as_int` and `check/graphics` new; Jade's tests, `png`,
`text_draw` and `text_measure` brought to the new types; the suite, 828 checks, passes as it is,
with every read from a list of values a copy, under the thread stress setting and with the C++
results checked, under the GC stress setting (804 there), and under the sanitizer, with and
without the GC stress setting.
Measured with Clang against group 8, best of five taken in turns, best of ten for those that
differed: the same check values; every benchmark within 3% but `numeric`, 5% slower with the
same instructions run (how the compiler lays out the instruction loop, as in part 4), and
`jade_layout`, 7% faster (a font's measurements kept, and the last one found again at once).
Two slowdowns the first measurements showed were mended before the report: a byte's widening
to an int, tested at every typed store, now only once a value has failed its type's check
(4 to 15% on calls, fields and loops), and the read through a value inside a value above
(27% on `layout_values`). At start: 1.5 ms more for a script printing one line (13.2 ms
against 11.7), the standard library being larger by graphics, grids and images; a program
using Jade compiled afresh the same (33.2 ms against 34.0).
After group 9 (2026-10-22), gap G1 closed: a generic function declared inside another now has
its own type arguments (a call of it by name was not given them) and those of the generic
functions around it, which its frame holds first (`typeArgBase`, bytecode format 21); a function
declared inside another is checked with the locals and type parameters around it in reach, as a
lambda is, where it saw none of them; and `is T` in a string template finds `T`. Gap G2 closed:
`object : Holder<int> { }` parses, the look-ahead after `object :` passing over a supertype's
type arguments; an object whose supertype names a type parameter of the code around it has its
class made every time, so each is of its own call's type, as one using a variable of the code
around it does; and a lambda starting with an anonymous object is no longer read as a map.
Such a class made every time costs about 1 µs a call (G66, in M6.7-gaps.html). Measured again with Clang against
group 8 after both, best of five in turns: every benchmark within 2%, the same check values,
`jade_layout` 7% faster; `numeric` even (-0.7%), so the 5% above was the instruction loop's
layout in that build, not a cost of the group. The suite, 838 checks, passes with every read
from a list of values a copy, under the thread stress setting, with the C++ results checked,
under the GC stress setting (814 there), and under the sanitizer, with and without it.
Gap G3 closed: a call given the frame's own type arguments exactly, in their order (`isA<T>(x)`
inside `fun <T>`), is handed them whole by FRAME_TYPES, with no type made or looked up; and
MAKE_TYPE keeps, in a table of 64 places in each VM (each core's in a parallel section, so
with no lock), the type it made last at a place from which types, good until the next
collection. `isA<T>(x)` passed on went from 113 to about 45 ms a million calls, as the type
written costs; in a parallel section from about 1,760 ms to 48 for two million, where every
core had queued on the lock of the table of types. Found on the way and fixed: inside a
generic function, a type argument naming its own type parameter was worked out again with the
called function's of the same name, so `isA<List<T>>` inside `fun <T>` tested `List<List<T>>`.
Gap G6 closed, with D181's first step: a message to a worker keeps its lists', maps' and sets'
types, sends a packed list as one block (a million ints there and back in about 10 ms, where
it was 43, arriving a `List<int>` where it arrived a `List<any?>`), and carries objects of data
classes and values of value types, made again by their constructors on the other side, their
class named by its global, which is the same in a worker: a class declared inside a function
is refused where it is sent, and one the other side has not imported is reported there. A
frozen list of values is copied, not shared. Found with it: G67, lists from `map` and the like
and sets carrying no type when the program runs.
Next, in this order (decided 2026-10-22): G6 verified and committed; D182, a type parameter
known to the checker by its declaration and not its name, with the error for a method's type
parameter named as its class's; then G67's fix, an empty `[]`, `{:}` or set given the type
written where it goes when that type names only the type parameters of the code around it,
so that `map`, `filter`, `mapOf`, `setOf` and `toSet` give typed, and so packed, results.

**M6.7. A faster VM.** Added 2026-10-16. The compiler's code generation and the VM's
instruction loop rewritten once the language has stopped moving: instructions that name
registers (a frame's slots) in place of a stack, as Lua 5 did, and combined instructions for
pairs that are common (a compare and a jump, a field read and a call). Built with Clang, each
instruction ends by going on to the next itself, through a computed `goto`, and a program's code
is threaded as it is loaded, each opcode replaced by the address of its instruction's code (D160);
built with MSVC, the same instructions are the cases of a switch. With them, from M6.6 (D178): a
frame's slots that keep only a payload where the compiler knows their type, instructions chosen
by type for arithmetic, which then need no test of their operands' kinds, and values of up to 32
bytes in consecutive slots (D149). And, as the values' layouts are reworked, a generic value
type's type arguments kept when the program runs (gap G4 in M6.7-gaps.html, decided 2026-10-22): where its class
is kept already, at no cost in bytes; a hidden slot for a phantom type parameter only in a
container declared wider than it; `==` and `hashCode` not looking at them; `is` and `as` going
by them, refined by the value's contents where a recorded one is wider. Invisible to programs
but for G4's `is` and `as`; bytecode files are made again. Measured before and after on the benchmarks of M6.5 part 4;
the aim is one and a half to three times on top of M6.5 and M6.6. Scripts run from source,
the shell, workers and tests keep running on the VM even once built programs can be native
code (M14), so this stays worth having.

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

**M12. Networking, for JadeMail, JadesDomain and JadeNet.** Added 2026-10-16. Three apps
that will probably be ported, all of them servers or tools on the network:
JadeMail (13,400 lines: an SMTP server and deliverer, an IMAP server, DNS checks and
MTA-STS), JadesDomain (6,400 lines: a DNS server over UDP and TCP, DNS over TLS and over
HTTPS, an admin server, zone transfers, a forwarder) and JadeNet (3,900 lines: its own
encrypted transport, tunnels, port forwarding, a netcat). Plain sockets and listening
sockets exist (M3); what they also need, by their use in the code:

- **TLS**, as client and server, with certificates from PKCS#12 files, and upgrading an open
  connection part way through (SMTP's STARTTLS): `SSLContext`, `SSLSocket`, `KeyStore` are
  used about 60 times. Astra's own TLS 1.2 and 1.3 (changed 2026-10-17: some apps run as
  dedicated servers on Linux, and the same code must agree with itself on every system;
  SChannel is Windows' only, and Linux's TLS libraries are third-party code that may not be
  there). Split as Go's `crypto/tls`, rustls and Java's own TLS are:
  - **The protocol in Astra**, most of the code: the handshake's states, building and reading
    its messages and extensions, the record layer's framing, the key schedule (a sequence of
    HKDF calls), X.509 certificates (DER) and checking their chains, alerts, session tickets
    and resumption, STARTTLS, PKCS#12. Reading untrusted input in Astra cannot overrun a
    buffer or use freed memory, the kind of mistake behind the worst TLS holes (Heartbleed),
    so this is safer than C++ as well as easier to read and test.
  - **The primitives in C++** (the cryptography below), working in place on part of a
    `bytes` (`aead.seal(into = buf, at = 5, from = buf, length = n)`), so a record is neither
    copied nor allocated: the Astra code handles a record's 5-byte header and C++ encrypts
    the up to 16 KB behind it. Sending and receiving cost what an all-C++ TLS costs; a
    handshake spends a little more of the processor on reading certificates (a fraction of a
    millisecond, less after M6.5 to M6.7), next to the public-key work and the network's
    round trips, which take longer.
  - **Modern suites only:** key exchange by ECDHE (X25519, P-256), encryption by AEAD
    (AES-GCM, ChaCha20-Poly1305); no CBC and no RSA key exchange. Every comparison that
    must take the same time whatever the secret is then inside the C++ primitives, and the
    classic timing attacks on TLS (Lucky 13, Bleichenbacher) belong to the suites left out.
    It is what current clients and servers use.
  A mistake in TLS is a hole in a mail server, so it is tested against the published test
  data (the RFCs' vectors, NIST's, Wycheproof's: data, not code) and against real clients and
  servers of each kind, and fuzzed. Trusted certificates come from the system's own store:
  Windows' certificate store, Linux's files under `/etc/ssl/certs`.
- **Cryptography**: hashes, signatures, X25519 key agreement, AES-GCM, HMAC, secure random
  numbers, for JadeNet's handshake and for jadelib-crypto, which JadeMail and JadesDomain
  use. Astra's own, in C++, one implementation on every system (changed 2026-10-17, for the
  same reason as TLS): checked against the published test values, written so no secret leaks
  through timing, using the processor's AES and SHA instructions where it has them, and
  secure random numbers from the system (`BCryptGenRandom` on Windows, `getrandom` on Linux).
  Public, as every C++ piece is (section 1): a `Crypto` library of Astra types any program
  can use (`Sha256`, `Hmac`, `Hkdf`, `AesGcm`, `ChaCha20Poly1305`, `X25519`, `Ed25519`,
  `Ecdsa`, `Rsa`, `SecureRandom`), each taking and filling parts of a `bytes` in place. The
  crypto is built and tested before TLS is started on it.
- **Linux** for the servers (moved from M13): JadeMail, JadesDomain and JadeGate's dedicated
  server all run on Linux as well as Windows (the user, 2026-10-17). The runtime without
  windows, built with GCC or Clang, runs the same programs, and everything this milestone
  builds works on both: TLS and the cryptography, UDP, addresses and looking names up, the
  HTTP client and server, files, paths and starting other programs. Jade's windows stay
  Windows-only until asked for.
- **UDP**: a datagram socket (`DatagramSocket`, `DatagramPacket`), for the DNS server and
  JadeMail's DNS client; 27 uses.
- **Addresses and DNS**: an address value type for IPv4 and IPv6, a host and port, and
  looking a name up (`InetAddress`, `InetSocketAddress`, about 110 uses).
- **HTTP**: a client (`java.net.http.HttpClient`, used for MTA-STS and DNS over HTTPS), and
  for JadesDomain a server (DNS over HTTPS, the admin pages).
- The parts of JadeLib these apps use, ported before them: `jadelib-json` (all three),
  `jadelib-crypto` (JadeMail, JadesDomain) and `jadelib-db` (JadeMail).

Servers fit Astra's model as it is: each connection is waiting most of the time, so threads
that take turns, or better tasks (D81), one per connection with no thread of its own, serve
them well. Ends with the three apps' own tests passing, on Windows and on Linux, and each talking to a real
client of its kind (a mail program, a DNS resolver, a second JadeNet).

**M13. JadeGate.** Added 2026-10-16. JadeGate (`D:\Projects\JG`) is a 2D action RPG on
Godot 4.7: a C++ core of 19,000 lines (simulation, ship designs, voxel objects and cloth, a
network protocol on ENet, physics on Box2D, and its own mod language, JadePy) and 20,000
lines of GDScript for interface, drawing, a 3D view of ship interiors, input and files. The
whole of it, core included, would be ported to Astra: one language for game, server, editors
and mods, with JadePy replaced by mods written in Astra. In order of risk:

- **First, a measurement.** One heavy slice of the simulation (a tick with many ships)
  ported and timed against the C++, after M6.5 to M6.7. If Astra is too slow for the tick,
  the rest waits for native code (M14).
- **Heavy parts in C++, as part of Astra's library** (written here, so not third-party
  code), as Godot does under GDScript: 2D physics (bodies, convex polygon collision, contacts
  with friction, in place of Box2D, of which JadeGate uses polygons, bodies, velocities and
  transforms), a spatial grid for nearness queries, mesh simplifying, and vector maths over
  packed lists (`positions.addScaled(velocities, dt)` in one loop).
- **Reliable networking over UDP** (M12's datagrams): ordered, acknowledged, resent and split
  packets, in place of ENet.
- **A sandbox for mods:** loaded code limited to what the game hands it (no files, no network
  of its own), perhaps a worker with chosen built-ins.
- **A 3D layer** on M11's OpenGL: meshes made in code, materials and lights, a camera,
  shaders, drawn into a part of a window.
- **Sound:** playing, mixing and placing sounds, OGG and WAV.
- **Game controllers** (XInput, part of Windows).
- **Linux** for the dedicated server: the runtime without windows is there from M12; what is
  left is the game server's own needs on it.
- **A debugger and a profiler** for Astra, which a program of this size needs.

Ends with the game playable in Astra against the C++ build's behaviour, its tests ported, a
dedicated server running, and the tick's time measured against the C++.

**M14. Native code for built programs.** Added 2026-10-16, placed when wanted. `astra build
--exe` writes C++ from the whole typed program and compiles it with the system's C++ compiler
(Visual Studio on the machine that builds, not on the machines that run it; a tool, as it is
for building Astra, not code inside the program), as Nim does. Typed code then runs close to
hand-written C++, typically ten to fifty times faster than bytecode. An x64 code generator of
Astra's own, needing no compiler, was considered: much more work for slower code unless much
is put into it. Compiling while running (as LuaJIT and V8 do) is not planned: built programs
are where speed is wanted, and the VM (M6.7) serves the rest. `astra code.astra` keeps working as
it does: it compiles the source to bytecode and runs it on the VM, with no C++ compiler needed;
native code is only what `astra build --exe` makes, and a program behaves the same either way,
which the tests check by running every test both ways.

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
| | Static members. | A `static { ... }` block, with the `static` prefix as shorthand for one member. No companion objects (5.1). The prefix and a second block are refused since D67. |
| | Enums. | Declared with `enum Name { ... }`, without Kotlin's `class`. An entry may be written by its bare name wherever the destination is declared as that enum; elsewhere the full name is required (5.1). |
| | Data classes. | Declared with `data Name(...)`, without Kotlin's `class` (5.1). |
| D1 | Lambda literals, trailing lambdas, receiver blocks, implicit `this`. | All four are added (5.1). |
| D2 | The meaning of `a..b`. | `..` includes its end, as in Kotlin; `..<` excludes it (`until`, at first kept beside it, is an error since D60); `..=` is removed (5.3). |
| D3 | The native drawing backend. | OpenGL: every window is an OpenGL window and all drawing goes through it, chosen so that an app's 3D view layers cleanly under the popups (section 4). |
| D4 | How strings are counted and indexed. | By character (code point), not byte. The same as Kotlin except for emoji and other characters outside the basic plane, which are one character here. Binary data gets its own type, `bytes` (5.4). |
| D5 | Kotlin's type names (`Int`, `String`, `Boolean`, `Double`) as aliases. | No. The built-in names stay lowercase and the porter rewrites them (section 6, rule 3). |
| D6 | `and` / `or` versus Kotlin's bitwise infix words. | Astra keeps its meaning; the porter applies the rule in 5.3. |
| D7 | `return` inside a lambda literal. | Kotlin's rule. In a lambda given to `let`, `also`, `apply`, `run` or `takeIf` a bare `return` leaves the enclosing function; in any other lambda literal it is a compile error; `return@label` leaves just the lambda (5.1). Widened by D58. |
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
| D18 | Static types. | Astra is statically typed, as Kotlin (5.7): variables typed by their initial value, every parameter typed, `any` cast before use, typed collections, function types and generics of the program's own; no `final`. Milestone M6.5. |
| D19 | Integer types. | Two: `byte` (0 to 255, the items of `bytes`) and `int` (64 bits). Byte arithmetic gives an `int`; `toByte()` is an error past the range, `toByteWrapped()` keeps the low 8 bits; a byte is a kind of value of its own when running. 16- and 32-bit values are read and written through functions on `bytes`, not as types, with `uint16(at)` and `uint32(at)` reading unsigned values as an `int` (L0 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 1 (2026-10-20). |
| D20 | How strings are stored. | Flexible width, as Python since 3.3: each string at 1, 2 or 4 bytes a character, the narrowest its widest character needs, so `s[i]`, `substring` and `length` are direct. C++ code asks for a string's UTF-8 form, made when first asked for and kept (L19 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 1 (2026-10-20). |
| D21 | Lists of values. | Packed automatically: a list whose items are values (`int`, `float`, `char`, `byte`, `bool`, `value` types) stores them one after another at their own widths, because it knows its item type; `bytes` is `list<byte>`. Lists of objects, of `any`, of `number` and of nullable types store values as now. One sequence type: a separate `pack<T>` was considered and merged into the list (L1 in [language-choices.html](language-choices.html); [value-lists.html](value-lists.html)). |
| D22 | Value types. | `value Point(...)`: no identity, may hold strings and objects; kept in lists of values, in place when of 8 bytes or less, inside their object when a field, in a box otherwise; read in place where safe; results built where they go; `var` fields changed only through their place; `mutating` methods. Milestone M6.6 ([value-lists.html](value-lists.html); L2 in [language-choices.html](language-choices.html)). |
| D23 | Unsigned integers. | None: `int` is signed, 64 bits, and data that wants unsigned values uses `byte` (0 to 255) and `bytes`. `>>>` and the unsigned helpers are D44 (L3 in [language-choices.html](language-choices.html)). |
| D24 | A character type. | `char`, one code point: exactly what `s[i]` gives, a string `s` holding `s.length` of them (L10 in [language-choices.html](language-choices.html)). How chars and strings combine is D25 (L18). Built in M6.6 part 8, group 1 (2026-10-20). |
| D25 | Arithmetic with characters. | `char + char` is a `string`; `char` ± `int` is a `char`; `char - char` is an `int`; `int` ± `char` is an `int`. A `char` takes only a `char`: `val c : char = 5` is an error A `char` and a `string` added, either way round, are a `string`; a result that is not a valid character is an error. Single quotes are for characters only. A negative index counts from the end, as today, for strings and lists `'c' == "c"` is true, and `'c'` and `"c"` hash alike; comparing a `char` with a string literal or `val` of another length is a compile error, and with any other string of another length `false` (L18 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 1 (2026-10-20). |
| D26 | Equality and hashing for keys. | `op ==` with `hashCode()`, both or neither (compile error otherwise). Keys must not be able to change: numbers, characters, bytes, booleans, strings, enums, value types, objects by identity, all-`val` `data` classes of such types, and classes whose `op ==` and `hashCode` read only such `val` fields; reading a `var`, or calling what the compiler cannot check, makes a class unstable, and so does a `var` field in a `data` class; an unstable key type is a compile error. For `map<any, V>` and `set<any>`, each class carries a stable flag worked out when compiling, checked at each insert. Collections are keys only as unchangeable copies made by `frozen()`: `frozenList<T>`, `frozenMap<K, V>`, `frozenSet<T>` (L11 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 2 (2026-10-20), as `FrozenList`, `FrozenMap` and `FrozenSet` by the naming rule. |
| D27 | Integer overflow. | Trap: an `int` operation whose result does not fit in 64 bits is an error at that line; wrapping is asked for with Swift's operators, `a &+ b`, `a &- b` and `a &* b` (L4 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 1 (2026-10-20). |
| D28 | A lambda where an interface is expected. | Any interface with exactly one abstract method takes a lambda, as in Java; there is no `fun interface` (a ported one is written `interface`) (L5 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 4 (2026-10-21). |
| D29 | Tail calls. | A function calling itself as its last step is made a loop automatically (also a method, where no subclass overrides it); `tail` (`tail fun walk(...)`, for Kotlin's `tailrec`) is an optional check that the call is in tail position; error traces count the repeats (L6 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 4 (2026-10-21). |
| D30 | `suspend` in the type system. | Changed 2026-10-17 (review R1): `suspend fun` marks a function that may wait, setting its task aside; the standard library marks its own calls that do (`delay`, a channel's `send` and `receive`, `Job.get`, `Countdown.await`, `waitUntil`, `suspendTask`, `Thread.sleep`). A function that calls one may wait too, and says so: an unmarked function calling a `suspend` one is a compile error, as in Kotlin, so ported code, marked already, compiles as written. A `suspend` function may be called from another, and from code that starts its own task or thread to run it: `task { }`, `thread { }`, a pool's or queue's `submit { }`, and the top level of a script. It is refused wherever waiting is impossible, even inside a `suspend` function: a sort's comparison, a getter or setter, `toString`, `==` and `hashCode`, a generator's block (where only `yield` waits, L78) and a parallel section. The error says why, and how to call it safely, naming the call and the place: `'fetch' may wait (it is a suspend function), and a getter must give its value at once: whoever reads 'title' would be held up. Call it from a suspend function, or start it in a task and use the result when it is ready: task { fetch() }.then { title = it }`; and for an unmarked caller, `'load' calls 'fetch', which may wait, so 'load' may wait too: mark it 'suspend fun load()', or run the call in a task of its own: task { fetch() }`. `reified` is an error saying why it is not needed: type arguments are kept when the program runs (L14), so `x is T` works in any generic function. Blocking input and output (files, sockets, other programs' streams) is not marked: it blocks its thread and never sets a task aside, as in Kotlin, so the apps' socket code ports as written; a server serves connections from threads or a pool (changes D120). Built in M6.5 (L7 in [language-choices.html](language-choices.html)) on today's library, as the user chose on 2026-10-18: a lambda that a function runs there and then (it only calls it, or passes it on to one that does: D59's rule, applied to checking; `let`, `also`, `takeIf`, `apply`, `run` and `with` always) may wait where that call may; marked now are the calls that set a flow aside today, `suspendCoroutine`, `delay` and `Future.await`, while `Future.get`, `BlockingQueue`, `CountDownLatch.await` and `Thread.sleep` block their thread and are marked in M6.6, when they become `Job`, `Channel` and `Countdown`; `startCoroutine`, `thread`, an executor's `submit` and `execute`, and `Future.supplyAsync` take `suspend` lambdas; the errors give their advice in the names already decided for M6.6, `task { fetch() }` and `.then { }` (D81, D89), as the user chose the same day. |
| D31 | Overloads that differ only in their type arguments. | Allowed: `total(items : list<int>)` and `total(items : list<float>)` may both exist, picked by the argument's type when compiling; where the type does not decide (an untyped empty `[]`, or a type that fits both), the call is a compile error until the type is written (L8 in [language-choices.html](language-choices.html)). Built with overloads in M6.5 part 3; confirmed in M6.6 part 8, group 4 (2026-10-21). |
| D32 | Read-only and changeable collections. | One list type, read-only unless marked `open`: `items : list<Component>` may only be read through that name and takes a `list<Button>`; `items : open list<Component>` may be changed and takes only that exact type. The mark goes wherever a type is written (`map<string, open list<int>>`), the same for maps and sets. A list made with `[...]` can be changed by whoever made it. `frozenList`, `frozenMap` and `frozenSet` (D26) are what nobody may change. Ported `List` → `list`, `MutableList` → `open list`. Kotlin's `open` and `final` are not keywords: in front of a declaration either is an error. Open: what `filter` and the like return, casting `list` to `open list`, maps passed by value type (L9 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 2 (2026-10-20), written `open List<T>` by the naming rule; the mark is the checker's. |
| D33 | Fitting a type someone else wrote to an interface. | `extend Person : Weighed` declares that a type the program did not write fits one of its interfaces; differing names are bridged inside it with property getters (`extend Car : Weighed { val name : string get() => model }`). No stored fields, no replacing the type's own members; a missing member is a compile error at the `extend`. Holds where its folder's names do: its folder and every file importing it, with the access words (`internal extend`, `private extend`); in each file `x is Weighed` agrees with the compiler; a value used as a `Weighed` stays one when passed on. Several types that already fit take one line, `extend Person, Car, Truck : Weighed`. Two identical `extend`s of the same type and interface are one, with no error, so a program works with or without a library holding the copy; two that differ are a compile error naming both, anywhere in the program. Costs what any interface costs (L20 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 3 (2026-10-20), for classes; built-in types not yet. |
| D34 | A list that a type can be added to. | `items : open list<accepts Button>` takes an open list of `Button`, `Component` or any type above `Button`, and refuses lists of `Label` or of `Button`'s subclasses; through it, adding takes a `Button` and reading gives `any` (Kotlin's `MutableList<in Button>`). Only in an `open` type; also for map values and sets, `open map<string, accepts Button>`, `open set<accepts Button>` (L9 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 2 (2026-10-20); reading gives `any?`. |
| D35 | What can be thrown. | Only a `Throwable`, in Java's tree. Unchecked: `Error` (the VM cannot go on), `RuntimeException` (mistakes: division by zero, index, null, cast, `require`, `check`), `CancellationException`, `InterruptedException`. Checked: every other `Exception` (`IOException`, `TimeoutException`, `ExecutionException`, the program's own). The compiler works out which checked exceptions leave each function over the whole program; `throws` is written only on a library's public functions. A function calling a lambda it was given throws what the lambda throws. A compile error only where one reaches what cannot pass it on: a stored callback, a thread or flow task, an override or `extend` whose original cannot throw it. `require` throws `IllegalArgumentException`, `check` and `error()` `IllegalStateException`; `throw Error(...)` in today's code becomes `Exception` or those; `catch (e)` catches every `Throwable`. Nothing checked when running (L12 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 5 (2026-10-21). |
| D36 | Fields set after construction. | `lateinit var x : T`: a non-null type, always written, no `!!` at uses; reading it before it is set is an error when running (L13 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 3 (2026-10-20). |
| D37 | Fields worked out when first read. | `lazy val x = ...` works out its starting value at the first read and keeps it; `lazy var x = ...` likewise, and may then be assigned (assigned before the first read, its starting value is never worked out). Type optional, taken from the starting value; fields, statics and top-level declarations. A starting value that throws leaves it unset for the next read to try again; a read reaching it while it is being worked out is an error; other threads and flows wait for the one value; one test per read. Kotlin's `by lazy` spelling and other delegates are not accepted (L13 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 3 (2026-10-20). |
| D38 | Reflection. | Full: a class's members with their types, superclass, interfaces (`extend`s included) and type arguments listed; members read, set and called by name; only what the asking code may use, as the access words say. Member types kept when running (memory, not speed). Open: the syntax, what a call by name throws (L12), what part 5's leaving out of unreachable code keeps for it (L14 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 6 (2026-10-21). |
| D39 | A fixed set of kinds (Kotlin's sealed classes). | `enum class` is Kotlin's `sealed class`; `enum` is unchanged. Its kinds are the classes and objects extending it, in its braces or outside them in the same file only; each writes `: Event()`, inside the braces too (a class there that does not is an ordinary nested class). Bare kind names wherever an `Event` is expected, as for enum entries (`val e : Event = Click(1, 2)`, `is Click` in a `when`). `enum interface` is `sealed interface`, the same rules. Every `when` over an `enum`, `enum class`, `enum interface` or `bool` covers every kind (and `null` for a nullable subject) or has an `else`, statement or not. Bare entries in an `enum class` are a compile error pointing to `enum`. `sealed` is no longer a word (L21 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 3 (2026-10-20). |
| D40 | Union types. | Yes, with `\|`: `source : string \| TextModel`. Used after `is` narrows it; without narrowing only the common class's members. `when` covers each type or has `else`. A member type goes where the union is asked for; order and repeats do not matter. Named with `type Json = ...`. Inference gives the union (`flag ? 1 : "one"` is `int \| string`); `number` is `int \| float`, `T?` is `T \| null`. Item types too: `list<int \| string>`. Nothing added when running. Built in M6.6 (L15 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 3 (2026-10-20). |
| D41 | Names for types. | `type` names any type: `type Grid = list<list<int>>`, `type Handler = (Event) -> unit`, `type Table<T> = map<string, list<T>>`. Another way of writing the type, not a new type (Kotlin's `typealias`): values pass both ways, `is` and reflection see the type itself. Access words apply. A name refers to itself only inside another type (`list<Json>`). Built in M6.6 (L15 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 3 (2026-10-20). |
| D42 | Changes to Astra before 1.0. | Made freely, in one step, with all the code in the repository changed with them; no deprecations, no old names kept. Kotlin's current names, none of its deprecated ones. A removed word gives an error saying what to write instead where that helps. Deprecation cycles from 1.0 (L16 in [language-choices.html](language-choices.html)). |
| D43 | Experimental features. | No switches: a feature lands finished, one milestone at a time; unfinished work stays on a branch (L17 in [language-choices.html](language-choices.html)). |
| D44 | A logical shift and unsigned helpers. | `x >>> n` (and `>>>=`) shifts in zeros; `toUnsignedString` (with a radix), `compareUnsigned`, `divideUnsigned`, `remainderUnsigned`, `parseUnsigned` treat an `int`'s 64 bits as unsigned. Kotlin's `ushr` ports as `>>>`. Still no unsigned types (D23, reconsidered and kept) (L22 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 1 (2026-10-20). |
| D45 | What making a new collection gives. | `filter`, `map`, `sorted`, `toList` and the like give an `open` list, map or set, which goes anywhere a read-only one is read; a result type of `list<T>` still promises read-only where wanted (L23 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 2 (2026-10-20). |
| D46 | Turning a read-only collection back into an `open` one. | `as open list<T>` and `is open list<T>` on a read-only `list<T>` are compile errors. `toOpenList()` makes a changeable copy (Kotlin's `toMutableList()`), and `toOpenMap()`, `toOpenSet()` likewise (L24 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 2 (2026-10-20). |
| D47 | Read-only maps and sets passed by their types. | A read-only map goes where a map of wider key or value types is read, a read-only set where a set of a wider item type is; a lookup with a key the map cannot hold gives `null`. `open` maps and sets only at their exact types (L25 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 2 (2026-10-20). |
| D48 | How reflection is written. | A postfix `#`: `Image#` the class named `Image`, `x#` the class of the object `x`, lists as `[Circle#, Square#]` (since 2026-10-16, in place of `x.class`); the class has `name`, `fields`, `methods`, `constructors`, `superclass`, `interfaces`, `typeArguments`, `field("name")`, `method("name")`; a field's `get(obj)` and `set(obj, value)`, a method's `call(obj, ...)`. `class` may again be a member's name. No `::`, `KClass` or `className` (L26 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 6 (2026-10-21). |
| D49 | What a call by name throws. | Wrapped: what the method throws arrives as a checked `InvocationTargetException` (Java's name), the original in `cause` (L27 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 6 (2026-10-21). |
| D50 | What reflection keeps in a built program. | Known classes reflected on kept whole; on an unknown type, the classes whose objects can flow to that line, followed across the whole program (all created classes where it cannot tell); never-created classes left out. Only the kinds of members reflection uses anywhere are kept for it: no methods or constructors unless `methods`, `method(...)`, `constructors` or `call` is used. No `Class.named(text)`; `Class.namedFrom(name, classes)` finds a class by name among a list the program gives (`[Circle#, Square#]`, or an `enum class`'s `kinds`), `null` otherwise, so the compiler knows every class reachable by name. No `keep` marks (L28 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 6 (2026-10-21). |
| D51 | What a method with no body may throw. | Only what it writes, in libraries and programs alike (`fun load() : string throws IOException`); nothing written, nothing checked; an implementation throwing more is a compile error at the implementation. Default methods work within the interface's allowance. (Changed the same day from (c).) (L29 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 5 (2026-10-21). |
| D52 | Function types and what they may throw. | `() -> unit` throws nothing checked; `() -> unit throws IOException` may. A stored function value throws only what its type writes, in programs and libraries alike (so an `onClick` handler that can throw must catch). Lambdas worked out from their bodies; a function only calling its lambda passes on what it throws. Several exceptions are joined with `\|` wherever `throws` is written: `throws IOException \| TimeoutException` (L30 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 5 (2026-10-21). |
| D53 | What an override may throw. | No more than the method it replaces: what an interface or abstract method writes, or what a base method's body throws or its written `throws` allows; otherwise a compile error at the override. A base method gives its overrides room by writing `throws`. The same for `extend` bridges and for lambdas taking the place of a one-method interface (L31 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 5 (2026-10-21). |
| D54 | `throws` written where it is not required. | Allowed on any function, as a check: a compile error if the function could throw anything else. Listing more than the body throws is allowed, giving overrides room; callers handle what is written. Required only on a library's public functions, methods with no body and throwing function types (L32 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 5 (2026-10-21). |
| D55 | Lambda literals. | Kept: `{ a, b -> }`, `it`, the last expression as the value, `{}` and `{:}`, beside `fun(x) { }`. A lambda using `it` inside another that uses `it` is a compile error asking for one to be named. Under static types the expected function type decides whether there is an `it` (L33 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 4 (2026-10-21). |
| D56 | Trailing lambdas and the parameters around them. | Kotlin's rules kept: a trailing lambda goes to the last parameter; a defaulted parameter may come before a required one. Astra's exception (the lambda as the last missing required parameter) removed in M6.5 part 3; `groupBy`, `associateBy` and the like become overloads, calls unchanged (L34 in [language-choices.html](language-choices.html)). Overloads themselves are in since M6.5 part 3: functions, methods, static methods, operators and extension functions may have several forms told apart by their parameters' types (and an extension's receiver type), the checker choosing the most specific when compiling and the program choosing by the values where types are not known (README, "A name with several forms"). An override has the parameters of what it overrides, and a method with other parameters is another form, written without `override`; an override that adds one (a parameter with a default, as untyped code did) is an error naming the forms above (decided 2026-10-17, on since step 5). |
| D57 | Blocks run on an object, and implicit `this`. | Kept as they are, with their lookup order. Added `this@Name`: the object of the enclosing class `Name`, or of the block given to the function `name`, as in Kotlin (3 uses in JadeUI, one in `TextArea`, ported in M7). Built in M6.6 (L35 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 4 (2026-10-21). |
| D58 | Scope functions, `this` in blocks, and `return` in lambdas. | All kept. `this` inside a block run on an object is that object, as in Kotlin (today: an error outside methods, the method's object inside them); the method's object is `this@Name`. `apply`, `run`, `with`, `forEach`, `map`, `filter`, `any`, `all`, `first`, `find`, `count`, `sumOf` and the like are compiled in place beside `let`, `also`, `takeIf`, so a bare `return` in their lambdas leaves the enclosing function and they run as plain loops; elsewhere a bare `return` stays an error (L36 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 4 (2026-10-21). |
| D59 | Inline functions. | `inline fun` added: copied into each call with its lambdas, so a bare `return` in them leaves the caller and nothing is allocated. Lambda parameters worked out (only called: copied in; stored or passed on: not), so no `noinline` or `crossinline`. No recursion. The standard library's in-place functions (D58) are written with it. Built in M6.6 (L60 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 4 (2026-10-21). |
| D60 | Two ways to write an exclusive range. | `..<` only: `a until b` is a compile error saying to write `a..<b`, and `until` stays an ordinary name; `..` includes its end; `downTo` and `step` stay. Built 2026-10-15 (changed the same day from keeping both) (L37 in [language-choices.html](language-choices.html)). |
| D61 | Null operators, jumps as values, and `nothing`. | `?.`, `?:`, `!!` and jumps as values kept. Added the type `nothing` (Kotlin's `Nothing`) for what never gives a value: jumps, and functions that never return (`fun error(message : string) : nothing`, `TODO()`), so `x ?: return` and `x ?: error(...)` give a non-null value. Built in M6.5 part 1 (L38 in [language-choices.html](language-choices.html)). |
| D62 | `and`, `or`, `!` and conditions. | Kotlin's rule: `and`, `or` and `!` take only `bool` and give a `bool`; `if`, `while` and `? :` need a `bool`. Defaults are `?:`, null tests `x != null`. Built in M6.5 part 1 (L61 in [language-choices.html](language-choices.html)). |
| D63 | `if`, `try` and `when` as values. | Kept: each has the value of the branch taken; `when` in Kotlin's form, covering every kind or with `else` (D39); `if` keeps its braces; `? :` stays (L39 in [language-choices.html](language-choices.html)). |
| D64 | Destructuring. | Kept for lists, data classes, value types, pairs, triples and map entries. A `for` over a map gives entries, as in Kotlin and Astra's own map functions (`for ((k, v) in map)`; keys by `map.keys`), instead of keys. No `componentN` opt-in for ordinary classes (the one ported use becomes a `value` type). Built in M6.6 (L40 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 2 (2026-10-20). |
| D65 | Data classes. | Kept beside `value` types. Extended only by data classes (an ordinary class extending one is a compile error); the child's header starts with the parent's fields, and each gets its own generated `==`, `hashCode`, `toString`, `copy` and destructuring; `==` needs the same class on both sides; `copy` gives the object's own class. Built in M6.6 (L41 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 3 (2026-10-20). |
| D66 | Singletons and anonymous objects. | `object` and `object : Shape { }` kept. A named `object` is made when first used, as in Kotlin, not where its line is reached, with `lazy val`'s rules (one object across threads and flows; a failure leaves it unmade for the next use to retry). Anonymous objects are made where written. Built in M6.6 (L42 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 3 (2026-10-20). |
| D67 | Static members. | Declared only in a class's one `static { }` block (Kotlin's one companion object ports as it); the `static` prefix and a second block are compile errors. Astra's own 154 prefixed statics move into their blocks. Built in M6.6 (L43 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 3 (2026-10-20). |
| D68 | Property accessors. | Kotlin's form only: accessors under the property, `field` for the stored value. Astra's older `get x()` / `set x(v)` members become a compile error showing the Kotlin form; Astra's own 188 move over. Built in M6.6 (L44 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 3 (2026-10-20). |
| D69 | A property and a method with one name. | Kept: a field and a method may share a name (`c.tooltip` reads, `c.tooltip("text")` calls), so JadeUI's chained calls port as written (L45 in [language-choices.html](language-choices.html)). |
| D70 | Where extension functions are in reach. | Follow imports, as names and `extend` do: in reach in their folder and in files importing it, with the access words; two imported extensions of one name on one type are an error where the call is ambiguous. The standard library's stay in reach everywhere. Built in M6.6 (L46 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 3 (2026-10-20), for the checker. |
| D71 | `public` and `@Volatile`. | Both compile errors saying they are not needed, as `open`, `final` and `sealed` are; ported code deletes them (2,506 `public`, 64 `@Volatile`). No annotation is accepted any more (L47 in [language-choices.html](language-choices.html)). |
| D72 | Infix functions. | `infix fun` added (a member or extension with one parameter, called `a name b`), with Kotlin's rules and precedence (below arithmetic and ranges, above `?:`, `in`, `is`, comparisons). `to`, `downTo` and `step` move from the parser to the standard library as `inline infix fun`s; `until` stays an error. Built in M6.6 (L62 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 4 (2026-10-21). |
| D73 | `Pair`, `Triple`, map entries and indexed values. | Become `value` types in M6.6: no identity, compared by contents; nothing made for them in lists of pairs or in loops that take them apart; boxed in ordinary variables and fields as now (L48 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 2 (2026-10-20). |
| D74 | One name for each thing in the standard library. | Kept: `add`, `removeLast`, `upper`, `lower`, `has` with `contains` and `containsKey`, `join`, `subList`, `substring`, `forEach`, `toInt`, `toFloat`, `roundToInt`, `n.char` and `char(n)`, `c.code`. Gone: `push`, `pop`, `uppercase`, `lowercase`, `joinToString`, `slice`, `each`, `toLong`, `toDouble`, `roundToLong`, `toChar`, `chr` (now `char`), `ord`, and the free functions `len`, `str`, `int`, `float`, `type` (for `size`/`length`, `toString()`, `toInt()`, `toFloat()`, `x#.name`). Each name that goes is a compile error saying what to write (L49 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 2 (2026-10-20). |
| D75 | The name of the fraction type. | `float`, made with `toFloat()`; Kotlin's `toDouble()` is a compile error saying so (D74 changed to match). Ported `Double` and `Float` are written `float` (L63 in [language-choices.html](language-choices.html)). |
| D76 | Limits of numbers. | On Astra's types at their real values: `int.maxValue` (2^63 − 1), `int.minValue`, `float.maxValue`, `float.minValue` (most negative), `float.minPositive` (Kotlin's `Double.MIN_VALUE`), `float.nan`, the infinities. `Int`, `Long`, `Double`, `Float` holders go (errors). Jade uses `int.maxValue` for "unbounded" (its `maxInt` goes); the native layer clamps sizes handed to Windows, OpenGL and the text engine. Built in M6.6 part 8, group 1 (2026-10-20) (L50 in [language-choices.html](language-choices.html)). |
| D77 | A map's `keys`, `values` and `entries`. | Read-only live views (`set<K>`, `Values<V>`, `set<MapEntry<K, V>>`), not copies: they show the map as it is; changing them is a compile error pointing to the map's own `remove`, `removeAll`, `retainAll`; a snapshot is `toList()` or `frozen()`. Built in M6.6 (L51 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 2 (2026-10-20): keys and entries. `values` (G22, 2026-10-24, as the user chose): a `Values<V>` of its own, not a list, as Kotlin's is a `Collection`: the values in key order, which may repeat and have no positions (`values[i]` an error pointing to `toList()[i]`); a map's values alone are one. |
| D78 | Printing. | Kotlin's names: `println` ends the line, `print` does not; `write` goes (an error pointing to `print`). Astra's own 1,923 `print` become `println` and its 29 `write` become `print`, in one step. Built in M6.6 (L52 in [language-choices.html](language-choices.html)). |
| D79 | Thread and input-output classes. | Astra's own where Astra has better, in parts decided separately: atomics (L64), locks (L65), channels (L66), thread pools (L67), futures (L68), durations (L69), processes (L70), what stays (L71) (L53 in [language-choices.html](language-choices.html)). |
| D80 | Threads. | Kept as they are: one heap, one thread running Astra code at a time (D10). Several cores come from workers and parallel sections, with L72 to L77 widening what they can do. No fully parallel threads (L54 in [language-choices.html](language-choices.html)). |
| D81 | What a function that can wait is called. | "Task", in code and text: `task { }` starts one (Kotlin's `startCoroutine`), `suspendTask { resume -> }` sets it aside, `resume` a plain function (`resume(value)`, `resume.fail(e)`). `Continuation` and `EmptyCoroutineContext` go; Kotlin's names are errors saying what to write. "Flow" in the README, examples and messages becomes "task"; thread pools speak of work and jobs. (L55 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 7 (2026-10-21). |
| D82 | Jade's types named after Java's. | Astra's own names where they read better, decided type by type (L79 to L87); API rule 2 changes accordingly: Jade keeps JadeUI's names for its own types, not the JDK's (L56 in [language-choices.html](language-choices.html)). |
| D83 | JadeUI's API in Jade. | Free to improve on JadeUI's names, parameters, order and defaults, but every rename or change is put to the user first, with what it gains and what it costs the apps, and made only when agreed; until then JadeUI's form is kept. Section 6, rule 1 changes accordingly (L57 in [language-choices.html](language-choices.html)). |
| D84 | `==` on collections. | Kept: `==` compares contents all the way down, `===` identity (D14 confirmed) (L58 in [language-choices.html](language-choices.html)). |
| D85 | Atomic values. | One `Atomic<T>` (`get`, `set`, `update { }`, `exchange`, `compareAndSet`; on `int` `increment`, `decrement`, `add`); Java's four classes go. The one shared thing a parallel section may change, with real atomic operations underneath; only values and frozen data stored from a section; `update`'s lambda may run more than once. (L64 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 7 (2026-10-21). |
| D86 | Locks. | `LockBox(value)` holds data reached only inside `useKey { it -> }`; a plain `Lock()` guards work with `useKey { }`; `waitUntil { condition }` inside gives the key back until it holds. `synchronized`, `ReentrantLock`, manual `lock`/`unlock`, `wait`/`notify` go (errors saying to use `useKey`). (L65 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 7 (2026-10-21). |
| D87 | Channels in place of blocking queues. | `Channel<T>`: no default size, so `Channel<T>.unbounded()` or `Channel<T>(capacity = n)` (0: each send waits for a receive); `Channel<T>()` is an error asking which. `send`, `receive` (with a timeout), `trySend`, `tryReceive`, `close`, `for` over it until closed and empty; blocks a thread, sets a task aside. `BlockingQueue` goes; `Deque` stays. (L66 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 7 (2026-10-21). |
| D88 | Thread pools. | `ThreadPool(threads, name, daemon)` and `JobQueue(name)` (one thread, jobs in order); both with `submit { }` (a `Job`, D89), `after(d) { }`, `every(d) { }`, `close()`, `closeNow()`, `use { }`. `Executors`, `Executor` and thread factories go. Jobs, not tasks. (L67 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 7 (2026-10-21). |
| D89 | The answer to come of a job. | `Job<T>` (Java's `Future`): from `submit { }`, from `task { }` (as Kotlin's `async`), or by hand (`complete`, `fail`). `then { }` and `onError { }` run on the caller's event loop; one `get(timeout)` for threads and tasks; a failure is a checked `ExecutionException`; `getOrNull`, `failed`, `cancel(interrupt)`; `Job.all`, `Job.any`. `Future` goes. (L68 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 7 (2026-10-21). |
| D90 | Durations. | `Duration`, a value type (`16.milliseconds`, `1.5.seconds`; arithmetic, comparisons, `inMilliseconds`), taken by every call that waits or times; a plain number there is a compile error. `TimeUnit` goes. (L69 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 7 (2026-10-21). |
| D91 | Starting other programs. | `Process(command..., directory, environment = {map}, input, output, error)` starts one; streams take `pipe`, `inherit`, `discard`, `toOutput` (errors into the output) or a `File`. `Process.run(...)` runs to the end and gives `exitCode`, `output`, `error`. `ProcessBuilder` goes. (L70 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 8 (2026-10-21). |
| D92 | Atomic fields in parallel sections. | A field marked `atomic var` may be changed by any core in a section through atomic operations only (`+=`, `-=`, `max`, `min`, `compareAndSet`); plain assignment there is a compile error; only values and frozen data stored. (L72 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 7 (2026-10-21). |
| D93 | Per-core results merged at the end. | `parallelStream().gather(into = { }, each = { result, x -> }, merge = { a, b -> })`: an empty result of the answer's shape per core, `each` the parallel work on every item, merged at the end into one answer; nothing shared. `fold` and `reduce` on a parallel stream keep the list meaning: one answer. (L73 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 7 (2026-10-21). |
| D94 | Shared collections in parallel sections. | `SharedMap` and `SharedSet`, changeable from any core, each operation whole (`add`, `remove`, `putIfAbsent`, `update`); keys and values shareable ones only. No `SharedList` (its order would vary each run): `map`, `flatMap` (added to parallel streams) and `gather` collect results in order. (L74 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 7 (2026-10-21). |
| D95 | Reading every item in a parallel section. | No read-only option: `cells.indices.parallelStream()` already makes `cells` shared and read-only for every core. The error for looking into another core's item says so; the README shows double buffering. (L75 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 7 (2026-10-21). |
| D96 | Dividing work recursively. | `parallel({ a }, { b })` and sections inside sections, on work stealing; captured data is read-only to each lambda. `parallelChunks` extended from `bytes` to all lists of values; `parallelSort()`; `splitAt(mid) { left, right -> }` hands out two views that cannot overlap, each owned by one lambda. (L76 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 7 (2026-10-21). |
| D97 | Large data for workers without copying. | Frozen data (`frozenList`, `frozenMap`, `frozenSet`, frozen `bytes`) sent to workers is shared, not copied: kept once outside the heaps, counted, read in place. `post(bytes, handOver = true)` moves a buffer with no copy, leaving the sender's empty. (L77 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 7 (2026-10-21). |
| D98 | Sequences and generators. | Kotlin's: a lazy `sequence<T>` with lazy operations; `generateSequence`, `asSequence()`, and generators `sequence { yield(x) }` running as tasks with small growing stacks; only `yield` may wait in one. (L78 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 7 (2026-10-21). |
| D99 | `Color`. | Name kept; a `value` type of four bytes, kept in place everywhere with no allocation; compares and keys by its parts. (L79 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 9 (2026-10-22). |
| D100 | `Font`. | Name kept; stays an object, holding the text engine's data for its face (L80 in [language-choices.html](language-choices.html)). |
| D101 | `FontMetrics`. | Merged into `Font`: `ascent`, `descent`, `lineHeight`, `leading`, `width(text)`; `FontMetrics` goes (an error saying to ask the font). (L81 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 9 (2026-10-22). |
| D102 | `Dimension`. | Becomes `Size(width, height)`, a `value` type; Java's getters and setters go; `Dimension` is an error saying to write `Size`. (L82 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 9 (2026-10-22). |
| D103 | `Point`. | Name kept; a `value` type; `translate` a `mutating` method; Java's getters go. (L83 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 9 (2026-10-22). |
| D104 | `Rectangle`. | Becomes `Rect(x, y, width, height)`, a `value` type kept inside components as bounds; `var` fields through their place; `translate`, `grow`, `setBounds` `mutating`; `Rectangle` an error saying to write `Rect`. (L84 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 9 (2026-10-22). |
| D105 | `Insets`. | Name kept; a `value` type; Java's order (top, left, bottom, right), named arguments for safety. (L85 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 9 (2026-10-22). |
| D106 | `BufferedImage`. | A `Drawable` interface, implemented by `Image` (pixels in memory: Java's `BufferedImage`) and `Surface` (on the graphics card); `canvas.drawImage` becomes `canvas.draw(picture : Drawable, x, y, w, h)`. `BufferedImage` and `drawImage` are errors saying what to write; `canvas.draw` and `canvas.fill` take a `Drawable` or a `Shape` (an interface of value types: `Rect`, `Oval`, `Line`, later `Path`); text keeps `drawString(text, x, baseline)`; the number-taking `drawOval`, `fillRect`, `drawLine` and the rest stay, making no shape. (L86 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 9 (2026-10-22). |
| D107 | The file type. | `Path`, in place of `File` (an error saying to write `Path`): a name of something on disk, file or folder, existing or not; Java's `java.nio.file.Path` lands on it too. Where earlier decisions say `File`, read `Path`. Details in L103 to L106 and L112 to L115 (L87 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 8 (2026-10-21). |
| D108 | Removed names declared again. | The "write X instead" error is given only when nothing of that name is in reach; a program or library may declare such a name (an extension, a `type` name, a function, a class) and use it, reaching only code that imports it. `until` counts as a name once infix functions exist. Refused modifiers stay refused. A porting library of Kotlin's and Java's names over Astra's is optional (L107 in [language-choices.html](language-choices.html)). |
| D109 | `vararg`. | Kept (L88 in [language-choices.html](language-choices.html)). |
| D110 | Labels on loops. | Kept as they are: `outer@ for`, `break@outer`, `continue@outer`, the same `name@` form as lambdas (L89 in [language-choices.html](language-choices.html)). |
| D111 | `do { } while`. | Kept (L90 in [language-choices.html](language-choices.html)). |
| D112 | Raw strings. | Kept; `$$"""` added (Kotlin 2.1, C#): `$$name` is read, a lone `$` is text. Built in M6.6 part 8, group 1 (2026-10-20) (L91 in [language-choices.html](language-choices.html)). |
| D113 | `finally`, and cleaning up. | `finally` kept; `use { }` added: closes a resource with `close()` when its block ends, however it ends; compiled in place. Built in M6.6 (L92 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 5 (2026-10-21). |
| D114 | `downTo` and `step`. | Kept, as standard-library infix functions (D72) (L93 in [language-choices.html](language-choices.html)). |
| D115 | Collection helpers. | Kept: `indices`, `lastIndex`, `withIndex()`, `forEachIndexed`, `repeat` (L94 in [language-choices.html](language-choices.html)). |
| D116 | Named arguments to built-in functions. | On every function, built-in or not: the built-ins are declared in Astra with their parameter names (M6.5 part 3); M2's hand-made list goes. Built in M6.5 part 3 (L95 in [language-choices.html](language-choices.html)). |
| D117 | `Thread`. | `Thread` kept for making, configuring and extending threads; `thread(name, daemon) { }` starts one and gives a `Job` of its result, as `task { }` and `submit { }` do. Durations for `join` and `sleep`. (L96 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 7 (2026-10-21). |
| D118 | `CountDownLatch`. | Named `Countdown(n)`: `countDown()`, `await(timeout)` (blocks a thread, sets a task aside), `count`; `CountDownLatch` an error saying so. (L97 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 7 (2026-10-21). |
| D119 | Streams. | `Reader` and `Writer` in place of `InputStream` and `OutputStream` (errors saying so), each doing bytes and text, both `Closeable`; `path.reader()` and `path.writer()` open files; `inputStream()`, `bufferedReader()` and the other do-nothing calls are errors pointing to them. (L98 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 8 (2026-10-21). |
| D120 | Sockets. | `Socket` and `ServerSocket` kept, with `reader`/`writer` (D119) and `readTimeout` (Java's `soTimeout`) as a duration; Java's getters go. TLS, UDP, addresses and HTTP are M12. (L99 in [language-choices.html](language-choices.html)). Changed 2026-10-17 (review R1): socket reads and writes block their thread and never set a task aside; a server serves each connection from a thread or a pool's job. Built in M6.6 part 8, group 8 (2026-10-21). |
| D121 | The exceptions' names. | Java's names kept (`IOException`, `TimeoutException` and the rest), under L12's `Throwable` tree (L100 in [language-choices.html](language-choices.html)). |
| D122 | `Picture` and `Image`. | One in-memory picture type, `Image`: `Picture`'s loading, saving and pixels, drawn on through `canvas()` (in memory, D123); `Picture` internal (an error saying `Image`); `snapshot()` gives an `Image`. `Surface` unchanged. (L101 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 9 (2026-10-22). |
| D123 | Drawing on images, surfaces and windows. | One `Canvas` with the same calls for all (the `Image`'s own drawing calls go; pixel access stays). An image is drawn on in memory by a software engine, and its copy on the graphics card, if it has one, is updated from memory (the changed part, when next drawn from the card); surfaces and windows are drawn on the graphics card. So images read back with no wait, draw the same pixels everywhere and draw without a graphics card; heavy off-screen drawing uses a `Surface` (L108 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 9 (2026-10-22). |
| D124 | Where images and fonts live. | In Astra's standard library, for every program with no import (the user, 2026-10-16, reversing the same day's move into Jade). The text engine stays where M4 put it (`Typeface`, `Fonts`, the text font, `FontAxis`), and the merged `Image` (D122), with PNG reading and writing, and the font (`Font`, with `TextFont` as L102 settles) join it there; Jade uses them. `Surface` and the window stay Jade's. `Canvas`, `Drawable` and the software engine that draws an image's canvas (D123) are in the standard library too, so any program draws into an image without Jade; Jade's canvases build on that `Canvas`, which carries no widget style (D132). The standard `Image` knows nothing of the graphics card: it keeps a change number, raised by every drawing call and every pixel set, and the rectangle changed since. Jade keeps the card copy, in a slot beside the image's pixels in the C++ that only the native layer uses (the texture and the change number it was copied at): drawing an image on a Jade canvas sends it whole the first time, nothing while the numbers match, and only the changed rectangle when they differ; the slot goes when the image is collected, its texture freed on the main thread. None of this is in the API; `Surface.of(image)` and `surface.toImage()` are for a program that wants the card copy in its own hands. An image drawn on by a worker is handed over, frozen or locked before it is shown, by the rules for parallel work. (Java's `BufferedImage` and its `SurfaceManager`, and Skia's generation IDs, work the same way.) Built in M6.6 part 8, group 9 (2026-10-22). |
| D125 | `TextFont` and `Font`. | One font type, `Font`, free of scale, in the standard library (D124): a family or `Typeface`, a size and named options; measured in layout units (`scale = ...` for pixels); drawn at each canvas's own scale. `TextFont` internal to the standard library (an error saying `Font`), kept by `Font` per scale; Jade hands the native layer the `Font` and the scale. Java's constructor and `deriveFont` stay unless changed separately (L57). (L102 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 9 (2026-10-22). |
| D126 | `mkdir` and `mkdirs`. | One call, `path.mkdir(parents = true)`: the chain by default, just the one folder with `parents = false`; a folder already there is not a failure; failing throws `IOException`. `mkdirs` an error saying `mkdir()`. (L103 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 8 (2026-10-21). |
| D127 | Character sets. | A small `Charset` that means something: UTF-8 (default), Latin-1, ASCII, UTF-16 LE and BE, on `reader`, `writer`, `readText`, `writeText`, `appendText`, `readLines`, `useLines`, `forEachLine` and the text-to-bytes conversions; ported calls work as written. Named `Charset.utf8`, `latin1`, `ascii`, `utf16le`, `utf16be`; Kotlin's `Charsets.UTF_8` and Java's `StandardCharsets.UTF_8` (and the rest) are errors naming the one to write. `useLines` gives lines as read (L55). (L105 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 8 (2026-10-21). |
| D128 | `parent` and `parentFile`. | `parent` is a `Path`; the folder's name is `path.parent.name` and its text `path.parent.text` (L114), so no `parentName`; `parentFile` an error saying `parent`. `parent` is null at the top and for a bare name (`path.parent?.name`), as in Java. (L104 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 8 (2026-10-21). |
| D129 | <code>renameTo</code> or <code>moveTo</code>. | `a.moveTo(b)`, beside `copyTo`; `renameTo` and Java's `Files.move` errors saying `moveTo`. (L116 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 8 (2026-10-21). |
| D130 | `size`, not `length`. | Every count of things in Astra is `size`: strings, lists, maps, sets, `bytes`, `StringBuilder`, a `Path`'s size on disk; parameters named `length` (`padStart`) renamed `size`. `length` on Astra's own types is an error saying `size`; a program's own classes may still name a member `length` (a vector's, say). Built in M6.6 (L117 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 2 (2026-10-20). |
| D131 | Java's `Files.x(path)`. | Onto `Path`: `Files.x(p)` is `p.x()` (`exists`, `readText`, `writeText`, `readBytes`, `writeBytes`, `readLines`, `mkdir`, `moveTo`, `copyTo`, `size`, `list`, `walk`, `reader`, `writer`, `delete`); `Paths.get`/`Path.of` are `Path(s)`; `toPath()`/`toFile()` errors; `fileName` is `name`. Added under Java's names: `Path.createTempDirectory(prefix)`, `Path.createTempFile(prefix, suffix)`, `deleteIfExists()`, `isExecutable` and permissions, `setLastModified`. No `Files` object. Every name in Astra's API says "directory", never "folder" (`Process(..., directory = ...)`). (L106 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 8 (2026-10-21). |
| D132 | A widget's style. | On the component, not the canvas: `paintInto` works out the style (`styleIn`) and keeps it as `paintStyle`; contents read `owner.paintStyle`, every content having an `owner`; `paint(canvas)` unchanged; `Canvas` and `sub` carry no style. JadeUI's 24 reads and 16 `sub(..., style)` change. Two bugs where a component painted in its parent's style (too big to keep; the overlay over a kept picture) fixed in Jade's port on 2026-10-17. (L109 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 9 (2026-10-22). |
| D133 | Writing an `Image`'s pixels. | As a `Bitmap`: `image.editPixels { bitmap -> }` writable for the block, marking the image changed after; `image.pixels` the same, read-only (no copy); `image.set(x, y, color)` for one pixel. `bitmap[x, y]` is a `Color` and `bitmap[x, y] = color` sets it, `get(x, y)` and `set(x, y, color)` the same; `width`, `height`; raw bytes by `bitmap.bytes`, writable only in the block. Built into the runtime. Parallel work by rows (`parallelChunks(0..<bitmap.height)`). New: indexing with several arguments, `op [](x, y)` and `op []=(x, y, value)`. Every change raises the change number (D124). (L110 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 9 (2026-10-22). |
| D134 | A pixel outside the image. | `get`, `set`, `bitmap[x, y]` and `bitmap[x, y] = c` throw `IndexOutOfBoundsException` outside, on `Image` and `Bitmap`; `getOrNull(x, y)` gives null there. (L111 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 9 (2026-10-22). |
| D135 | `absolute` and `canonical`. | `path.absolute` (made whole; need not exist) and `path.canonical` (`.`, `..` and links worked out; `IOException` if missing), both `Path`s; `absolutePath`, `absoluteFile`, `canonicalPath`, `canonicalFile`, `toAbsolutePath()`, `toRealPath()` errors naming them. (L112 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 8 (2026-10-21). |
| D136 | The static file shortcuts. | `File.read`, `write`, `append`, `exists` (static, taking text) go: errors saying `Path(p).readText()`, `writeText`, `appendText`, `exists()`; Astra's 7 uses rewritten. `Path.createTempDirectory`/`createTempFile` stay (D131). (L113 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 8 (2026-10-21). |
| D137 | The text of a path. | `path.text`, and `toString()` the same (so `"$path"` works); `.path` and `getPath()` on a `Path` errors saying `.text`. (L114 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 8 (2026-10-21). |
| D138 | `list` and `listFiles`. | `dir.list()` gives `Path`s (names by `.name`); `dir.walk()` every path below, as a sequence; `listFiles`, `walkTopDown` errors. (L115 in [language-choices.html](language-choices.html)). Built in M6.6 part 8, group 8 (2026-10-21). |
| D139 | Constants: `const`. | Written `const x = 1000`, not `const val` or `val const` (errors saying so). A check, as `tail`: its value must be worked out from literals and other constants, or a compile error says what is not constant; folded into each use. Top level, `object` and `static { }` only. The apps' 690 `const val` lose their `val`. Built in M6.5 (R2 in [decision-review.html](decision-review.html)): the checker works the value out with the VM's own arithmetic and the compiler writes it in place of each read; any expression made only of literals and constants is folded the same way, except one that would fail when running (a division by zero), which is left to fail then. A `val` whose value is constant and the same for every read (top level, object, static or local) is folded as well, as the user asked on 2026-10-18; a read an access word refuses is left to be refused. `Math` is declared to the checker as `external object Math`, with `pi` and `e` as constants; the standard library's and Jade's fixed values (`TimeUnit`'s units, `Font`'s styles and families, `BufferedImage`'s types and the like) are constants. The shell checks each input as a file is checked, and takes back one that does not compile. |
| D140 | `contains` and `in`. | One name: `contains`, and `in` calling it. Maps: `map.keys.contains(k)` / `k in map.keys`, `map.values.contains(v)` / `v in map.values` (live views, no copy); `map.contains(...)` and `... in map` only for a whole entry (`"a" to 1`, a `MapEntry`), else a compile error naming the keys or values form. `has`, `containsKey`, `containsValue` errors. Changes L49 (D74). Built in M6.6 (R3 in [decision-review.html](decision-review.html)). Built in M6.6 part 8, group 2 (2026-10-20). |
| D141 | `getRGB` and `setRGB`. | Errors saying to write `get(x, y).argb` and `set(x, y, Color(argb))`; pixels are always a `Color` (`color.argb` for the number, `Color(argb)` from it). Settles L101 (D122). (R4 in [decision-review.html](decision-review.html)). Built in M6.6 part 8, group 9 (2026-10-22). |
| D142 | `n.char` and `char(n)`. | `n.char` only, matching `c.code`; `char(n)` (and Kotlin's `toChar()`) errors saying `n.char`. Changes L49 (D74). Built in M6.6 part 8, group 1 (2026-10-20) (R5 in [decision-review.html](decision-review.html)). |
| D143 | Bundles of named arguments: `params(...)`. | `val style = params(bold = true, size = 14)` holds named arguments; `label("hello", ...style)` passes them, checked at each call against that function's parameters (an unknown name or a wrong type is a compile error); a name written at the call wins (`...style, size = 18`); `style.copy(size = 18)` and `style + params(italic = true)` build new ones. A value type whose type is its names and their types; expanded at each call, so no cost when running. For functions not designed together; functions that are use an options class. (R20 in [decision-review.html](decision-review.html)). Built in M6.6 part 8, group 9 (2026-10-22). |
| D144 | Making a `Font`. | One way: `Font(name, size, bold = ..., italic = ..., weight = ...)` (or from a `Typeface`); Java's `Font(name, Font.BOLD, 14)` and the style flags `Font.bold`, `italic`, `plain` are errors showing the named form. `deriveFont` becomes `font.copy(size = ..., bold = ...)` (an error pointing to it), the one name for a changed copy, as data classes, value types and bundles. Changes L80, L102 (D100, D125). (R6 in [decision-review.html](decision-review.html)). Built in M6.6 part 8, group 9 (2026-10-22). |
| D145 | Building only what changed. | Three starting targets, measured on a 20,000-line program and then a ported app: a first run in under half a second, a second run with nothing changed in under 20 ms on top of `astra.exe` starting, a run after a one-line change in under 100 ms. Each file compiled once and cached by a hash of its text; each declaration keeps a summary (type, checked exceptions, may-wait, stable `==`/hash, `const` value, `inline` body); a file is rebuilt only if it or a summary it uses changed, so a body edit that keeps its summary rebuilds one file. Standard library and Jade compiled when `astra.exe` is built. Whole-program work done on summaries; the build-only parts (dead code, L28's reach) only in `astra build`. M14 compiles its C++ per part. A script's whole compiled program is kept in a cache of the machine's own (`%LOCALAPPDATA%\Astra\cache`, `~/.cache/astra`), so a second run with nothing changed (sizes and times, then hashes, of every file, and `astra.exe`'s version) starts with no compiling; it may hold what suits this processor only, and is never copied with the script. Entries are written atomically after the start; a damaged one is rebuilt; at most 500 MB, the entries used longest ago removed first. Built in M6.5 (R10 in [decision-review.html](decision-review.html)): tests/budgets.ps1 measures the three on the standard library, Jade, the examples and the tests (19,319 lines), from the process being made to the program ready: 116 ms, 11.5 ms on top of starting, and 73 ms (2026-10-18). A unit is a file, or a folder imported whole; its kept code's numbers are renumbered when it is put back; a summary is a file's text outside named functions' bodies, with the result types not written and R1's in-place facts; the bodies of what is put back are not checked. Every file is still parsed each run (a few at a time, on threads), for the checker reads what it declares; the standard library is attached to astra.exe by the build's last step. Jade is kept on its first use on each machine, not yet attached when astra.exe is built. |
| D146 | Counting from the end: `^k`. | In an index, `^k` is `size - k` (`items[^1]` the last, `items[^k]` computed, `^0` an error), for lists, strings and anything indexed by position. Negative numbers never count from the end: `items[-1]` a compile error saying `items[^1]`, a computed negative index an `IndexOutOfBoundsException`. `last()`, `lastOrNull()`, `lastIndex` stay. Anyone's functions can take `^n`: it makes an `Index` (a value type: a number and a from-the-end flag), an `int` passing as one; `at.of(size)` resolves it; an `int` parameter does not take `^n`; a `Range` may have `^` ends (`r.of(size)`), so `op [](i : Index)` and `op [](r : Range)` give any class `thing[^1]` and `thing[2..^1]`; a range with a `^` end cannot be looped over without a length. Changes L18 (D25). Built in M6.6 (R11 in [decision-review.html](decision-review.html)). Built in M6.6 part 8, group 2 (2026-10-20). |
| D147 | Slices. | A range in an index gives that part, inclusive as `..` is: `items[1..]` from 1 to the end, `items[0..^1]` the whole, `items[0..^2]` all but the last, `items[..^2]` (start 0), `items[2..4]` three items, `items[0..<^1]` with the end left out; strings and `bytes` too. A copy (an open list, L23), as `subList` gave; `splitAt` (L76) for views. Out of range an error (a slice may end right at the end: `items[3..]` of three is empty); a slice that runs backwards an error. `subList` and `substring` errors pointing to it (`items.subList(1, 4)` is `items[1..<4]`). One copy, no other cost. Two dimensions on a `Bitmap`: `bitmap[3..5, 10..50]` a new `Bitmap` (columns, then rows; a number takes one column or row); a class may declare `op [](xs, ys)` with ranges. Inside `editPixels`, `bitmap[3..5, 10..50] = other` copies a same-sized `Bitmap` into that region (pixels replaced, not blended; a size mismatch an error), marking only that rectangle changed; `op []=(xs, ys, value)` likewise. Changes L49 (D74). Built in M6.6 (R11 in [decision-review.html](decision-review.html)). Built in M6.6 part 8, group 2 (2026-10-20). |
| D148 | Storage widths, and 32-bit helpers. | `int16`, `int32`, `uint16`, `uint32` and `float32` are storage types as `byte` is: compact in packed lists, value types' fields and fields; read as `int` or `float`; arithmetic stays 64-bit; storing what does not fit an error (`toInt16Wrapped()` and the like for the low bits). Helpers for 32-bit work: `wrap32()`, `rotateLeft32`, `rotateRight32`, `hash(a, b, ...)`. Changes L0, L3 (D19, D23). Built in M6.6 part 8, group 1 (2026-10-20) (R12 in [decision-review.html](decision-review.html)). |
| D149 | Value types in variables. | Up to 32 bytes, a value type takes consecutive slots in locals, parameters and results once types are known, so `Point`, `Size`, `Rect`, `Insets` and `Pair` in variables make no object; boxed only where the type is not known (`any`, a union, a captured variable that outlives the call). Changes L2, L48, L82 to L85. Built in M6.7, with the frame slots that keep only a payload, which need the same change to how a frame's slots are laid out (moved to part 6 from part 5, and to M6.7 by the user, D178, on 2026-10-19; R13 in [decision-review.html](decision-review.html)). |
| D150 | `Path`: properties and functions. | Properties for what is in the path (`name`, `extension`, `nameWithoutExtension`, `parent`, `text`, `absolute`); functions for what asks the disk (`exists()`, `isFile()`, `isDirectory()`, `isExecutable()`, `size()`, `lastModified()`, `canonical()`, `list()`, `walk()`), so a property never throws. Changes L106, L112, L117 (D131, D135, D130). (R14 in [decision-review.html](decision-review.html)). Built in M6.6 part 8, group 8 (2026-10-21). |
| D151 | What a bare `catch (e)` catches. | Every `Exception`, and nothing else; an `Error` (`StackOverflowError`, `OutOfMemoryError`) only when named (`catch (e : Error)` or `Throwable`). Changes L12 (D35). Built in M6.6 with D35, whose tree of throwables it needs (R15 in [decision-review.html](decision-review.html); moved from M6.5 by the user on 2026-10-18). Built in M6.6 part 8, group 5 (2026-10-21). |
| D152 | `size` in two dimensions. | On things with a width and a height (`Rect`, `Image`, `Surface`, `Bitmap`, `Component`, `Window`) `size` is a `Size`; on collections, strings and `bytes` a count; a path's `size()` its bytes on disk. (R16 in [decision-review.html](decision-review.html)). Built in M6.6 part 8, group 9 (2026-10-22). |
| D153 | Annotations, and `astra test`. | Declared like classes (`annotation JsonField(val name : string = "")`), attached to classes, functions, fields and parameters, checked when compiling, read through reflection (`field.annotation<JsonField>()`), kept only where reflection reads them; no code generation. The standard library's own: `@Test`, `@BeforeTest`, `@AfterTest`, `@BeforeAll`, `@AfterAll`, `@Timeout`, a test's temporary directory, read by `astra test` (built in M6.6, with `assertEquals` and the like), and `@Deprecated` from 1.0. Kotlin's JVM annotations are errors saying what to write (`@Synchronized` a `Lock`, `@JvmStatic` the `static { }` block, `@Throws` `throws`, `@Suppress` deleted). Changes L47 (D71). Built in M6.6 (R17 in [decision-review.html](decision-review.html)). Built in M6.6 part 8, group 6 (2026-10-21). |
| D154 | The case of type names. | A fixed set is lowercase: the scalars `bool`, `char`, `byte`, `int`, `float`, the storage widths `int16`, `int32`, `uint16`, `uint32`, `float32`, and `string`, `any`, `nothing` and `unit`. Every other type is Title case, the standard library's included: `List<T>`, `Map<K, V>`, `Set<T>`, `Bytes`, `Sequence<T>`, `FrozenList<T>`, `FrozenMap<K, V>`, `FrozenSet<T>`, `Number`, `Color`, `Duration`, `Path`, `Channel<T>`. So `list`, `map`, `set` and `bytes` are free as variable names. Code written in these documents before this uses the old lowercase container names; Astra's own code moves in one step (L16). Built in M6.5 part 3, where every type in the repository is written (moved from M6.6) (R21 in [decision-review.html](decision-review.html)). |
| D155 | `Grid<T>`. | `Bitmap` becomes a generic built-in grid, `Grid<T>`: packed by item type as a `List` is (a `Color` 4 bytes a cell, `byte` 1, `bool` 1, `int16` 2, `float32` 4; objects unpacked), made with `Grid<bool>(width = 64, height = 64)` and a fill value. Everything decided for `Bitmap` holds for every `T`: `g[x, y]` and `g[x, y] = v`, `get` and `set`, `width`, `height` and `size`, slices `g[3..5, 10..50]` and writing into a region, errors out of range with `getOrNull`, work by rows across cores, `.bytes` for a value type's raw storage, `map { }` to a grid of another type (`pixels.map { it.alpha }` a `Grid<byte>`). An image's pixels are a `Grid<Color>` (`image.pixels`, `editPixels { }`); a grid of another type is data, not drawable. Where D133, D134, D141, D147 and D152 say `Bitmap`, read `Grid<Color>`. Adding rows and columns gives a new grid: `insertRows(at, count, fill)`, `insertColumns(...)`, `prependRows`, `appendRows`, `prependColumns`, `appendColumns`, and `insertRows(at, rows)` with another grid; `removeRows(at, count)` and `removeColumns(at, count)` (`at` may be `^k`); `fill` defaults to the type's zero; an unused result is a compile error. Built in M6.6 part 8, group 9 (2026-10-22). |
| D156 | `.=`: assigning a member's result back. | `target .= member` means `target = target.member`, the path to the target written and worked out once, as `+=` does (a path written twice can later be changed in one place and not the other): `a.b.c .= copy(p1 = 4)`, `grid .= insertRows(at = 3)`, `items .= sorted()`, `name .= trim()`, `cells[i] .= copy(alive = false)`, `dir .= parent`. The target is anything assignable (a `var`, a `var` field, a list element, a grid cell); the right side any member, a method call or a property, its result fitting the target's type. Raku's `.=`. (R23 in [decision-review.html](decision-review.html)). Built in M6.6 part 8, group 9 (2026-10-22). |
| D157 | No open classes; `extend` may add stored fields. | Every field is declared, in the header or the body; assigning a field not declared is a compile error everywhere, so a class that declares no fields no longer "stays open" (README, "Fields"). An `extend` may add stored fields to a class the program did not write (`extend Button { var tag : string? = null }`): the compiler sees every `extend` before laying a class out, each is a real slot of every instance of the class and its subclasses, in an extension area after the declared fields, laid out per program in its last step (an ancestor's extension fields first), so compiled code for the class, in `astra.exe` or the cache, is right for every program; reading one is the class's area start plus a fixed offset, one load more than a declared field; the name is in reach only where the `extend` is (L20); it needs an initial value or `lateinit`; two that add the same name are an error unless identical; not on value types or data classes. Costs one slot per instance for each such field in the program; for R10 it changes only the files using it and the program's last step, never the class's compiled code. Open classes go in M6.5 part 3, when everything is typed; extension fields with `extend`. Changes L20 (D33) (R24 in [decision-review.html](decision-review.html)). |
| D158 | `++`, `+=` and the rest on a place that may be null. | Null-safe forms, as `?.` is a null-safe `.`: `++?`, `--?`, `+=?`, `-=?`, `*=?`, `/=?`, `%=?` change the place when it holds a value and do nothing when it is null or a map's key is missing (`stock["apples"]--?`, `maybe.count++?`, `a?.visits++?`). The plain operators on a place that may be null are a compile error saying why and naming `++?`, the index's `default` and `?:`; a `?` form on a place that cannot be null is an error naming the plain one. A missing key that should start from a value has it in the index, as Swift's `dict[k, default: 0]`: `counts[word, default = 0]++`, `groups[kind, default = []] += [item]`; as a read it gives the value or the default without adding the key, typed as the map's values, never null; an update through it is one lookup. `map.update(key, start) { old -> new }` for a change that is not one operator; no `increment`. `update` is in the standard library; the operators and the index's `default` come in M6.5 part 3 (L118 in [language-choices.html](language-choices.html)). |
| D159 | Assigning to the name of a function. | A compile error, as in Kotlin (2026-10-18): `fun area() : int` cannot be given another value, `area = { ... }` or `area += 1`; the error says to keep a function that changes in a `var` of a function type. A call of a function declared once with a return type then gives that type, so the compiler leaves out checks of values it has proved (M6.5 part 4). |
| D160 | How the VM goes from one instruction to the next. | Decided 2026-10-18, from a measurement on M6.5 part 4's numeric loop with the instructions astra.exe emits (Clang 19, against one switch at 183 ms): a computed `goto` at the end of each instruction, through a table 108 ms, with the code threaded 103 ms; a tail call from each instruction's function 134 ms, threaded 128. Built with Clang (the one in Visual Studio), each instruction ends with its own computed `goto`, and a program's code is threaded when it is loaded: each opcode replaced by the address of its instruction's code, the operands as they are. Neither the executable nor bytecode files change (the addresses differ from run to run); the code held while running is about three times as large, some 1 MB for a program of 19,000 lines. Built with MSVC, which has no computed `goto`, the same instructions, each written once, are the cases of a switch. Built in M6.7, with the VM's loop rewritten. |
| D161 | How a built program leaves out the compiler. | Decided 2026-10-18, at the user's question whether the bases could be kept in `astra.exe` with nothing in it twice: one `astra.exe`, its code in sections, those a running program never needs (the compiler, checker, parser, lexer, cache, shell; the standard library's source text; Jade's window, drawing and the text engine) each in a section of its own. `astra build` writes a copy without the sections the program does not need, the space they took kept empty so that no other address moves, and then the program. The build proves the rest needs nothing left out by linking it once more on its own; the tests run as programs built this way. In place of separate base executables beside `astra.exe` (2.7 MB more for every install, the common parts three times) or the compiler in a DLL (two files, and Jade could not be left out the same way). Built in M6.5 part 5. |
| D162 | The type of `items.itemType`. | Proposed 2026-10-18 (M6.6 part 1), to be confirmed: a built-in type `Type`, a type as the program runs. `items.itemType`, `map.keyType` and `map.valueType` give one (`any` for a collection made without its type); it prints as written (`List<int>`), two are `==` when they are the same type, and `x is Type` tests for one. |
| D163 | How the C++ functions' results are checked. | Proposed 2026-10-18 (M6.6 part 1), to be confirmed: by a test setting, not on every call. `ASTRA_CHECK_NATIVE_RESULTS=1` compiles a check after every call of an `external` function against its declaration (the standard library compiled from its source, compiled programs kept apart); the suite passes with it, and each checkpoint runs it beside the GC and thread stress settings. The C++ is fixed code, so the suite proves its declarations once, and the compiler may then trust them when the program runs. A list or map a C++ function gives is always tagged at the call with the type the checker worked out, where it has none. |
| D164 | What checking the C++ results changed. | Proposed 2026-10-18 (M6.6 part 1), to be confirmed. The coerce functions give one of their numbers as it was, so `float.coerceIn(Number, Number)` and the like say `Number`, with forms for all ints and all floats that say `int` and `float`. A type parameter that the object a call is made on says nothing of (a `Map` written without type arguments) is unknown, not taken from the arguments: `data.get(key, "missing")` is not a `string?` there. `Deque.toList` builds a `List<T>` rather than casting its `List<T?>`, and `Set ==` gives false for a set of other things rather than failing at a cast, now that casts are checked in full. |
| D165 | The rules of a value type, as built. | Proposed 2026-10-19 (M6.6 part 2), to be confirmed. `value Point(val x : int, val y : int)` needs a header, every entry a field; it is a data class (printing, `==`, `copy`, coming apart) that implements interfaces and extends no class, and no class extends it. A field is given its value in the header or where it is declared in the body (one declared there with none is an error), and in the type's `init`, and never again: `p.x = 4` and `x += 1` in a method are compile errors saying to make a changed copy, until changing through a place (part 4). `===` on a value type is a compile error, as a value has no identity; through `any` it compares contents. A value type cannot hold itself but where the field may be null, which the checker reports. Every core may read a value, which never changes. |
| D166 | Lists of values, as built. | Proposed 2026-10-19 (M6.6 part 3), to be confirmed. A list is packed when it is given its type (by its literal, by what it is declared as, or at the call of a C++ function that made it): a `List<int>`, `List<float>`, `List<bool>`, a value type's list, and the nullable ones of those. Putting in a value the list was not made to hold is an error where it is put in (`cannot put a value of type string in a List<int>`), which an unchecked cast through `List<any>` used to allow. A copy of a list (`toList`, `slice`, `subList`, `reversed`, `+` of two of one type) keeps its type and store. `sortBy` and `sortByDescending` call the selector once per item, not twice per comparison. |
| D167 | Changing a value through its place. | Proposed 2026-10-19 (M6.6 part 4), to be confirmed. A `var` field of a value is changed only through a place that holds the value: a `var` variable (local, global, or one a lambda holds), a `var` field of an object, an item of a list, a value's field that is itself a place, or a lambda's parameter (what `forEach { it.move() }` is given); `p.x = 4`, `p.x += 1`, `pts[i].y++` and `line.to.x = 9`. A `mutating fun` may change `this`'s fields and call the type's other mutating methods, and is called only on a place; `mutating` on anything but a value type is an error. A variable or field changed this way gets a value of its own first, so `val q = p; p.x = 10` leaves `q` as it was. Every part of `pts[next()].x += 1` is worked out once. |
| D168 | Reading a value where it is. | Proposed 2026-10-19 (M6.6 part 4), to be confirmed. A value read from a list or another value (`pts[i]`, a loop's item, a lambda's `it`, a parameter, `this`) is not copied where the checker shows that nothing which may change a list of its type (remove, insert, sort, set, `replaceAll` and the rest, but not `add` at the end) runs while it is in use, through what the code calls, overrides included; a function given as an argument counts as one that may. A loop is judged by its body; given to a variable from outside the loop, the item is judged by the whole function. `pts[i]` used at once (a field read from it, an argument, a comparison) is judged by its statement; kept in a variable, by its innermost for or while loop, or by the function when the statement may give it to a variable from outside the loop; in a lambda, by the lambda (G10, 2026-10-24). A call that may wait counts as one that changes every list, since other code runs meanwhile. Whatever keeps a value longer than the stack does (a field, a list, a global, a captured variable, a C++ function) takes a copy. The test setting `ASTRA_COPY_READS=1` copies every read, and the suite prints the same with it. |
| D169 | Results built where they go. | Proposed 2026-10-19 (M6.6 part 4), to be confirmed. `pts[i] = f(...)`, where `f` gives a value of the list's type, gives `f` the item as where its result goes; a function that gives a value passes that place on to a call whose result it returns, and making a value from its header alone writes its fields there, every argument worked out before any is written. So `pts[i] = pts[i].moved(1)`, `pts[i] = Point(...)` and `pts[i] = pts[i].copy(x = 4)` make no copy. `list.replaceAll { it.moved() }` replaces each item by what the lambda gives, built where the item is. |
| D170 | Parallel sections over a list of values. | Proposed 2026-10-19 (M6.6 part 4), to be confirmed. `pts.parallelStream()` deals each core its own part of the list, and hands it the items where they are: `forEach { it.move() }` changes them there, `map` and the rest read them there. `parallelStream().replaceAll { it.moved() }` replaces each item on every core, built where it is. A core reads and writes only its own part (`pts[0]` from another core's part is an error saying so) and changes the list itself not at all (`sortBy`, `replaceAll` and `sortByDescending` count as changing a list); a built-in function given the whole list is refused, but for `isEmpty` and `isNotEmpty`. |
| D171 | Small values in the value itself. | Proposed 2026-10-19 (M6.6 part 5), to be confirmed. A value type whose layout is at most 8 bytes and holds no object (`value Rgba(val argb : int)`, `value Meters(val m : float)`, `value Flags(var on : bool, var shown : bool)`) is kept in the value itself, its fields in the 8 bytes a value has for its contents and its type's number beside them: in variables, arguments, results, fields, lists and maps, with no box and nothing allocated. The program sees no difference; C++ functions are given it in a box, but `list.add`. |
| D172 | Values inside their objects. | Proposed 2026-10-19 (M6.6 part 5), to be confirmed. A field of a value type larger than 8 bytes, not null, is kept inside its object, after its other slots, laid out when the first object of the class is made: `comp.bounds = r` copies the value's bytes in, `comp.bounds.w = 30` and `comp.bounds.origin.move(1)` change it there, and `comp.bounds = Rect(...)` builds it there (D169). A field is laid out so when its type is written or it is given a value made by its type's name (`var cached = Size(0, 0)`). A field of an object made before its class's field types exist keeps a box. |
| D173 | Fields and results read where they are. | Proposed 2026-10-19 (M6.6 part 5), to be confirmed. Giving a field of a value type another value counts, for D168, as a change to values of that type, as changing a list of them does. A field kept inside its object is read where it is (a reference into the object) where nothing that may change values of its type runs while the read is in use; otherwise it is copied. What a function gives back that it read where it is stays so where the caller is shown to be as safe (a call marked so, WithRefResult); a call's result read at once by one of its fields (`node.sizes().pref`), and a field given back by `return`, always are. Where nothing says so, a function gives back a copy. |
| D174 | `list.add` copies where the item is. | Proposed 2026-10-19 (M6.6 part 5), to be confirmed. `out.add(p)` for an item read where it is, in a loop say, copies its bytes straight into `out`, with no box made for it on the way; `add(i, item)` copies it first, since the items after `i` move. |
| D175 | Generic functions copied per layout. | Proposed 2026-10-19 (M6.6 part 5), to be confirmed: not built now. Measured on a million points: `map` 46 ms against 41 by hand, `sumOf` 25 against 23, `filter` 98 against 43, `sortedBy` 84. The check of section 10 costs a few percent; `filter`'s difference comes from copying each item handed to its lambda, which inline functions (D59) remove and copies per layout would not. So copies per layout wait until a measurement asks for them. |
| D176 | A number's conversions and Math's functions as instructions. | Proposed 2026-10-19 (M6.6 part 6), to be confirmed. `x.toInt()`, `toLong()`, `toFloat()`, `toDouble()`, `roundToInt()`, `roundToLong()`, `isNaN()`, `isInfinite()`, `isFinite()`, and `Math.sqrt`, `floor`, `ceil`, `round`, `abs`, and `min` and `max` of two ints or two floats, where the checker knows the call is the standard library's, are one instruction (Numeric), giving what the library's own gives, errors included. Drawing a million particles a frame is 34 to 40% faster. |
| D177 | Fields kept as their payload alone. | Proposed 2026-10-19 (M6.6 part 6), to be confirmed. An int, a float or a bool field whose type is known (written, or worked out from what it is first given), and not null, is kept in its object as its 8 bytes of contents alone, before the object's slots, where every object of the class has it at the same place: an object of three `float` fields takes 72 bytes in place of 96. A val, and a field an init must set, has a bit saying whether it has been given a value, so that giving a val a second value and an init that leaves one unset are still errors; any other payload reads as 0, 0.0 or false before it is first given one, where a slot read an error. Field reads and writes are as fast as before. |
| D178 | Frame slots, typed arithmetic and values in variables in M6.7. | Decided 2026-10-19 by the user: payload-only slots in objects now (M6.6 part 6); in frames, with instructions chosen by type for arithmetic and values of up to 32 bytes in consecutive slots (D149), in M6.7, on the rewritten loop, since each changes every instruction that touches a frame's slots and M6.7 rewrites those anyway. |
| D179 | Jade's geometry as value types. | Decided 2026-10-19 by the user (M6.6 part 7): `Point`, `Dimension`, `Insets`, `Rectangle` and `Sizes` are value types. Two are `==` when their fields are; each is copied where JadeUI's would be shared, so `bounds.translate(1, 1)` changes the `bounds` it is called on and no other; `setSize`, `translate`, `setLocation`, `grow` and `setBounds` are mutating methods, called on a place. A component's `bounds` and the sizes it keeps are inside it. Code that changed a shared one to change another's (none in Jade) changes only its own. |
| D180 | `Rectangle` takes all four numbers. | Proposed 2026-10-19 (M6.6 part 7), to be confirmed. A value type is made from its header alone, and Astra has no constructors of other forms (secondary constructors were left out: 4 uses), so `Rectangle(x, y, width, height)` takes all four, with no defaults: Java's `Rectangle()` is `Rectangle(0, 0, 0, 0)`, `Rectangle(w, h)` is `Rectangle(0, 0, w, h)` (both compile errors as written, so that `Rectangle(30, 40)` cannot quietly mean the corner), and `Rectangle(other)` is `other`, a value being copied wherever it is kept. `Point()`, `Dimension()` and `Insets()` keep their zeros, as in Java. |
| D181 | What a message to a worker may hold. | Decided 2026-10-22 by the user, in three steps. First, built with gap G6: besides numbers, strings, booleans, `null`, `bytes`, and lists, maps and sets of those, an object of a data class or a value of a value type, made again on the other side by its constructor from its header fields; a class declared at the top level of a file both sides import, named in the message by the global it is kept in (a worker runs the same program); lists and maps keep their types, and a packed list goes as its block of items, whole. An object of an ordinary class is refused where it is sent, as before: the worker would have a copy, not the object. Second, open, for when real programs need it: an ordinary class may say it can be sent (`class Student(...) : Sendable`), the checker making sure its fields are of types that can, and a message copying the graph of such objects whole, objects shared in it shared in the copy and cycles kept. Never: every class copied without asking, which would make objects whose `init` never ran, whose lambdas lost their surroundings, and whose holds on windows, files or locks broke in the worker. |
| D182 | What a type parameter is, to the checker. | Decided 2026-10-22 by the user. A type parameter is the one its declaration makes, not a name: `class Apple<T>`'s is Apple's T, `fun <R> other()` in it is other's R, `fun <T> augment()` is augment's T, a generic function declared inside another has its own. Inside the checker each is a thing of its own (its declaration, not a dotted name, which overloaded forms, files and local functions could make ambiguous), and what it is replaced by, worked out and kept in reach goes by that, never by the name; the compiler and the VM, where they are slots by position, do not change. Messages still say `T`, and `T (of augment)` only where two would be confused. A method that declares a type parameter of the same name as its class's (`fun <T> eat()` in `class Apple<T>`) is a compile error: Apple already has a type parameter T; name this one otherwise, or leave it out to use the class's. Built next, before gap G67's fix, which it makes safe (found with G3: the same name in the function called and the code calling it was worked out wrongly). |
| D183 | `Deque` is `Deck`. | Decided 2026-10-24 by the user: the standard library's queue open at both ends is called `Deck`, simpler to say and to read than `Deque`. Its members are as they were (`addFirst`, `addLast`, `removeFirst`, `removeLast`, `first`, `last`, indexing). `Deque`, and Kotlin's `ArrayDeque`, are compile errors saying `Deck`. |
| | How files are loaded under namespaces. | Importing a folder loads every `.astra` file directly in it, as one unit. Importing a file loads that file only, into its folder's namespace (5.1). |
| | A lambda's parameter list over several lines. | Allowed, as in Kotlin (5.1). |

### Open

Eleven. The language choices, all decided since; eight from M6.6 for the user to answer; one
left for the milestone that raises it; and one for when programs need it. (The measuring rule on scaled displays, open earlier, was settled in M4:
section 4, point 8.)

- **The language choices Kotlin made for the JVM** (L0 to L117 in
  [language-choices.html](language-choices.html)), written during M6.5 for the user to decide.
  Decided: D19 to D157. Every choice is decided.
- **M6.6 part 1's proposals**, D162 to D164: the built-in type `Type`, the C++ results
  checked by a test setting, and what that check changed in the standard library.
- **M6.6 part 2's proposal**, D165: the rules of a value type as built. (Jade's geometry as
  value types, open with it, was decided in part 7: D179.)
- **M6.6 part 3's proposal**, D166: lists of values as built.
- **M6.6 part 4's proposals**, D167 to D170: changing a value through its place, reading a
  value where it is, results built where they go, and parallel sections over a list of values.
- **M6.6 part 5's proposals**, D171 to D175: small values in the value itself, values inside
  their objects, fields and results read where they are, `list.add` copying where the item
  is, and generic functions not copied per layout for now.
- **M6.6 part 6's proposals**, D176 and D177: a number's conversions and Math's functions as
  instructions, and fields kept as their payload alone.
- **M6.6 part 7's proposal**, D180: `Rectangle` taking all four numbers.
- **The largest value type** (value-lists.html): decided "any, up to 64 KB"; offered on
  2026-10-18, not yet answered, is "any size, with a copy on read past 64 KB", the limit
  coming only from the 16-bit offset in a reference into a list item.
- **How JadeMem's real sampling is done** (M8): on sample data, or through a way for Astra
  to call Windows system functions, which this plan does not size.
- **Sendable classes for workers** (D181, its second step): an ordinary class saying it may be
  sent, checked for fields that can be, and copied as a graph. Open until a program needs it.

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
