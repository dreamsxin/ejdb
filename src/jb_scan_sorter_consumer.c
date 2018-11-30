#include "ejdb2_internal.h"

static void jb_scan_sorter_release(struct _JBEXEC *ctx) {
  struct _JBSSC *ssc = &ctx->ssc;
  if (ssc->refs) {
    free(ssc->refs);
  }
  if (ssc->sof_active) {
    ssc->sof.close(&ssc->sof);
  } else if (ssc->docs) {
    free(ssc->docs);
  }
  memset(ssc, 0, sizeof(*ssc));
}

static int jb_scan_sorter_cmp(const void *o1, const void *o2, void *op) {
  int rv = 0;
  uint32_t r1, r2;
  struct _JBL d1, d2;
  struct _JBEXEC *ctx = op;
  struct _JBSSC *ssc = &ctx->ssc;
  struct JQP_AUX *aux = ctx->ux->q->qp->aux;
  uint8_t *p1, *p2;
  assert(aux->orderby_num > 0);

  memcpy(&r1, o1, sizeof(r1));
  memcpy(&r2, o2, sizeof(r2));

  p1 = ssc->docs + r1 + sizeof(uint64_t) /*id*/;
  p2 = ssc->docs + r2 + sizeof(uint64_t) /*id*/;

  iwrc rc = jbl_from_buf_keep_onstack2(&d1, p1);
  RCGO(rc, finish);
  rc = jbl_from_buf_keep_onstack2(&d2, p2);
  RCGO(rc, finish);

  for (int i = 0; i < aux->orderby_num; ++i) {
    struct _JBL v1 = {0};
    struct _JBL v2 = {0};
    JBL_PTR ptr = aux->orderby_ptrs[i];
    int desc = (ptr->op & 1) ? -1 : 1; // If `-1` do desc sorting
    _jbl_at(&d1, ptr, &v1);
    _jbl_at(&d2, ptr, &v2);
    rv = _jbl_cmp_atomic_values(&v1, &v2) * desc;
    if (rv) break;
  }

finish:
  if (rc) {
    ssc->rc = rc;
    longjmp(ssc->fatal_jmp, 1);
  }
  return rv;
}

static iwrc jb_scan_sorter_apply(IWPOOL *pool, struct _JBEXEC *ctx, JQL q, struct _EJDB_DOC *doc) {
  iwrc rc = 0;
  uint64_t id = doc->id;
  struct JQP_AUX *aux = q->qp->aux;
  IWKV_val key = {
    .data = &id,
    .size = sizeof(id)
  };
  if (!pool) {
    pool = iwpool_create(1024);
    if (!pool) {
      rc = iwrc_set_errno(IW_ERROR_ALLOC, errno);
      RCRET(rc);
    }
  }
  rc = jql_apply(q, doc->raw, &doc->node, pool);
  if (aux->apply && doc->node) {
    binn bv;
    rc = _jbl_from_node(&bv, doc->node);
    RCRET(rc);
    if (bv.writable && bv.dirty) {
      binn_save_header(&bv);
    }
    IWKV_val val = {
      .data = bv.ptr,
      .size = bv.size
    };
    rc = iwkv_put(ctx->jbc->cdb, &key, &val, 0);
    binn_free(&bv);
  }
  return rc;
}

static iwrc jb_scan_sorter_do(struct _JBEXEC *ctx) {
  iwrc rc = 0;
  int64_t step = 1, id;
  struct _JBL jbl;
  EJDB_EXEC ux = ctx->ux;
  struct _JBSSC *ssc = &ctx->ssc;
  uint32_t rnum = ssc->refs_num;
  struct JQP_AUX *aux = ux->q->qp->aux;

  if (rnum) {
    if (setjmp(ssc->fatal_jmp)) { // Init error jump
      rc = ssc->rc;
      goto finish;
    }
    if (!ssc->docs) {
      size_t sp;
      rc = ssc->sof.probe_mmap(&ssc->sof, 0, &ssc->docs, &sp);
      RCGO(rc, finish);
    }
    qsort_r(ssc->refs, rnum, sizeof(ssc->refs[0]), jb_scan_sorter_cmp, ctx);
  }

  for (int64_t i = ux->skip; step && i < rnum && i >= 0;) {
    uint8_t *rp = ssc->docs + ssc->refs[i];
    memcpy(&id, rp, sizeof(id));
    rp += sizeof(id);
    rc = jbl_from_buf_keep_onstack2(&jbl, rp);
    RCGO(rc, finish);
    struct _EJDB_DOC doc = {
      .id = id,
      .raw = &jbl
    };
    if (aux->apply || aux->projection) {
      IWPOOL *pool = ux->pool;
      if (!pool) {
        pool = iwpool_create(1024);
        if (!pool) {
          rc = iwrc_set_errno(IW_ERROR_ALLOC, errno);
          goto finish;
        }
      }
      rc = jb_scan_sorter_apply(pool, ctx, ux->q, &doc);
      if (pool && pool != ux->pool) {
        iwpool_destroy(pool);
      }
      RCGO(rc, finish);
    }
    rc = ux->visitor(ux, &doc, &step);
    RCGO(rc, finish);
    i += step;
    if (--ux->limit < 1) {
      break;
    }
  }

finish:
  jb_scan_sorter_release(ctx);
  return rc;
}

static iwrc jb_scan_sorter_init(struct _JBSSC *ssc, off_t initial_size) {
  IWFS_EXT_OPTS opts = {
    .initial_size = initial_size,
    .rspolicy = iw_exfile_szpolicy_fibo,
    .file = {
      .path = "jb-",
      .omode = IWFS_OTMP | IWFS_OUNLINK
    }
  };
  iwrc rc = iwfs_exfile_open(&ssc->sof, &opts);
  RCRET(rc);
  rc = ssc->sof.add_mmap(&ssc->sof, 0, UINT64_MAX, 0);
  if (rc) {
    ssc->sof.close(&ssc->sof);
  }
  return rc;
}

iwrc jb_scan_sorter_consumer(struct _JBEXEC *ctx, IWKV_cursor cur, uint64_t id, int64_t *step, iwrc err) {
  if (!id) {
    // End of scan
    if (err) {
      // In the case of error do not perform sorting just release resources
      jb_scan_sorter_release(ctx);
      return err;
    } else {
      return jb_scan_sorter_do(ctx);
    }
  }

  iwrc rc;
  size_t vsz = 0;
  bool matched;
  struct _JBL jbl;
  struct _JBSSC *ssc = &ctx->ssc;
  EJDB db = ctx->jbc->db;
  IWFS_EXT *sof = &ssc->sof;

start: {
    if (cur) {
      rc = iwkv_cursor_copy_val(cur, ctx->jblbuf + sizeof(id), ctx->jblbufsz - sizeof(id), &vsz);
    } else {
      IWKV_val key = {
        .data = &id,
        .size = sizeof(id)
      };
      rc = iwkv_get_copy(ctx->jbc->cdb, &key, ctx->jblbuf + sizeof(id), ctx->jblbufsz - sizeof(id), &vsz);
    }
    if (rc == IWKV_ERROR_NOTFOUND) rc = 0;
    else RCRET(rc);
    if (vsz + sizeof(id) > ctx->jblbufsz) {
      size_t nsize = MAX(vsz + sizeof(id), ctx->jblbufsz * 2);
      void *nbuf = realloc(ctx->jblbuf, nsize);
      if (!nbuf) {
        return iwrc_set_errno(IW_ERROR_ALLOC, errno);
      }
      ctx->jblbuf = nbuf;
      ctx->jblbufsz = nsize;
      goto start;
    }
  }

  rc = jbl_from_buf_keep_onstack(&jbl, ctx->jblbuf + sizeof(id), vsz);
  RCRET(rc);

  rc = jql_matched(ctx->ux->q, &jbl, &matched);
  if (!matched) {
    return 0;
  }

  if (!ssc->refs) {
    ssc->refs_asz = 64 * 1024; // 64K
    ssc->refs = malloc(db->opts.document_buffer_sz);
    if (!ssc->refs) {
      return iwrc_set_errno(IW_ERROR_ALLOC, errno);
    }
    ssc->docs_asz = 128 * 1024; // 128K
    ssc->docs = malloc(ssc->docs_asz);
    if (!ssc->docs) {
      return iwrc_set_errno(IW_ERROR_ALLOC, errno);
    }
  } else if (ssc->refs_asz <= (ssc->refs_num + 1) * sizeof(ssc->refs[0])) {
    ssc->refs_asz *= 2;
    uint32_t *nrefs = realloc(ssc->refs, ssc->refs_asz);
    if (!nrefs) {
      return iwrc_set_errno(IW_ERROR_ALLOC, errno);
    }
    ssc->refs = nrefs;
  }

  vsz += sizeof(id);
  memcpy(ctx->jblbuf, &id, sizeof(id));

start2: {
    if (ssc->docs) {
      uint32_t rsize = ssc->docs_npos + vsz;
      if (rsize > ssc->docs_asz)  {
        ssc->docs_asz = MIN(rsize * 2, db->opts.sort_buffer_sz);
        if (rsize > ssc->docs_asz) {
          size_t sz;
          rc = jb_scan_sorter_init(ssc, (ssc->docs_npos + vsz) * 2);
          RCRET(rc);
          rc = sof->write(sof, 0, ssc->docs, ssc->docs_npos, &sz);
          RCRET(rc);
          free(ssc->docs);
          ssc->docs = 0;
          ssc->sof_active = true;
          goto start2;
        } else {
          void *nbuf = realloc(ssc->docs, ssc->docs_asz);
          if (!nbuf) {
            return iwrc_set_errno(IW_ERROR_ALLOC, errno);
          }
          ssc->docs = nbuf;
        }
      }
      memcpy(ssc->docs + ssc->docs_npos, ctx->jblbuf, vsz);
    } else {
      size_t sz;
      rc = sof->write(sof, ssc->docs_npos, ctx->jblbuf, vsz, &sz);
      RCRET(rc);
    }
    ssc->refs[ssc->refs_num++] = ssc->docs_npos;
    ssc->docs_npos += vsz;
  }

  return rc;
}