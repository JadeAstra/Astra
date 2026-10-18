// Included last, after its other includes, in each file of Jade's window and drawing, the text
// engine and the image codecs: the functions the file defines after it go in a section of the
// executable of their own, which `astra build` leaves out of a program that reaches neither
// Jade nor the text engine (M6.5 part 5, D161). See section_compiler.h.
// No include guard: each file that includes it is to have it.

#if defined(_MSC_VER)
#pragma code_seg(".astraj")
#endif
