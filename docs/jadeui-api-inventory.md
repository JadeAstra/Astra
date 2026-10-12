<!-- Machine-extracted from the JadeUI sources (jadelib-ui 0.6.0-SNAPSHOT) as a porting checklist.
     Signatures were pulled from the public declarations; bodies and KDoc were not read.
     Check a signature against the Kotlin source before relying on it. -->

JADELIB-UI PUBLIC API INVENTORY

## 0. Method, sources, gaps

- **API dump exists:** `C:\Users\sciss\IdeaProjects\JadeLib\jadelib-ui\api\jadelib-ui.api`, 5146 lines, about 330 class entries. Root package is lines 1–2543, sub-packages 2544–5146. It is JVM-descriptor format (no parameter names or defaults), so names and defaults below come from source.
- **Source size:** 75 .kt files, 32,900 lines (not ~29,500).
- **How extracted:** the module uses Kotlin explicit-API mode, so every public declaration carries `public`. I mechanically extracted every `public` declaration with its full parameter list from all 75 files. I cross-checked the dump for `JadeUi`, `JadeDialogs`, overloads, JDK types and some class supertypes.
- **Not read:** function bodies and KDoc, apart from `Block.kt`, `dialogs/JadeDialogs.kt` and `Chrome.kt` lines 84–298, which I read in full.
- **Not listed:** members that are public only through `override` (`sizes`/`paint`/`onEvent`/`owner`/`model` and other interface implementations). They follow from the implemented interfaces, which I list where I have them.
- **Notation:** `[ps]` = public getter, private setter. `[DEPRECATED]` = `@Deprecated`. "internal ctor" = public type an app cannot construct.
- **Non-public default constants used in public signatures:**
  - `DEFAULT_FONT` = Font(SANS_SERIF, PLAIN, 12) (internal, `Widgets.kt:13`)
  - `MENU_FONT` = SANS_SERIF 13; `FX_FONT` = SANS_SERIF 13; `SHEET_FONT` = SANS_SERIF 12
  - `CODE_FONT` / `HEX_FONT` / `TEXT_FONT` = MONOSPACED 13; `MB_FONT` = SERIF 20
- `Bordered` and `ScrollAware` are declared `internal interface` but appear as supertypes in the dump.

## 1. Root package files (`com.jadelib.ui`)

Already read by you, one line each:
- **Jade.kt** — `Sizes`, `Style`, enums `Fill`/`Anchor`/`Key`/`EditCommand`/`CursorKind`, `JadeEvent` (Click, KeyType, KeyPress, Drag, Release, Wheel, Shortcut, Context, Hover), capability interfaces (`JadeContent`, `FocusTarget`, `FocusHolder`, `FocusAware`, `TabAware`, `CursorProvider`, `TooltipProvider`, `OverlayContent`, `HeightForWidth`, `OwnedContent`, `BandAware`, `DropdownProvider`, `EdgeEscape`, `TypesInPlace`), object `Palette`, `elide(text, font, maxWidth)`.
- **JadeComponent.kt** — class `JadeComponent(content)`, factories `label`/`button`/`swing`/`textField`×2/`grid`, the `JadeComponent.*` extensions (layout, renderFrame, renderToImage×2, renderInto, record, dispatch, click, drag, componentAt, cursorAt, componentPathAt, focusables, takesKeys, descendants, find, windowPosition), `List<JadeComponent>.nextFocus`, `focusAfterPress`, `WheelTarget`, `DragTarget`, `CapturedTarget` (internal ctor).
- **Canvas.kt** — `TextMetrics`, interface `JadeCanvas`, sealed `DrawOp` (Rect, RoundRect, Oval, Line, Poly, Text, Image), `RecordingCanvas` (internal ctor), internal `AwtCanvas`.
- **Host.kt** — `JadeHost`, `JadeHostSpec`, `JadeHostProvider`, `jadeHost(...)`, internal `HostProviders`.
- **JadeGrid.kt** — `JadeGrid(insets, hgap, vgap)` with `add`/`remove`/`row`/`RowScope.cell`/`invalidateOverride`/`layout`/`childAt`, `Cell` (internal ctor).

### Block.kt (221)
```
class JadeBlock(source: String, var label: String, private val view: (String) -> JadeComponent,
    var onEdit: (String) -> Unit = {}, var onSelect: (String) -> Unit = {}, val chromeless: Boolean = false)
    : JadeContent, OwnedContent, HeightForWidth, EdgeEscape, TypesInPlace
  var onActivate: (String) -> Unit = {}
  var source: String [ps]
  val renderedContent: JadeContent
  val bodyTakesInput: Boolean
  fun setSource(newSource: String)
  fun refresh()
  fun keepSource(newSource: String)
  fun setError(message: String?)
  overrides: owner, heightForWidth(width), onEscape, enterFromEdge(fromTop), takesTyping, sizes(), paint(canvas), onEvent(event)
  internal: allChildren(), activeChild(), activeChildIsEditor(), editorModelForTest(), layout(width, height)
fun block(source: String, label: String, onEdit: (String) -> Unit = {}, view: (String) -> JadeComponent): JadeComponent
```
The factory's parameter order differs from the constructor (`view` is last).

### Chrome.kt (934)
```
class StyledText(val text: String, val runs: List<StyleRun> = emptyList(), val objects: List<String> = emptyList(),
    val data: Map<String, ByteArray> = emptyMap(), val picture: ByteArray? = null)
  fun plainText(): String
interface JadeClipboard { fun copy(content: StyledText); fun paste(): StyledText? }
class MemoryClipboard : JadeClipboard
sealed interface JadeMenuEntry
class JadeMenuItem(val label: String, val enabled: Boolean = true, val accelerator: String? = null,
    val enabledFn: (() -> Boolean)? = null, val onClick: () -> Unit) : JadeMenuEntry
  val isEnabled: Boolean
class JadeCheckItem(val label: String, val checked: Boolean, val enabled: Boolean = true,
    val accelerator: String? = null, val onToggle: (Boolean) -> Unit) : JadeMenuEntry
object JadeSeparator : JadeMenuEntry
class JadeMenu(val label: String, val entries: List<JadeMenuEntry>) : JadeMenuEntry
class JadeMenuBar(val menus: List<JadeMenu>)
interface ContextMenuProvider { fun contextMenu(): List<JadeMenuItem> }
class FileFilter(val description: String, val extensions: List<String>)

interface JadeFileDialog
  var startDir: String?                       (default: get null, set no-op)
  [DEPRECATED] fun open(title: String = "Open", filters: List<FileFilter> = emptyList()): String?
  [DEPRECATED] fun save(title: String = "Save", suggestedName: String = "", filters: List<FileFilter> = emptyList()): String?
  [DEPRECATED] fun chooseDirectory(title: String = "Choose Folder"): String?
  fun open(title: String = "Open", filters: List<FileFilter> = emptyList(), onResult: (String?) -> Unit)
  fun save(title: String = "Save", suggestedName: String = "", filters: List<FileFilter> = emptyList(), onResult: (String?) -> Unit)
  fun chooseDirectory(title: String = "Choose Folder", onResult: (String?) -> Unit)
class NoFileDialog : JadeFileDialog

interface JadeInputDialog
  [DEPRECATED] fun prompt(title: String, message: String = "", initial: String = ""): String?
  [DEPRECATED] fun promptMultiline(title: String, initial: String = ""): String?
  fun prompt(spec: PromptSpec, onResult: (String?) -> Unit): JadeDialogHandle<PromptSpec>
  fun promptValueAndFormat(spec: ValueFormatSpec, onResult: (Pair<String, String>?) -> Unit): JadeDialogHandle<ValueFormatSpec>
  [DEPRECATED] fun promptMultilineLive(title: String, initial: String = "", onChange: (String) -> Unit,
      staticizeLabel: String = "Make static", staticize: ((String) -> String?)? = null, links: TextLinks? = null): String?
  [DEPRECATED] fun promptValueAndFormat(title: String, value: String, format: String,
      onChange: (value: String, format: String) -> Unit, links: TextLinks? = null, valueNote: String = "",
      formatNote: String = "", formatNames: List<String> = emptyList(),
      valueCompleter: com.jadelib.ui.sheet.Completer? = null): Pair<String, String>?
class NoInputDialog : JadeInputDialog

interface JadeChoiceDialog
  [DEPRECATED] fun choose(title: String, message: String, options: List<String>): Int?
  [DEPRECATED] fun pick(title: String, message: String, options: List<String>, initial: Int = 0): Int?
  [DEPRECATED] fun pickNamed(title: String, message: String, options: List<String>, initial: Int = 0,
      nameLabel: String = "Name:", defaultName: String = ""): Pair<Int, String>?
  fun choose(spec: ChoiceSpec, onResult: (Int?) -> Unit): JadeDialogHandle<ChoiceSpec>
  fun pick(spec: PickSpec, onResult: (PickResult?) -> Unit): JadeDialogHandle<PickSpec>
class NoChoiceDialog : JadeChoiceDialog

interface JadeChecklistDialog
  [DEPRECATED] fun check(title: String, message: String, items: List<String>, checked: Set<String>): Set<String>?
  fun check(spec: ChecklistSpec, onResult: (Set<String>?) -> Unit): JadeDialogHandle<ChecklistSpec>
class NoChecklistDialog : JadeChecklistDialog

class FindSpec(val showReplace: Boolean = false, val initialQuery: String = "",
    val onFindNext: (query: String) -> Unit = {}, val onReplace: (query: String, with: String) -> Unit = { _, _ -> },
    val onReplaceAll: (query: String, with: String) -> Unit = { _, _ -> }, val onClosed: () -> Unit = {},
    val options: List<FindOption> = emptyList())
  var draftKey: String? = null
  val key: String                             (draftKey ?: "find")
class FindOption(val label: String, var checked: Boolean = false, val replaceOnly: Boolean = false)
interface JadeFindDialog
  [DEPRECATED] fun open(spec: FindSpec)
  fun close()
  fun show(spec: FindSpec): JadeDialogHandle<FindSpec>
class NoFindDialog : JadeFindDialog

class SourceSpec(val id: String, val title: String, val initial: String, val onChange: (String) -> Unit,
    val onClose: () -> Unit = {}, val links: TextLinks? = null)
  var draftKey: String? = null
  val key: String                             (draftKey ?: "source:$id")
interface JadeSourceWindow
  [DEPRECATED] fun open(spec: SourceSpec)
  fun syncIfShowing(id: String, text: String)
  fun close()
  val showing: String?
  fun show(spec: SourceSpec): JadeDialogHandle<SourceSpec>
class NoSourceWindow : JadeSourceWindow

class FormField(val key: String, val label: String, val pickable: Boolean = false)
class FormKind(val id: String, val label: String, val fields: List<FormField>)
class FormSpec(val title: String, val kinds: List<FormKind>, val selected: String, val initial: Map<String, String>,
    val onChange: (String, Map<String, String>) -> Unit = { _, _ -> })
  var draftKey: String? = null
  val key: String                             (draftKey ?: "form:$title")
class FormResult(val kind: String, val values: Map<String, String>, val pick: String? = null)
interface JadeFormDialog
  [DEPRECATED] fun edit(spec: FormSpec): FormResult?
  fun edit(spec: FormSpec, onResult: (FormResult?) -> Unit): JadeDialogHandle<FormSpec>
class NoFormDialog : JadeFormDialog

sealed class OptionControl(val key: String, val label: String)
class ChoiceOption(key: String, label: String, val choices: List<String>, val selected: Int) : OptionControl
class CheckOption(key: String, label: String, val checked: Boolean) : OptionControl
class OptionsSpec(val title: String, val controls: List<OptionControl>, val actions: List<String> = emptyList(),
    val message: String? = null)
  var draftKey: String? = null
  val key: String                             (draftKey ?: "options:$title")
class OptionsResult(val values: Map<String, String>, val action: String? = null)
interface JadeOptionsDialog
  [DEPRECATED] fun show(spec: OptionsSpec): OptionsResult?
  fun show(spec: OptionsSpec, onResult: (OptionsResult?) -> Unit): JadeDialogHandle<OptionsSpec>
class NoOptionsDialog : JadeOptionsDialog

enum class ImageSource { FILE, SVG_CODE }
class ImageChoice(val source: ImageSource, val value: String)
class ImageSpec(val renderPreview: (ImageSource, String) -> java.awt.image.BufferedImage?,
    val title: String = "Insert Image", val source: ImageSource = ImageSource.FILE, val value: String = "",
    val draftKey: String? = null)
  val key: String                             (draftKey ?: "image:$title")
interface JadeImageDialog
  [DEPRECATED] fun choose(renderPreview: (ImageSource, String) -> java.awt.image.BufferedImage?): ImageChoice?
  fun choose(spec: ImageSpec, onResult: (ImageChoice?) -> Unit): JadeDialogHandle<ImageSpec>
class NoImageDialog : JadeImageDialog

class WindowSpec(val title: String, val content: JadeComponent, val width: Int = 800, val height: Int = 600,
    val focus: JadeComponent? = null, val onClosed: () -> Unit = {})
  var dialog: Boolean = false
  var sizeToContent: Boolean = false
  var centerOnOwner: Boolean = false
  var resizable: Boolean = true
  var modal: Boolean = false                  [internal set]
  var keyFilter: ((key: JadeEvent.KeyPress, focused: JadeComponent?) -> Boolean)? = null
interface JadeWindow { var title: String; fun toFront(); fun close(); fun fitToContent() {} }
interface JadeWindowOpener
  fun open(spec: WindowSpec): JadeWindow?
  fun blockUntil(begin: (finish: () -> Unit) -> Unit): Boolean = false
class NoWindowOpener : JadeWindowOpener

interface JadeDialogHandle<S>
  val key: String
  val title: String
  val open: Boolean
  fun toFront()
  fun close()
  fun update(spec: S)
  fun detach(reason: String, retargetLabel: String? = null, retarget: (() -> Unit)? = null)
internal class FinishedDialog<S>(key, title) : JadeDialogHandle<S>     (returned by the default async implementations)

class PromptSpec(val title: String, val message: String = "", val initial: String = "", val multiline: Boolean = false,
    val onChange: ((String) -> Unit)? = null, val staticizeLabel: String = "Make static",
    val staticize: ((String) -> String?)? = null, val links: TextLinks? = null, val draftKey: String? = null,
    val okLabel: String = "OK")
  val key: String                             (draftKey ?: "prompt:$title")
class ValueFormatSpec(val title: String, val value: String, val format: String,
    val onChange: (value: String, format: String) -> Unit = { _, _ -> }, val links: TextLinks? = null,
    val valueNote: String = "", val formatNote: String = "", val formatNames: List<String> = emptyList(),
    val valueCompleter: com.jadelib.ui.sheet.Completer? = null, val draftKey: String? = null)
  val key: String                             (draftKey ?: "value-format:$title")
class ChoiceSpec(val title: String, val message: String, val options: List<String>, val defaultOption: Int? = null,
    val draftKey: String? = null)
  val key: String                             (draftKey ?: "choice:$title")
class PickSpec(val title: String, val message: String, val options: List<String>, val initial: Int = 0,
    val nameLabel: String? = null, val defaultName: String = "", val draftKey: String? = null)
  val key: String                             (draftKey ?: "pick:$title")
class PickResult(val index: Int, val name: String)
class ChecklistSpec(val title: String, val message: String, val items: List<String>, val checked: Set<String>,
    val draftKey: String? = null)
  val key: String                             (draftKey ?: "checklist:$title")
```

### Collections.kt (452)
```
class TreeNode(val label: String, val children: List<TreeNode> = emptyList())
class JadeTree(val roots: List<TreeNode>, var onSelect: (TreeNode) -> Unit = {}, var font: Font = DEFAULT_FONT)
    : JadeContent, Bordered, ScrollAware, OwnedContent
  var selected: TreeNode? [ps]
  fun expandAll()
  fun collapseAll()
class JadeTable(columns: List<String>, rows: List<List<String>>, selected: Int = -1, var editable: Boolean = false,
    var onSelect: (Int) -> Unit = {}, var onEdit: (row: Int, col: Int, value: String) -> Unit = { _, _, _ -> },
    var onActivate: (Int) -> Unit = {}, var font: Font = DEFAULT_FONT)
    : JadeContent, Bordered, ScrollAware, OwnedContent
  var columns: List<String>
  val rows: MutableList<MutableList<String>>
  var selected: Int
  var maxColumnWidth: Int = 260
  fun setRows(newRows: List<List<String>>)
fun tree(roots: List<TreeNode>, onSelect: (TreeNode) -> Unit = {}): JadeComponent
fun table(columns: List<String>, rows: List<List<String>>, editable: Boolean = false,
    onEdit: (row: Int, col: Int, value: String) -> Unit = { _, _, _ -> }, onSelect: (Int) -> Unit = {}): JadeComponent
```
`table()` orders its parameters differently from the `JadeTable` constructor.

### Combo.kt (151)
```
class JadeComboBox(items: List<String>, selected: Int = 0, var editable: Boolean = false,
    var onSelect: (Int) -> Unit = {}, font: Font = DEFAULT_FONT)
    : JadeContent, DropdownProvider, TextEditable, OwnedContent
  var items: List<String>
  var font: Font
  var selected: Int [ps]
  fun text(): String
  fun show(index: Int)
fun comboBox(items: List<String>, selected: Int = 0, editable: Boolean = false, onSelect: (Int) -> Unit = {}): JadeComponent
```

### Commands.kt (188)
No public API. `internal object Commands`, `internal object OvrExporter`.

### Controls.kt (532)
```
class JadeCheckBox(label: String, checked: Boolean = false, var onChange: (Boolean) -> Unit = {}, font: Font = DEFAULT_FONT)
    : JadeContent, OwnedContent
  var label: String; var font: Font; var checked: Boolean
class RadioGroup(selected: Int = -1)
  var selected: Int [ps]
  fun onChange(listener: (Int) -> Unit)
  fun select(value: Int)
class JadeRadioButton(label: String, val value: Int, val group: RadioGroup, font: Font = DEFAULT_FONT)
    : JadeContent, OwnedContent
  var label: String; var font: Font
class JadeSlider(value: Double = 0.0, var onChange: (Double) -> Unit = {}) : JadeContent
  var value: Double                           (coerced 0..1)
class JadeProgressBar(value: Double = 0.0) : JadeContent
  var value: Double
class JadeList(items: List<String>, selected: Int = -1, var onSelect: (Int) -> Unit = {}, var font: Font = DEFAULT_FONT)
    : JadeContent, Bordered, ScrollAware, OwnedContent, ContextMenuProvider
  var contextMenuFor: ((Int) -> List<JadeMenuItem>)? = null
  var items: List<String>
  var selected: Int
  var muted: (Int) -> Boolean = { false }
  var header: String? = null
  var onReorder: ((Int, Int) -> Unit)? = null
  var onActivate: ((Int) -> Unit)? = null
class JadeToggle(checked: Boolean = false, var onChange: (Boolean) -> Unit = {}) : JadeContent
  var checked: Boolean
interface HoldRepeat { fun onHold(nowNanos: Long); fun onReleaseHold() }
class JadeSpinner(value: Int = 0, min: Int = Int.MIN_VALUE, max: Int = Int.MAX_VALUE, var step: Int = 1,
    var onChange: (Int) -> Unit = {}, font: Font = DEFAULT_FONT)
    : JadeContent, HoldRepeat, TextEditable, OwnedContent
  var min: Int; var max: Int; var font: Font
  var value: Int [ps]
fun checkBox(label: String, checked: Boolean = false, onChange: (Boolean) -> Unit = {}): JadeComponent
fun radioButton(label: String, value: Int, group: RadioGroup): JadeComponent
fun slider(value: Double = 0.0, onChange: (Double) -> Unit = {}): JadeComponent
fun progressBar(value: Double = 0.0): JadeComponent
fun listBox(items: List<String>, selected: Int = -1, onSelect: (Int) -> Unit = {}): JadeComponent
fun toggle(checked: Boolean = false, onChange: (Boolean) -> Unit = {}): JadeComponent
fun spinner(value: Int = 0, min: Int = 0, max: Int = 100, step: Int = 1, onChange: (Int) -> Unit = {}): JadeComponent
```
`spinner()` defaults `min`/`max` to 0/100; the constructor defaults them to `Int.MIN_VALUE`/`Int.MAX_VALUE`.

### Doc.kt (95)
```
class JadeDoc(blocks: List<JadeComponent>) : JadeContent, OwnedContent
  val blocks: List<JadeComponent>
  fun add(block: JadeComponent, index: Int = blockList.size)
  fun removeAt(index: Int)
  fun setBlocks(newBlocks: List<JadeComponent>)
  internal fun layout(width: Int, height: Int)
fun doc(blocks: List<JadeComponent>): JadeComponent
```

### Drafts.kt (64)
```
class JadeDrafts()
  operator fun get(key: String): Map<String, String>?
  fun put(key: String, fields: Map<String, String>)
  fun remove(key: String)
  val keys: Set<String>
  fun exportAll(): Map<String, Map<String, String>>
  fun importAll(saved: Map<String, Map<String, String>>)
  fun clear()
  fun onChange(listener: () -> Unit)
  fun removeOnChange(listener: () -> Unit)
```

### HScrollBar.kt (130)
No public API. `internal class HScrollBar(private val host: Host)`.

### Layers.kt (99)
```
fun JadeComponent.renderBaseInto(target: BufferedImage, width: Int, height: Int, scaleX: Double, scaleY: Double,
    doLayout: Boolean = true, clip: Rectangle? = null)
fun JadeComponent.renderOverlayInto(target: BufferedImage, width: Int, height: Int, scaleX: Double, scaleY: Double,
    clip: Rectangle? = null): Boolean
fun JadeComponent.hasOverlay(): Boolean
class GlViewPlacement(val view: JadeGlView, val component: JadeComponent, val bounds: Rectangle)
fun JadeComponent.glViews(): List<GlViewPlacement>
class FrameTime(val time: Double, val delta: Double, val frame: Long)
internal object RenderPass
```

### Overrides.kt (500)
Holds `object JadeUi` (section 3). Internal only: `ResolvedLayout`, `Spacer`, `OverrideLoader`.

### RawInput.kt (370)
```
sealed interface RawInput
  data class PointerDown(val x: Int, val y: Int, val button: PointerButton, val mods: Modifiers, val clicks: Int = 1)
  data class PointerUp(val x: Int, val y: Int, val button: PointerButton, val mods: Modifiers)
  data class PointerMove(val x: Int, val y: Int, val dx: Double, val dy: Double, val buttons: Set<PointerButton>, val mods: Modifiers)
  data class Wheel(val x: Int, val y: Int, val dx: Double, val dy: Double, val mods: Modifiers)
  data class KeyDown(val key: KeyCode, val mods: Modifiers, val repeat: Boolean = false)
  data class KeyUp(val key: KeyCode, val mods: Modifiers)
  data class Text(val text: String)
  data class Hover(val inside: Boolean)
  data class Focus(val focused: Boolean)
enum class PointerButton { LEFT, RIGHT, MIDDLE, BACK, FORWARD, OTHER }
data class Modifiers(val shift: Boolean = false, val ctrl: Boolean = false, val alt: Boolean = false,
    val meta: Boolean = false, val shortcut: Boolean = ctrl)
  companion object { val NONE: Modifiers }
enum class KeyCode {
  A..Z, DIGIT_0..DIGIT_9, F1..F25,
  SPACE, APOSTROPHE, COMMA, MINUS, PERIOD, SLASH, SEMICOLON, EQUAL, LEFT_BRACKET, BACKSLASH, RIGHT_BRACKET, GRAVE_ACCENT,
  ESCAPE, ENTER, TAB, BACKSPACE, INSERT, DELETE, RIGHT, LEFT, DOWN, UP, PAGE_UP, PAGE_DOWN, HOME, END,
  CAPS_LOCK, SCROLL_LOCK, NUM_LOCK, PRINT_SCREEN, PAUSE, MENU,
  NUMPAD_0..NUMPAD_9, NUMPAD_DECIMAL, NUMPAD_DIVIDE, NUMPAD_MULTIPLY, NUMPAD_SUBTRACT, NUMPAD_ADD, NUMPAD_ENTER, NUMPAD_EQUAL,
  SHIFT_LEFT, CONTROL_LEFT, ALT_LEFT, META_LEFT, SHIFT_RIGHT, CONTROL_RIGHT, ALT_RIGHT, META_RIGHT, UNKNOWN }
enum class CursorMode { NORMAL, HIDDEN, LOCKED }
class KeyState()
  fun isDown(key: KeyCode): Boolean
  val down: Set<KeyCode>
  internal: press, release, drain
interface RawInputTarget
  fun onRawInput(event: RawInput): Boolean
  val keys: KeyState
  val cursorMode: CursorMode                  (default NORMAL)
  val takesKeyboard: Boolean                  (default true)
  val capturesTab: Boolean                    (default false)
  fun releaseCursor() {}
class RawInputRouter()
  val keyboardTarget: JadeComponent?
  val pointerCaptured: Boolean
  val cursorMode: CursorMode
  fun pointerDown(tree: JadeComponent, x: Int, y: Int, button: PointerButton, mods: Modifiers, clicks: Int = 1): Boolean
  fun pointerUp(x: Int, y: Int, button: PointerButton, mods: Modifiers): Boolean
  fun pointerMove(tree: JadeComponent, x: Double, y: Double, mods: Modifiers): Boolean
  fun pointerExited()
  fun wheel(tree: JadeComponent, x: Int, y: Int, dx: Double, dy: Double, mods: Modifiers): Boolean
  fun keyDown(key: KeyCode, mods: Modifiers, repeat: Boolean = false): Boolean
  fun keyUp(key: KeyCode, mods: Modifiers): Boolean
  fun text(text: String): Boolean
  fun focus(comp: JadeComponent): Boolean
  fun pointerDelta(dx: Double, dy: Double, mods: Modifiers): Boolean
  fun windowFocusLost()
  fun releaseKeyboard()
  fun keyboardBounds(): Rectangle?
  fun cursorBounds(): Rectangle?
  fun pointerCursor(tree: JadeComponent, x: Int, y: Int): CursorKind
```

### RenderLoop.kt (101)
```
class RenderLoop()
  val isDirty: Boolean
  fun requestLayout()
  fun markDirty()
  fun markDamage(r: Rectangle)
  fun layoutDue(w: Int, h: Int): Boolean
  fun ensureLayout(root: JadeComponent, w: Int, h: Int)
  class Frame internal constructor(val layout: Boolean, val partial: Rectangle?)
  fun plan(w: Int, h: Int, buffersResized: Boolean): Frame
  fun drawn(frame: Frame, w: Int, h: Int)
  fun mayIdle(holding: Boolean): Boolean
  fun renderFrame(root: JadeComponent, buf: BufferedImage, w: Int, h: Int, sx: Double = 1.0, sy: Double = 1.0): Frame
```

### Screen.kt (476)
```
class JadeMenuBarView(var bar: JadeMenuBar, var font: Font = MENU_FONT) : JadeContent
  internal: height(), titleIndexAt(x), titleLeftOf(i), titleWidthOf(i)
class JadeScreen(val content: JadeComponent, menuBar: JadeMenuBarView? = null) : JadeContent, OwnedContent
  fun setMenuBar(bar: JadeMenuBar)
  fun menuTitleBounds(label: String): Rectangle?
  fun popupItemBounds(label: String): Rectangle?
  fun popupContains(x: Int, y: Int): Boolean
  fun topPopupIsContextMenu(): Boolean
  fun openPopup(comp: JadeComponent, x: Int, y: Int)
  fun openContextMenu(items: List<JadeMenuItem>, x: Int, y: Int)
  fun openDropdownMenu(items: List<String>, selected: Int, x: Int, y: Int, minWidth: Int = 0, onChoose: (Int) -> Unit)
  fun hasPopup(): Boolean
  fun showTip(comp: JadeComponent, x: Int, y: Int)
  fun clearTip()
  fun menuBarHeight(): Int
  fun serveOverlays()
  fun closeAll()
  fun triggerAccelerator(chord: String): Boolean
  internal: class Popup, layoutScreen(w, h), hasOverlayLayer(), paintOverlayLayer(canvas)
fun screen(content: JadeComponent, menuBar: JadeMenuBar? = null): JadeComponent
internal: class JadeMenuList, fun normalizeChord(chord: String): String
```
The factory takes a `JadeMenuBar`; the constructor takes a `JadeMenuBarView`.

### Scroll.kt (258)
```
class JadeScroll(val child: JadeComponent) : JadeContent, OwnedContent
  var scrollY: Int [ps]
  val viewportHeight: Int
  fun scrollTo(y: Int)
  fun reveal(top: Int, bottom: Int, block: String = "nearest")
  internal fun layoutViewport(viewportW: Int, viewportH: Int, consumeReveal: Boolean = true): Int
fun scroll(child: JadeComponent): JadeComponent
internal interface ScrollAware
interface Pinnable { fun pinTopOffset(px: Int) }
```

### Split.kt (159)
```
class JadeSplit(val first: JadeComponent, val second: JadeComponent, vertical: Boolean = false, ratio: Double = 0.5)
    : JadeContent, CursorProvider, OwnedContent
  var vertical: Boolean
  var ratio: Double [ps]
  internal fun layout(width, height)
fun split(first: JadeComponent, second: JadeComponent, vertical: Boolean = false, ratio: Double = 0.5): JadeComponent
```

### Tabs.kt (246)
```
class JadeTabs(tabs: List<Pair<String, JadeComponent>>, font: Font = DEFAULT_FONT)
    : JadeContent, OwnedContent, TooltipProvider
  var font: Font
  class Tab internal constructor(val title: String, val content: JadeComponent)
  val tabs: List<Tab>
  fun add(title: String, content: JadeComponent): Int
  fun replace(index: Int, content: JadeComponent)
  fun rename(index: Int, title: String)
  fun remove(index: Int)
  var active: Int = 0
  var showBar: Boolean = true
  var dirty: (Int) -> Boolean = { false }
  var onClose: ((Int) -> Unit)? = null
  var onSelect: ((Int) -> Unit)? = null
  fun tabAt(x: Int, y: Int): Int
  var tabTooltip: (Int) -> String?
  internal fun layout(width, height)
fun tabs(vararg tabs: Pair<String, JadeComponent>): JadeComponent
```

### Text.kt (817)
```
interface TextEditable { val model: SharedTextModel }
data class StyleRun(val start: Int, val end: Int, val color: Color)
class TextLink(val range: IntRange, val card: JadeComponent?, val open: () -> Unit)
typealias TextLinks = (text: String, offset: Int) -> TextLink?
class SharedTextModel(text: String = "")
  var caret: Int [ps]
  var anchor: Int [ps]
  val text: String; val length: Int; val selectionStart: Int; val selectionEnd: Int; val hasSelection: Boolean
  fun addListener(listener: () -> Unit)
  fun removeListener(listener: () -> Unit)
  fun setText(value: String)
  fun setText(value: String, objects: List<String>)
  var objectTags: List<String> [ps]
  fun objectAt(index: Int): String?
  fun insertObject(tag: String)
  fun moveCaret(index: Int, extendSelection: Boolean = false)
  fun moveCaretWord(forward: Boolean, extendSelection: Boolean = false)
  fun select(start: Int, end: Int)
  fun selectAll()
  fun selectedText(): String
  fun styledSelection(): StyledText
  fun objectsIn(from: Int, to: Int): List<String>
  fun insertRich(s0: String, runs: List<StyleRun>, objects: List<String>)
  fun insertStyled(s0: String, runs: List<StyleRun>)
  val lineCount: Int
  fun lineOf(index: Int): Int
  fun lineStart(index: Int): Int
  fun lineEnd(index: Int): Int
  fun columnOf(index: Int): Int
  fun offsetAt(line: Int, column: Int): Int
  fun wordBoundsAt(index: Int): Pair<Int, Int>
  fun selectWordAt(index: Int)
  var caretVisible: Boolean [ps]
  var focused: Boolean [ps]
  fun toggleCaret()
  fun showCaret()
  fun setCaretVisible(value: Boolean)
  fun setFocused(value: Boolean)
  var styleRuns: List<StyleRun> [ps]
  fun setStyleRuns(runs: List<StyleRun>)
  fun insert(s0: String)
  fun deleteBackward()
  fun deleteForward()
  fun compoundEdit(block: () -> Unit)
  val canUndo: Boolean; val canRedo: Boolean
  fun undo()
  fun redo()
class JadeTextField(override val model: SharedTextModel = SharedTextModel(), font: Font = DEFAULT_FONT, columns: Int = 12)
    : JadeContent, ContextMenuProvider, TextEditable, CursorProvider, OverlayContent, OwnedContent
  var font: Font; var columns: Int
  var inlineObjects: com.jadelib.ui.doc.InlineObjects? = null
  var onHover: ((offset: Int, x: Int, y: Int) -> Unit)? = null
  var links: TextLinks? = null
  var onLinkHover: ((link: TextLink?, x: Int, y: Int) -> Unit)? = null
  var hoveredLink: TextLink? [ps]
  var onCommit: (String) -> Unit = {}
  var onRevert: (String) -> Unit = {}
  var persistentHighlight: Boolean = false
  fun markCommitted()
  fun commit()
  fun revert()
internal fun colorAt(runs, index, textLen, default): Color
```

### TextArea.kt (887)
```
class JadeTextArea(override val model: SharedTextModel = SharedTextModel(), font: Font = DEFAULT_FONT, rows: Int = 6, columns: Int = 30)
    : JadeContent, ScrollAware, ContextMenuProvider, TextEditable, Bordered, CursorProvider, OverlayContent,
      EdgeEscape, OwnedContent, TabAware, HeightForWidth, WheelTarget
  var font: Font; var rows: Int; var columns: Int
  var wrap: Boolean = false
  var onHover: ((offset: Int, x: Int, y: Int) -> Unit)? = null
  var links: TextLinks? = null
  var onLinkHover: ((link: TextLink?, x: Int, y: Int) -> Unit)? = null
  var hoveredLink: TextLink? [ps]
  var onCaretClick: (() -> Unit)? = null
  var squiggles: List<IntRange> = emptyList()
  var cornerLabel: String = ""
  var themed: Boolean = true
  var backgroundOverride: Color? = null
  var foregroundOverride: Color? = null
  var statusText: String = ""
  var statusIsError: Boolean = false
  var currentLine: Int = -1
  var readOnly: Boolean = false
  var onKeyIntercept: ((JadeEvent.KeyPress) -> Boolean)? = null
  var onComplete: (() -> Unit)? = null
  fun caretRect(): IntArray?
  internal fun statusH(): Int
fun textArea(text: String = ""): JadeComponent
fun textArea(model: SharedTextModel): JadeComponent
```

### TextNav.kt (276)
```
object TextNav
  fun isWordChar(c: Char): Boolean
  fun nextWordBoundary(text: CharSequence, from: Int): Int
  fun prevWordBoundary(text: CharSequence, from: Int): Int
  fun wordBoundsAt(text: CharSequence, index: Int): Pair<Int, Int>
  fun lineStart(text: CharSequence, index: Int): Int
  fun lineEnd(text: CharSequence, index: Int): Int
  fun lineOf(text: CharSequence, index: Int): Int
  fun lineCount(text: CharSequence): Int
  fun columnOf(text: CharSequence, index: Int): Int
  fun offsetAt(text: CharSequence, line: Int, column: Int): Int
class DragSelector(private val text: () -> CharSequence, private val lineBoundsAt: (Int) -> IntRange)
  enum class Granularity { CHAR, WORD, LINE }
  val granularity: Granularity
  fun press(offset: Int, clicks: Int, shift: Boolean): IntRange?
  fun drag(offset: Int): IntRange
interface CaretGeometry
  fun rowStart(offset: Int): Int
  fun rowEnd(offset: Int): Int
  fun rowNeighbour(offset: Int, delta: Int, sticky: Int): Int
  fun stickyFor(offset: Int): Int
  fun pageRows(): Int = 1
class CaretKeys(private val text: () -> CharSequence, private val geometry: CaretGeometry, private val caret: () -> Int,
    private val moveTo: (offset: Int, extendSelection: Boolean) -> Unit)
  fun clearSticky()
  fun handle(key: Key, shift: Boolean, ctrl: Boolean): Boolean
```

### Widgets.kt (340)
```
class JadeLabel(text: String, font: Font = DEFAULT_FONT) : JadeContent, OwnedContent
  var text: String; var font: Font
class JadeWrappedLabel(text: String, font: Font = DEFAULT_FONT) : JadeContent, OwnedContent, HeightForWidth
  var text: String; var font: Font
  var preferredWidth: Int = 420
  internal fun wrap(width: Int): List<String>
fun wrappedLabel(text: String, font: Font = DEFAULT_FONT): JadeComponent
class JadeButton(text: String, var onClick: () -> Unit = {}, font: Font = DEFAULT_FONT) : JadeContent, OwnedContent
  var text: String; var font: Font
class JadeIconButton(image: java.awt.image.BufferedImage, var onClick: () -> Unit = {}, size: Int = 30) : JadeContent, OwnedContent
  var image: BufferedImage; var size: Int
  var enabled: Boolean = true
fun iconButton(image: java.awt.image.BufferedImage, onClick: () -> Unit = {}): JadeComponent
class JadeToolbarSeparator(private val color: Color? = null, private val height: Int = 20) : JadeContent
fun toolbarSeparator(): JadeComponent
class SwingContent(val swing: JComponent) : JadeContent
internal val DEFAULT_FONT: Font
```

## 2. Sub-packages

Root also depends back on four of these, so the dependency graph has cycles:
- `Chrome.kt` uses `doc.ProseDoc` (the `OBJECT` char) and `sheet.Completer`.
- `Text.kt` uses `doc.InlineObjects`, `doc.InlineObjectBoxes`, `doc.ProseDoc`.
- `JadeComponent.kt` uses `doc.JadeDocView` and `doc.JadeProseView`.
- `RawInput.kt` and `Layers.kt` use `gl.JadeGlView`.
- `Overrides.kt` (`JadeUi`) uses `dialogs.JadeDialogs`.

### dialogs (1401 lines; Choices 203, DialogSession 382, Forms 312, JadeDialogs 113, Panels 171, Prompts 220)
The Jade-drawn default implementations of every `JadeUi` dialog slot, each a modeless window opened through `JadeUi.windowOpener`. Depends on root (heavily) and `sheet` (Completer, CompletionPopup, FormatCompletion).

The only public declaration in the package:
```
object JadeDialogs
  val input: JadeInputDialog
  val choice: JadeChoiceDialog
  val checklist: JadeChecklistDialog
  val form: JadeFormDialog
  val options: JadeOptionsDialog
  val image: JadeImageDialog
  val find: JadeFindDialog
  val source: JadeSourceWindow
  fun install()                               (puts all eight back into their JadeUi slots)
  internal: registry, unkeyed, openDialogs(), closeAll()
```
- Everything else is internal: `PromptDialogs`, `ChoiceDialogs`, `ChecklistDialogs`, `FormDialogs`, `OptionsDialogs`, `ImageDialogs`, `FindPanel`, `SourcePanel`, `DialogSession<*, *>`, `blockingAnswer<R>(call, replacement, begin)`, `BlockingWarning`.
- Apps reach the dialogs through `JadeUi.inputDialog` etc. and the interfaces and Spec classes in `Chrome.kt`.
- `JadeUi.openDialogs()` and `JadeUi.closeAllDialogs()` are the public route to the registry.

### chart (774; ChartRenderer 622, ChartView 152)
Bar, line, area, scatter and pie charts drawn on Graphics2D, plus a view with hit-testing. Depends on root, java.awt (Graphics2D, geom).
```
enum class ChartType { BAR, LINE, AREA, SCATTER, PIE }
class ChartSeries(val values: List<Double>, val name: String = "", val color: Color? = null)
class ChartData(val type: ChartType, val series: List<ChartSeries>, val labels: List<String> = emptyList(), val title: String = "",
    val valueFormat: ((Double) -> String)? = null, val colorByCategory: Boolean = false, val categoryColors: List<Color?> = emptyList())
data class ChartPoint(val series: Int, val point: Int)
class ChartHit(val series: Int, val point: Int, val label: String, val value: Double, val color: Color, val shape: Shape)
  val id: ChartPoint
class ChartLayout(val hits: List<ChartHit>)
  fun at(x: Int, y: Int): ChartHit?
  companion { val EMPTY }
object ChartRenderer
  val PALETTE: List<Color>
  fun render(data: ChartData, g: Graphics2D, w: Int, h: Int, highlight: ChartPoint? = null): ChartLayout
  fun shade(base: Color, cycle: Int): Color
class JadeChartView(data: ChartData) : JadeContent, OwnedContent, TooltipProvider, CursorProvider
  var data [ps]
  var onSelect: ((ChartHit) -> Unit)?
  var canSelect: (ChartHit) -> Boolean
  var tooltipFor: (ChartHit) -> String
  var hovered: ChartPoint? [ps]
  fun setData(newData: ChartData)
  fun layout(): ChartLayout
fun chartView(data: ChartData): JadeComponent
fun barChart(values: List<Double>, labels: List<String> = emptyList(), title: String = "", colorByCategory: Boolean = false): JadeComponent
fun lineChart(vararg series: ChartSeries, labels: List<String> = emptyList(), title: String = ""): JadeComponent
fun pieChart(values: List<Double>, labels: List<String> = emptyList(), title: String = ""): JadeComponent
```

### code (401; CodeHighlighter 309, JadeCodeView 92)
Tokenising syntax highlighter with user-definable languages, plus a read-only code view. Depends on root, java.awt, java.io.File.
```
object CodeHighlighter
  enum class Role { KEYWORD, STRING, COMMENT, NUMBER, DEFAULT }
  data class Token(val start: Int, val end: Int, val role: Role)
  data class Lang(val keywords: Set<String>, val lineComment: String? = null, val blockComment: Pair<String, String>? = null,
      val tripleStrings: Boolean = false, val quotes: String = "\"'", val wordChars: String = "_",
      val properties: Boolean = false, val hexColors: Boolean = false)
  data class LangSpec(val name: String, val aliases: List<String>, val lang: Lang)
  fun languages(): List<String>
  fun register(spec: LangSpec)
  fun clearCustom()
  fun userLangDir(): File
  fun ensureUserLangDir(): File
  fun ensureLangDir(dir: File): File
  fun loadLangDir(dir: File): Int
  fun parseLangFile(text: String): LangSpec?
  fun cssClass(role: Role): String?
  fun toHtml(text: String, lang: String?, escape: (String) -> String): String
  fun tokenize(text: String, lang: String?): List<Token>
fun codeColor(role: CodeHighlighter.Role, dark: Boolean): Color?
class JadeCodeView(source: String, lang: String? = null, font: Font = CODE_FONT)       (not a JadeContent; wraps a text area)
  val area: JadeTextArea
  var lang: String?
  var dark: Boolean = false
fun codeView(source: String, lang: String? = null): JadeComponent
```

### css (1190; Css 568, CssCounters 137, CssValues 367, DocTheme 118)
CSS parser with selector matching and cascade, value parsers, counters, and document theme extraction. Depends on `html` (Html) and java.awt.Color; no dependency on root.
```
object Css
  class Elem(val tag: String, val id: String? = null, val classes: Set<String> = emptySet(), val parent: Elem? = null,
      val target: Boolean = false, val index: Int = 0, val siblings: Int = 0, val attrs: Map<String, String> = emptyMap(),
      val prev: Elem? = null)
    fun attr(name: String): String?
  class StyleSheet(val rules: List<Rule>, val rootVars: Map<String, String>, val widthConditions: List<WidthCondition> = emptyList())
    fun sameMediaAt(a: Int?, b: Int?): Boolean
    fun computed(elem: Elem, inherited: Map<String, String> = emptyMap(), hovered: Boolean = false, pseudo: String? = null): Map<String, String>
    val hasPseudo: Boolean
    fun resolveVars(value: String): String
  class Rule(val selectors: List<Selector>, val decls: Map<String, String>, val order: Int)
  class Selector(val parts: List<Part>, val hover: Boolean = false, val pseudo: String? = null)
    val specificity: Int
    fun matches(elem: Elem): Boolean
  class Part(val sel: Simple, val combinator: Char)
  class AttrSel(val name: String, val op: Char?, val value: String, val ignoreCase: Boolean)
    fun matches(e: Elem): Boolean
  class Simple(val tag: String?, val id: String?, val classes: List<String>, val target: Boolean = false,
      val nth: List<Nth> = emptyList(), val attrs: List<AttrSel> = emptyList(), val not: List<Selector> = emptyList(),
      val root: Boolean = false)
    val specificity: Int
    fun matches(e: Elem): Boolean
  class Nth(val a: Int, val b: Int, val fromEnd: Boolean = false)
    fun matches(e: Elem): Boolean
  data class WidthCondition(val min: Boolean, val px: Int)
    fun holds(width: Int?): Boolean
  fun isInherited(prop: String): Boolean
  fun declarations(text: String): Map<String, String>
  fun parse(css: String, dark: Boolean = false, seedVars: Map<String, String> = emptyMap(), viewportWidth: Int? = null): StyleSheet
  fun selectors(text: String): List<Selector>
class CssCounters(start: Map<String, Int> = emptyMap())
  val values: MutableMap<String, Int>
  fun snapshot(): Map<String, Int>
  fun enter(decls: Map<String, String>)
  fun content(v: String?, attrs: Map<String, String> = emptyMap()): String?
  fun walk(el: Html.El, parent: Css.Elem?, sheet: Css.StyleSheet, index: Int = 0, siblings: Int = 0)
  companion:
    fun styles(el: Html.El, parent: Css.Elem?, sheet: Css.StyleSheet, index: Int, siblings: Int, out: MutableList<Map<String, String>>)
    fun format(n: Int, style: String): String
object CssValues
  class Units(val em: Float, val rem: Float = 16f, val vw: Float? = null, val vh: Float? = null)
  const val AUTO: Float = Float.NEGATIVE_INFINITY
  class Border(val width: Float, val style: String, val color: Color?) { val shown: Boolean }
  class Gradient(val angle: Double, val stops: List<Pair<Color, Float?>>, val repeating: Boolean = false)
  class Shadow(val dx: Float, val dy: Float, val blur: Float, val color: Color, val spread: Float = 0f, val inset: Boolean = false)
  sealed class Track
    class Fixed(val px: Float); class Fr(val fr: Float); object Auto
    class MinMax(val min: Track, val max: Track); class AutoRepeat(val tracks: List<Track>, val fit: Boolean)
  fun length(v: String?, u: Units, percentOf: Float? = null): Float?
  fun fontSizePx(v: String?, parentPx: Float, rootPx: Float): Float
  fun fontFamily(list: String?): String
  fun words(v: String, commas: Boolean = false): List<String>
  fun commaList(v: String): List<String>
  fun sides(style: Map<String, String>, prop: String, u: Units, percentOf: Float?): FloatArray
  fun borderParts(v: String?, u: Units): Triple<Float?, String?, Color?>
  fun border(style: Map<String, String>, side: String, u: Units, currentColor: Color?): Border
  fun gradient(v: String?): Gradient?
  fun shadows(v: String?, u: Units, currentColor: Color): List<Shadow>
  fun tracks(v: String?, u: Units, percentOf: Float?): List<Track>
object DocTheme
  fun forDocument(fullHtml: String, dark: Boolean = osDark()): Map<String, String>
  fun stylesheet(fullHtml: String, dark: Boolean = osDark()): Css.StyleSheet
  fun color(value: String?): Color?
  fun osDark(): Boolean
```

### doc (8633; JadeHtmlView 3572, JadeDocView 1947, JadeProseView 1258, ProseDoc 722, ProseTree 416, RunList 181, SourceEditPopup 175, Markdown 128, ProseStyler 115, InlineObjects 87, CssProseStyler 32)
Rich-text document stack: prose model and editor, HTML/CSS box layout view, block-based document view, and Markdown to HTML. Depends on root, `css` (Css, CssCounters, CssValues, DocTheme), `html` (Html, HtmlEntities), java.awt. `ProseTree`, `RunList` and `SourceEditPopup` have no public declarations.
```
interface InlineObjects
  fun size(tag: String): Dimension?
  fun paint(canvas: JadeCanvas, tag: String, x: Int, y: Int, w: Int, h: Int)
  fun data(tag: String): ByteArray?
  fun adopt(tag: String, data: ByteArray?): String
  fun separate(tag: String, data: ByteArray?): String
  fun picture(bytes: ByteArray): String?
  companion { fun placeholder(canvas: JadeCanvas, x: Int, y: Int, w: Int, h: Int) }
data class InlineStyle(val bold: Boolean = false, val italic: Boolean = false, val underline: Boolean = false,
    val color: Color? = null, val link: String? = null, val span: String? = null, val strong: Boolean = false,
    val em: Boolean = false, val obj: String? = null)
data class ParaStyle(val heading: Int = 0, val id: String? = null, val cls: String? = null, val bullet: String? = null,
    val indent: Int = 0, val ordered: Boolean = false, val tag: String? = null, val wrapper: String? = null,
    val align: String? = null, val continues: Boolean = false, val style: String? = null)
class ProseDoc(html: String)
  val length: Int
  var revision: Long [ps]
  fun changedSince(since: Long): IntArray?
  fun text(): String
  fun styleAt(i: Int): InlineStyle
  fun paraStyleAt(offset: Int): ParaStyle
  fun typingStyleAt(offset: Int): InlineStyle
  fun objectAt(offset: Int): String?
  fun insertObject(offset: Int, tag: String, style: InlineStyle = typingStyleAt(offset))
  fun setObject(offset: Int, tag: String)
  fun insert(offset: Int, s: String, style: InlineStyle)
  fun delete(lo: Int, hi: Int)
  fun appendFrom(other: ProseDoc)
  fun applyHeading(lo: Int, hi: Int, level: Int)
  fun applyList(lo: Int, hi: Int, ordered: Boolean)
  fun applyAlign(lo: Int, hi: Int, align: String?)
  fun paraIdAt(offset: Int): String?
  fun offsetOfParaId(id: String): Int?
  fun setParaId(offset: Int, id: String?)
  fun applyBold(lo: Int, hi: Int); fun applyItalic(lo, hi); fun applyUnderline(lo, hi); fun applyCode(lo, hi)
  fun setColor(lo: Int, hi: Int, color: Color?)
  fun setLink(lo: Int, hi: Int, url: String?)
  fun toHtml(): String
  fun htmlOfRange(lo: Int, hi: Int): String
  fun inlineHtml(lo: Int, hi: Int): Pair<String, IntArray>
  fun readInline(html: String): List<Pair<Char, InlineStyle>>
  fun replaceInline(lo: Int, hi: Int, chars: List<Pair<Char, InlineStyle>>)
  companion:
    val WRAP_SEP: Char = Char(0x1F)
    val WRAP_ID: Char = Char(0x1E)
    fun wrapperTag(part: String): String
    fun wrapperTags(wrapper: String?): List<String>
    const val LINE_BREAK: Char                (literal value not confirmed; the extract shows it as a space-like character)
    const val OBJECT: Char = '\uFFFC'
interface ProseStyler
  val sheet: Css.StyleSheet                   (default EMPTY_SHEET)
  val ancestors: List<Css.Elem>               (default empty)
  val textColor: Color
  (the dump at lines 3700–3711 may list further members I did not capture)
data class ProsePalette(val fg: Color, val muted: Color, val link: Color, val accentSoft: Color, val border: Color,
    val codeBg: Color, val codeFg: Color, val kbdBg: Color)
  companion { val LIGHT; val DARK }
open class DefaultProseStyler(val baseFont: Font = Font(SANS_SERIF, PLAIN, 16), protected val palette: ProsePalette = ProsePalette.LIGHT,
    private val lineHeightFactor: Float = 1.65f) : ProseStyler
  companion { fun dark(baseFont: Font = Font(SANS_SERIF, PLAIN, 16)): DefaultProseStyler }
class CssProseStyler(override val sheet: Css.StyleSheet, fallbackText: Color = Color(0x1C, 0x1E, 0x21),
    override val ancestors: List<Css.Elem> = emptyList()) : ProseStyler
enum class ProseEdge { UP, DOWN, LEFT, RIGHT }
class JadeProseView(val prose: ProseDoc, styler: ProseStyler = DefaultProseStyler())
    : JadeContent, FocusTarget, FocusAware, ScrollAware, CursorProvider, ContextMenuProvider, HeightForWidth, OverlayContent, OwnedContent
  vars:
    styler; prefWidth: Int = 720; onChange: () -> Unit; inlineObjects: InlineObjects?
    onObjectActivate: (offset: Int, tag: String) -> Unit
    onLinkClick: (String) -> Unit; onCaretMove: () -> Unit; onLinkActivate: (String) -> Unit
    selectionColor: Color; highlightQuery: String = ""; highlightColor: Color
    caret: Int [ps]; anchor: Int [ps]; caretVisible: Boolean = true
    onExit: (edge: ProseEdge, x: Int) -> Boolean; onMergeBack: () -> Boolean
    counterStart: Map<String, Int>; tabWidth: Int = 4
  vals:
    selectionStart, selectionEnd, hasSelection, anchorOffset
    sourceShown, sourcePinned, sourceText: String?, sourceRect: java.awt.Rectangle?
  funs:
    invalidate()
    lineTopOf(offset: Int, width: Int): Int?
    offsetAt(x: Int, y: Int): Int?
    deleteRange(start: Int, end: Int)
    appendProse(other: ProseDoc)
    select(start: Int, end: Int); selectAll()
    caretParaStyle(): ParaStyle; caretParaId(): String?; setCaretParaId(id: String?)
    undo(); redo()
    linkAt(x: Int, y: Int): String?
    insertText(s: String)
    replaceObject(at: Int, tag: String, with: String): Boolean
    insertObject(tag: String)
    caretParagraphHasText(): Boolean
    toggleBold(); toggleItalic(); toggleUnderline(); toggleCode()
    setColor(color: Color?); setLink(url: String?)
    toggleList(ordered: Boolean); setAlign(align: String?); setHeading(level: Int)
    countMatches(query: String, ignoreCase: Boolean = true): Int
    replaceAll(query: String, replacement: String, ignoreCase: Boolean = true): Int
    toHtml(): String
    caretGeometryAt(offset: Int): IntArray?
    htmlSplitAt(offset: Int): Pair<String, String>
    selectedText(): String
    caretTo(offset: Int); caretToStart(); caretToEnd(); pressAtEnd(); caretToTop(x: Int); caretToBottom(x: Int)
fun proseView(html: String, styler: ProseStyler = DefaultProseStyler()): JadeComponent

class JadeHtmlView(private var root: Html.El, private val sheet: Css.StyleSheet, viewportWidth: Int? = null,
    private val targetId: String? = null, private val ancestors: List<Css.Elem> = emptyList(),
    private val counterStart: Map<String, Int> = emptyMap())
    : JadeContent, Pinnable, CursorProvider, HeightForWidth, OwnedContent, FocusTarget, FocusAware, EdgeEscape, TypesInPlace
  secondary constructor(html: String, css: String, dark: Boolean = false, viewportWidth: Int? = null, targetId: String? = null,
      ancestors: List<Css.Elem> = emptyList(), counterStart: Map<String, Int> = emptyMap())
  class PlacedText(val node: Html.Text, val start: Int, val text: String, val x: Int, val lineTop: Int, val lineHeight: Int,
      val baseline: Int, val font: Font, val tracking: Int, val color: Color, internal cuts, internal cutDx,
      val objectWidth: Int = -1, val objectHeight: Int = 0)
    fun xAt(i: Int): Int
    val width: Int
    fun moved(dx: Int, dy: Int): PlacedText
  interface Replaced { fun prefWidth(): Int; fun heightFor(width: Int): Int; val ownsBox: Boolean }
      (the dump at 3445–3451 may list more members)
  class Parts internal ctor
    val extents: Map<Html.El, Rectangle>
    fun placedText(part: Html.El): List<PlacedText>
  vars:
    prefWidth: Int = 360; editingText: Boolean = false; paintBand: IntRange?
    replacedFor: ((Html.El) -> Replaced?)?
    viewportHeight: Int?; scrolledByHost: Boolean = false; scrollTop: Int = 0
    onSourceEdit: ((path: List<Int>, html: String) -> Unit)?
    editable: Boolean = false; onEdit: (String) -> Unit
    caret: Int [ps]; anchor: Int [ps]
    onElementClick: ((path: List<Int>) -> Boolean)?
    onLinkClick: (String) -> Unit
    selectionColor: Color; caretVisible: Boolean = true; keepsLayouts: Boolean = false
  vals:
    replaced: MutableMap<Html.El, Replaced>
    sourceShown, sourcePinned, sourceText, sourceRect
    selectionStart, selectionEnd, hasSelection
  funs:
    placedText(width: Int): List<PlacedText>
    showHtml(html: String)
    relayout()
    maxContentWidth(): Int
    extentsOf(parts: Collection<Html.El>, width: Int): Map<Html.El, Rectangle>
    partsOf(parts: Collection<Html.El>, width: Int): Parts
    contentRects(width: Int): Map<Html.El, Rectangle>
    rectOf(el: Html.El, width: Int): Rectangle?
    rectOf(path: List<Int>, width: Int): Rectangle?
    text(): String
    offsetAt(x: Int, y: Int): Int?
    caretTo(offset: Int)
    select(start: Int, end: Int); selectAll(); selectedText(): String
    topOf(id: String, width: Int): Int?
    pathAt(x: Int, y: Int): List<Int>?
    caretGeometry(): IntArray?
fun htmlView(html: String, css: String, dark: Boolean = false, viewportWidth: Int? = null, targetId: String? = null,
    ancestors: List<Css.Elem> = emptyList(), counterStart: Map<String, Int> = emptyMap()): JadeComponent

class DocPage(val body: Html.El, val sheet: Css.StyleSheet, val html: Css.Elem? = null, val viewportWidth: Int? = null,
    val targetId: String? = null)
enum class FloatSide { LEFT, RIGHT }
data class FixedLength(val value: Float, val percent: Boolean = false) { fun of(base: Int): Int }
data class FixedPlacement(val left: FixedLength? = null, val top: FixedLength? = null, val right: FixedLength? = null,
    val bottom: FixedLength? = null, val width: FixedLength? = null, val height: FixedLength? = null)
class DocItem internal constructor(val component: JadeComponent, val prose: JadeProseView?, var source: String)
  val isProse: Boolean
  vars:
    columnGroup = -1; columnIndex; columnWidth; columnSticky; columnMaxWidth; columnPadX
    pinned; fixed; fixedLeft; fixedTop; fixedWidth; fixedBottom; fixedPlacement: FixedPlacement?
    floatSide: FloatSide?; floatWidth
    marginTop = -1; marginBottom = -1
    blockWidth; blockHeight; blockHeightVh; resizable
    frameBg: Color?; frameBorder: Color?; frameRadius; framePad
    element: Html.El?; drawsOwnBox
  fun html(): String
  companion:
    fun prose(view: JadeProseView): DocItem
    fun prose(html: String, styler: ProseStyler = DefaultProseStyler()): DocItem
    fun block(component: JadeComponent, source: String): DocItem
class JadeDocView(items: List<DocItem>)
    : JadeContent, FocusTarget, FocusAware, ScrollAware, ContextMenuProvider, CursorProvider, OwnedContent, TabAware, WheelTarget
  val items: MutableList<DocItem>
  data class Span(val anchorItem: Int, val anchorOffset: Int, val focusItem: Int, val focusOffset: Int)
    val forward: Boolean; val from: Pair<Int, Int>; val to: Pair<Int, Int>
    fun covers(i: Int): Boolean
  callback vars:
    onChange: () -> Unit; onTyped: (() -> Unit)?; onTypingEnd: (() -> Unit)?
    onAppendTrailingProse: (() -> Unit)?; onUndo: (() -> Unit)?; onRedo: (() -> Unit)?
    onRequestDeleteSpan: ((fromItem: Int, fromOffset: Int, toItem: Int, toOffset: Int) -> Pair<Int, Int>?)?
    blockText: ((index: Int) -> String)?
    onFocusChange: (Boolean) -> Unit
    onActivate: (index: Int) -> Unit
    onObjectActivate: ((prose: JadeProseView, offset: Int, tag: String) -> Unit)?
    onInsertAt: ((index: Int) -> Unit)?
    onRequestResize: ((index: Int, width: Int) -> Unit)?
    onViewportWidth: (Int) -> Unit; onScrollOffset: (Int) -> Unit
    onEscapeKey: (() -> Boolean)?
    onBlockClick: ((blockIndex: Int, localX: Int, localY: Int) -> Boolean)?
    onBlockDrag: ((blockIndex: Int, localX: Int, localY: Int) -> Unit)?
    onRequestDelete: ((index: Int) -> Unit)?
    onRequestMove: ((from: Int, to: Int) -> Unit)?
    onRequestMoveInto: ((from: Int, targetItem: Int, offset: Int) -> Unit)?
    cloneBlock: (DocItem) -> DocItem?
    onRequestDuplicate: ((index: Int) -> Unit)?
    onBlockCopy: ((index: Int) -> Boolean)?; onBlockCut: ((index: Int) -> Unit)?
    onBlockPaste: ((index: Int, atCaret: Boolean) -> Boolean)?
    onRequestMergeBack: ((index: Int) -> Boolean)?
    onEditSource: ((index: Int) -> Unit)?
    blockMenu: ((index: Int) -> List<JadeMenuItem>)?
  state vars:
    activeIndex: Int [ps]; blockSelected: Boolean [ps]; span: Span? [ps]
    selectionOutline; spanTint; dropIndicatorColor; plusBg; plusFg: Color
    inlineObjects: InlineObjects?; page: DocPage?
    contentMaxWidth: Int = 0; contentPadX: Int = 0; pageTop: Int?
    background: Color = Color.WHITE; readOnly: Boolean = false
  vals: handlingTyping: Boolean; scrollTop: Int; viewportHeight: Int
  funs:
    endTypingRun()
    withActiveProse(op: (JadeProseView) -> Unit)
    activeProseOrNull(): JadeProseView?
    selectBlock(index: Int)
    revealBlock(index: Int)
    revealAnchor(index: Int, y: Int)
    revealAnchorAt(index: Int, lineTop: (width: Int) -> Int?)
    undo(); redo()
    moveItem(from: Int, toIndexBefore: Int)
    moveUp(index: Int = activeIndex); moveDown(index: Int = activeIndex)
    moveItemInto(from: Int, targetItem: Int, offset: Int)
    deleteSelected()
    setItems(newItems: List<DocItem>)
    duplicate(index: Int = activeIndex)
    insertProseAfter(index: Int = activeIndex, html: String = "<p></p>", styler: ProseStyler? = null)
    toHtml(): String
fun docView(items: List<DocItem>): JadeComponent
object Markdown { fun toHtml(md: String): String }
val DEFAULT_MARKDOWN_CSS: String              (top-level public val)
fun markdownView(md: String, css: String = DEFAULT_MARKDOWN_CSS): JadeComponent
```

### gl (475; JadeGlView.kt)
Embeds an OpenGL-rendered region in the tree, with per-context resource holders and offscreen capture hooks. Depends on root, java.awt, java.util.
```
class GlFrame(val x: Int, val y: Int, val width: Int, val height: Int, val framebufferWidth: Int, val framebufferHeight: Int,
    val framebuffer: Int, val contentScale: Float, val time: Double, val delta: Double, val frame: Long,
    val contextId: Long, val shareGroup: Long)
interface GlRenderer { fun init(frame: GlFrame) {}; fun render(frame: GlFrame); fun dispose(contextId: Long) {} }
class JadeGlView(renderer: GlRenderer, preferredWidth: Int = 320, preferredHeight: Int = 240)
    : JadeContent, OwnedContent, RawInputTarget, CursorProvider
  @Volatile var renderer: GlRenderer
  var preferredSize: Dimension
  @Volatile var animating: Boolean = false
  fun requestFrame()
  val wantsFrame: Boolean
  var onInput: ((RawInput) -> Boolean)? = null
  var onCursor: ((x: Int, y: Int) -> CursorKind)? = null
  @Volatile var hasKeyboard: Boolean [ps]
  @Volatile var showFocus: Boolean = true
  @Volatile var keepWhenDetached: Boolean = false
  fun isInitialised(contextId: Long): Boolean
  fun drawFrame(x: Int, y: Int, width: Int, height: Int, framebufferWidth: Int, framebufferHeight: Int, framebuffer: Int,
      contentScale: Float, nowNanos: Long, contextId: Long, shareGroup: Long)
  fun releaseContext(contextId: Long): Boolean
  fun releaseOffscreen()
  companion { val FOCUS_COLOR: Color; const val FOCUS_WIDTH: Int = 2 }
object GlContexts
  fun onDestroy(contextId: Long, hook: () -> Unit)
  fun removeOnDestroy(contextId: Long, hook: () -> Unit)
  fun onShareGroupDestroy(shareGroup: Long, hook: () -> Unit)
  fun created(contextId: Long, shareGroup: Long)
  fun destroyed(contextId: Long)
class PerContext<T : Any>(private val create: (GlFrame) -> T, private val dispose: (T) -> Unit = {})
  operator fun get(frame: GlFrame): T
  fun release(contextId: Long)
class PerShareGroup<T : Any>(private val create: (GlFrame) -> T, private val dispose: (T) -> Unit = {})
  operator fun get(frame: GlFrame): T
interface GlOffscreen
  fun picture(view: JadeGlView, width: Int, height: Int, contentScale: Float, maxWaitMillis: Long): BufferedImage?
  fun release(view: JadeGlView) {}
interface GlOffscreenProvider { fun create(): GlOffscreen? }
class GlViewTracker()
  fun noteShown(views: List<GlViewPlacement>)
  fun detached(tree: JadeComponent): List<JadeGlView>
  fun drain(): List<JadeGlView>
```
There is no factory; apps write `JadeComponent(JadeGlView(renderer))`.

### hex (346; JadeHexView.kt)
Hex viewer and editor over a byte source. Depends on root and another module, `com.jadelib.io` (ByteSource, InMemoryByteSource).
```
class JadeHexView(source: ByteSource, font: Font = HEX_FONT, var editable: Boolean = false, var onChange: () -> Unit = {})
    : JadeContent, FocusTarget, OwnedContent
  var source: ByteSource
  var font: Font
  fun hasUnsavedEdits(): Boolean
  fun changedBytes(original: ByteSource): List<Pair<Long, Int>>
  fun markSaved()
  fun firstVisibleRow(): Long
  fun cursorOffset(): Long
  fun visibleLines(): List<String>
  fun typeForTest(digit: String)
fun hexView(source: ByteSource, editable: Boolean = false, onChange: () -> Unit = {}): JadeComponent
fun hexView(bytes: ByteArray): JadeComponent
fun hexEditor(bytes: ByteArray, onChange: () -> Unit = {}): JadeComponent
```

### html (270; Html 133, HtmlEntities 137)
Minimal HTML parser, serialiser and entity table. No dependencies.
```
object Html
  sealed class Node
  class Text(var text: String) : Node()
  class El(val tag: String, val classes: Set<String>, val id: String?, val attrs: MutableMap<String, String>,
      val children: MutableList<Node> = ArrayList()) : Node()
  const val FRAGMENT: String = "#fragment"
  fun textNodes(el: El): List<Text>
  fun serialize(n: Node): String
  fun parse(html: String): El
object HtmlEntities
  fun decode(s: String): String
  fun named(name: String): String?
  fun escapeText(s: String): String
  const val NBSP: Char
  val size: Int
```

### icons (74; ToolbarIcons.kt)
SVG toolbar icons rasterised to BufferedImage. Depends on `svg` (SvgRenderer) and java.awt.
```
object ToolbarIcons
  const val (SVG strings): PLUS, TABLE, NEW, OPEN, SAVE, SAVE_CLEAN, IMAGE, MATH, SCRIPT, CHART, HEX, TOC
  fun render(svg: String, color: Color, size: Int = 24, badge: Boolean = false): BufferedImage
  fun table(color: Color, size: Int = 24): BufferedImage
  same (color: Color, size: Int = 24): BufferedImage shape for: newDoc, open, save, saveClean, image, math, script, chart, hex, toc
```

### image (2481; PictureEdit 937, JadePictureEditor 842, JadePictureViewer 456, JadeImageView 246)
Image block view, zoomable picture canvas and viewer, and a layered picture editor with markup shapes. Depends on root, java.awt (+geom, image), java.io, java.util.Base64, javax.imageio.
```
class JadeImageView(val src: String?, width: Int = 0, height: Int = 0, private val onResize: (Int, Int) -> Unit = { _, _ -> },
    private val base: java.net.URI? = null, decoded: BufferedImage? = null)
    : JadeContent, FocusAware, FocusTarget, HeightForWidth, OwnedContent
  var loadError: String? [ps]
  var onActivate: (() -> Unit)?
  var alt: String?
  var fitToWidth: Boolean = false
  val imgWidth: Int; val imgHeight: Int
fun imageView(src: String?, width: Int = 0, height: Int = 0, onResize: (Int, Int) -> Unit = { _, _ -> }): JadeComponent

class PictureInfo(val name: String, val image: BufferedImage, val type: String, val bytes: Int, val uses: Int)
interface PictureTool
  fun press(x: Double, y: Double, clicks: Int, shift: Boolean): Boolean
  fun drag(x: Double, y: Double, shift: Boolean) {}
  fun release(x: Double, y: Double) {}
  fun patch(): Pair<BufferedImage, java.awt.Point>?
  fun hover(x: Double, y: Double) {}
  fun preview(g: java.awt.Graphics2D, zoom: Double) {}
  fun key(event: JadeEvent): Boolean
  fun cursor(x: Double, y: Double): CursorKind
class JadePictureCanvas(image: BufferedImage) : JadeContent, OwnedContent, WheelTarget, DragTarget, FocusTarget, CursorProvider
  var image: BufferedImage
  fun showEdited(img: BufferedImage)
  var tool: PictureTool?
  fun toPicture(vx: Int, vy: Int): java.awt.geom.Point2D.Double
  fun toView(px: Double, py: Double): IntArray
  var fit: Boolean [ps]
  var onZoom: () -> Unit
  var onStep: (Int) -> Unit
  val zoom: Double
  fun pictureRect(): IntArray
  fun zoomTo(z: Double, ax: Int = w / 2, ay: Int = h / 2)
  fun zoomStep(dir: Int, ax: Int = w / 2, ay: Int = h / 2)
  fun fitToView()
class JadePictureViewer(info: PictureInfo)
  var info [ps]
  val canvas: JadePictureCanvas
  var onStep: ((Int) -> Unit)?; var onCopy: (() -> Unit)?; var onSaveAs: (() -> Unit)?; var onEdit: (() -> Unit)?
  val component: JadeComponent
  fun show(next: PictureInfo)
  fun describe(): String
  val focusTarget: JadeComponent
  companion { fun sizeText(bytes: Int): String }

class EditResult(val flat: BufferedImage, val layers: List<BufferedImage>?, val layersText: String?)
enum class EditTool(val label: String, val hint: String)
  { CURSOR, SELECT, PEN, HIGHLIGHTER, BRUSH, ERASER, LINE, ARROW, BOX, ELLIPSE, TEXT, FILL, PICKER }
class JadePictureEditor(val edit: PictureEdit, private val hasOriginal: Boolean = false, private val shared: Boolean = false)
  var onSave: (EditResult, separate: Boolean) -> Unit
  var onCancel: () -> Unit; var onRevert: () -> Unit
  val canvas: JadePictureCanvas
  var tool: EditTool = EditTool.PEN
  var color: Color; var width: Float = 4f
  val picked: MutableList<ShapeRef>
  val selected: ShapeRef?
  fun toggleAdjust(); fun setAdjust(b: Int, c: Int, s: Int); fun applyAdjust()
  fun copySelection(): Boolean; fun cutSelection(); fun pasteSelection(): Boolean
  fun undo(); fun redo()
  fun askResize(); fun askColor()
  fun result(): EditResult
  var current: Int [ps]
  fun choose(i: Int)
  fun cancel()
  val component: JadeComponent
  val focusTarget: JadeComponent
  companion { val SWATCHES: List<Color>; val WIDTHS: List<Float> = listOf(2f, 4f, 8f, 16f) }

enum class MarkKind { PEN, HIGHLIGHTER, ARROW, BOX, ELLIPSE, TEXT, LINE }
enum class StrokeStyle { PEN, HIGHLIGHTER, BRUSH, ERASER }
class MarkShape(val kind: MarkKind, val points: MutableList<Point2D.Double>, var color: Color, var width: Float = 3f,
    var text: String = "", var size: Int = 20)
  var before: MarkShape?
  fun copy(): MarkShape
  fun bounds(): java.awt.geom.Rectangle2D
  fun hit(x: Double, y: Double, slack: Double = 5.0): Boolean
  fun moveBy(dx: Double, dy: Double)
  fun draw(g: Graphics2D)
  companion                                   (public; I captured no members)
class PictureLayer(var name: String, var pixels: BufferedImage, shapes: List<MarkShape> = emptyList(), var visible: Boolean = true,
    var editable: Boolean = true, var opacity: Float = 1f)
  val shapes: MutableList<MarkShape>
  fun copy(): PictureLayer
  fun rendered(): BufferedImage
class Floating internal constructor(val source: Rectangle, internal pieces, internal shapes)
  var cx, cy, w, h, angle: Double; var flipX, flipY: Boolean
  fun transform(): AffineTransform
  fun outline(): java.awt.Shape
  fun local(p: Point2D.Double): Point2D.Double
  fun picture(q: Point2D.Double): Point2D.Double
class ShapeRef(val layer: PictureLayer, val shape: MarkShape)
  secondary constructor(layer: PictureLayer, index: Int)
  val index: Int
class PictureEdit(layers: List<PictureLayer>)
  secondary constructor(base: BufferedImage, shapes: List<MarkShape> = emptyList())
  val layers: MutableList<PictureLayer>; val width: Int; val height: Int; val base: BufferedImage
  fun target(): PictureLayer?
  fun worked(): List<PictureLayer>
  var selection: Rectangle? [ps]; var floating: Floating? [ps]; val hasSelection: Boolean
  fun selectionOutline(): java.awt.Shape?
  fun select(r: Rectangle?)
  fun lift(): Floating?
  fun moveFloating(change: Floating.() -> Unit)
  fun commitFloating(); fun clearSelection(); fun deleteSelection()
  fun fillSelection(color: Color): Boolean
  fun cropToSelection()
  fun selectionImage(): BufferedImage?
  fun paste(img: BufferedImage, cx: Double = width / 2.0, cy: Double = height / 2.0): Boolean
  fun revertShapes(refs: List<ShapeRef>); fun removeShapes(refs: List<ShapeRef>)
  fun restyleShapes(refs: List<ShapeRef>, color: Color? = null, width: Float? = null)
  var onChange: () -> Unit
  val canUndo; val canRedo; val changed: Boolean
  fun checkpoint(); fun undo(); fun redo()
  fun addLayer(at: Int = layers.lastIndex): Int
  fun deleteLayer(i: Int)
  fun moveLayer(i: Int, by: Int): Int
  fun renameLayer(i: Int, name: String)
  fun setVisible(i: Int, v: Boolean); fun setEditable(i: Int, v: Boolean)
  fun setOpacity(i: Int, o: Float, step: Boolean = true)
  fun mergeDown(i: Int)
  fun rotate(clockwise: Boolean); fun flip(horizontal: Boolean)
  fun crop(r: Rectangle); fun resize(w: Int, h: Int)
  fun adjust(brightness: Int, contrast: Int, saturation: Int)
  fun greyscale(); fun invert()
  fun stroke(points: List<Point2D.Double>, color: Color, width: Float, style: StrokeStyle = StrokeStyle.PEN): Boolean
  fun fillRegion(x: Int, y: Int, tolerance: Int = 32): BooleanArray?
  fun fill(x: Int, y: Int, color: Color, tolerance: Int = 32): Boolean
  fun pick(x: Int, y: Int): Color?
  fun addShape(s: MarkShape): Boolean
  fun removeShape(ref: ShapeRef)
  fun shapeAt(x: Double, y: Double, slack: Double = 5.0): ShapeRef?
  val allShapes: List<MarkShape>
  fun flattened(): BufferedImage
  sealed interface Effect
    class Stroke(val points: List<Point2D.Double>, val color: Color, val width: Float, val style: StrokeStyle)
    class AddShape(val shape: MarkShape)
    class Fill(val mask: BooleanArray, val color: Color)
  fun preview(r: Rectangle, effect: Effect): BufferedImage
  val layered: Boolean
  fun layersText(): String
  companion:
    const val BACKGROUND: String = "Background"
    fun argb(img: BufferedImage): BufferedImage
    fun adjusted(img: BufferedImage, brightness: Int, contrast: Int, saturation: Int): BufferedImage
    fun strokeBounds(points: List<Point2D.Double>, width: Float, style: StrokeStyle): Rectangle
    fun strokeArea(points: List<Point2D.Double>, width: Float, round: Boolean = true): java.awt.Shape
    fun writeMarkup(shapes: List<MarkShape>): String
    fun writeLayers(layers: List<PictureLayer>): String
    fun parseMarkup(json: String?): List<MarkShape>
    fun parseLayers(json: String?, pixels: List<BufferedImage?>): List<PictureLayer>?
```
There are no factories for the viewer or editor; apps construct them and use `.component`.

### map (749; JadeMapView.kt)
Pan/zoom 2-D map of labelled points and polygons with selection and drawing. Depends on root and java.awt.
```
enum class MapTool { PAN, DRAW }
class MapPoint(var x: Double, var y: Double, val label: String, val fields: List<Pair<String, String>> = emptyList(),
    val color: Color? = null, val layer: String = "")
class MapShape(val xs: DoubleArray, val ys: DoubleArray, val closed: Boolean = true, val stroke: Color? = null,
    val fill: Color? = null, val label: String = "", val fields: List<Pair<String, String>> = emptyList(),
    val layer: String = "", val editable: Boolean = true)
class JadeMapView(points: List<MapPoint> = emptyList()) : JadeContent, OwnedContent, CursorProvider, WheelTarget, DragTarget
  var onSelect: (() -> Unit)?; var onActivate: (() -> Unit)?
  var editable: Boolean = false
  var selectedShape: MapShape?; var selectedPoint: MapPoint?
  var onDeleteSelected: (() -> Unit)?
  var onPointMoved: ((MapPoint) -> Unit)?
  var onShapeMoved: ((List<MapShape>) -> Unit)?
  var onShapeCreated: ((DoubleArray, DoubleArray) -> Unit)?
  var onBorderDrawn: ((DoubleArray, DoubleArray) -> Unit)?
  var tool: MapTool = MapTool.PAN
  var points: List<MapPoint>; var shapes: List<MapShape>
  fun captureView(): DoubleArray
  fun restoreView(s: DoubleArray)
fun mapView(points: List<MapPoint> = emptyList()): JadeComponent
```

### math (1013; JadeMathBlock 538, MathTypeset 397, JadeMathView 78)
Math expression typesetting, a single-expression view and a multi-line calculation block with results. Depends on root and java.awt.
```
object MathTypeset
  fun layout(expr: String, baseFont: Font, metrics: (Font) -> FontMetrics): Box
  abstract class Box
    var width: Float; var ascent: Float; var descent: Float
    val height: Float
    abstract fun paint(g: Graphics2D, x: Float, baseline: Float)
class JadeMathView(expr: String, var font: Font = Font(SERIF, PLAIN, 22), var color: Color = Color(0x1a, 0x24, 0x20)) : JadeContent
  var expr [ps]
  fun setExpr(e: String)
fun mathView(expr: String): JadeComponent
class JadeMathBlock(source: List<String>, var font: Font = MB_FONT, var onSelect: (line: Int, source: String) -> Unit = { _, _ -> },
    var onEdit: (line: Int, source: String) -> Unit = { _, _ -> })
    : JadeContent, FocusTarget, TextEditable, EdgeEscape, OwnedContent, ScrollAware, ContextMenuProvider
  var onLinesChanged: ((List<String>) -> Unit)?
  var cornerLabel: String = ""
  var selected: Int [ps]
  fun setSource(newLines: List<String>)
  fun setResults(results: List<String?>, errors: List<Boolean> = emptyList(), errorTokens: List<String?> = emptyList())
  var markedLines: List<Pair<Int, Color>>
  fun source(): List<String>
  fun lineAtLocal(y: Int): Int
  var onEditingText: ((line: Int, source: String) -> Unit)?
  fun previewLine(line: Int, source: String?)
  test hooks: resultTextForTest(i), editingLineForTest(), isEditingForTest(), previewForTest(i), startEditForTest(i), selectLineForTest(i)
fun mathBlock(source: List<String>, onEdit: (line: Int, source: String) -> Unit = { _, _ -> }): JadeComponent
```

### sheet (4065; JadeSheet 3198, JadeFormulaBar 613, Completion 107, CellRefs 75, FormulaSplit 72)
Spreadsheet grid widget (selection, editing, merges, freeze, fill, clipboard, validation), formula bar and completion popup. Depends on root and `doc` (InlineObjects, InlineObjectBoxes, ProseDoc). `CellRefs.kt` has no public declarations.
```
interface Completer
  fun candidates(text: String, caret: Int, asked: Boolean = false): List<String>
  fun accept(text: String, caret: Int, choice: String): Pair<String, Int>?
class CompletionPopup(private val display: Display)
  secondary constructor(anchor: () -> Pair<Int, Int>?)
  interface Display { fun show(choices: List<String>, index: Int, pick: (Int) -> Unit): Boolean; fun hide() }
  var choices: List<String> [ps]
  val isOpen: Boolean
  fun refresh(completer: Completer?, read: () -> Pair<String, Int>, write: (String, Int) -> Unit, asked: Boolean = false)
  fun caretMovedTo(caret: Int)
  val highlighted: Int
  fun key(key: Key): Boolean
  fun accept()
  fun hide()
interface FormulaSplit
  class Parts(val value: String, val format: String, val valueStart: Int, val formatStart: Int)
  fun split(source: String): Parts
  fun join(value: String, format: String, original: String, parts: Parts): String
  val formatNames: List<String>               (default empty)
  class Inherited(val text: String, val from: String)
object FormatCompletion
  fun candidates(text: String, caret: Int, names: List<String>): List<String>
  fun accept(text: String, caret: Int, name: String): Pair<String, Int>?
  fun completer(names: List<String>): Completer
class JadeFormulaBar(font: Font = FX_FONT) : JadeContent, FocusTarget, FocusAware, TextEditable, OwnedContent, FocusHolder
  [ps] vars: label: String; editable: Boolean; note: String; inheritedValue: FormulaSplit.Inherited?; inheritedFormat: FormulaSplit.Inherited?
  vars:
    onFocusChange: (Boolean) -> Unit
    linkedToSelection: Boolean = false
    onExpand: (() -> Unit)?
    pointTarget: (pressed: JadeComponent, x: Int, y: Int) -> Boolean
    split: FormulaSplit?
    valueCompleter: Completer?
    onLeave: (() -> Unit)?
    onCommit: (String) -> Unit; onCancel: () -> Unit; onInput: (String) -> Unit
    inlineObjects: InlineObjects?
    onHover: ((offset: Int, x: Int, y: Int) -> Unit)?
    links: TextLinks?
    onLinkHover: ((link: TextLink?, x: Int, y: Int) -> Unit)?
  vals: isSplit; shownValue: String; format: String; editingFormat: Boolean; text: String; hoveredLink: TextLink?
  funs:
    isFocused(): Boolean
    focusForEdit()
    isPointModeActive(): Boolean
    showMirrorCaret(offset: Int); setMirrorCaretVisible(visible: Boolean); hideMirrorCaret()
    insertReference(ref: String)
    ownParts(): Pair<String, String>?
    setParts(value: String, format: String)
    setText(source: String)
    setTarget(label: String, source: String, editable: Boolean = true, note: String = "", formatted: Boolean = false,
        inheritedValue: FormulaSplit.Inherited? = null, inheritedFormat: FormulaSplit.Inherited? = null,
        format: String? = null, objects: List<String> = emptyList())
    clear()
  test hooks: formatModelForTest(): SharedTextModel, formatCompletionsForTest(), valueCompletionsForTest(), commitForTest(), expandForTest()
fun formulaBar(onCommit: (String) -> Unit = {}): JadeComponent

data class SheetTheme(val cellBg: Color = Color.WHITE, val cellFg: Color = CELL_FG, val gridLine: Color = GRID,
    val headerBg: Color = HEADER_BG, val headerFg: Color = HEADER_FG, val theadBg: Color = THEAD_BG,
    val cellFont: Font = SHEET_FONT, val cellPad: Int = PAD, val codeBg: Color = CODE_BG)
  companion { val DEFAULT }
enum class SheetAlign { LEFT, CENTER, RIGHT }
data class SheetMarquee(val r0: Int, val c0: Int, val r1: Int, val c1: Int, val color: Color)
data class SheetSpan(val r: Int, val c: Int, val rowSpan: Int, val colSpan: Int)
data class SheetCell(val text: String = "", val editText: String? = null, val bg: Color? = null, val fg: Color? = null,
    val bold: Boolean = false, val italic: Boolean = false, val underline: Boolean = false, val align: SheetAlign = SheetAlign.LEFT,
    val header: Boolean = false, val readOnly: Boolean = false, val locked: Boolean = false, val borderPx: Int = 1,
    val errorDetail: String? = null, val codeWords: List<String> = emptyList(), val findText: String? = null,
    val objects: List<String> = emptyList())
  val isEmpty: Boolean
class JadeSheet(rows: Int = 20, cols: Int = 8, font: Font = SHEET_FONT, var onEdit: (row: Int, col: Int, value: String) -> Unit = { _, _, _ -> })
    : JadeContent, FocusTarget, TextEditable, ScrollAware, DropdownProvider, ContextMenuProvider, EdgeEscape, Pinnable, CursorProvider, OwnedContent, WheelTarget
  data vars:
    rows; cols; font
    merges: List<SheetSpan>; hiddenRows: Set<Int>; highlightQuery: String; validatedCols: Set<Int>
    freezeRows; freezeCols
    canRegenerate; headerDefaultsApply = true
    headersAlwaysVisible = true; tableLabel: String; active = true; linkedToFxBar
    filteredCols: Set<Int>; conflictedCells: Set<Pair<Int, Int>>
    wrapCells; stretchToWidth; theme: SheetTheme = SheetTheme.DEFAULT; autoFitColumns
    errorText: String?; cornerRadius: Int; cornerBackground: Color?
    fxPreview: String?; fxPreviewCaret: Int?; fxPreviewCaretVisible = true; fxPreviewAnchor: Int?
    externalMarquees: List<SheetMarquee>; completer: Completer?; inlineObjects: InlineObjects?
    editorLinks: TextLinks?
  [ps] vars: selRow, selCol, selEndRow, selEndCol, scrollX
  vals: hasRangeSelection, editing, columnWidth
  callback vars:
    onFreeze: (rows: Int, cols: Int) -> Unit
    onRegenerate: () -> Unit; onCopyTable: () -> Unit
    onSelectionChanged: (row: Int, col: Int) -> Unit; onSelectionDone: () -> Unit
    onStructureEdit: (isCol: Boolean, at: Int, delta: Int) -> Unit
    onSort: (col: Int, ascending: Boolean) -> Unit
    onFill: (fromRow: Int, fromCol: Int, toRow: Int, toCol: Int, down: Boolean) -> Unit
    onMerge: (r0: Int, c0: Int, r1: Int, c1: Int) -> Unit; onUnmerge: (r: Int, c: Int) -> Unit
    validationOptionsFor: (col: Int) -> List<String>
    onMoveCells: (r0: Int, c0: Int, r1: Int, c1: Int, toR: Int, toC: Int, copy: Boolean, cut: Boolean) -> Boolean
    onCut: (r0: Int, c0: Int, r1: Int, c1: Int) -> Unit
    isCutElsewhere: (text: String) -> Boolean
    onMoveCellsIn: (toR: Int, toC: Int, text: String) -> Boolean
    onEditScope: () -> Unit
    onHeadersWhenUnfocused: ((hide: Boolean) -> Unit)?
    onFilter: (col: Int) -> Unit; onValidate: (col: Int) -> Unit; onClearFilter: (col: Int) -> Unit
    onInsertMultiple: (isCol: Boolean, at: Int) -> Unit
    onErrorHover: (detail: String?, x: Int, y: Int) -> Unit
    onResolveConflict: (row: Int, col: Int, useRow: Boolean) -> Unit
    onLock: (cells: List<Pair<Int, Int>>, locked: Boolean) -> Unit
    onRefused: (reason: String) -> Unit
    onEditorHover: ((offset: Int, x: Int, y: Int) -> Unit)?
    onEditorLinkHover: ((link: TextLink?, x: Int, y: Int) -> Unit)?
    scopeDefaultText: (kind: Char, index: Int) -> String
    onScopeEdit: (kind: Char, index: Int, text: String) -> Unit
    onClearScopeDefault: (kind: Char, index: Int) -> Unit
    onEditingText: (String) -> Unit; onEditCancel: () -> Unit
    onEditRich: ((row: Int, col: Int, value: String, objects: List<String>) -> Unit)?
    onViewPicture: ((row: Int, col: Int, tag: String) -> Unit)?
    onColumnResized: (col: Int, width: Int) -> Unit; onRowResized: (row: Int, height: Int) -> Unit
    offsetFormula: (value: String, dRow: Int, dCol: Int) -> String
    onCopied: (r0: Int, c0: Int, r1: Int, c1: Int, text: String) -> Unit
    onPasted: (r: Int, c: Int, dr: Int, dc: Int, text: String) -> Unit
    onPointArrow: ((dr: Int, dc: Int, extend: Boolean) -> Boolean)?
  funs:
    styled(r: Int, c: Int): SheetCell
    cell(r: Int, c: Int): String
    matches(r: Int, c: Int, query: String): Boolean
    setCell(r: Int, c: Int, value: String)
    setCell(r: Int, c: Int, cell: SheetCell)
    clearCells()
    freezeAtSelection(); clearFreeze()
    selectionBounds(): IntArray
    insertRow(at: Int = selRow); deleteRow(at: Int = selRow); insertColumn(at: Int = selCol); deleteColumn(at: Int = selCol)
    sortByColumn(col: Int = selCol, ascending: Boolean = true)
    mergeSelection(); unmergeSelection()
    fillDown(); fillRight()
    selectedColumnSet(): Set<Int>; selectedRowSet(): Set<Int>; selectedCells(): List<Pair<Int, Int>>
    isSparseSelection(): Boolean; isMixedAxisSelection(): Boolean
    select(r: Int, c: Int); extendTo(r: Int, c: Int)
    selectedSource(): String; selectedRef(): String; refFor(r: Int, c: Int): String; selectionRefLabel(): String
    selectedReadOnly(): Boolean; isSingleCellSelection(): Boolean
    refuse(r: Int, c: Int): Boolean
    editorText(): String
    handEditToFormulaBar()
    insertObject(tag: String): Boolean
    scrollXBy(dx: Int): Boolean
    ownColumnWidth(c: Int): Int?; ownRowHeight(r: Int): Int?
    setColumnWidth(c: Int, width: Int?); setRowHeight(r: Int, height: Int?)
    setSizes(widths: Map<Int, Int>, heights: Map<Int, Int>)
    selectAll()
    columnSelected(): Int; rowSelected(): Int
    selectColumnScope(c: Int); selectRowScope(r: Int); extendScopeTo(i: Int)
    columnsSelected(): IntRange?; rowsSelected(): IntRange?
    wholeTableSelected(): Boolean
    insertReference(ref: String)
    cellAtLocal(x: Int, y: Int): Pair<Int, Int>?
    isEditingFormula(): Boolean; isPointing(): Boolean; finishEdit(): Boolean; isEditing(): Boolean; isEditingScope(): Boolean
  test hooks:
    fxPreviewForTest(), hasReferenceMarksForTest(), rowHeightForTest(), rowHeightForTest(r: Int)
    columnWidthForTest(c), rowTopForTest(r), columnLeftForTest(c), cellAtForTest(x, y)
    startEditForTest(r: Int, c: Int, text: String), editorTextForTest(), commitEditForTest(), completionsForTest()
fun sheet(rows: Int = 20, cols: Int = 8, onEdit: (row: Int, col: Int, value: String) -> Unit = { _, _, _ -> }): JadeComponent
```

### svg (1023; SvgRenderer 829, SvgView 194)
SVG subset renderer onto Graphics2D, plus a view with selectable text. Depends on root, `css` (Css), java.awt + geom, org.w3c.dom.
```
object SvgRenderer
  var theme: Map<String, String> [ps]
  fun setTheme(vars: Map<String, String>)
  fun intrinsicSize(svg: String): Pair<Int, Int>?
  class TextHit(val text: String, val bounds: java.awt.geom.Rectangle2D)
  fun render(svg: String, g2: Graphics2D, compW: Int, compH: Int, textSink: MutableList<TextHit>? = null, vars: Map<String, String>? = null)
class JadeSvgView(svg: String) : JadeContent, HeightForWidth
  var svg: String
  var failure: String?
  var themeVars: Map<String, String>
  fun textRuns(): List<String>
  fun selectedText(): String
  fun selectAllText()
  fun clearTextSelection()
fun svgView(svg: String): JadeComponent
```

### text (251; JadeLargeTextView.kt)
Read-only viewer for very large text files, paged with a background line index. Depends on root, `com.jadelib.io` (LineIndex, PagedFile, another module), java.io.
```
class JadeLargeTextView(file: File, var font: Font = TEXT_FONT, eager: Boolean = false) : JadeContent, FocusTarget
  fun startIndexing()
  val isIndexed: Boolean; val lineCount: Long; val byteLength: Long
  fun visibleLines(): List<String>
  fun firstVisibleRow(): Long
  var highlightLine: Long = -1
  var highlightQuery: String?
  fun scrollToLine(line: Long)
  fun showHit(line: Long, query: String?)
  fun clearHit()
fun largeTextView(file: File): JadeComponent
```

### treemap (552; Treemap 200, TreemapRenderer 223, TreemapView 129)
Squarified treemap layout, an image renderer and an interactive view. Depends on root and java.awt.
```
class TreemapNode(val label: String, val weight: Long, val children: List<TreemapNode> = emptyList(), val color: Color? = null,
    val payload: Any? = null)
  val isLeaf: Boolean
class TreemapTile(val node: TreemapNode, val bounds: Rectangle, val depth: Int)
object Treemap
  fun layout(root: TreemapNode, bounds: Rectangle, minSide: Int = 2, padding: Int = 1, maxDepth: Int = 12): List<TreemapTile>
object TreemapRenderer
  fun render(tiles: List<TreemapTile>, width: Int, height: Int, colorOf: (TreemapTile) -> Color,
      background: Color = Color(0x20, 0x20, 0x20), labelFor: ((TreemapTile) -> String?)? = null): BufferedImage
class TreemapView(root: TreemapNode) : JadeContent, OwnedContent, TooltipProvider, CursorProvider
  var root [ps]
  var colorOf: (TreemapTile) -> Color
  var labelFor: ((TreemapTile) -> String?)?
  var tooltipFor: (TreemapTile) -> String?
  var onSelect: ((TreemapTile) -> Unit)?
  var hovered: TreemapTile? [ps]
  fun setRoot(newRoot: TreemapNode)
  fun tiles(): List<TreemapTile>
  fun at(x: Int, y: Int): TreemapTile?
fun treemap(root: TreemapNode): JadeComponent
```

## 3. `object JadeUi` (`Overrides.kt:14`; verified against dump lines 1560–1623)
```
var overrideRoot: java.nio.file.Path? = null
var scheduler: ((delayMs: Long, task: () -> Unit) -> Unit)? = null
var clipboard: JadeClipboard = MemoryClipboard()
var shortcutDown: Boolean = false
fun invalidateRenderCaches()
fun onRepaintNeeded(hook: () -> Unit)
fun removeRepaintHook(hook: () -> Unit)
fun requestRepaint()
fun onLayoutNeeded(hook: () -> Unit)
fun removeLayoutHook(hook: () -> Unit)
fun clearHostHooks()
fun requestLayout()
var fileDialog: JadeFileDialog = NoFileDialog()
var inputDialog: JadeInputDialog = JadeDialogs.input
var choiceDialog: JadeChoiceDialog = JadeDialogs.choice
var checklistDialog: JadeChecklistDialog = JadeDialogs.checklist
var findDialog: JadeFindDialog = JadeDialogs.find
var formDialog: JadeFormDialog = JadeDialogs.form
var optionsDialog: JadeOptionsDialog = JadeDialogs.options
var sourceWindow: JadeSourceWindow = JadeDialogs.source
var imageDialog: JadeImageDialog = JadeDialogs.image
var windowOpener: JadeWindowOpener = NoWindowOpener()
val drafts: JadeDrafts = JadeDrafts()
fun openDialogs(): List<JadeDialogHandle<*>>
fun closeAllDialogs(): Unit
fun onFrameNeeded(hook: () -> Unit)
fun removeFrameNeededHook(hook: () -> Unit)
fun requestFrame()
fun onFrame(hook: (FrameTime) -> Unit)
fun removeFrameHook(hook: (FrameTime) -> Unit)
val wantsFrames: Boolean
fun runFrameHooks(nowNanos: Long)
var navigate: ((forward: Boolean) -> Unit)? = null
fun onPointerPressed(listener: (x: Int, y: Int) -> Unit)
fun removePointerPressed(listener: (x: Int, y: Int) -> Unit)
fun pointerPressed(x: Int, y: Int)
fun onOverlayNeeded(hook: (comp: JadeComponent?, x: Int, y: Int) -> Unit)
fun showOverlay(comp: JadeComponent?, x: Int = 0, y: Int = 0)
fun onTipNeeded(hook: (comp: JadeComponent?, x: Int, y: Int) -> Unit)
fun showTip(comp: JadeComponent?, x: Int = 0, y: Int = 0)
fun onFocusRequested(hook: (JadeComponent) -> Unit)
fun requestFocus(comp: JadeComponent)
fun onPointerExit(hook: () -> Unit)
fun pointerExited()
internal: register(id: String, grid: JadeGrid), registeredGrids(), ensureCommandsProcessed()
```
`onOverlayNeeded`, `onTipNeeded`, `onFocusRequested` and `onPointerExit` each hold a single hook (assignment). The other `on*` functions add to lists and have matching `remove*` functions.

## 4. Top-level factory functions (all return `JadeComponent` unless noted)

Root:
```
label(text: String)
button(text: String, onClick: () -> Unit = {})
swing(component: JComponent)
textField(text: String = "")
textField(model: SharedTextModel)
grid(id: String? = null, build: JadeGrid.() -> Unit)
block(source: String, label: String, onEdit: (String) -> Unit = {}, view: (String) -> JadeComponent)
tree(roots: List<TreeNode>, onSelect: (TreeNode) -> Unit = {})
table(columns: List<String>, rows: List<List<String>>, editable: Boolean = false,
      onEdit: (row: Int, col: Int, value: String) -> Unit = { _, _, _ -> }, onSelect: (Int) -> Unit = {})
comboBox(items: List<String>, selected: Int = 0, editable: Boolean = false, onSelect: (Int) -> Unit = {})
checkBox(label: String, checked: Boolean = false, onChange: (Boolean) -> Unit = {})
radioButton(label: String, value: Int, group: RadioGroup)
slider(value: Double = 0.0, onChange: (Double) -> Unit = {})
progressBar(value: Double = 0.0)
listBox(items: List<String>, selected: Int = -1, onSelect: (Int) -> Unit = {})
toggle(checked: Boolean = false, onChange: (Boolean) -> Unit = {})
spinner(value: Int = 0, min: Int = 0, max: Int = 100, step: Int = 1, onChange: (Int) -> Unit = {})
doc(blocks: List<JadeComponent>)
screen(content: JadeComponent, menuBar: JadeMenuBar? = null)
scroll(child: JadeComponent)
split(first: JadeComponent, second: JadeComponent, vertical: Boolean = false, ratio: Double = 0.5)
tabs(vararg tabs: Pair<String, JadeComponent>)
textArea(text: String = "")
textArea(model: SharedTextModel)
wrappedLabel(text: String, font: Font = DEFAULT_FONT)
iconButton(image: java.awt.image.BufferedImage, onClick: () -> Unit = {})
toolbarSeparator()
jadeHost(root: JadeComponent, title: String = "jadelib-ui", initialWidth: Int = 900, initialHeight: Int = 640,
         exitOnClose: Boolean = true, menuBar: JadeMenuBar? = null, onClose: () -> Unit = {},
         onFilesDropped: (List<File>) -> Unit = {}, onFilesDroppedAt: ((List<File>, Int, Int) -> Boolean)? = null,
         icons: List<BufferedImage> = emptyList(), onFront: () -> Unit = {}): JadeHost
```
Sub-packages:
```
chart:   chartView(data: ChartData)
         barChart(values: List<Double>, labels: List<String> = emptyList(), title: String = "", colorByCategory: Boolean = false)
         lineChart(vararg series: ChartSeries, labels: List<String> = emptyList(), title: String = "")
         pieChart(values: List<Double>, labels: List<String> = emptyList(), title: String = "")
code:    codeView(source: String, lang: String? = null)
         codeColor(role: CodeHighlighter.Role, dark: Boolean): Color?          (helper, not a factory)
doc:     docView(items: List<DocItem>)
         htmlView(html: String, css: String, dark: Boolean = false, viewportWidth: Int? = null, targetId: String? = null,
                  ancestors: List<Css.Elem> = emptyList(), counterStart: Map<String, Int> = emptyMap())
         proseView(html: String, styler: ProseStyler = DefaultProseStyler())
         markdownView(md: String, css: String = DEFAULT_MARKDOWN_CSS)
hex:     hexView(source: ByteSource, editable: Boolean = false, onChange: () -> Unit = {})
         hexView(bytes: ByteArray)
         hexEditor(bytes: ByteArray, onChange: () -> Unit = {})
image:   imageView(src: String?, width: Int = 0, height: Int = 0, onResize: (Int, Int) -> Unit = { _, _ -> })
map:     mapView(points: List<MapPoint> = emptyList())
math:    mathView(expr: String)
         mathBlock(source: List<String>, onEdit: (line: Int, source: String) -> Unit = { _, _ -> })
sheet:   sheet(rows: Int = 20, cols: Int = 8, onEdit: (row: Int, col: Int, value: String) -> Unit = { _, _, _ -> })
         formulaBar(onCommit: (String) -> Unit = {})
svg:     svgView(svg: String)
text:    largeTextView(file: File)
treemap: treemap(root: TreemapNode)
```
- No factory exists in `gl`, `css`, `html`, `icons` or `dialogs`, nor for `JadePictureViewer`/`JadePictureEditor` (use `.component`) or `JadeCodeView` beyond `codeView`.
- Fluent builders on `JadeComponent`: `withId(id)`, `tooltip(text)`, `background(color)`, `foreground(color)`, `preferredSize(size)`, `minimumSize(size)`, `maximumSize(size)`.
- `DocItem.prose(...)` and `DocItem.block(component, source)` are companion factories.

## 5. Overloaded names (true Kotlin overloads, derived from the dump)

Top-level:
```
textField(text: String = "")  |  textField(model: SharedTextModel)
textArea(text: String = "")   |  textArea(model: SharedTextModel)
hexView(source: ByteSource, editable: Boolean = false, onChange: () -> Unit = {})  |  hexView(bytes: ByteArray)
JadeComponent.renderToImage(width: Int, height: Int)  |  JadeComponent.renderToImage(width: Int, height: Int, scaleX: Double, scaleY: Double)
```
Members:
```
JadeCanvas.drawImage(image, x, y)  |  drawImage(image, x, y, w, h)                (same pair on RecordingCanvas and AwtCanvas)
Palette.bg(rgb: Int)  |  bg(rgb: Int, themed: Boolean)
Palette.fg(rgb: Int)  |  fg(rgb: Int, themed: Boolean)
SharedTextModel.setText(value: String)  |  setText(value: String, objects: List<String>)
JadeFileDialog.open(title, filters): String? [DEPRECATED]                  |  open(title, filters, onResult: (String?) -> Unit)
JadeFileDialog.save(title, suggestedName, filters): String? [DEPRECATED]   |  save(title, suggestedName, filters, onResult)
JadeFileDialog.chooseDirectory(title): String? [DEPRECATED]                |  chooseDirectory(title, onResult)
JadeInputDialog.prompt(title, message, initial): String? [DEPRECATED]      |  prompt(spec: PromptSpec, onResult)
JadeInputDialog.promptValueAndFormat(title, value, format, onChange, links, valueNote, formatNote, formatNames, valueCompleter) [DEPRECATED]
                                                                           |  promptValueAndFormat(spec: ValueFormatSpec, onResult)
JadeChoiceDialog.choose(title, message, options): Int? [DEPRECATED]        |  choose(spec: ChoiceSpec, onResult)
JadeChoiceDialog.pick(title, message, options, initial): Int? [DEPRECATED] |  pick(spec: PickSpec, onResult)
JadeChecklistDialog.check(title, message, items, checked) [DEPRECATED]     |  check(spec: ChecklistSpec, onResult)
JadeFormDialog.edit(spec): FormResult? [DEPRECATED]                        |  edit(spec, onResult)
JadeOptionsDialog.show(spec): OptionsResult? [DEPRECATED]                  |  show(spec, onResult)
JadeImageDialog.choose(renderPreview): ImageChoice? [DEPRECATED]           |  choose(spec: ImageSpec, onResult)
JadeSheet.setCell(r, c, value: String)  |  setCell(r, c, cell: SheetCell)
JadeSheet.rowHeightForTest()  |  rowHeightForTest(r: Int)
DocItem.prose(view: JadeProseView)  |  DocItem.prose(html: String, styler: ProseStyler = DefaultProseStyler())
JadeHtmlView.rectOf(el: Html.El, width: Int)  |  rectOf(path: List<Int>, width: Int)
```
The same overload pairs repeat on the `No*Dialog` implementations. `JadeFindDialog` and `JadeSourceWindow` are not overloaded: the deprecated `open(spec)` and the new `show(spec)` have different names.

Overloaded constructors:
```
JadeHtmlView(root: Html.El, sheet: Css.StyleSheet, viewportWidth, targetId, ancestors, counterStart)
    |  JadeHtmlView(html: String, css: String, dark, viewportWidth, targetId, ancestors, counterStart)
CompletionPopup(display: Display)  |  CompletionPopup(anchor: () -> Pair<Int, Int>?)
ShapeRef(layer: PictureLayer, shape: MarkShape)  |  ShapeRef(layer: PictureLayer, index: Int)
PictureEdit(layers: List<PictureLayer>)  |  PictureEdit(base: BufferedImage, shapes: List<MarkShape> = emptyList())
```

Not overloads in Kotlin, but they collide in a flat or dynamic namespace:
- **Property and method with the same name on one class:** `JadeComponent.tooltip` / `tooltip(text)`, `background` / `background(color)`, `foreground` / `foreground(color)`.
- **Property plus explicit `setX` method** (a setter-generating port would clash):
  - `SharedTextModel`: `caretVisible` / `setCaretVisible`, `focused` / `setFocused`, `styleRuns` / `setStyleRuns`, `text` / `setText`
  - `JadeBlock.source` / `setSource`; `JadeTable.rows` / `setRows`; `JadeDoc.blocks` / `setBlocks`
  - `JadeChartView.data` / `setData`; `JadeMathView.expr` / `setExpr`; `TreemapView.root` / `setRoot`
  - `JadeDocView.items` / `setItems`; `JadeFormulaBar.text` / `setText`; `SvgRenderer.theme` / `setTheme`; `JadeScreen.setMenuBar`
- **`open` as both property and function:** `JadeDialogHandle.open` is a `Boolean` val; `JadeFileDialog.open(...)` and `JadeWindowOpener.open(spec)` are functions.
- **Same function name in different scopes:**
  - `layout`: `JadeComponent.layout(width, height)` extension, `JadeGrid.layout(width, height)`, `JadeChartView.layout()`, `MathTypeset.layout(...)`, `Treemap.layout(...)`
  - `block`: top-level and `DocItem.block`
  - `table`: top-level and `ToolbarIcons.table`
  - `showTip`: `JadeUi.showTip(comp?, x = 0, y = 0)` and `JadeScreen.showTip(comp, x, y)`
  - `pointerExited`, `requestRepaint`, `requestLayout`, `requestFrame`: each on several types
- **Factory names equal to package names:** `doc`, `sheet`, `treemap`.
- **Nested type names reused:** `Text` (`RawInput.Text`, `DrawOp.Text`, `Html.Text`), `Wheel` and `Hover` (`RawInput` and `JadeEvent`), `Fill` (root enum and `PictureEdit.Effect.Fill`), `Parts` (`JadeHtmlView.Parts`, `FormulaSplit.Parts`), `Role` (`CodeHighlighter.Role`).
- **Two key enums:** `Key` and `KeyCode`.

## 6. JDK/AWT types in public signatures

Counts are occurrences in the dump's member descriptors. They include getters, setters, constructors and data-class `copy`/`componentN`, so they overstate distinct uses by roughly 2–4×.
```
java.awt.Color                 293   everywhere (styles, Palette, canvas, themes, chart, sheet, doc, image, map, treemap)
java.awt.Font                  109   every text widget ctor and property, JadeCanvas.font/measure, elide, SheetTheme, stylers
java.awt.image.BufferedImage    76   JadeCanvas.drawImage, render* extensions, RenderLoop, JadeHostSpec.icons, JadeIconButton,
                                     ImageSpec.renderPreview, ToolbarIcons, TreemapRenderer, GlOffscreen, all of image/
java.awt.Rectangle              33   JadeComponent.bounds, JadeCanvas.visibleBounds, RenderLoop, RawInputRouter, JadeScreen bounds,
                                     GlViewPlacement, JadeHtmlView rects, TreemapTile/Treemap.layout, PictureEdit
java.awt.Dimension              30   Sizes(min, pref, max), JadeComponent size overrides and fluent setters, InlineObjects.size,
                                     JadeGlView.preferredSize
java.io.File                     8   JadeHostSpec/jadeHost onFilesDropped(At), CodeHighlighter lang dirs, JadeLargeTextView/largeTextView
java.awt.Graphics2D              8   ChartRenderer.render, SvgRenderer.render, MathTypeset.Box.paint, MarkShape.draw, PictureTool.preview
java.awt.Shape                   6   ChartHit.shape, Floating.outline, PictureEdit.selectionOutline/strokeArea
java.awt.geom.Point2D.Double     5   JadePictureCanvas.toPicture, Floating.local/picture (plus erased List<Point2D.Double> in MarkShape.points,
                                     PictureEdit.stroke, Effect.Stroke)
java.awt.Insets                  4   JadeGrid ctor and insets property
javax.swing.JComponent           3   SwingContent(swing), swing(component)
java.awt.geom.Rectangle2D        3   SvgRenderer.TextHit.bounds, MarkShape.bounds
java.nio.file.Path               2   JadeUi.overrideRoot
java.net.URI                     2   JadeImageView ctor (base)
java.awt.geom.AffineTransform    1   Floating.transform
java.awt.Point                   1   JadeComponent.windowPosition() (also erased in PictureTool.patch(): Pair<BufferedImage, Point>)
```
- **Not visible in the dump because of erasure:** `java.awt.FontMetrics` in `MathTypeset.layout(expr, baseFont, metrics: (Font) -> FontMetrics)`; `File` and `BufferedImage` inside `List<>` and lambda types.
- **Kotlin and JVM basics also counted:** String 968, List 360, Map 65, Set 36, CharSequence 9 (TextNav), Collection 2, boxed Integer/Float/Character for nullable primitives.
- **Other-module types:** `com.jadelib.io.ByteSource` (`JadeHexView`, `hexView`), and internally `InMemoryByteSource`, `LineIndex`, `PagedFile`.
- **`java.lang.Enum` supertypes:** 19 public enums.

## Deprecated members (complete; all in `Chrome.kt`, mirrored on the `No*` and `dialogs` implementations)
- `JadeFileDialog`: `open(title, filters)`, `save(title, suggestedName, filters)`, `chooseDirectory(title)` — the blocking forms.
- `JadeInputDialog`: `prompt(title, message, initial)`, `promptMultiline(title, initial)`, `promptMultilineLive(...)`, `promptValueAndFormat(title, value, format, onChange, ...)`.
- `JadeChoiceDialog`: `choose(title, message, options)`, `pick(title, message, options, initial)`, `pickNamed(...)`.
- `JadeChecklistDialog`: `check(title, message, items, checked)`.
- `JadeFindDialog.open(spec)`; `JadeSourceWindow.open(spec)`.
- `JadeFormDialog.edit(spec)`; `JadeOptionsDialog.show(spec)`; `JadeImageDialog.choose(renderPreview)`.

## Public types an app cannot construct (internal constructors)
`CapturedTarget`, `RecordingCanvas`, `JadeGrid.Cell`, `JadeGrid.RowScope`, `JadeTabs.Tab`, `RenderLoop.Frame`, `DocItem` (use `DocItem.prose` / `DocItem.block`), `JadeHtmlView.Parts`, `image.Floating`.