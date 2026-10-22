// What Grid<T> (src/std/grid.astra, D155) is built on: the built-in __grid. A grid keeps its
// cells row by row in a List<T>, packed by T as any list is (a Color 4 bytes a cell, a bool 1);
// the list is made here from the grid's own type argument, which the object keeps, since a list
// made in generic code would not know T. The bulk work (filling, copying a region, writing one
// in) is done here a row at a time.

#include <cstring>

#include "builtins.h"
#include "vm.h"

namespace astra {

namespace {

ObjList* listArgument(VM& vm, Value v) {
    if (!v.isObjType(ObjType::List)) throw vm.runtimeError(Thrown::ClassCastException, "expected a grid's cells");
    return asList(v);
}

size_t sizeArgument(VM& vm, Value v, const char* what) {
    const int64_t n = vm.toInt(v, what);
    if (n < 0) throw vm.runtimeError(Thrown::IllegalArgumentException, std::string(what) + " is negative");
    return static_cast<size_t>(n);
}

// `count` copies of `value` added to the end of `list`.
void appendCopies(VM& vm, ObjList* list, size_t count, Value value) {
    if (count == 0) return;
    const size_t start = list->size();
    vm.listInsert(list, start, &value, 1);
    if (!list->packed()) {
        list->values().resize(start + count, list->values()[start]);
        vm.account((count - 1) * sizeof(Value));
        return;
    }
    PackedItems& pack = *list->pack;
    const size_t width = pack.width;
    pack.block.resize((start + count) * width);
    for (size_t i = 1; i < count; i++) std::memcpy(pack.block.data() + (start + i) * width, pack.block.data() + start * width, width);
    if (pack.nullable) {
        const bool isNull = vm.listGet(list, start).isNull();
        pack.nulls.resize((start + count + 63) / 64, 0);
        for (size_t i = start + 1; i < start + count; i++) {
            if (isNull) pack.nulls[i / 64] |= uint64_t{1} << (i % 64);
            else pack.nulls[i / 64] &= ~(uint64_t{1} << (i % 64));
        }
    }
    pack.count = start + count;
    vm.account((count - 1) * width);
}

// `count` cells of the item type's zero added: 0, 0.0, false, a value of zeros (a clear Color),
// null where the type takes it. False where the type has no zero (a grid of objects).
bool appendZeros(VM& vm, ObjList* list, size_t count) {
    if (count == 0) return true;
    if (list->packed()) {
        PackedItems& pack = *list->pack;
        const size_t start = pack.count;
        pack.block.resize((start + count) * pack.width, 0);
        if (pack.nullable) {
            // The zero of a type that takes null is null.
            pack.nulls.resize((start + count + 63) / 64, 0);
            for (size_t i = start; i < start + count; i++) pack.nulls[i / 64] |= uint64_t{1} << (i % 64);
        }
        pack.count = start + count;
        vm.account(count * pack.width);
        return true;
    }
    const ObjTypeInfo* item = list->type != nullptr && list->type->args.size() == 1 ? list->type->args[0] : nullptr;
    if (item != nullptr && !item->nullable && item->id != 0) return false;
    list->values().resize(list->values().size() + count, Value::null());
    vm.account(count * sizeof(Value));
    return true;
}

// Rows `y` to `y + h` of the columns `x` to `x + w` of a grid `srcWidth` wide, added to the end
// of `to`.
void appendRegion(VM& vm, ObjList* to, ObjList* from, size_t srcWidth, size_t x, size_t y, size_t w, size_t h) {
    for (size_t row = 0; row < h; row++) vm.listCopyRange(to, from, (y + row) * srcWidth + x, w);
}

}  // namespace

void registerGrid(VM& vm) {
    ObjClass* grid = vm.alloc<ObjClass>(vm.intern("Grid"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        grid->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("Grid.") + name, minArgs, maxArgs, fn)));
    };

    // cells(grid, count, fill?): the cells of a new grid, a List of the grid's item type with
    // `count` of them, each `fill`, or the type's zero.
    def("cells", 2, 3, [](VM& vm, int argc, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::Instance)) throw vm.runtimeError(Thrown::ClassCastException, "expected a grid");
        const ObjTypeInfo* type = asInstance(args[0])->type();
        const size_t count = sizeArgument(vm, args[1], "the number of cells");
        ObjList* list = newList(vm);
        TempRoot keep(vm, Value::object(list));
        if (type != nullptr && type->args.size() == 1) vm.typeList(list, vm.typeOf(kTypeList, false, nullptr, {type->args[0]}));
        if (argc > 2) {
            appendCopies(vm, list, count, args[2]);
        } else if (!appendZeros(vm, list, count)) {
            throw vm.runtimeError(Thrown::IllegalArgumentException, "a grid of objects has no zero to start from: give it a fill, Grid(width, height, fill)");
        }
        return Value::object(list);
    });
    // fill(list, count, value): `count` copies of `value` added to the end.
    def("fill", 3, 3, [](VM& vm, int, Value* args) -> Value {
        appendCopies(vm, listArgument(vm, args[0]), sizeArgument(vm, args[1], "the number of cells"), args[2]);
        return Value::null();
    });
    // zeros(list, count): `count` of the item type's zero added; false where it has none.
    def("zeros", 2, 2, [](VM& vm, int, Value* args) -> Value {
        return Value::boolean(appendZeros(vm, listArgument(vm, args[0]), sizeArgument(vm, args[1], "the number of cells")));
    });
    // region(from, fromWidth, x, y, w, h, to): a part of one grid's cells added to another's.
    def("region", 7, 7, [](VM& vm, int, Value* args) -> Value {
        appendRegion(vm, listArgument(vm, args[6]), listArgument(vm, args[0]), sizeArgument(vm, args[1], "width"), sizeArgument(vm, args[2], "x"),
                     sizeArgument(vm, args[3], "y"), sizeArgument(vm, args[4], "width"), sizeArgument(vm, args[5], "height"));
        return Value::null();
    });
    // paste(to, toWidth, x, y, from, fromWidth, w, h): `w` by `h` cells of one grid written into
    // another at (x, y), a row at a time.
    def("paste", 8, 8, [](VM& vm, int, Value* args) -> Value {
        ObjList* to = listArgument(vm, args[0]);
        ObjList* from = listArgument(vm, args[4]);
        const size_t toWidth = sizeArgument(vm, args[1], "width"), x = sizeArgument(vm, args[2], "x"), y = sizeArgument(vm, args[3], "y");
        const size_t fromWidth = sizeArgument(vm, args[5], "width"), w = sizeArgument(vm, args[6], "width"), h = sizeArgument(vm, args[7], "height");
        const bool sameLayout = to->packed() && from->packed() && to->pack->store == from->pack->store && to->pack->width == from->pack->width &&
                                to->pack->itemClass == from->pack->itemClass && to->pack->narrow == from->pack->narrow && !to->pack->nullable &&
                                !from->pack->nullable && to != from;
        for (size_t row = 0; row < h; row++) {
            const size_t at = (y + row) * toWidth + x, src = row * fromWidth;
            if (sameLayout) {
                std::memcpy(vm.itemAt(to, at), vm.itemAt(from, src), w * to->pack->width);
                continue;
            }
            for (size_t col = 0; col < w; col++) vm.listSet(to, at + col, vm.listGet(from, src + col));
        }
        return Value::null();
    });
    // bytes(list): a copy of a packed grid's storage, its cells' bytes one after another.
    def("bytes", 1, 1, [](VM& vm, int, Value* args) -> Value {
        ObjList* list = listArgument(vm, args[0]);
        if (!list->packed())
            throw vm.runtimeError(Thrown::UnsupportedOperationException, "a grid of objects keeps no bytes: only a grid of numbers, bools or value types does");
        ObjBytes* made = vm.alloc<ObjBytes>();
        made->data = list->pack->block;
        made->data.resize(list->pack->count * list->pack->width);
        vm.account(made->data.size());
        return Value::object(made);
    });

    vm.defineGlobal("__grid", Value::object(grid));
}

}  // namespace astra
