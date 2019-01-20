#include <stdlib.h>
#include <string.h>

#include "openssl_stack_allocator.h"

#ifdef OPENSSLSTACKALLOC_DEBUG
	#define  OPENSSLSTACKALLOC_PASS_FILE_LINE , file, line
#else
	#define  OPENSSLSTACKALLOC_PASS_FILE_LINE
#endif


namespace OpenSSLStackAllocator
{

// Pointer to the active arena, if any
static thread_local StackArena *active_stack_arena = nullptr;

class Internal
{
public:
	static inline void Overflow( StackArena *arena, size_t sz )
	{
		arena->overflow_total += sz;
		if ( arena->overflow_max_size < sz )
			arena->overflow_max_size = sz;
	}

	static inline void SetTop( StackArena *arena, char *top )
	{
		arena->top = top;
		size_t used = top - arena->begin;
		if ( arena->high_water_mark < used )
			arena->high_water_mark = used;
	}

	static void *AllocFromArena( StackArena *arena, size_t sz )
	{
		// Align requests
		sz = (sz + 7) & ~7;

		// Add on space needed for the bookkeeping at the beginning.
		size_t total_sz = sz + sizeof(size_t);

		// Does it fit?
		char *new_top = arena->top + total_sz;
		if ( new_top > arena->end )
		{

			// Doesn't fit
			Overflow( arena, sz );

			return nullptr;
		}

		// Give them the next chunk
		size_t *block = (size_t*)arena->top;
		SetTop( arena, new_top );
		++arena->active_allocations;

		// Save the size of the block, return a pointer to their space
		*block = sz;
		return block+1;
	}

	static void *ReallocFromArena( StackArena *arena, void *ptr, size_t sz  OPENSSLSTACKALLOC_DECLARE_FILE_LINE )
	{

		// Check for a special case of reallocating the last block.
		// We can actually be smart here.
		size_t *block = (size_t*)ptr - 1;
		size_t old_sz = *block;
		void *result = nullptr;
		if ( (char*)ptr + old_sz == arena->top )
		{

			// Align requests
			sz = (sz + 7) & ~7;

			// Will it fit?
			size_t total_sz = sz + sizeof(size_t);
			char *new_top = (char*)block + total_sz;
			if ( new_top <= arena->end )
			{

				// Extend/shrink the block, and remember the new size
				SetTop( arena, new_top );
				*block = sz;
				return ptr;
			}

			// Doesn't fit.
			Overflow( arena, sz );
		}
		else
		{

			// Block being resized is in the middle.  If we are shrinking it,
			// we can do that!  But we cannot recover the space
			if ( sz <= old_sz )
				return ptr;

			// Growing a block in the middle.  We can't be smart.
			// Try to allocate a new block from the end
			result = AllocFromArena( arena, sz );
		}

		// Did we fail to get data from this arena?
		if ( !result )
		{
			result = (*heap_malloc_func)( sz  OPENSSLSTACKALLOC_PASS_FILE_LINE );
			if ( !result )
				return nullptr;
		}

		// Copy the data to the new location
		// and free from the arena
		memcpy( result, ptr, old_sz );
		FreeFromArena( arena, ptr );
		return result;
	}

	static StackArena *FindArenaOwner( void *ptr )
	{
		StackArena *arena = active_stack_arena;
		while (arena)
		{
			// Pointer in our range?
			if ( ptr >= arena->begin && ptr < arena->end )
			{

				// Looks like it belongs to us.  Check for a bug
				size_t *block = (size_t*)ptr - 1;
				if ( (char*)block < arena->begin || (char*)ptr + *block > arena->end )
				{
					// Memory corruption, or pointer doesn't point to the beginning of
					// a block.
					(*bug_func)();
				}
				else if ( arena->active_allocations <= 0 )
				{
					(*bug_func)();
				}

				return arena;
			}

			// Look up the all stack, if any.
			// (This list will usually very very short,
			// and probably will only have a single element.)
			arena = arena->prev_arena;
		}

		// Memory doesn't belong to any active arena
		return nullptr;
	}

	static inline void FreeFromArena( StackArena *arena, void *ptr )
	{
		--arena->active_allocations;

		// Check for the special case of freeing the last block,
		// we can actually free up the memory
		size_t *block = (size_t*)ptr - 1;
		if ( (char*)ptr + *block == arena->top )
			arena->top = (char*)block;
	}
};

static void *DefaultHeapMalloc( size_t sz  OPENSSLSTACKALLOC_DECLARE_FILE_LINE )
{
	// Oh hey on MSVC (and other platforms?) we could call the
	// debug versions and pass thorugh the file and line
	#ifdef OPENSSLSTACKALLOC_DEBUG
		(void)file;
		(void)line;
	#endif
	return malloc(sz);
}

static void *DefaultHeapRealloc( void *ptr, size_t sz  OPENSSLSTACKALLOC_DECLARE_FILE_LINE )
{
	#ifdef OPENSSLSTACKALLOC_DEBUG
		(void)file;
		(void)line;
	#endif
	return realloc(ptr, sz);
}

static void DefaultHeapFree( void *ptr  OPENSSLSTACKALLOC_DECLARE_FILE_LINE )
{
	#ifdef OPENSSLSTACKALLOC_DEBUG
		(void)file;
		(void)line;
	#endif
	return free(ptr);
}

void *(*heap_malloc_func)( size_t sz  OPENSSLSTACKALLOC_DECLARE_FILE_LINE ) = DefaultHeapMalloc;
void *(*heap_realloc_func)( void *ptr, size_t sz  OPENSSLSTACKALLOC_DECLARE_FILE_LINE ) = DefaultHeapRealloc;
void (*heap_free_func)( void *ptr  OPENSSLSTACKALLOC_DECLARE_FILE_LINE ) = DefaultHeapFree;
void (*bug_func)() = nullptr;
void (*overflow_func)( size_t arena_size, size_t alloc_size ) = nullptr;

void *Malloc( size_t sz  OPENSSLSTACKALLOC_DECLARE_FILE_LINE )
{

	// Any active arena?
	StackArena *arena = active_stack_arena;
	if ( arena )
	{

		// Try allocating from the arena
		void *result = Internal::AllocFromArena( arena, sz );
		if ( result )
			return result;

		// Didn't fit, fallback to heap
	}

	// Use default heap allocation
	return (*heap_malloc_func)( sz OPENSSLSTACKALLOC_PASS_FILE_LINE );
}

void *Realloc( void *ptr, size_t sz  OPENSSLSTACKALLOC_DECLARE_FILE_LINE )
{
	// Check for special case where realloc is same as malloc
	if ( !ptr )
		return Malloc( sz OPENSSLSTACKALLOC_PASS_FILE_LINE );

	// Zero size?  This is not actually defined by the spec,
	// but we will treat it as a free call
	if ( sz == 0 )
	{
		Free( ptr OPENSSLSTACKALLOC_PASS_FILE_LINE );
		return nullptr;
	}

	// See if memory block is in an arena
	StackArena *arena = Internal::FindArenaOwner( ptr );
	if ( arena )
		return Internal::ReallocFromArena( arena, ptr, sz OPENSSLSTACKALLOC_PASS_FILE_LINE );

	// Just use regular heap
	return (*heap_realloc_func)( ptr, sz OPENSSLSTACKALLOC_PASS_FILE_LINE );
}

void Free( void *ptr  OPENSSLSTACKALLOC_DECLARE_FILE_LINE )
{
	if ( !ptr )
		return;

	// Did it come from an arena?  If so, free it from arena
	StackArena *arena = Internal::FindArenaOwner( ptr );
	if ( arena )
		Internal::FreeFromArena( arena, ptr );
	else
		(*heap_free_func)( ptr OPENSSLSTACKALLOC_PASS_FILE_LINE );
}


StackArena::StackArena( void *ptr, size_t sz )
: begin( (char*)ptr ), end((char*)ptr + sz), top( (char*)ptr ), prev_arena( active_stack_arena ), active_allocations( 0 )
, high_water_mark( 0 ), overflow_total( 0 ), overflow_max_size ( 0 )
{
	active_stack_arena = this;
}

StackArena::~StackArena()
{
	// Check for lifetime of allocation inside arena living past lifetime
	// of arena
	if ( active_allocations != 0 )
		(*bug_func)();

	// Pop the stack.  We should be on top!
	if ( active_stack_arena != this )
		(*bug_func)();
	active_stack_arena = prev_arena;
}

} // namespace StackAllocator
