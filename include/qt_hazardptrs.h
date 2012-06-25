#ifndef QT_HAZARDPTRS_H
#define QT_HAZARDPTRS_H

#include "qt_visibility.h"

//#define FREELIST_DEPTH       8
#define HAZARD_PTRS_PER_SHEP 2

typedef struct {
    void (*free)(void *);
    void *ptr;
} hazard_freelist_entry_t;

/*typedef struct dfreelist_s {
    hazard_freelist_entry_t entry;
    struct dfreelist_s *next;
} hazard_dynfreelist_entry_t;*/

typedef struct {
    hazard_freelist_entry_t     *freelist;
    //hazard_dynfreelist_entry_t *dfreelist;
    unsigned int                count;
} hazard_freelist_t;

void INTERNAL initialize_hazardptrs(void);
void INTERNAL hazardous_ptr(unsigned int which,
                            void        *ptr);
void INTERNAL hazardous_release_node(void  (*freefunc)(void *),
                                     void *ptr);

#endif // ifndef QT_HAZARDPTRS_H
/* vim:set expandtab: */