/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int
__lsm_free_chunks(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree);

/*
 * __wt_lsm_worker --
 *	The worker thread for an LSM tree, responsible for writing in-memory
 *	trees to disk and merging on-disk trees.
 */
void *
__wt_lsm_worker(void *arg)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk, *chunk_array;
	WT_LSM_TREE *lsm_tree;
	WT_SESSION_IMPL *session;
	const char *cfg[] = { "name=,drop=", NULL };
	size_t chunk_alloc;
	int i, nchunks, progress;

	lsm_tree = arg;
	session = lsm_tree->worker_session;

	chunk_array = NULL;
	chunk_alloc = 0;

	while (F_ISSET(lsm_tree, WT_LSM_TREE_OPEN)) {
		progress = 0;

		__wt_spin_lock(session, &lsm_tree->lock);
		if (!F_ISSET(lsm_tree, WT_LSM_TREE_OPEN)) {
			__wt_spin_unlock(session, &lsm_tree->lock);
			break;
		}
		/* Take a copy of the current state of the LSM tree. */
		for (nchunks = lsm_tree->nchunks - 1;
		    nchunks > 0 && lsm_tree->chunk[nchunks].ncursor > 0;
		    --nchunks)
			;
		if (chunk_alloc < lsm_tree->chunk_alloc)
			ret = __wt_realloc(session,
			    &chunk_alloc, lsm_tree->chunk_alloc,
			    &chunk_array);
		if (ret == 0 && nchunks > 0)
			memcpy(chunk_array, lsm_tree->chunk,
			    nchunks * sizeof(*lsm_tree->chunk));
		__wt_spin_unlock(session, &lsm_tree->lock);
		WT_ERR(ret);

		/*
		 * Write checkpoints in all completed files, then find
		 * something to merge.
		 */
		for (i = 0, chunk = chunk_array; i < nchunks; i++, chunk++) {
			if (F_ISSET(chunk, WT_LSM_CHUNK_ONDISK))
				continue;

			/* XXX durability: need to checkpoint the metadata? */
			/*
			 * NOTE: we pass a non-NULL config, because otherwise
			 * __wt_checkpoint thinks we're closing the file.
			 */
			WT_WITH_SCHEMA_LOCK(session, ret =
			    __wt_schema_worker(session, chunk->uri,
			    __wt_checkpoint, cfg, 0));
			if (ret == 0) {
				__wt_spin_lock(session, &lsm_tree->lock);
				F_SET(&lsm_tree->chunk[i], WT_LSM_CHUNK_ONDISK);
				lsm_tree->dsk_gen++;
				__wt_spin_unlock(session, &lsm_tree->lock);
				progress = 1;
			}
		}

		/* Clear any state from previous worker thread iterations. */
		session->btree = NULL;

		if (nchunks > 0 && __wt_lsm_major_merge(session, lsm_tree) == 0)
			progress = 1;

		/* Clear any state from previous worker thread iterations. */
		session->btree = NULL;

		if (lsm_tree->nold_chunks != lsm_tree->old_avail &&
		    __lsm_free_chunks(session, lsm_tree) == 0)
			progress = 1;

		if (!progress)
			__wt_sleep(0, 10);
	}

err:	__wt_free(session, chunk_array);

	return (NULL);
}

static int
__lsm_free_chunks(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_DECL_RET;
	char *uri;
	const char *drop_cfg[] = { NULL };
	int found, i;

	found = 0;
	for (i = 0; i < lsm_tree->nold_chunks; i++) {
		uri = (char *)lsm_tree->old_chunks[i].uri;
		if (uri == NULL)
			continue;
		if (!found) {
			found = 1;
			/* TODO: Do we need the lsm_tree lock for all drops? */
			__wt_spin_lock(session, &lsm_tree->lock);
		}
		WT_WITH_SCHEMA_LOCK(session,
		    ret = __wt_schema_drop(session, uri, drop_cfg));
		if (ret == EBUSY)
			/*
			 * TODO: Chunks resulting from a merge fail this for
			 * ever, and are thus never freed. Understand why
			 * they are held open.
			 */
			continue;
		if (ret != 0)
			goto err;
		__wt_free(session, uri);
		memset(
		    &lsm_tree->old_chunks[i], 0, sizeof(*lsm_tree->old_chunks));
		++lsm_tree->old_avail;
	}
	if (found) {
err:		ret = __wt_lsm_meta_write(session, lsm_tree);
		__wt_spin_unlock(session, &lsm_tree->lock);
	}
	/* Returning non-zero means there is no work to do. */
	return (found ? 0 : WT_NOTFOUND);
}
