#include "types.h"
#include "x86.h"
#include "stdmem.h"
#include "stdarg.h"
#include "stdlib.h"
#include "file.h"
#include "stdlock.h"
#include "chronos.h"

#define M_MAGIC (void*)(0x43524E53)
static int mem_init = 0;
static int starting_mem = 0;

/* A node in the free list. */
typedef struct free_node
{
	int sz; /* The size available here (not including this header) */
	struct free_node* next; /* The next free header in the list.*/
} free_node;

/* An allocated node. */
typedef struct alloc_node
{
	int sz; /* The size of this allocated region. */
	void* magic; /* Make sure this node isn't currupted. */
} alloc_node;

static struct free_node* head; /* The head of the free list */
static struct free_node* curr; /* Pointer to the current location. */
static void (*mem_printf)(char*); 

void* malloc(uint sz)
{
	/* As init gotten called yet? */
	if(!mem_init) msetup();

	/* Do we have any free space? */
	if(head == NULL) 
	{
		if(mem_printf)
			mem_printf("stdmem: cannot alloc: out of mem\n");
		return NULL;
	}
	/* Has the user requested 0 bytes of space? */
	if(sz == 0) return NULL;

	/* Round up to a 4 byte boundary (performance) */
	sz = B4_ROUNDUP(sz);

	/* 
	 * If we get back to start_search, we know that we have 
	 * looped through the list and we should stop. 
	 */
	free_node* start_search = curr;
	int found = 0; /* Did we find a large enough node? */

	/*
	 * Search for a node in the free list that is large
	 * enough to service the user's request.
	 */
	do
	{
		/* If we just passed the last node, start at the beginning. */
		if(curr == NULL) curr = head;

		if(curr->sz >= sz)
		{
			/* We found a large enough node. */
			found = 1;
			break;
		}

		/* The current node is not large enough, iterate forward. */
		curr = curr->next;
	} while(curr != start_search);

	if(found == 0)
	{
		if(mem_printf)
			mem_printf("stdmem: cannot alloc: not enough mem\n");
		/* We don't have a node large enough to service the request. */
		return NULL;
	}

	/* We did find a large enough node. */
	int remaining_bytes = curr->sz - sz - sizeof(free_node);
	alloc_node* new_header = NULL;
	if(remaining_bytes < 0)
	{
		/* We are just going to allocate the entire node */
		/* Theres going to be a broken pointer. */
		sz = curr->sz;
		new_header = (alloc_node*)curr;

		/* If we are the head, there is no broken pointer. */
		/* We must iterate the list forward. */
		if(curr == head)
		{
			head = curr->next;
		} else {
			/* Something was pointing to us. */
			free_node* broken = head;
			while(broken->next != curr) 
				broken = broken->next;

			/* Fix the pointer. */
			broken->next = curr->next;
		}
	} else {
		/* Split the left over memory. */
		curr->sz = remaining_bytes;
		new_header = (alloc_node*)(((uchar*)curr) 
				+ remaining_bytes + sizeof(free_node));
	}

	if(mem_printf)
	{
		mem_printf("stdmem: ");
		char nbuff[64];
		itoa(sz, nbuff, 64, 16);
		mem_printf(nbuff);
		mem_printf("\n");
	}

	/* Assign new header size and magic */
	new_header->sz = sz;
	new_header->magic = M_MAGIC;

	/* Give the user their pointer. */
	uchar* user_mem = (uchar*)new_header;
	return user_mem + sizeof(free_node);
}

int mfree(void* ptr)
{
	if(ptr == NULL) 
	{
		if(mem_printf)
			mem_printf("stdmem: tried to free null pointer\n");
		return 0;
	}
	if(!mem_init) msetup();;

	/* Security: check magic */
	alloc_node* allocated = (alloc_node*)
		(((uchar*)ptr) - sizeof(free_node));
	if(allocated->magic != M_MAGIC)
	{
		if(mem_printf)
			mem_printf("stdmem: Magic number curruption!\n");
		return 1;
	}

	uint sz = allocated->sz;
	free_node* free = (free_node*)allocated;
	free->sz = sz;
	free->next = NULL;

	/* If we didn't have a head, we have one now. */
	if(head == NULL)
	{
		if(mem_printf)
			mem_printf("stdmem: new head assigned.\n");
		head = free;
		return 0;
	}

	/* If ptr is before the head, there is a new head. */
	uint head_i = (uint)head;
	uint free_i = (uint)allocated;

	if(free_i < head_i)
	{
		free->next = head;
		head = free;

		/* Can we merge the new head with the old head? */
		if((char*)free + sizeof(struct free_node) + free->sz 
				== (char*)free->next)
		{	
			if(mem_printf)
				mem_printf(
						"stdmem: Trying to merge new head.\n");
			free->sz += sizeof(struct free_node) 
				+ free->next->sz;
			free->next = free->next->next;
		}
		return 0;
	}

	/* We will assume everything is ok. */
	free_node* above = NULL; /* The node above us in the address space. */
	free_node* below = NULL; /* The node below us in the address space. */
	free_node* curr_search = head;

	/* Find the node above and below the new free node. */
	while(curr_search)
	{
		below = curr_search;
		uint below_node = (uint)curr_search;
		uint above_node = (uint)curr_search->next;
		if((below_node < free_i && above_node > free_i)
				|| above_node == 0)
		{
			above = below->next;
			break;
		}

		/* We didn't find the above and below nodes, keep searching. */
		curr_search = curr_search->next;
	}

	/* Adjust the list. */
	below->next = free;
	free->next = above;

	/* Try to merge below with current */
	uint below_i = (uint)below;
	if(below_i + sizeof(free_node) + below->sz == (uint)free)
	{
		if(mem_printf)
			mem_printf("stdmem: merged below.\n");
		/* We can merge with the node below us */
		below->sz += sizeof(free_node) + free->sz;
		below->next = free->next;

		/* Clear this node (Security) */
		free->next = NULL;
		free->sz = (uint)-1;
		free = below;
	}

	/* Try to merge above with current */
	if(above && free_i + sizeof(free_node) + free->sz == (uint)above)
	{
		if(mem_printf)
			mem_printf("stdmem: merged above.\n");
		/* We can merge with the node above us. */
		free->next = above->next;
		free->sz += sizeof(free_node) + above->sz;

		/* Clear above node (Security) */
		above->next = NULL;
		above->sz = (uint)-1;
	}

	if(mem_printf)
	{
		uint available = 0;
		struct free_node* n = head;
		while(n)
		{
			available += n->sz;
			n = n->next;
		}
		char nbuff[64];
		itoa(available, nbuff, 64, 16);
		mem_printf("stdmem: ");
		mem_printf(nbuff);
		mem_printf(" bytes available.\n");
	}

	return 0;
}

void minit(uint start_addr, uint end_addr)
{
	if(mem_printf)mem_printf("stdmem: running minit.\n");
	if(sizeof(struct free_node) != sizeof(struct alloc_node))
	{
		if(mem_printf)
			mem_printf("stdmem: allocator currupt!\n");
		for(;;);
	}

	/* total_bytes is the amount of bytes we were given */
	uint total_bytes = end_addr - start_addr;
	starting_mem = total_bytes;
	/* Start of the free list were creating */
	free_node* start_list = (free_node*)start_addr;
	start_list->sz = B4_ROUNDDOWN(total_bytes - sizeof(struct free_node));
	start_list->next = NULL; /* The only element in the list */

	head = start_list; /* Assign head */
	curr = start_list; /* Assign current free node */

	mem_init = 1; /* We have initilized the free list */
}




/* Set debug */
void mem_debug(void (*f)(char*))
{
	mem_printf = f;
	if(mem_printf)
		mem_printf("stdmem: memory debugging enabled.\n");
}
