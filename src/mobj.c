#include "mobj.h"
#include "pframe.h"
#include "util/string.h"

/*
 * Create an instance of the mobj
 */
void mobj_create(mobj_t *o, mobj_ops_t *ops) {
    // initalizes the fields
    o->mo_ops = *ops;
    list_init(&o->mo_pframes);
    spinlock_init(&o->mo_mutex);
    o->parent = NULL;
}

/*
 * Find a pframe from the mobj list of pframes.  
 */
void mobj_find_pframe(mobj_t *o, long pagenum, pframe_t **pfp) {
    *pfp = NULL; // initalize pointer

    list_link_t *link = o->mo_pframes.head; 

    while (link != NULL) { // go through the list of pframes
        pframe_t *cur_frame = link->parent;

        if (cur_frame->pf_pagenum == pagenum) { // return once you find the pframe (compare pagenums)
            *pfp = cur_frame;
            return;
        }

        link = link->next;
    }
}

/*
 * Returns page using mobj_find_pframe
 */
void mobj_get_pframe(mobj_t *o, long pagenum, pframe_t **pfp) {
    mobj_find_pframe(o, pagenum, pfp); // find the pframe

    if (*pfp) { // if it exists nothing to do here
        return;
    }

    if (o->mo_ops.read_pframe(o, pagenum, pfp) != 0)
        *pfp = NULL; // if it doesn't exist, set the pointer to null
}

/*
 * Read data from pframe
 */
long mobj_read(mobj_t *o, long pos, long len, char *buf) {
    long read = 0;

    while (read < len) { // while there is room left to read to
        long page = pos / FRAME_LEN;
        long offset = pos % FRAME_LEN;

        pframe_t *pframe;
        mobj_find_pframe(o, page, &pframe);

        if (pframe == NULL) { // if frame is not in mobj list
            break;
        }

        buf[read] = pframe->bytes[offset]; // write data into buffer 
        pos++;
        read++;
    }
    return read;
}

/*
 * Write data to pframe
 */
long mobj_write(mobj_t *o, long pos, long len, const char *buf) {
    long write = 0;

    while (write < len) { // until len bytes are written
        long page = pos / FRAME_LEN;
        long offset = pos % FRAME_LEN;

        pframe_t *pf;
        mobj_find_pframe(o, page, &pf);

        if (pf == NULL) { // if no page frames found we cannot write to them
            break;
        }

        pf->bytes[offset] = buf[write]; // write from buffer into the page frame

        write++;
        pos++;
    }

    return write;
}

/*
 * Flush the changes to our mobj in the memory to the disk
 */
void mobj_flush(mobj_t *o) {
    list_link_t *link = o->mo_pframes.head; 

    while (link != NULL) { // go through list of pframes
        pframe_t *pf = link->parent;

        if (o->mo_ops.flush_pframe(o, pf) != 0) { // flush each page frame 
            return; // if flushing fails leave
        }
        link = link->next;
    }
}
