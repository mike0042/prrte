/*
 * $HEADER$
 */

#include "lam_config.h"
#include "lam/mem/free_lists.h"
#include "lam/runtime/runtime.h"
#include "lam/util/output.h"
#include "lam/os/numa.h"
#include "lam/os/lam_system.h"
#include "lam/mem/mem_globals.h"

#ifndef ROB_HASNT_FINISHED_THIS_YET
#define ROB_HASNT_FINISHED_THIS_YET 0
#endif

/* private list functions */

#if RB_HASNT_FINISHED_THIS_YET
static lam_list_item_t *lam_free_lists_request_elt(lam_free_lists_t *flist, 
                                            int pool_idx);
#endif

static void lam_free_lists_append(lam_free_lists_t *flist, void *chunk, int pool_idx);

static int lam_free_lists_create_more_elts(lam_free_lists_t *flist, int pool_idx);

static void *lam_free_lists_get_mem_chunk(lam_free_lists_t *flist, int index, size_t *len, int *err);

static int lam_free_lists_mem_pool_construct(lam_free_lists_t *flist, int nlists, long pages_per_list, ssize_t chunk_size,
                      size_t page_size, long min_pages_per_list,
                      long default_min_pages_per_list, long default_pages_per_list,
                      long max_pages_per_list, ssize_t max_mem_in_pool);

lam_class_info_t lam_free_lists_t_class_info = {
    "lam_free_lists_t",
    CLASS_INFO(lam_object_t), 
    (lam_construct_t) lam_free_lists_construct,
    (lam_destruct_t) lam_free_lists_destruct
};


void lam_free_lists_construct(lam_free_lists_t *flist)
{
    OBJ_CONSTRUCT_SUPER(flist, lam_object_t);
    lam_mutex_init(&flist->fl_lock);
    flist->fl_pool = NULL;
    flist->fl_elt_cls = NULL;
    flist->fl_description = NULL;
    flist->fl_free_lists = NULL;
    flist->fl_is_shared = 0;
    flist->fl_nlists = 0;
    flist->fl_elt_per_chunk = 0;
    flist->fl_elt_size = 0;
    flist->fl_retry_more_resources = 0;
    flist->fl_enforce_affinity = 0;
    flist->fl_affinity = NULL;
    flist->fl_threshold_grow = 0;

#if LAM_ENABLE_MEM_PROFILE
    flist->fl_elt_out = NULL;
    flist->fl_elt_max = NULL;
    flist->fl_elt_sum = NULL;
    flist->fl_nevents = NULL;
    flist->fl_chunks_req = NULL;
    flist->fl_chunks_returned = NULL;
#endif
}


void lam_free_lists_destruct(lam_free_lists_t *flist)
{
    int         i;
    
    OBJ_RELEASE(flist->fl_pool);
    for ( i = 0; i < flist->fl_nlists; i++ )
        OBJ_RELEASE(flist->fl_free_lists[i]);
    
    if ( flist->fl_affinity )
        free(flist->fl_affinity);

#if LAM_ENABLE_MEM_PROFILE    
    if ( flist->fl_elt_out )
        free(flist->fl_elt_out);
    
    if ( flist->fl_elt_max )
        free(flist->fl_elt_max);
    
    if ( flist->fl_elt_sum )
        free(flist->fl_elt_sum);
    
    if ( flist->fl_nevents )
        free(flist->fl_nevents);
    
    if ( flist->fl_chunks_req )
        free(flist->fl_chunks_req);
    
    if ( flist->fl_chunks_returned )
        free(flist->fl_chunks_returned);
#endif
    
    OBJ_DESTRUCT_SUPER(flist, lam_object_t);
}


int lam_free_lists_construct_with(
        lam_free_lists_t *flist, 
        int nlists,
        int pages_per_list,
        size_t chunk_size, 
        size_t page_size,
        size_t elt_size,
        int min_pages_per_list, 
        int max_pages_per_list,
        int max_consec_req_fail,
        const char *description,
        bool retry_for_more_resources,
        lam_affinity_t *affinity,
        bool enforce_affinity,
        lam_mem_pool_t *mem_pool)
{
    /* lam_free_lists_construct must have been called prior to calling this function */
    size_t  max_mem_in_pool;
    size_t  initial_mem_per_list;
    long    max_mem_per_list;
    int     list, pool;
    int     err = LAM_SUCCESS;

    flist->fl_description = description;
    flist->fl_nlists = nlists;
    
    /* set up the memory pool */
    if ( mem_pool )
    {
        flist->fl_pool = mem_pool;
        OBJ_RETAIN(flist->fl_pool);
    } 
    else
    {
        /* instantiate memory pool */
        max_mem_in_pool = max_pages_per_list * page_size;
        err = lam_free_lists_mem_pool_construct(
            flist,
            nlists, 
            pages_per_list, 
            chunk_size,
            page_size, 
            min_pages_per_list,
            min_pages_per_list, 
            pages_per_list,
            max_pages_per_list, 
            max_mem_in_pool);
        if (err != LAM_SUCCESS)
        {
            return err;
        }
    }
    
    /* reset pool chunk size */
    chunk_size = lam_mp_get_chunk_size(flist->fl_pool);
    
    /* Number of elements per chunk */
    flist->fl_elt_per_chunk = chunk_size / elt_size;
    
    initial_mem_per_list = min_pages_per_list * page_size;
    
    /* adjust initial_mem_per_list to increments of chunk_size */
    if ( initial_mem_per_list < chunk_size )
    {
        min_pages_per_list = (((chunk_size - 1) / page_size) + 1);
        initial_mem_per_list = min_pages_per_list * page_size;
    }
    
    /* determine upper limit on number of pages in a given list */
    if ( (max_pages_per_list != -1) && (max_pages_per_list < min_pages_per_list) )
        max_pages_per_list = min_pages_per_list;
    
    if (max_pages_per_list == -1)
        max_mem_per_list = -1;
    else
        max_mem_per_list = max_pages_per_list * page_size;
    
    /* initialize empty lists of available descriptors */
    flist->fl_free_lists = (lam_seg_list_t **)
                    malloc(sizeof(lam_seg_list_t *) *
                   flist->fl_nlists);
    if ( !flist->fl_free_lists )
    {
      lam_abort(1, "Error: Out of memory");
    }
 
    /* run constructors */
    for (list = 0; list < flist->fl_nlists; list++)
    {
        if ( flist->fl_is_shared )
        {
            /* process shared memory allocation */
            flist->fl_free_lists[list] =
            (lam_seg_list_t *)
            lam_fmp_get_mem_segment(&lam_per_proc_shmem_pools,
                sizeof(lam_seg_list_t), CACHE_ALIGNMENT, list);
        } 
        else
        {
            /* process private memory allocation */
            flist->fl_free_lists[list] =
                (lam_seg_list_t *)malloc(sizeof(lam_seg_list_t));
        }
        
        if (!flist->fl_free_lists[list]) {
          lam_abort(1, "Error: Out of memory");
        }

        OBJ_CONSTRUCT(&flist->fl_free_lists[list], lam_seg_list_t);
        
        lam_sgl_set_min_bytes_pushed(flist->fl_free_lists[list],
                                     initial_mem_per_list);
        lam_sgl_set_max_bytes_pushed(flist->fl_free_lists[list],
                                     max_mem_per_list);
        lam_sgl_set_max_consec_fail(flist->fl_free_lists[list],
                                    max_consec_req_fail);
    } /* end list loop */
    
    flist->fl_retry_more_resources = retry_for_more_resources;
    flist->fl_enforce_affinity = enforce_affinity;
    if ( enforce_affinity )
    {
        flist->fl_affinity = (affinity_t *)malloc(sizeof(affinity_t) *
                                    flist->fl_nlists);
        if ( !flist->fl_affinity ) {
          lam_abort(1, "Error: Out of memory");
        }

        /* copy policies in */
        for ( pool = 0; pool < flist->fl_nlists; pool++ )
        {
            flist->fl_affinity[pool] = affinity[pool];
        }
    }


    /* initialize locks for memory pool and individual list and link locks */
    for ( pool = 0; pool < flist->fl_nlists; pool++ ) {
        
        /* gain exclusive use of list */
        if ( 1 == lam_sgl_lock_list(flist->fl_free_lists[pool]) ) {
            
            while ( lam_sgl_get_bytes_pushed(flist->fl_free_lists[pool])
                   < lam_sgl_get_min_bytes_pushed(flist->fl_free_lists[pool]) )
            {
                if (lam_free_lists_create_more_elts(flist, pool) != LAM_SUCCESS)
                {
                  lam_abort(1, "Error: Setting up initial private "
                            "free list for %s.\n", flist->fl_description);
                }
            }
            
            lam_sgl_unlock_list(flist->fl_free_lists[pool]);
        }
        else
        {
            /* only 1 process should be initializing the list */
            lam_abort(1, "Error: Setting up initial private free "
                      "list %d for %s.\n", pool, flist->fl_description);
        }
    }   
    
    return err;
    
}


static int lam_free_lists_mem_pool_construct(lam_free_lists_t *flist,
                      int nlists, long pages_per_list, ssize_t chunk_size,
                      size_t page_size, long min_pages_per_list,
                      long default_min_pages_per_list, long default_pages_per_list,
                      long max_pages_per_list, ssize_t max_mem_in_pool)
{
    int         err = LAM_SUCCESS;
    long        total_pgs_to_alloc;
    ssize_t     mem_in_pool;
    size_t      to_alloc;
    
    /* set chunksize - multiple of page size */
    chunk_size =
        ((((chunk_size - 1) / page_size) + 1) * page_size);
    
    /* determine number how much memory to allocate */
    if ( pages_per_list == -1 ) {
        /* minimum size is  defaultNPagesPerList*number of local procs */
        total_pgs_to_alloc = default_pages_per_list * nlists;
    } else {
        total_pgs_to_alloc = pages_per_list * nlists;
    }
    
    mem_in_pool = total_pgs_to_alloc * page_size;
    
    /* Initialize memory pool */
    if ( flist->fl_is_shared ) {
        /* shared memory allocation */
        to_alloc = sizeof(lam_mem_pool_t);
        flist->fl_pool =
            (lam_mem_pool_t *)lam_fmp_get_mem_segment(&lam_shmem_pools,
                                                      to_alloc, 
                                                      CACHE_ALIGNMENT, 0);
        if ( flist->fl_pool ) {
            OBJ_CONSTRUCT(&flist->fl_pool, shmem_pool_t);
        }
    } else {
        /* process private memory allocation */
        flist->fl_pool = OBJ_NEW(lam_mem_pool_t);
    }

    err = lam_mp_construct_with(
        flist->fl_pool, 
        mem_in_pool, 
        max_mem_in_pool,
        chunk_size, 
        page_size);
    return err;
}


static void *lam_free_lists_get_mem_chunk(lam_free_lists_t *flist, int index, size_t *len, int *err)
{
    void        *chunk = 0;
    uint64_t    sz_to_add;
    
    /* check to make sure that the amount to add to the list does not 
       exceed the amount allowed */
    sz_to_add = lam_mp_get_chunk_size(flist->fl_pool);

#if LAM_ENABLE_MEM_PROFILE
    flist->fl_chunks_req[index]++;
#endif
    
    if (index >= flist->fl_nlists)
    {
      lam_output(0, "Error: Array out of bounds");
      return chunk;
    }
        
    if ( lam_sgl_get_max_bytes_pushed(flist->fl_free_lists[index]) != -1 ) 
    {
        if (sz_to_add +
            lam_sgl_get_bytes_pushed(flist->fl_free_lists[index]) >
            lam_sgl_get_max_bytes_pushed(flist->fl_free_lists[index]) )
        {
            lam_sgl_inc_consec_fail(flist->fl_free_lists[index]); 
            if ( lam_sgl_get_consec_fail(flist->fl_free_lists[index]) >=
                lam_sgl_get_max_consec_fail(flist->fl_free_lists[index]) )
            {
                *err = LAM_ERR_OUT_OF_RESOURCE;
                lam_output(0, "Error: List out of memory in pool for %s",
                           flist->fl_description);
                return chunk;
            } else
                *err = LAM_ERR_TEMP_OUT_OF_RESOURCE;
            
            return chunk;
        }
    }
    /* set len */
    *len = sz_to_add;
    
    
    /* get chunk of memory */
    chunk = lam_mp_request_chunk(flist->fl_pool, index);
    if ( 0 == chunk )
    {
        /* increment failure count */
        lam_sgl_inc_consec_fail(flist->fl_free_lists[index]); 
        if ( lam_sgl_get_consec_fail(flist->fl_free_lists[index]) >=
             lam_sgl_get_max_consec_fail(flist->fl_free_lists[index]) )
        {
            *err = LAM_ERR_OUT_OF_RESOURCE;
            lam_output(0, "Error: List out of memory in pool for %s\n",
                       flist->fl_description);
            return chunk;
        } else
            *err = LAM_ERR_TEMP_OUT_OF_RESOURCE;
        
        return chunk;
    }
    
    /* set consecutive failure count to 0 - if we fail, we don't get 
       this far in the code. */
    lam_sgl_set_consec_fail(flist->fl_free_lists[index], 0);
    
#if LAM_ENABLE_MEM_PROFILE
    flist->fl_chunks_returned[index]++;
#endif

    return chunk;
}



#if ROB_HASNT_FINISHED_THIS_YET
static lam_list_item_t *lam_free_lists_request_elt(lam_free_lists_t *flist, int pool_idx)
{
    lam_dbl_list_t      *seg_list = &(flist->fl_free_lists[pool_idx]->sgl_list);
    volatile lam_list_item_t *elt = lam_dbl_get_last(seg_list);
    
    if ( elt )
        lam_sgl_set_consec_fail(seg_list, 0);
    return elt;
}
#endif


static void lam_free_lists_append(lam_free_lists_t *flist, void *chunk, int pool_idx)
{
    /* ASSERT: mp_chunk_sz >= fl_elt_per_chunk * fl_elt_size */
    /* push items onto list  */
    lam_sgl_append_elt_chunk(flist->fl_free_lists[pool_idx],
        chunk, lam_mp_get_chunk_size(flist->fl_pool),
        flist->fl_elt_per_chunk, flist->fl_elt_size);
}




static int lam_free_lists_create_more_elts(lam_free_lists_t *flist, int pool_idx)
{
    int         err = LAM_SUCCESS, desc;
    size_t      len_added;
    char        *current_loc;
    
    void *ptr = lam_free_lists_get_mem_chunk(flist, pool_idx, &len_added, &err);
    
    if (0 == ptr ) {
      lam_output(0, "Error: Can't get new elements for %s\n", 
                 flist->fl_description);
        return err;
    }
    
    /* attach memory affinity */
    if ( flist->fl_enforce_affinity )
    {
        if (!lam_set_affinity(ptr, len_added,
                         flist->fl_affinity[pool_idx]))
        {
            err = LAM_ERROR;
#ifdef _DEBUGQUEUES
            lam_err(("Error: Can't set memory policy (pool_idx=%d)\n",
                     pool_idx));
            return err;
#endif                          /* _DEBUGQUEUES */
        }
    }
    
    /* Construct new descriptors using placement new */
    current_loc = (char *) ptr;
    for (desc = 0; desc < flist->fl_elt_per_chunk; desc++)
    {
        /* bypass OBJ_CONSTRUCT() in this case (generic types) */
        ((lam_object_t *) current_loc)->obj_class_info = flist->fl_elt_cls;
        ((lam_object_t *) current_loc)
            ->obj_class_info->cls_construct((lam_object_t *) current_loc);
        current_loc += flist->fl_elt_size;
    }
    
    /* push chunk of memory onto the list */
    lam_free_lists_append(flist, ptr, pool_idx);
    
    return err;
}




lam_list_item_t *lam_free_lists_get_elt(lam_free_lists_t *flist, int index, int *error)
{
#if ROB_HASNT_FINISHED_THIS_YET
    int         error;
    volatile    lam_list_item_t *elem = NULL;
    
    elem = lam_free_lists_request_elt(flist, index);
    
    if ( elem ) 
    {
        error = LAM_SUCCESS;
    } 
    else if ( lam_sgl_get_consec_fail(&(flist->fl_free_lists[index]->sgl_list))
               < flist->fl_threshold_grow ) 
    {
        error = LAM_ERR_TEMP_OUT_OF_RESOURCE;
    } 
    else 
    {
        error = LAM_SUCCESS;
        while ( (LAM_SUCCESS) && (0 == elem) &&
                (flist->fl_retry_more_resources) )
        {
            error = lam_free_lists_create_more_elts(flist, index);
            /* get element if managed to add resources to the list */
            if ( LAM_SUCCESS == error )
            {
                elem = lam_free_lists_request_elt(flist, index);
            }            
        }

        if ( (LAM_ERR_OUT_OF_RESOURCE == error)
             || (LAM_ERR_FATAL == error) )
        {
            return 0;
        }
    }
#if LAM_ENABLE_MEM_PROFILE
    flist->fl_elt_out[index]++;
    flist->fl_elt_sum[index] += flist->fl_elt_out[index];
    flist->fl_nevents[index]++;
    if (flist->fl_elt_max[index] < flist->fl_elt_out[index])
    {
        flist->fl_elt_max[index] = flist->fl_elt_out[index];
    }
#endif
    
    return elem;
#else
    return NULL;
#endif
}

int lam_free_lists_return_elt(lam_free_lists_t *flist, int index, lam_list_item_t *item)
{
#if ROB_HASNT_FINISHED_THIS_YET
    mb();
    lam_dbl_append(&(flist->fl_free_lists[index]->sgl_list), item);
    mb();
    
#if LAM_ENABLE_MEM_PROFILE
    flist->fl_elt_out[index]--;
#endif
    
    return LAM_SUCCESS;
#else
    return LAM_ERROR;
#endif
}


