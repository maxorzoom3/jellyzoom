/* Link-time names for the system's libSceVideodec2 (never run; the console's module is loaded at run time with
 * sceSysmoduleLoadModule(207) and these imports bind to it). Names as in EVO Player / Jelly5. */
#include <stdint.h>
#define F(n, ...) int32_t n(__VA_ARGS__) { return -1; }
F(sceVideodec2QueryComputeMemoryInfo, void *a) F(sceVideodec2AllocateComputeQueue, const void *a, const void *b, void **c)
F(sceVideodec2ReleaseComputeQueue, void *a) F(sceVideodec2QueryDecoderMemoryInfo, const void *a, void *b)
F(sceVideodec2CreateDecoder, const void *a, const void *b, void **c) F(sceVideodec2DeleteDecoder, void *a)
F(sceVideodec2MapDirectMemory, void *a, const void *b) F(sceVideodec2Reset, void *a)
F(sceVideodec2Decode, void *a, void *b, void *c, void *d) F(sceVideodec2Flush, void *a, void *b, void *c)
