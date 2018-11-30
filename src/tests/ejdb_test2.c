#include "ejdb2.h"
#include <stdlib.h>
#include <CUnit/Basic.h>

int init_suite() {
  int rc = ejdb_init();
  return rc;
}

int clean_suite() {
  return 0;
}

static iwrc put_json(EJDB db, const char *coll, const char *json) {
  const size_t len = strlen(json);
  char buf[len + 1];
  memcpy(buf, json, len);
  buf[len] = '\0';
  for (int i = 0; buf[i]; ++i) {
    if (buf[i] == '\'') buf[i] = '"';
  }
  JBL jbl;
  uint64_t llv;
  iwrc rc = jbl_from_json(&jbl, buf);
  RCRET(rc);
  rc = ejdb_put_new(db, coll, jbl, &llv);
  jbl_destroy(&jbl);
  return rc;
}

// Test document sorting overflow on disk
void ejdb_test2_2() {
  EJDB_OPTS opts = {
    .kv = {
      .path = "ejdb_test2_2.db",
      .oflags = IWKV_TRUNC
    },
    .document_buffer_sz = 16 * 1024, // 16K
    .sort_buffer_sz = 1024 * 1024,   // 1M
    .no_wal = true
  };
  EJDB db;
  EJDB_LIST list = 0;
  const int vbufsz = 512 * 1024;
  const int dbufsz = vbufsz + 128;
  char *vbuf = malloc(vbufsz);
  char *dbuf = malloc(dbufsz);
  CU_ASSERT_PTR_NOT_NULL_FATAL(vbuf);
  CU_ASSERT_PTR_NOT_NULL_FATAL(dbuf);
  memset(vbuf, 'z', vbufsz);
  vbuf[vbufsz - 1] = '\0';

  iwrc rc = ejdb_open(&opts, &db);
  CU_ASSERT_EQUAL_FATAL(rc, 0);

  for (int i = 0; i < 6; ++i) {
    snprintf(dbuf, dbufsz, "{\"f\":%d, \"d\":\"%s\"}", i, vbuf);
    rc = put_json(db, "c1", dbuf);
    CU_ASSERT_EQUAL_FATAL(rc, 0);
  }
  // Here is we will overflow sort buffer
  rc = ejdb_list2(db, "c1", "/f | asc /f", 0, &list);
  CU_ASSERT_EQUAL_FATAL(rc, 0);
  int i = 0;
  for (EJDB_DOC doc = list->first; doc; doc = doc->next, ++i) {
    JBL jbl;
    rc = jbl_at(doc->raw, "/f", &jbl);
    CU_ASSERT_EQUAL_FATAL(rc, 0);
    int64_t llv = jbl_get_i64(jbl);
    jbl_destroy(&jbl);
    CU_ASSERT_EQUAL(llv, i);
  }
  ejdb_list_destroy(&list);

  rc = ejdb_close(&db);
  CU_ASSERT_EQUAL_FATAL(rc, 0);
  free(vbuf);
  free(dbuf);
}

void ejdb_test2_1() {
  EJDB_OPTS opts = {
    .kv = {
      .path = "ejdb_test2_1.db",
      .oflags = IWKV_TRUNC
    },
    .no_wal = true
  };

  EJDB db;
  uint64_t llv = 0, llv2;
  IWPOOL *pool = 0;
  EJDB_LIST list = 0;
  IWXSTR *xstr = iwxstr_new();
  CU_ASSERT_PTR_NOT_NULL_FATAL(xstr);
  int i = 0;

  iwrc rc = ejdb_open(&opts, &db);
  CU_ASSERT_EQUAL_FATAL(rc, 0);

  rc = put_json(db, "a", "{'f':2}");
  CU_ASSERT_EQUAL_FATAL(rc, 0);
  rc = put_json(db, "a", "{'f':1}");
  CU_ASSERT_EQUAL_FATAL(rc, 0);
  rc = put_json(db, "a", "{'f':3}");
  CU_ASSERT_EQUAL_FATAL(rc, 0);
  rc = put_json(db, "a", "{'a':'foo'}");
  CU_ASSERT_EQUAL_FATAL(rc, 0);
  rc = put_json(db, "a", "{'a':'gaz'}");
  CU_ASSERT_EQUAL_FATAL(rc, 0);
  rc = put_json(db, "a", "{'a':'bar'}");
  CU_ASSERT_EQUAL_FATAL(rc, 0);

  rc = ejdb_list2(db, "not_exists", "/*", 0, &list);
  CU_ASSERT_EQUAL_FATAL(rc, 0);
  i = 0;
  for (EJDB_DOC doc = list->first; doc; doc = doc->next, ++i);
  CU_ASSERT_EQUAL(i, 0);
  ejdb_list_destroy(&list);

  rc = ejdb_list2(db, "a", "/*", 0, &list);
  CU_ASSERT_EQUAL_FATAL(rc, 0);
  i = 0;
  for (EJDB_DOC doc = list->first; doc; doc = doc->next, ++i) {
    iwxstr_clear(xstr);
    rc = jbl_as_json(doc->raw, jbl_xstr_json_printer, xstr, 0);
    CU_ASSERT_EQUAL_FATAL(rc, 0);
    switch (i) {
      case 0:
        CU_ASSERT_STRING_EQUAL(iwxstr_ptr(xstr), "{\"a\":\"bar\"}");
        break;
      case 1:
        CU_ASSERT_STRING_EQUAL(iwxstr_ptr(xstr), "{\"a\":\"gaz\"}");
        break;
      case 2:
        CU_ASSERT_STRING_EQUAL(iwxstr_ptr(xstr), "{\"a\":\"foo\"}");
        break;
      case 5:
        CU_ASSERT_STRING_EQUAL(iwxstr_ptr(xstr), "{\"f\":2}");
        break;
    }
  }
  CU_ASSERT_EQUAL(i, 6);
  ejdb_list_destroy(&list);

  rc = ejdb_list2(db, "a", "/*", 1, &list);
  CU_ASSERT_EQUAL_FATAL(rc, 0);
  i = 0;
  for (EJDB_DOC doc = list->first; doc; doc = doc->next, ++i);
  CU_ASSERT_EQUAL(i, 1);
  ejdb_list_destroy(&list);

  rc = ejdb_list2(db, "a", "/f", 0, &list);
  CU_ASSERT_EQUAL_FATAL(rc, 0);
  i = 0;
  for (EJDB_DOC doc = list->first; doc; doc = doc->next, ++i);
  CU_ASSERT_EQUAL(i, 3);
  ejdb_list_destroy(&list);

  rc = ejdb_list2(db, "a", "/* | skip 1", 0, &list);
  CU_ASSERT_EQUAL_FATAL(rc, 0);
  i = 0;
  for (EJDB_DOC doc = list->first; doc; doc = doc->next, ++i) {
    iwxstr_clear(xstr);
    rc = jbl_as_json(doc->raw, jbl_xstr_json_printer, xstr, 0);
    CU_ASSERT_EQUAL_FATAL(rc, 0);
    switch (i) {
      case 0:
        CU_ASSERT_STRING_EQUAL(iwxstr_ptr(xstr), "{\"a\":\"gaz\"}");
        break;
    }
  }
  CU_ASSERT_EQUAL(i, 5);
  ejdb_list_destroy(&list);

  rc = ejdb_list2(db, "a", "/* | skip 2 limit 3", 0, &list);
  CU_ASSERT_EQUAL_FATAL(rc, 0);
  i = 0;
  for (EJDB_DOC doc = list->first; doc; doc = doc->next, ++i) {
    iwxstr_clear(xstr);
    rc = jbl_as_json(doc->raw, jbl_xstr_json_printer, xstr, 0);
    CU_ASSERT_EQUAL_FATAL(rc, 0);
    switch (i) {
      case 0:
        CU_ASSERT_STRING_EQUAL(iwxstr_ptr(xstr), "{\"a\":\"foo\"}");
        break;
    }
  }
  CU_ASSERT_EQUAL(i, 3);
  ejdb_list_destroy(&list);

  // Add {f:5}, {f:6}
  rc = put_json(db, "a", "{'f':5}");
  CU_ASSERT_EQUAL_FATAL(rc, 0);

  rc = put_json(db, "a", "{'f':6}");
  CU_ASSERT_EQUAL_FATAL(rc, 0);

  rc = ejdb_list2(db, "a", "/f | asc /f", 0, &list);
  CU_ASSERT_EQUAL_FATAL(rc, 0);
  i = 0;
  for (EJDB_DOC doc = list->first; doc; doc = doc->next, ++i) {
    iwxstr_clear(xstr);
    rc = jbl_as_json(doc->raw, jbl_xstr_json_printer, xstr, 0);
    CU_ASSERT_EQUAL_FATAL(rc, 0);
    switch (i) {
      case 0:
        CU_ASSERT_STRING_EQUAL(iwxstr_ptr(xstr), "{\"f\":1}");
        break;
      case 1:
        CU_ASSERT_STRING_EQUAL(iwxstr_ptr(xstr), "{\"f\":2}");
        break;
      case 2:
        CU_ASSERT_STRING_EQUAL(iwxstr_ptr(xstr), "{\"f\":3}");
        break;
      case 3:
        CU_ASSERT_STRING_EQUAL(iwxstr_ptr(xstr), "{\"f\":5}");
        break;
      case 4:
        CU_ASSERT_STRING_EQUAL(iwxstr_ptr(xstr), "{\"f\":6}");
        break;
    }
  }
  CU_ASSERT_EQUAL(i, 5);
  ejdb_list_destroy(&list);

  rc = ejdb_list2(db, "a", "/f | desc /f", 0, &list);
  CU_ASSERT_EQUAL_FATAL(rc, 0);
  i = 0;
  for (EJDB_DOC doc = list->first; doc; doc = doc->next, ++i) {
    iwxstr_clear(xstr);
    rc = jbl_as_json(doc->raw, jbl_xstr_json_printer, xstr, 0);
    CU_ASSERT_EQUAL_FATAL(rc, 0);
    switch (i) {
      case 0:
        CU_ASSERT_STRING_EQUAL(iwxstr_ptr(xstr), "{\"f\":6}");
        break;
      case 1:
        CU_ASSERT_STRING_EQUAL(iwxstr_ptr(xstr), "{\"f\":5}");
        break;
      case 2:
        CU_ASSERT_STRING_EQUAL(iwxstr_ptr(xstr), "{\"f\":3}");
        break;
      case 3:
        CU_ASSERT_STRING_EQUAL(iwxstr_ptr(xstr), "{\"f\":2}");
        break;
      case 4:
        CU_ASSERT_STRING_EQUAL(iwxstr_ptr(xstr), "{\"f\":1}");
        break;
    }
  }
  CU_ASSERT_EQUAL(i, 5);
  ejdb_list_destroy(&list);

  rc = ejdb_close(&db);
  CU_ASSERT_EQUAL_FATAL(rc, 0);
  iwxstr_destroy(xstr);
}

int main() {
  CU_pSuite pSuite = NULL;
  if (CUE_SUCCESS != CU_initialize_registry()) return CU_get_error();
  pSuite = CU_add_suite("ejdb_test1", init_suite, clean_suite);
  if (NULL == pSuite) {
    CU_cleanup_registry();
    return CU_get_error();
  }
  if (
    //(NULL == CU_add_test(pSuite, "ejdb_test2_1", ejdb_test2_1)) ||
    (NULL == CU_add_test(pSuite, "ejdb_test2_2", ejdb_test2_2))
  ) {
    CU_cleanup_registry();
    return CU_get_error();
  }
  CU_basic_set_mode(CU_BRM_VERBOSE);
  CU_basic_run_tests();
  int ret = CU_get_error() || CU_get_number_of_failures();
  CU_cleanup_registry();
  return ret;
}