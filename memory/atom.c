/*
 * Atom table functions
 *
 * Copyright 1993, 1994, 1995 Alexandre Julliard
 */

/*
 * Warning: The code assumes that LocalAlloc() returns a block aligned
 * on a 4-bytes boundary (because of the shifting done in
 * HANDLETOATOM).  If this is not the case, the allocation code will
 * have to be changed.
 */

#ifndef WINELIB

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "atom.h"
#include "instance.h"
#include "ldt.h"
#include "stackframe.h"
#include "user.h"

#ifdef CONFIG_IPC
#include "dde_atom.h"
#include "options.h"
#endif

#define DEFAULT_ATOMTABLE_SIZE    37
#define MIN_STR_ATOM              0xc000

#define ATOMTOHANDLE(atom)        ((HANDLE)(atom) << 2)
#define HANDLETOATOM(handle)      ((ATOM)(0xc000 | ((handle) >> 2)))

#define HAS_ATOM_TABLE(sel)  \
          ((INSTANCEDATA*)PTR_SEG_OFF_TO_LIN(sel,0))->atomtable != 0)

#define GET_ATOM_TABLE(sel)  ((ATOMTABLE*)PTR_SEG_OFF_TO_LIN(sel, \
          ((INSTANCEDATA*)PTR_SEG_OFF_TO_LIN(sel,0))->atomtable))
		

/***********************************************************************
 *           ATOM_InitTable
 */
static WORD ATOM_InitTable( WORD selector, WORD entries )
{
    int i;
    HANDLE handle;
    ATOMTABLE *table;

      /* Allocate the table */

    handle = LOCAL_Alloc( selector, LMEM_FIXED,
                          sizeof(ATOMTABLE) + (entries-1) * sizeof(HANDLE) );
    if (!handle) return 0;
    table = (ATOMTABLE *)PTR_SEG_OFF_TO_LIN( selector, handle );
    table->size = entries;
    for (i = 0; i < entries; i++) table->entries[i] = 0;

      /* Store a pointer to the table in the instance data */

    ((INSTANCEDATA *)PTR_SEG_OFF_TO_LIN( selector, 0 ))->atomtable = handle;
    return handle;
}


/***********************************************************************
 *           ATOM_Init
 *
 * Global table initialisation.
 */
WORD ATOM_Init()
{
    return ATOM_InitTable( USER_HeapSel, DEFAULT_ATOMTABLE_SIZE );
}


/***********************************************************************
 *           ATOM_GetTable
 *
 * Return a pointer to the atom table of a given segment, creating
 * it if necessary.
 */
static ATOMTABLE * ATOM_GetTable( WORD selector, BOOL create )
{
    INSTANCEDATA *ptr = (INSTANCEDATA *)PTR_SEG_OFF_TO_LIN( selector, 0 );
    if (!ptr->atomtable)
    {
        if (!create) return NULL;
        if (!ATOM_InitTable( selector, DEFAULT_ATOMTABLE_SIZE )) return NULL;
        /* Reload ptr in case it moved in linear memory */
        ptr = (INSTANCEDATA *)PTR_SEG_OFF_TO_LIN( selector, 0 );
    }
    return (ATOMTABLE *)((char *)ptr + ptr->atomtable);
}


/***********************************************************************
 *           ATOM_MakePtr
 *
 * Make an ATOMENTRY pointer from a handle (obtained from GetAtomHandle()).
 */
static ATOMENTRY * ATOM_MakePtr( WORD selector, HANDLE handle )
{
    return (ATOMENTRY *)PTR_SEG_OFF_TO_LIN( selector, handle );
}


/***********************************************************************
 *           ATOM_Hash
 */
static WORD ATOM_Hash( WORD entries, LPCSTR str, WORD len )
{
    WORD i, hash = 0;

    for (i = 0; i < len; i++) hash ^= toupper(str[i]) + i;
    return hash % entries;
}


/***********************************************************************
 *           ATOM_AddAtom
 */
static ATOM ATOM_AddAtom( WORD selector, SEGPTR name )
{
    WORD hash;
    HANDLE entry;
    ATOMENTRY * entryPtr;
    ATOMTABLE * table;
    int len;
    char *str;

    /* Check for integer atom */

    if (!HIWORD(name)) return (ATOM)LOWORD(name);
    str = PTR_SEG_TO_LIN( name );
    if (str[0] == '#') return atoi( &str[1] );

    if ((len = strlen( str )) > 255) len = 255;
    if (!(table = ATOM_GetTable( selector, TRUE ))) return 0;
    hash = ATOM_Hash( table->size, str, len );
    entry = table->entries[hash];
    while (entry)
    {
	entryPtr = ATOM_MakePtr( selector, entry );
	if ((entryPtr->length == len) && 
	    (!lstrncmpi( entryPtr->str, str, len )))
	{
	    entryPtr->refCount++;
	    return HANDLETOATOM( entry );
	}
	entry = entryPtr->next;
    }

    entry = LOCAL_Alloc( selector, LMEM_FIXED, sizeof(ATOMENTRY)+len-1 );
    if (!entry) return 0;
    /* Reload the table ptr in case it moved in linear memory */
    table = ATOM_GetTable( selector, FALSE );
    str = PTR_SEG_TO_LIN( name );
    entryPtr = ATOM_MakePtr( selector, entry );
    entryPtr->next = table->entries[hash];
    entryPtr->refCount = 1;
    entryPtr->length = len;
    memcpy( entryPtr->str, str, len );
    table->entries[hash] = entry;
    return HANDLETOATOM( entry );
}


/***********************************************************************
 *           ATOM_DeleteAtom
 */
static ATOM ATOM_DeleteAtom( WORD selector, ATOM atom )
{
    ATOMENTRY * entryPtr;
    ATOMTABLE * table;
    HANDLE entry, *prevEntry;
    WORD hash;
    
    if (atom < MIN_STR_ATOM) return 0;  /* Integer atom */

    if (!(table = ATOM_GetTable( selector, FALSE ))) return 0;
    entry = ATOMTOHANDLE( atom );
    entryPtr = ATOM_MakePtr( selector, entry );

      /* Find previous atom */
    hash = ATOM_Hash( table->size, entryPtr->str, entryPtr->length );
    prevEntry = &table->entries[hash];
    while (*prevEntry && *prevEntry != entry)
    {
	ATOMENTRY * prevEntryPtr = ATOM_MakePtr( selector, *prevEntry );
	prevEntry = &prevEntryPtr->next;
    }    
    if (!*prevEntry) return atom;

      /* Delete atom */
    if (--entryPtr->refCount == 0)
    {
	*prevEntry = entryPtr->next;
        LOCAL_Free( selector, entry );
    }    
    return 0;
}


/***********************************************************************
 *           ATOM_FindAtom
 */
static ATOM ATOM_FindAtom( WORD selector, SEGPTR name )
{
    ATOMTABLE * table;
    WORD hash;
    HANDLE entry;
    int len;
    char *str;

    /* Check for integer atom */

    if (!HIWORD(name)) return (ATOM)LOWORD(name);
    str = PTR_SEG_TO_LIN( name );
    if (str[0] == '#') return atoi( &str[1] );

    if ((len = strlen( str )) > 255) len = 255;
    if (!(table = ATOM_GetTable( selector, FALSE ))) return 0;
    hash = ATOM_Hash( table->size, str, len );
    entry = table->entries[hash];
    while (entry)
    {
	ATOMENTRY * entryPtr = ATOM_MakePtr( selector, entry );
	if ((entryPtr->length == len) && 
	    (!lstrncmpi( entryPtr->str, str, len )))
	    return HANDLETOATOM( entry );
	entry = entryPtr->next;
    }
    return 0;
}


/***********************************************************************
 *           ATOM_GetAtomName
 */
static WORD ATOM_GetAtomName( WORD selector, ATOM atom,
                              LPSTR buffer, short count )
{
    ATOMTABLE * table;
    ATOMENTRY * entryPtr;
    HANDLE entry;
    char * strPtr;
    int len;
    char text[8];
    
    if (!count) return 0;
    if (atom < MIN_STR_ATOM)
    {
	sprintf( text, "#%d", atom );
	len = strlen(text);
	strPtr = text;
    }
    else
    {
        if (!(table = ATOM_GetTable( selector, FALSE ))) return 0;
	entry = ATOMTOHANDLE( atom );
	entryPtr = ATOM_MakePtr( selector, entry );
	len = entryPtr->length;
	strPtr = entryPtr->str;
    }
    if (len >= count) len = count-1;
    memcpy( buffer, strPtr, len );
    buffer[len] = '\0';
    return len;
}


/***********************************************************************
 *           InitAtomTable   (KERNEL.68)
 */
WORD InitAtomTable( WORD entries )
{
    return ATOM_InitTable( CURRENT_DS, entries );
}


/***********************************************************************
 *           GetAtomHandle   (KERNEL.73)
 */
HANDLE GetAtomHandle( ATOM atom )
{
    if (atom < MIN_STR_ATOM) return 0;
    return ATOMTOHANDLE( atom );
}


/***********************************************************************
 *           AddAtom   (KERNEL.70)
 */
ATOM AddAtom( SEGPTR str )
{
    return ATOM_AddAtom( CURRENT_DS, str );
}


/***********************************************************************
 *           DeleteAtom   (KERNEL.71)
 */
ATOM DeleteAtom( ATOM atom )
{
    return ATOM_DeleteAtom( CURRENT_DS, atom );
}


/***********************************************************************
 *           FindAtom   (KERNEL.69)
 */
ATOM FindAtom( SEGPTR str )
{
    return ATOM_FindAtom( CURRENT_DS, str );
}


/***********************************************************************
 *           GetAtomName   (KERNEL.72)
 */
WORD GetAtomName( ATOM atom, LPSTR buffer, short count )
{
    return ATOM_GetAtomName( CURRENT_DS, atom, buffer, count );
}


/***********************************************************************
 *           GlobalAddAtom   (USER.268)
 */
ATOM GlobalAddAtom( SEGPTR str )
{
#ifdef CONFIG_IPC
    if (Options.ipc) return DDE_GlobalAddAtom( str );
#endif
    return ATOM_AddAtom( USER_HeapSel, str );
}


/***********************************************************************
 *           GlobalDeleteAtom   (USER.269)
 */
ATOM GlobalDeleteAtom( ATOM atom )
{
#ifdef CONFIG_IPC
    if (Options.ipc) return DDE_GlobalDeleteAtom( atom );
#endif
    return ATOM_DeleteAtom( USER_HeapSel, atom );
}


/***********************************************************************
 *           GlobalFindAtom   (USER.270)
 */
ATOM GlobalFindAtom( SEGPTR str )
{
#ifdef CONFIG_IPC
    if (Options.ipc) return DDE_GlobalFindAtom( str );
#endif
    return ATOM_FindAtom( USER_HeapSel, str );
}


/***********************************************************************
 *           GlobalGetAtomName   (USER.271)
 */
WORD GlobalGetAtomName( ATOM atom, LPSTR buffer, short count )
{
#ifdef CONFIG_IPC
    if (Options.ipc) return DDE_GlobalGetAtomName( atom, buffer, count );
#endif
    return ATOM_GetAtomName( USER_HeapSel, atom, buffer, count );
}

#endif  /* WINELIB */
