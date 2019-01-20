// #define OPENSSLSTACKALLOC_DEBUG if you are using the debug versions
// of the functions which pass a source file and line number
#define OPENSSLSTACKALLOC_DEBUG
#ifdef OPENSSLSTACKALLOC_DEBUG
	#define  OPENSSLSTACKALLOC_DECLARE_FILE_LINE , const char *file, int line
#else
	#define  OPENSSLSTACKALLOC_DECLARE_FILE_LINE
#endif

// This is a system for optimizing the allocations OpenSSL makes in its
// crypto library.  These allocations have the property that their
// lifetime can be bounded to a function call, and thus we can make
// them on the stack.
namespace OpenSSLStackAllocator
{

class Internal;

// Replacements for the standard C memory allocation functions.  You can
// pass these to CRYPTO_set_mem_functions.  They are threadsafe.  When an
// allocation request is made, we check if an arena is active, and if so,
// try to allocate from the arena.  If no arena is active, or the arena
// is full, we fallback to the heap.  (More specifically, we use the
// heap_xxx_func function pointers below.)
extern void *Malloc( size_t sz  OPENSSLSTACKALLOC_DECLARE_FILE_LINE );
extern void *Realloc( void *ptr, size_t sz  OPENSSLSTACKALLOC_DECLARE_FILE_LINE );
extern void Free( void *ptr  OPENSSLSTACKALLOC_DECLARE_FILE_LINE );

// These functions are used when memory is not allocated on the stack.
// When does this happen?
//
// - When no arena is active
// - When the arena overflows
//
// You can set them to come from your own allocator.  (E.g. the functions
// you would previously have passed to CRYPTO_set_mem_functions.)  By
// default they are assigned to functions that use the corresponding
// default CRT functions.
extern void *(*heap_malloc_func)( size_t sz  OPENSSLSTACKALLOC_DECLARE_FILE_LINE );
extern void *(*heap_realloc_func)( void *ptr, size_t sz  OPENSSLSTACKALLOC_DECLARE_FILE_LINE );
extern void (*heap_free_func)( void *ptr  OPENSSLSTACKALLOC_DECLARE_FILE_LINE );

// This function is called when any sort of bad access pattern occurs
// that is likely to mean we have corrupted memory already, or are about to.
// The default implementation is NULL, but we always call this function,
// so we will crash.  (Crashing is probably what you want, the only question
// is whether you want to do any extra logging first.)
extern void (*bug_func)();

// Base class for an active arena on the stack.
// If the arena size is variable, you can use it directly,
// like this:
//
//   StackArena arena( alloca(N), N );
//
// If it's fixed, using the fixed-size template version
// is probably better:
//
//   StackArenaFixed<1024> arena;
//
// Note that this is an ARENA-style allocator, not a local heap.  The whole
// point of this class is to make these temporary allocations super fast.
// When memory is "freed", all we do is decrement a counter of the active
// allocations, we do not actually "free" any memory.  So the total number
// of allocations needs to fit into the arena.
class StackArena
{
public:
	StackArena( void *ptr, size_t sz );
	~StackArena();

	// High water mark for number of bytes used in the arena.
	size_t high_water_mark;

	// Allocations that were made and fell back
	// to the heap because they didn't fit
	size_t overflow_total, overflow_max_size;
private:
	friend class Internal;
	StackArena *prev_arena;
	char *const begin;
	char *const end;
	char *top;
	int active_allocations;
};

// Fixed size stack arena
template< int SizeInBytes >
class StackArenaFixed : public StackArena
{
public:
	StackArenaFixed() : StackArena( arena, sizeof(arena) ) {}
private:
	// Declare arena in terms of larger units, so that we'll be aligned
	long long arena[ ( SizeInBytes + sizeof(long long) - 1 ) / sizeof(long long) ];
};

} // namespace StackAllocator
