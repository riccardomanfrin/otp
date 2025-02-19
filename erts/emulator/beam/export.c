/*
 * %CopyrightBegin%
 *
 * Copyright Ericsson AB 1996-2021. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * %CopyrightEnd%
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "sys.h"
#include "erl_vm.h"
#include "global.h"
#include "export.h"
#include "hash.h"
#include "jit/beam_asm.h"

#define EXPORT_INITIAL_SIZE   4000
#define EXPORT_LIMIT  (512*1024)

#define EXPORT_HASH(m,f,a) ((atom_val(m) * atom_val(f)) ^ (a))

#ifdef DEBUG
#  define IF_DEBUG(x) x
#else
#  define IF_DEBUG(x)
#endif

static IndexTable export_tables[ERTS_NUM_CODE_IX];  /* Active not locked */

static erts_atomic_t total_entries_bytes;

/* This lock protects the staging export table from concurrent access
 * AND it protects the staging table from becoming active.
 */
erts_mtx_t export_staging_lock;

struct export_entry
{
    IndexSlot slot; /* MUST BE LOCATED AT TOP OF STRUCT!!! */
    Export* ep;
};

/* Helper struct that brings things together in one allocation
*/
struct export_blob
{
    Export exp;
    struct export_entry entryv[ERTS_NUM_CODE_IX];
    /* Note that entryv is not indexed by "code_ix".
     */
};

/* Helper struct only used as template
*/
struct export_templ
{
    struct export_entry entry;
    Export exp;
};

static struct export_blob* entry_to_blob(struct export_entry* ee)
{
    return ErtsContainerStruct(ee->ep, struct export_blob, exp);
}

void
export_info(fmtfn_t to, void *to_arg)
{
    int lock = !ERTS_IS_CRASH_DUMPING;
    if (lock)
	export_staging_lock();
    index_info(to, to_arg, &export_tables[erts_active_code_ix()]);
    hash_info(to, to_arg, &export_tables[erts_staging_code_ix()].htable);
    if (lock)
	export_staging_unlock();
}


static HashValue
export_hash(struct export_entry* ee)
{
    Export* x = ee->ep;
    return EXPORT_HASH(x->info.mfa.module, x->info.mfa.function,
                       x->info.mfa.arity);
}

static int
export_cmp(struct export_entry* tmpl_e, struct export_entry* obj_e)
{
    Export* tmpl = tmpl_e->ep;
    Export* obj = obj_e->ep;
    return !(tmpl->info.mfa.module == obj->info.mfa.module &&
	     tmpl->info.mfa.function == obj->info.mfa.function &&
	     tmpl->info.mfa.arity == obj->info.mfa.arity);
}


static struct export_entry*
export_alloc(struct export_entry* tmpl_e)
{
    struct export_blob* blob;
    unsigned ix;

    if (tmpl_e->slot.index == -1) {  /* Template, allocate blob */
	Export* tmpl = tmpl_e->ep;
	Export* obj;

	blob = (struct export_blob*) erts_alloc(ERTS_ALC_T_EXPORT, sizeof(*blob));
	erts_atomic_add_nob(&total_entries_bytes, sizeof(*blob));
	obj = &blob->exp;
	obj->info.op = 0;
	obj->info.u.gen_bp = NULL;
	obj->info.mfa.module = tmpl->info.mfa.module;
	obj->info.mfa.function = tmpl->info.mfa.function;
	obj->info.mfa.arity = tmpl->info.mfa.arity;
        obj->bif_number = -1;
        obj->is_bif_traced = 0;

        memset(&obj->trampoline, 0, sizeof(obj->trampoline));

        if (BeamOpsAreInitialized()) {
            obj->trampoline.common.op = BeamOpCodeAddr(op_call_error_handler);
        }

        for (ix=0; ix<ERTS_NUM_CODE_IX; ix++) {
            erts_activate_export_trampoline(obj, ix);

            blob->entryv[ix].slot.index = -1;
            blob->entryv[ix].ep = &blob->exp;
        }

	ix = 0;

	DBG_TRACE_MFA_P(&obj->info.mfa, "export allocation at %p", obj);
    }
    else { /* Existing entry in another table, use free entry in blob */
	blob = entry_to_blob(tmpl_e);
	for (ix = 0; blob->entryv[ix].slot.index >= 0; ix++) {
	    ASSERT(ix < ERTS_NUM_CODE_IX);
	}
    }
    return &blob->entryv[ix];
}

static void
export_free(struct export_entry* obj)
{
    struct export_blob* blob = entry_to_blob(obj);
    int i;
    obj->slot.index = -1;
    for (i=0; i < ERTS_NUM_CODE_IX; i++) {
	if (blob->entryv[i].slot.index >= 0) {
	    DBG_TRACE_MFA_P(&blob->exp.info.mfa, "export entry slot %u freed for %p",
			  (obj - blob->entryv), &blob->exp);
	    return;
	}
    }
    DBG_TRACE_MFA_P(&blob->exp.info.mfa, "export blob deallocation at %p", &blob->exp);
    erts_free(ERTS_ALC_T_EXPORT, blob);
    erts_atomic_add_nob(&total_entries_bytes, -sizeof(*blob));
}

void
init_export_table(void)
{
    HashFunctions f;
    int i;

    erts_mtx_init(&export_staging_lock, "export_tab", NIL,
        ERTS_LOCK_FLAGS_PROPERTY_STATIC | ERTS_LOCK_FLAGS_CATEGORY_GENERIC);
    erts_atomic_init_nob(&total_entries_bytes, 0);

    f.hash = (H_FUN) export_hash;
    f.cmp  = (HCMP_FUN) export_cmp;
    f.alloc = (HALLOC_FUN) export_alloc;
    f.free = (HFREE_FUN) export_free;
    f.meta_alloc = (HMALLOC_FUN) erts_alloc;
    f.meta_free = (HMFREE_FUN) erts_free;
    f.meta_print = (HMPRINT_FUN) erts_print;

    for (i=0; i<ERTS_NUM_CODE_IX; i++) {
	erts_index_init(ERTS_ALC_T_EXPORT_TABLE, &export_tables[i], "export_list",
			EXPORT_INITIAL_SIZE, EXPORT_LIMIT, f);
    }
}

static struct export_entry* init_template(struct export_templ* templ,
					  Eterm m, Eterm f, unsigned a)
{
    templ->entry.ep = &templ->exp;
    templ->entry.slot.index = -1;
    templ->exp.info.mfa.module = m;
    templ->exp.info.mfa.function = f;
    templ->exp.info.mfa.arity = a;
    templ->exp.bif_number = -1;
    templ->exp.is_bif_traced = 0;
    return &templ->entry;
}

/*
 * Return a pointer to the export entry for the given function,
 * or NULL otherwise.  Notes:
 *
 * 1) BIFs have export entries and can be called through
 *    a wrapper in the export entry.
 * 2) Functions referenced by a loaded module, but not yet loaded
 *    also have export entries.  The export entry contains
 *    a wrapper which invokes the error handler if a function is
 *    called through such an export entry.
 * 3) This function is suitable for the implementation of erlang:apply/3.
 */
extern Export* /* inline-helper */
erts_find_export_entry(Eterm m, Eterm f, unsigned int a,ErtsCodeIndex code_ix);

Export*
erts_find_export_entry(Eterm m, Eterm f, unsigned int a, ErtsCodeIndex code_ix)
{
    struct export_templ templ;
    struct export_entry *ee =
        hash_fetch(&export_tables[code_ix].htable,
                   init_template(&templ, m, f, a),
                   (H_FUN)export_hash, (HCMP_FUN)export_cmp);
    if (ee) return ee->ep;
    return NULL;
}

/*
 * Find the export entry for a loaded function.
 * Returns a NULL pointer if the given function is not loaded, or
 * a pointer to the export entry.
 *
 * Note: This function never returns export entries for BIFs
 * or functions which are not yet loaded.  This makes it suitable
 * for use by the erlang:function_exported/3 BIF or whenever you
 * cannot depend on the error_handler.
 */

Export*
erts_find_function(Eterm m, Eterm f, unsigned int a, ErtsCodeIndex code_ix)
{
    struct export_templ templ;
    struct export_entry* ee;

    ee = hash_get(&export_tables[code_ix].htable, init_template(&templ, m, f, a));

    if (ee == NULL
        || (erts_is_export_trampoline_active(ee->ep, code_ix) &&
            !BeamIsOpCode(ee->ep->trampoline.common.op, op_i_generic_breakpoint))) {
        return NULL;
    }

    return ee->ep;
}

/*
 * Returns a pointer to an existing export entry for a MFA,
 * or creates a new one and returns the pointer.
 *
 * This function acts on the staging export table. It should only be used
 * to load new code.
 */

Export*
erts_export_put(Eterm mod, Eterm func, unsigned int arity)
{
    ErtsCodeIndex code_ix = erts_staging_code_ix();
    struct export_templ templ;
    struct export_entry* ee;
    Export* res;

    ASSERT(is_atom(mod));
    ASSERT(is_atom(func));
    export_staging_lock();
    ee = (struct export_entry*) index_put_entry(&export_tables[code_ix],
						init_template(&templ, mod, func, arity));
    export_staging_unlock();

    res = ee->ep;

#ifdef BEAMASM
    res->dispatch.addresses[ERTS_SAVE_CALLS_CODE_IX] = beam_save_calls_export;
#endif

    return res;
}

/*
 * Find the existing export entry for M:F/A. Failing that, create a stub
 * export entry (making a call through it will cause the error_handler to
 * be called).
 *
 * Stub export entries will be placed in the loader export table.
 */

Export*
erts_export_get_or_make_stub(Eterm mod, Eterm func, unsigned int arity)
{
    ErtsCodeIndex code_ix;
    Export* ep;
    IF_DEBUG(int retrying = 0;)
    
    ASSERT(is_atom(mod));
    ASSERT(is_atom(func));

    do {
	code_ix = erts_active_code_ix();
	ep = erts_find_export_entry(mod, func, arity, code_ix);
	if (ep == 0) {
	    /*
	     * The code is not loaded (yet). Put the export in the staging
	     * export table, to avoid having to lock the active export table.
	     */
	    export_staging_lock();
	    if (erts_active_code_ix() == code_ix) {
		struct export_templ templ;
	        struct export_entry* entry;

		IndexTable* tab = &export_tables[erts_staging_code_ix()];
		init_template(&templ, mod, func, arity);
		entry = (struct export_entry *) index_put_entry(tab, &templ.entry);
		ep = entry->ep;

#ifdef BEAMASM
                ep->dispatch.addresses[ERTS_SAVE_CALLS_CODE_IX] =
                    beam_save_calls_export;
#endif

		ASSERT(ep);
	    }
	    else { /* race */
		ASSERT(!retrying);
		IF_DEBUG(retrying = 1);
	    }
	    export_staging_unlock();
	}
    } while (!ep);
    return ep;
}

Export *export_list(int i, ErtsCodeIndex code_ix)
{
    return ((struct export_entry*) erts_index_lookup(&export_tables[code_ix], i))->ep;
}

int export_list_size(ErtsCodeIndex code_ix)
{
    return erts_index_num_entries(&export_tables[code_ix]);
}

int export_table_sz(void)
{
    int i, bytes = 0;

    export_staging_lock();
    for (i=0; i<ERTS_NUM_CODE_IX; i++) {
	bytes += index_table_sz(&export_tables[i]);
    }
    export_staging_unlock();
    return bytes;
}
int export_entries_sz(void)
{
    return erts_atomic_read_nob(&total_entries_bytes);
}
Export *export_get(Export *e)
{
    struct export_entry ee;
    struct export_entry* entry;

    ee.ep = e;
    entry = (struct export_entry*)hash_get(&export_tables[erts_active_code_ix()].htable, &ee);
    return entry ? entry->ep : NULL;
}

IF_DEBUG(static ErtsCodeIndex debug_export_load_ix = 0;)


void export_start_staging(void)
{
    ErtsCodeIndex dst_ix = erts_staging_code_ix();
    ErtsCodeIndex src_ix = erts_active_code_ix();
    IndexTable* dst = &export_tables[dst_ix];
    IndexTable* src = &export_tables[src_ix];
    int i;

    ASSERT(dst_ix != src_ix);
    ASSERT(debug_export_load_ix == ~0);

    export_staging_lock();
    /*
     * Insert all entries in src into dst table
     */
    for (i = 0; i < src->entries; i++) {
        struct export_entry* src_entry;
        ErtsDispatchable *disp;

        src_entry = (struct export_entry*) erts_index_lookup(src, i);
        disp = &src_entry->ep->dispatch;

        disp->addresses[dst_ix] = disp->addresses[src_ix];

#ifndef DEBUG
        index_put_entry(dst, src_entry);
#else /* DEBUG */
        {
            struct export_entry* dst_entry =
                (struct export_entry*)index_put_entry(dst, src_entry);
            ASSERT(entry_to_blob(src_entry) == entry_to_blob(dst_entry));
        }
#endif
    }
    export_staging_unlock();

    IF_DEBUG(debug_export_load_ix = dst_ix);
}

void export_end_staging(int commit)
{
    ASSERT(debug_export_load_ix == erts_staging_code_ix());
    IF_DEBUG(debug_export_load_ix = ~0);
}

