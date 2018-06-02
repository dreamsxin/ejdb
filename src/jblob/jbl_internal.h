#pragma once
#ifndef JBL_INTERNAL_H
#define JBL_INTERNAL_H

#include "binn.h"
#include <iowow/iwlog.h>
#include "ejdb2cfg.h"

typedef struct _JBLPTR {
  int cnt;          /**< Number of nodes */
  int pos;          /**< Current node position (like a cursor) */
  char *n[1];       /**< Path nodes */
} *JBLPTR;

struct _JBL {
  binn bn;
};

typedef struct _JBLVCTX {  
  binn *bn;
  void *op;
  void *result;
  bool terminate;
} JBLVCTX;

typedef enum {
  JBL_VCMD_OK = 0,
  JBL_VCMD_TERMINATE = 1,
  JBL_VCMD_SKIP_NESTED = 1 << 1  
} jbl_visitor_cmd_t;


typedef jbl_visitor_cmd_t (*JBLVISITOR)(int lvl, binn *bv, char *key, int idx, JBLVCTX *vctx, iwrc *rc);

#ifdef IW_TESTS
iwrc jbl_ptr(const char *path, JBLPTR *jpp);
void jbl_ptr_destroy(JBLPTR *jpp);
#endif

#endif