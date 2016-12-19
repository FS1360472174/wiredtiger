/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_log_system_prevlsn --
 *	Write a system log record for the previous LSN.
 */
int
__wt_log_system_prevlsn(
    WT_SESSION_IMPL *session, WT_FH *log_fh, WT_LSN *lsn)
{
	WT_DECL_ITEM(logrec_buf);
	WT_DECL_RET;
	WT_LOG *log;
	WT_LOG_RECORD *logrec;
	WT_LOGSLOT tmp;
	WT_MYSLOT myslot;
	const char *fmt = WT_UNCHECKED_STRING(III);
	uint32_t rectype = WT_LOGREC_PREVLSN;
	size_t recsize;

	log = S2C(session)->log;
	WT_RET(__wt_logrec_alloc(session, log->allocsize, &logrec_buf));
	memset((uint8_t *)logrec_buf->mem, 0, log->allocsize);
	WT_ERR(__wt_struct_size(session, &recsize, fmt, rectype,
	    lsn->l.file, lsn->l.offset));
	WT_ASSERT(session, recsize <= log->allocsize);
	WT_ERR(__wt_struct_pack(session,
	    (uint8_t *)logrec_buf->data + logrec_buf->size, recsize, fmt,
	    rectype, lsn->l.file, lsn->l.offset));
	logrec = (WT_LOG_RECORD *)logrec_buf->mem;
	/*
	 * We know system records are this size.  And we have to adjust
	 * the size now because we're not going through the normal log
	 * write path and the packing functions needed the correct offset
	 * earlier.
	 */
	logrec_buf->size = logrec->len = log->allocsize;
	/*
	 * We do not compress nor encrypt this record.
	 */
	logrec->checksum = 0;
	logrec->flags = 0;
	__wt_log_record_byteswap(logrec);
	logrec->checksum = __wt_checksum(logrec, log->allocsize);
#ifdef WORDS_BIGENDIAN
	logrec->checksum = __wt_bswap32(logrec->checksum);
#endif
	WT_CLEAR(tmp);
	memset(&myslot, 0, sizeof(myslot));
	myslot.slot = &tmp;
	__wt_log_slot_activate(session, &tmp);
	/*
	 * Override the file handle to the one we're using.
	 */
	tmp.slot_fh = log_fh;
	WT_ERR(__wt_log_fill(session, &myslot, true, logrec_buf, NULL));
err:	__wt_logrec_free(session, &logrec_buf);
	return (ret);
}

/*
 * __wt_log_recover_prevlsn --
 *	Process a system log record for the previous LSN in recovery.
 */
int
__wt_log_recover_prevlsn(WT_SESSION_IMPL *session,
    const uint8_t **pp, const uint8_t *end, WT_LSN *lsnp)
{
	WT_DECL_RET;
	WT_LOG *log;
	uint32_t prev_file, prev_offset;
	const char *fmt = WT_UNCHECKED_STRING(II);

	log = S2C(session)->log;
	if ((ret = __wt_struct_unpack(session, *pp, WT_PTRDIFF(end, *pp), fmt,
	    &prev_file, &prev_offset)) != 0)
		WT_RET_MSG(session, ret,
		    "log_recover_prevlsn: unpack failure");
	if (lsnp != NULL)
		WT_SET_LSN(lsnp, prev_file, prev_offset);
	*pp = end;
	return (0);
}
