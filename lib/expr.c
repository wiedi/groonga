/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2010-2012 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/
#include "groonga_in.h"
#include "db.h"
#include "ql.h"
#include <string.h>
#include <float.h>
#include "ii.h"
#include "geo.h"
#include "util.h"

static inline int
function_proc_p(grn_obj *expr)
{
  return (expr &&
          expr->header.type == GRN_PROC &&
          ((grn_proc *)expr)->type == GRN_PROC_FUNCTION);
}

grn_obj *
grn_expr_alloc(grn_ctx *ctx, grn_obj *expr, grn_id domain, grn_obj_flags flags)
{
  grn_obj *res = NULL;
  grn_expr *e = (grn_expr *)expr;
  if (e) {
    if (e->values_curr >= e->values_size) {
      // todo : expand values.
      ERR(GRN_NO_MEMORY_AVAILABLE, "no more e->values");
      return NULL;
    }
    res = &e->values[e->values_curr++];
    if (e->values_curr > e->values_tail) { e->values_tail = e->values_curr; }
    grn_obj_reinit(ctx, res, domain, flags);
  }
  return res;
}

static grn_hash *
grn_expr_get_vars(grn_ctx *ctx, grn_obj *expr, unsigned int *nvars)
{
  grn_hash *vars = NULL;
  if (expr->header.type == GRN_PROC || expr->header.type == GRN_EXPR) {
    grn_id id = DB_OBJ(expr)->id;
    grn_expr *e = (grn_expr *)expr;
    int added = 0;
    grn_hash **vp;
    if (grn_hash_add(ctx, ctx->impl->expr_vars, &id, sizeof(grn_id), (void **)&vp, &added)) {
      if (!*vp) {
        *vp = grn_hash_create(ctx, NULL, GRN_TABLE_MAX_KEY_SIZE, sizeof(grn_obj),
                              GRN_OBJ_KEY_VAR_SIZE|GRN_OBJ_TEMPORARY|GRN_HASH_TINY);
        if (*vp) {
          uint32_t i;
          grn_obj *value;
          grn_expr_var *v;
          for (v = e->vars, i = e->nvars; i; v++, i--) {
            grn_hash_add(ctx, *vp, v->name, v->name_size, (void **)&value, &added);
            GRN_OBJ_INIT(value, v->value.header.type, 0, v->value.header.domain);
            GRN_TEXT_PUT(ctx, value, GRN_TEXT_VALUE(&v->value), GRN_TEXT_LEN(&v->value));
          }
        }
      }
      vars = *vp;
    }
  }
  *nvars = vars ? GRN_HASH_SIZE(vars) : 0;
  return vars;
}

grn_rc
grn_expr_clear_vars(grn_ctx *ctx, grn_obj *expr)
{
  if (expr->header.type == GRN_PROC || expr->header.type == GRN_EXPR) {
    grn_hash **vp;
    grn_id eid, id = DB_OBJ(expr)->id;
    if ((eid = grn_hash_get(ctx, ctx->impl->expr_vars, &id, sizeof(grn_id), (void **)&vp))) {
      if (*vp) {
        grn_obj *value;
        GRN_HASH_EACH(ctx, *vp, i, NULL, NULL, (void **)&value, {
          GRN_OBJ_FIN(ctx, value);
        });
        grn_hash_close(ctx, *vp);
      }
      grn_hash_delete_by_id(ctx, ctx->impl->expr_vars, eid, NULL);
    }
  }
  return ctx->rc;
}

grn_obj *
grn_proc_get_info(grn_ctx *ctx, grn_user_data *user_data,
                  grn_expr_var **vars, unsigned int *nvars, grn_obj **caller)
{
  grn_proc_ctx *pctx = (grn_proc_ctx *)user_data;
  if (caller) { *caller = pctx->caller; }
  if (pctx->proc) {
    if (vars) {
      *vars = pctx->proc->vars;
   // *vars = grn_expr_get_vars(ctx, (grn_obj *)pctx->proc, nvars);
    }
    if (nvars) { *nvars = pctx->proc->nvars; }
  } else {
    if (vars) { *vars = NULL; }
    if (nvars) { *nvars = 0; }
  }
  return (grn_obj *)pctx->proc;
}

grn_obj *
grn_proc_get_var(grn_ctx *ctx, grn_user_data *user_data, const char *name, unsigned int name_size)
{
  grn_proc_ctx *pctx = (grn_proc_ctx *)user_data;
  return pctx->proc ? grn_expr_get_var(ctx, (grn_obj *)pctx->proc, name, name_size) : NULL;
}

grn_obj *
grn_proc_get_var_by_offset(grn_ctx *ctx, grn_user_data *user_data, unsigned int offset)
{
  grn_proc_ctx *pctx = (grn_proc_ctx *)user_data;
  return pctx->proc ? grn_expr_get_var_by_offset(ctx, (grn_obj *)pctx->proc, offset) : NULL;
}

grn_obj *
grn_proc_alloc(grn_ctx *ctx, grn_user_data *user_data, grn_id domain, grn_obj_flags flags)
{
  grn_proc_ctx *pctx = (grn_proc_ctx *)user_data;
  return pctx->caller ? grn_expr_alloc(ctx, (grn_obj *)pctx->caller, domain, flags) : NULL;
}

/* grn_expr */

static const char *opstrs[] = {
  "PUSH",
  "POP",
  "NOP",
  "CALL",
  "INTERN",
  "GET_REF",
  "GET_VALUE",
  "AND",
  "BUT",
  "OR",
  "ASSIGN",
  "STAR_ASSIGN",
  "SLASH_ASSIGN",
  "MOD_ASSIGN",
  "PLUS_ASSIGN",
  "MINUS_ASSIGN",
  "SHIFTL_ASSIGN",
  "SHIFTR_ASSIGN",
  "SHIFTRR_ASSIGN",
  "AND_ASSIGN",
  "XOR_ASSIGN",
  "OR_ASSIGN",
  "JUMP",
  "CJUMP",
  "COMMA",
  "BITWISE_OR",
  "BITWISE_XOR",
  "BITWISE_AND",
  "BITWISE_NOT",
  "EQUAL",
  "NOT_EQUAL",
  "LESS",
  "GREATER",
  "LESS_EQUAL",
  "GREATER_EQUAL",
  "IN",
  "MATCH",
  "NEAR",
  "NEAR2",
  "SIMILAR",
  "TERM_EXTRACT",
  "SHIFTL",
  "SHIFTR",
  "SHIFTRR",
  "PLUS",
  "MINUS",
  "STAR",
  "SLASH",
  "MOD",
  "DELETE",
  "INCR",
  "DECR",
  "INCR_POST",
  "DECR_POST",
  "NOT",
  "ADJUST",
  "EXACT",
  "LCP",
  "PARTIAL",
  "UNSPLIT",
  "PREFIX",
  "SUFFIX",
  "GEO_DISTANCE1",
  "GEO_DISTANCE2",
  "GEO_DISTANCE3",
  "GEO_DISTANCE4",
  "GEO_WITHINP5",
  "GEO_WITHINP6",
  "GEO_WITHINP8",
  "OBJ_SEARCH",
  "EXPR_GET_VAR",
  "TABLE_CREATE",
  "TABLE_SELECT",
  "TABLE_SORT",
  "TABLE_GROUP",
  "JSON_PUT"
};

static void
put_value(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  int len;
  char namebuf[GRN_TABLE_MAX_KEY_SIZE];
  if ((len = grn_column_name(ctx, obj, namebuf, GRN_TABLE_MAX_KEY_SIZE))) {
    GRN_TEXT_PUT(ctx, buf, namebuf, len);
  } else {
    grn_text_otoj(ctx, buf, obj, NULL);
  }
}

grn_rc
grn_expr_inspect(grn_ctx *ctx, grn_obj *buf, grn_obj *expr)
{
  uint32_t i, j;
  grn_expr_var *var;
  grn_expr_code *code;
  grn_expr *e = (grn_expr *)expr;
  grn_hash *vars = grn_expr_get_vars(ctx, expr, &i);
  GRN_TEXT_PUTS(ctx, buf, "noname");
  GRN_TEXT_PUTC(ctx, buf, '(');
  {
    int i = 0;
    grn_obj *value;
    const char *name;
    uint32_t name_len;
    GRN_HASH_EACH(ctx, vars, id, &name, &name_len, &value, {
      if (i++) { GRN_TEXT_PUTC(ctx, buf, ','); }
      GRN_TEXT_PUT(ctx, buf, name, name_len);
      GRN_TEXT_PUTC(ctx, buf, ':');
      put_value(ctx, buf, value);
    });
  }
  GRN_TEXT_PUTC(ctx, buf, ')');
  GRN_TEXT_PUTC(ctx, buf, '{');
  for (j = 0, code = e->codes; j < e->codes_curr; j++, code++) {
    if (j) { GRN_TEXT_PUTC(ctx, buf, ','); }
    grn_text_itoa(ctx, buf, code->modify);
    if (code->op == GRN_OP_PUSH) {
      for (i = 0, var = e->vars; i < e->nvars; i++, var++) {
        if (&var->value == code->value) {
          GRN_TEXT_PUTC(ctx, buf, '?');
          if (var->name_size) {
            GRN_TEXT_PUT(ctx, buf, var->name, var->name_size);
          } else {
            grn_text_itoa(ctx, buf, (int)i);
          }
          break;
        }
      }
      if (i == e->nvars) {
        put_value(ctx, buf, code->value);
      }
    } else {
      if (code->value) {
        put_value(ctx, buf, code->value);
        GRN_TEXT_PUTC(ctx, buf, ' ');
      }
      GRN_TEXT_PUTS(ctx, buf, opstrs[code->op]);
    }
  }
  GRN_TEXT_PUTC(ctx, buf, '}');
  return GRN_SUCCESS;
}

grn_obj *
grn_ctx_pop(grn_ctx *ctx)
{
  if (ctx && ctx->impl && ctx->impl->stack_curr) {
    return ctx->impl->stack[--ctx->impl->stack_curr];
  }
  return NULL;
}

grn_rc
grn_ctx_push(grn_ctx *ctx, grn_obj *obj)
{
  if (ctx && ctx->impl && ctx->impl->stack_curr < GRN_STACK_SIZE) {
    ctx->impl->stack[ctx->impl->stack_curr++] = obj;
    return GRN_SUCCESS;
  }
  return GRN_STACK_OVER_FLOW;
}

static grn_obj *
const_new(grn_ctx *ctx, grn_expr *e)
{
  if (!e->consts) {
    if (!(e->consts = GRN_MALLOCN(grn_obj, GRN_STACK_SIZE))) {
      ERR(GRN_NO_MEMORY_AVAILABLE, "malloc failed");
      return NULL;
    }
  }
  if (e->nconsts < GRN_STACK_SIZE) {
    return e->consts + e->nconsts++;
  } else {
    ERR(GRN_STACK_OVER_FLOW, "too many constants.");
    return NULL;
  }
}

void
grn_obj_pack(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  grn_text_benc(ctx, buf, obj->header.type);
  if (GRN_DB_OBJP(obj)) {
    grn_text_benc(ctx, buf, DB_OBJ(obj)->id);
  } else {
    // todo : support vector, query, accessor, snip..
    uint32_t vs = GRN_BULK_VSIZE(obj);
    grn_text_benc(ctx, buf, obj->header.domain);
    grn_text_benc(ctx, buf, vs);
    if (vs) { GRN_TEXT_PUT(ctx, buf, GRN_BULK_HEAD(obj), vs); }
  }
}

const uint8_t *
grn_obj_unpack(grn_ctx *ctx, const uint8_t *p, const uint8_t *pe, uint8_t type, uint8_t flags, grn_obj *obj)
{
  grn_id domain;
  uint32_t vs;
  GRN_B_DEC(domain, p);
  GRN_OBJ_INIT(obj, type, flags, domain);
  GRN_B_DEC(vs, p);
  if (pe < p + vs) {
    ERR(GRN_INVALID_FORMAT, "benced image is corrupt");
    return p;
  }
  grn_bulk_write(ctx, obj, p, vs);
  return p + vs;
}

void
grn_expr_pack(grn_ctx *ctx, grn_obj *buf, grn_obj *expr)
{
  grn_expr_code *c;
  grn_expr_var *v;
  grn_expr *e = (grn_expr *)expr;
  uint32_t i, j;
  grn_text_benc(ctx, buf, e->nvars);
  for (i = e->nvars, v = e->vars; i; i--, v++) {
    grn_text_benc(ctx, buf, v->name_size);
    if (v->name_size) { GRN_TEXT_PUT(ctx, buf, v->name, v->name_size); }
    grn_obj_pack(ctx, buf, &v->value);
  }
  i = e->codes_curr;
  grn_text_benc(ctx, buf, i);
  for (c = e->codes; i; i--, c++) {
    grn_text_benc(ctx, buf, c->op);
    grn_text_benc(ctx, buf, c->nargs);
    if (!c->value) {
      grn_text_benc(ctx, buf, 0); /* NULL */
    } else {
      for (j = 0, v = e->vars; j < e->nvars; j++, v++) {
        if (&v->value == c->value) {
          grn_text_benc(ctx, buf, 1); /* variable */
          grn_text_benc(ctx, buf, j);
          break;
        }
      }
      if (j == e->nvars) {
        grn_text_benc(ctx, buf, 2); /* others */
        grn_obj_pack(ctx, buf, c->value);
      }
    }
  }
}

const uint8_t *
grn_expr_unpack(grn_ctx *ctx, const uint8_t *p, const uint8_t *pe, grn_obj *expr)
{
  grn_obj *v;
  uint8_t type;
  uint32_t i, n, ns;
  grn_expr_code *code;
  grn_expr *e = (grn_expr *)expr;
  GRN_B_DEC(n, p);
  for (i = 0; i < n; i++) {
    GRN_B_DEC(ns, p);
    v = grn_expr_add_var(ctx, expr, ns ? p : NULL, ns);
    p += ns;
    GRN_B_DEC(type, p);
    if (GRN_TYPE <= type && type <= GRN_COLUMN_INDEX) { /* error */ }
    p = grn_obj_unpack(ctx, p, pe, type, 0, v);
    if (pe < p) {
      ERR(GRN_INVALID_FORMAT, "benced image is corrupt");
      return p;
    }
  }
  GRN_B_DEC(n, p);
  /* confirm e->codes_size >= n */
  e->codes_curr = n;
  for (i = 0, code = e->codes; i < n; i++, code++) {
    GRN_B_DEC(code->op, p);
    GRN_B_DEC(code->nargs, p);
    GRN_B_DEC(type, p);
    switch (type) {
    case 0 : /* NULL */
      code->value = NULL;
      break;
    case 1 : /* variable */
      {
        uint32_t offset;
        GRN_B_DEC(offset, p);
        code->value = &e->vars[i].value;
      }
      break;
    case 2 : /* others */
      GRN_B_DEC(type, p);
      if (GRN_TYPE <= type && type <= GRN_COLUMN_INDEX) {
        grn_id id;
        GRN_B_DEC(id, p);
        code->value = grn_ctx_at(ctx, id);
      } else {
        if (!(v = const_new(ctx, e))) { return NULL; }
        p = grn_obj_unpack(ctx, p, pe, type, GRN_OBJ_EXPRCONST, v);
        code->value = v;
      }
      break;
    }
    if (pe < p) {
      ERR(GRN_INVALID_FORMAT, "benced image is corrupt");
      return p;
    }
  }
  return p;
}

grn_obj *
grn_expr_open(grn_ctx *ctx, grn_obj_spec *spec, const uint8_t *p, const uint8_t *pe)
{
  grn_expr *expr = NULL;
  if ((expr = GRN_MALLOCN(grn_expr, 1))) {
    int size = 256;
    expr->consts = NULL;
    expr->nconsts = 0;
    GRN_TEXT_INIT(&expr->name_buf, 0);
    GRN_TEXT_INIT(&expr->dfi, 0);
    GRN_PTR_INIT(&expr->objs, GRN_OBJ_VECTOR, GRN_ID_NIL);
    expr->vars = NULL;
    expr->nvars = 0;
    GRN_DB_OBJ_SET_TYPE(expr, GRN_EXPR);
    if ((expr->values = GRN_MALLOCN(grn_obj, size))) {
      int i;
      for (i = 0; i < size; i++) {
        GRN_OBJ_INIT(&expr->values[i], GRN_BULK, GRN_OBJ_EXPRVALUE, GRN_ID_NIL);
      }
      expr->values_curr = 0;
      expr->values_tail = 0;
      expr->values_size = size;
      if ((expr->codes = GRN_MALLOCN(grn_expr_code, size))) {
        expr->codes_curr = 0;
        expr->codes_size = size;
        expr->obj.header = spec->header;
        if (grn_expr_unpack(ctx, p, pe, (grn_obj *)expr) == pe) {
          goto exit;
        } else {
          ERR(GRN_INVALID_FORMAT, "benced image is corrupt");
        }
        GRN_FREE(expr->codes);
      }
      GRN_FREE(expr->values);
    }
    GRN_FREE(expr);
    expr = NULL;
  }
exit :
  return (grn_obj *)expr;
}

/* data flow info */
typedef struct {
  grn_expr_code *code;
  grn_id domain;
  unsigned char type;
} grn_expr_dfi;

#define DFI_POP(e,d) {\
  if (GRN_BULK_VSIZE(&(e)->dfi) >= sizeof(grn_expr_dfi)) {\
    GRN_BULK_INCR_LEN((&(e)->dfi), -(sizeof(grn_expr_dfi)));\
    (d) = (grn_expr_dfi *)(GRN_BULK_CURR(&(e)->dfi));\
    (e)->code0 = (d)->code;\
  } else {\
    (d) = NULL;\
    (e)->code0 = NULL;\
  }\
}

#define DFI_PUT(e,t,d,c) {\
  grn_expr_dfi dfi;\
  dfi.type = (t);\
  dfi.domain = (d);\
  dfi.code = (c);\
  if ((e)->code0) { (e)->code0->modify = (c) ? ((c) - (e)->code0) : 0; }\
  grn_bulk_write(ctx, &(e)->dfi, (char *)&dfi, sizeof(grn_expr_dfi));\
  (e)->code0 = NULL;\
}

grn_expr_dfi *
dfi_value_at(grn_expr *expr, int offset)
{
  grn_obj *obj = &expr->dfi;
  int size = GRN_BULK_VSIZE(obj) / sizeof(grn_expr_dfi);
  if (offset < 0) { offset = size + offset; }
  return (0 <= offset && offset < size)
    ? &(((grn_expr_dfi *)GRN_BULK_HEAD(obj))[offset])
    : NULL;
}

grn_obj *
grn_expr_create(grn_ctx *ctx, const char *name, unsigned int name_size)
{
  grn_id id;
  grn_obj *db;
  grn_expr *expr = NULL;
  if (!ctx || !ctx->impl || !(db = ctx->impl->db)) {
    ERR(GRN_INVALID_ARGUMENT, "db not initialized");
    return NULL;
  }
  if (name_size) {
    ERR(GRN_FUNCTION_NOT_IMPLEMENTED, "");
    return NULL;
  }
  GRN_API_ENTER;
  if (grn_db_check_name(ctx, name, name_size)) {
    GRN_DB_CHECK_NAME_ERR("[expr][create]", name, name_size);
    GRN_API_RETURN(NULL);
  }
  if (!GRN_DB_P(db)) {
    ERR(GRN_INVALID_ARGUMENT, "named expr is not supported");
    GRN_API_RETURN(NULL);
  }
  id = grn_obj_register(ctx, db, name, name_size);
  if (id && (expr = GRN_MALLOCN(grn_expr, 1))) {
    int size = GRN_STACK_SIZE;
    expr->consts = NULL;
    expr->nconsts = 0;
    GRN_TEXT_INIT(&expr->name_buf, 0);
    GRN_TEXT_INIT(&expr->dfi, 0);
    GRN_PTR_INIT(&expr->objs, GRN_OBJ_VECTOR, GRN_ID_NIL);
    expr->code0 = NULL;
    expr->vars = NULL;
    expr->nvars = 0;
    expr->cacheable = 1;
    expr->taintable = 0;
    expr->values_curr = 0;
    expr->values_tail = 0;
    expr->values_size = size;
    expr->codes_curr = 0;
    expr->codes_size = size;
    GRN_DB_OBJ_SET_TYPE(expr, GRN_EXPR);
    expr->obj.header.domain = GRN_ID_NIL;
    expr->obj.range = GRN_ID_NIL;
    if (!grn_db_obj_init(ctx, db, id, DB_OBJ(expr))) {
      if ((expr->values = GRN_MALLOCN(grn_obj, size))) {
        int i;
        for (i = 0; i < size; i++) {
          GRN_OBJ_INIT(&expr->values[i], GRN_BULK, GRN_OBJ_EXPRVALUE, GRN_ID_NIL);
        }
        if ((expr->codes = GRN_MALLOCN(grn_expr_code, size))) {
          goto exit;
        }
        GRN_FREE(expr->values);
      }
    }
    GRN_FREE(expr);
    expr = NULL;
  }
exit :
  GRN_API_RETURN((grn_obj *)expr);
}

#define GRN_PTR_POP(obj,value) {\
  if (GRN_BULK_VSIZE(obj) >= sizeof(grn_obj *)) {\
    GRN_BULK_INCR_LEN((obj), -(sizeof(grn_obj *)));\
    value = *(grn_obj **)(GRN_BULK_CURR(obj));\
  } else {\
    value = NULL;\
  }\
}

grn_rc
grn_expr_close(grn_ctx *ctx, grn_obj *expr)
{
  uint32_t i;
  grn_expr *e = (grn_expr *)expr;
  GRN_API_ENTER;
  /*
  if (e->obj.header.domain) {
    grn_hash_delete(ctx, ctx->impl->qe, &e->obj.header.domain, sizeof(grn_id), NULL);
  }
  */
  grn_expr_clear_vars(ctx, expr);
  for (i = 0; i < e->nconsts; i++) {
    grn_obj_close(ctx, &e->consts[i]);
  }
  if (e->consts) { GRN_FREE(e->consts);  }
  grn_obj_close(ctx, &e->name_buf);
  grn_obj_close(ctx, &e->dfi);
  for (;;) {
    grn_obj *obj;
    GRN_PTR_POP(&e->objs, obj);
    if (obj) {
#ifdef ENABLE_MEMORY_DEBUG
      grn_obj_unlink(ctx, obj);
#else
      if (obj->header.type) {
        grn_obj_unlink(ctx, obj);
      } else {
        GRN_LOG(ctx, GRN_LOG_WARNING, "GRN_VOID object is tried to be unlinked");
      }
#endif
    } else { break; }
  }
  grn_obj_close(ctx, &e->objs);
  for (i = 0; i < e->nvars; i++) {
    grn_obj_close(ctx, &e->vars[i].value);
  }
  if (e->vars) { GRN_FREE(e->vars); }
  for (i = 0; i < e->values_tail; i++) {
    grn_obj_close(ctx, &e->values[i]);
  }
  GRN_FREE(e->values);
  GRN_FREE(e->codes);
  GRN_FREE(e);
  GRN_API_RETURN(ctx->rc);
}

grn_obj *
grn_expr_add_var(grn_ctx *ctx, grn_obj *expr, const char *name, unsigned int name_size)
{
  uint32_t i;
  char *p;
  grn_expr_var *v;
  grn_obj *res = NULL;
  grn_expr *e = (grn_expr *)expr;
  GRN_API_ENTER;
  if (DB_OBJ(expr)->id & GRN_OBJ_TMP_OBJECT) {
    res = grn_expr_get_or_add_var(ctx, expr, name, name_size);
  } else {
    if (!e->vars) {
      if (!(e->vars = GRN_MALLOCN(grn_expr_var, GRN_STACK_SIZE))) {
        ERR(GRN_NO_MEMORY_AVAILABLE, "malloc failed");
      }
    }
    if (e->vars && e->nvars < GRN_STACK_SIZE) {
      v = e->vars + e->nvars++;
      if (name_size) {
        GRN_TEXT_PUT(ctx, &e->name_buf, name, name_size);
      } else {
        uint32_t ol = GRN_TEXT_LEN(&e->name_buf);
        GRN_TEXT_PUTC(ctx, &e->name_buf, '$');
        grn_text_itoa(ctx, &e->name_buf, e->nvars);
        name_size = GRN_TEXT_LEN(&e->name_buf) - ol;
      }
      v->name_size = name_size;
      res = &v->value;
      GRN_VOID_INIT(res);
      for (i = e->nvars, p = GRN_TEXT_VALUE(&e->name_buf), v = e->vars; i; i--, v++) {
        v->name = p;
        p += v->name_size;
      }
    }
  }
  GRN_API_RETURN(res);
}

grn_obj *
grn_expr_get_var(grn_ctx *ctx, grn_obj *expr, const char *name, unsigned int name_size)
{
  uint32_t n;
  grn_obj *res = NULL;
  grn_hash *vars = grn_expr_get_vars(ctx, expr, &n);
  if (vars) { grn_hash_get(ctx, vars, name, name_size, (void **)&res); }
  return res;
}

grn_obj *
grn_expr_get_or_add_var(grn_ctx *ctx, grn_obj *expr, const char *name, unsigned int name_size)
{
  uint32_t n;
  grn_obj *res = NULL;
  grn_hash *vars = grn_expr_get_vars(ctx, expr, &n);
  if (vars) {
    int added = 0;
    char name_buf[16];
    if (!name_size) {
      char *rest;
      name_buf[0] = '$';
      grn_itoa((int)GRN_HASH_SIZE(vars) + 1, name_buf + 1, name_buf + 16, &rest);
      name_size = rest - name_buf;
      name = name_buf;
    }
    grn_hash_add(ctx, vars, name, name_size, (void **)&res, &added);
    if (added) { GRN_TEXT_INIT(res, 0); }
  }
  return res;
}

grn_obj *
grn_expr_get_var_by_offset(grn_ctx *ctx, grn_obj *expr, unsigned int offset)
{
  uint32_t n;
  grn_obj *res = NULL;
  grn_hash *vars = grn_expr_get_vars(ctx, expr, &n);
  if (vars) { res = (grn_obj *)grn_hash_get_value_(ctx, vars, offset + 1, &n); }
  return res;
}

#define EXPRVP(x) ((x)->header.impl_flags & GRN_OBJ_EXPRVALUE)

#define CONSTP(obj) ((obj) && ((obj)->header.impl_flags & GRN_OBJ_EXPRCONST))

#define PUSH_CODE(e,o,v,n,c) {\
  (c) = &(e)->codes[e->codes_curr++];\
  (c)->value = (v);\
  (c)->nargs = (n);\
  (c)->op = (o);\
  (c)->flags = 0;\
  (c)->modify = 0;\
}

#define APPEND_UNARY_MINUS_OP(e) {                              \
  grn_expr_code *code_;                                         \
  grn_id domain;                                                \
  unsigned char type;                                           \
  grn_obj *x;                                                   \
  DFI_POP(e, dfi);                                              \
  code_ = dfi->code;                                            \
  domain = dfi->domain;                                         \
  type = dfi->type;                                             \
  x = code_->value;                                             \
  if (CONSTP(x)) {                                              \
    switch (domain) {                                           \
    case GRN_DB_UINT32:                                         \
      {                                                         \
        unsigned int value;                                     \
        value = GRN_UINT32_VALUE(x);                            \
        if (value > (unsigned int)0x80000000) {                 \
          domain = GRN_DB_INT64;                                \
          x->header.domain = domain;                            \
          GRN_INT64_SET(ctx, x, -((long long int)value));       \
        } else {                                                \
          domain = GRN_DB_INT32;                                \
          x->header.domain = domain;                            \
          GRN_INT32_SET(ctx, x, -((int)value));                 \
        }                                                       \
      }                                                         \
      break;                                                    \
    case GRN_DB_INT64:                                          \
      GRN_INT64_SET(ctx, x, -GRN_INT64_VALUE(x));               \
      break;                                                    \
    case GRN_DB_FLOAT:                                          \
      GRN_FLOAT_SET(ctx, x, -GRN_FLOAT_VALUE(x));               \
      break;                                                    \
    default:                                                    \
      PUSH_CODE(e, op, obj, nargs, code);                       \
      break;                                                    \
    }                                                           \
  } else {                                                      \
    PUSH_CODE(e, op, obj, nargs, code);                         \
  }                                                             \
  DFI_PUT(e, type, domain, code_);                              \
}

#define PUSH_N_ARGS_ARITHMETIC_OP(e, op, obj, nargs, code) {    \
  PUSH_CODE(e, op, obj, nargs, code);                           \
  {                                                             \
    int i = nargs;                                              \
    while (i--) {                                               \
      DFI_POP(e, dfi);                                          \
    }                                                           \
  }                                                             \
  DFI_PUT(e, type, domain, code);                               \
}

grn_obj *
grn_expr_append_obj(grn_ctx *ctx, grn_obj *expr, grn_obj *obj, grn_operator op, int nargs)
{
  uint8_t type = GRN_VOID;
  grn_id domain = GRN_ID_NIL;
  grn_expr_dfi *dfi;
  grn_expr_code *code;
  grn_obj *res = NULL;
  grn_expr *e = (grn_expr *)expr;
  GRN_API_ENTER;
  if (e->codes_curr >= e->codes_size) {
    ERR(GRN_NO_MEMORY_AVAILABLE, "stack is full");
    goto exit;
  }
  {
    switch (op) {
    case GRN_OP_PUSH :
      if (obj) {
        PUSH_CODE(e, op, obj, nargs, code);
        DFI_PUT(e, obj->header.type, GRN_OBJ_GET_DOMAIN(obj), code);
      } else {
        ERR(GRN_INVALID_ARGUMENT, "obj not assigned for GRN_OP_PUSH");
        goto exit;
      }
      break;
    case GRN_OP_NOP :
      /* nop */
      break;
    case GRN_OP_POP :
      if (obj) {
        ERR(GRN_INVALID_ARGUMENT, "obj assigned for GRN_OP_POP");
        goto exit;
      } else {
        PUSH_CODE(e, op, obj, nargs, code);
        DFI_POP(e, dfi);
      }
      break;
    case GRN_OP_CALL :
      {
        grn_obj *proc = NULL;
        if (e->codes_curr - nargs > 0) {
          proc = e->codes[e->codes_curr - nargs - 1].value;
        }
        if (proc && !function_proc_p(proc)) {
          grn_obj buffer;

          GRN_TEXT_INIT(&buffer, 0);
          switch (proc->header.type) {
          case GRN_TABLE_HASH_KEY:
          case GRN_TABLE_PAT_KEY:
          case GRN_TABLE_NO_KEY:
          case GRN_COLUMN_FIX_SIZE:
          case GRN_COLUMN_VAR_SIZE:
          case GRN_COLUMN_INDEX:
            grn_inspect_name(ctx, &buffer, proc);
            break;
          default:
            grn_inspect(ctx, &buffer, proc);
            break;
          }
          ERR(GRN_INVALID_ARGUMENT, "invalid function: <%.*s>",
              GRN_TEXT_LEN(&buffer), GRN_TEXT_VALUE(&buffer));
          GRN_OBJ_FIN(ctx, &buffer);
          goto exit;
        }
      }
      PUSH_CODE(e, op, obj, nargs, code);
      {
        int i = nargs;
        while (i--) { DFI_POP(e, dfi); }
      }
      if (!obj) { DFI_POP(e, dfi); }
      // todo : increment e->values_tail.
      DFI_PUT(e, type, domain, code); /* cannot identify type of return value */
      e->cacheable = 0;
      break;
    case GRN_OP_INTERN :
      if (obj && CONSTP(obj)) {
        grn_obj *value;
        value = grn_expr_get_var(ctx, expr, GRN_TEXT_VALUE(obj), GRN_TEXT_LEN(obj));
        if (!value) { value = grn_ctx_get(ctx, GRN_TEXT_VALUE(obj), GRN_TEXT_LEN(obj)); }
        if (value) {
          obj = value;
          op = GRN_OP_PUSH;
          type = obj->header.type;
          domain = GRN_OBJ_GET_DOMAIN(obj);
        }
      }
      PUSH_CODE(e, op, obj, nargs, code);
      DFI_PUT(e, type, domain, code);
      break;
    case GRN_OP_EQUAL :
      PUSH_CODE(e, op, obj, nargs, code);
      if (nargs) {
        grn_id xd, yd = GRN_ID_NIL;
        grn_obj *x, *y = NULL;
        int i = nargs - 1;
        if (obj) {
          xd = GRN_OBJ_GET_DOMAIN(obj);
          x = obj;
        } else {
          DFI_POP(e, dfi);
          x = dfi->code->value;
          xd = dfi->domain;
        }
        while (i--) {
          DFI_POP(e, dfi);
          y = dfi->code->value;
          yd = dfi->domain;
        }
        if (CONSTP(x)) {
          if (CONSTP(y)) {
            /* todo */
          } else {
            grn_obj dest;
            if (xd != yd) {
              GRN_OBJ_INIT(&dest, GRN_BULK, 0, yd);
              if (!grn_obj_cast(ctx, x, &dest, 0)) {
                grn_obj_reinit(ctx, x, yd, 0);
                grn_bulk_write(ctx, x, GRN_BULK_HEAD(&dest), GRN_BULK_VSIZE(&dest));
              }
              GRN_OBJ_FIN(ctx, &dest);
            }
          }
        } else {
          if (CONSTP(y)) {
            grn_obj dest;
            if (xd != yd) {
              GRN_OBJ_INIT(&dest, GRN_BULK, 0, xd);
              if (!grn_obj_cast(ctx, y, &dest, 0)) {
                grn_obj_reinit(ctx, y, xd, 0);
                grn_bulk_write(ctx, y, GRN_BULK_HEAD(&dest), GRN_BULK_VSIZE(&dest));
              }
              GRN_OBJ_FIN(ctx, &dest);
            }
          }
        }
      }
      DFI_PUT(e, type, domain, code);
      break;
    case GRN_OP_TABLE_CREATE :
    case GRN_OP_EXPR_GET_VAR :
    case GRN_OP_MATCH :
    case GRN_OP_PREFIX :
    case GRN_OP_SUFFIX :
    case GRN_OP_NOT_EQUAL :
    case GRN_OP_LESS :
    case GRN_OP_GREATER :
    case GRN_OP_LESS_EQUAL :
    case GRN_OP_GREATER_EQUAL :
    case GRN_OP_GEO_DISTANCE1 :
    case GRN_OP_GEO_DISTANCE2 :
    case GRN_OP_GEO_DISTANCE3 :
    case GRN_OP_GEO_DISTANCE4 :
    case GRN_OP_GEO_WITHINP5 :
    case GRN_OP_GEO_WITHINP6 :
    case GRN_OP_GEO_WITHINP8 :
    case GRN_OP_OBJ_SEARCH :
    case GRN_OP_TABLE_SELECT :
    case GRN_OP_TABLE_SORT :
    case GRN_OP_TABLE_GROUP :
    case GRN_OP_JSON_PUT :
    case GRN_OP_GET_REF :
    case GRN_OP_ADJUST :
    case GRN_OP_TERM_EXTRACT :
      PUSH_CODE(e, op, obj, nargs, code);
      if (nargs) {
        int i = nargs - 1;
        if (!obj) { DFI_POP(e, dfi); }
        while (i--) { DFI_POP(e, dfi); }
      }
      DFI_PUT(e, type, domain, code);
      break;
    case GRN_OP_AND :
    case GRN_OP_OR :
    case GRN_OP_BUT :
      PUSH_CODE(e, op, obj, nargs, code);
      if (nargs != 2) {
        GRN_LOG(ctx, GRN_LOG_WARNING, "nargs(%d) != 2 in relative op", nargs);
      }
      if (obj) {
        GRN_LOG(ctx, GRN_LOG_WARNING, "obj assigned to relative op");
      }
      {
        int i = nargs;
        while (i--) {
          DFI_POP(e, dfi);
          if (dfi) {
            dfi->code->flags |= GRN_EXPR_CODE_RELATIONAL_EXPRESSION;
          } else {
            ERR(GRN_SYNTAX_ERROR, "stack under flow in relative op");
          }
        }
      }
      DFI_PUT(e, type, domain, code);
      break;
    case GRN_OP_PLUS :
      if (nargs > 1) {
        PUSH_N_ARGS_ARITHMETIC_OP(e, op, obj, nargs, code);
      }
      break;
    case GRN_OP_MINUS :
      if (nargs == 1) {
        APPEND_UNARY_MINUS_OP(e);
      } else {
        PUSH_N_ARGS_ARITHMETIC_OP(e, op, obj, nargs, code);
      }
      break;
    case GRN_OP_BITWISE_NOT :
      PUSH_CODE(e, op, obj, nargs, code);
      break;
    case GRN_OP_STAR :
    case GRN_OP_SLASH :
    case GRN_OP_MOD :
    case GRN_OP_SHIFTL :
    case GRN_OP_SHIFTR :
    case GRN_OP_SHIFTRR :
    case GRN_OP_BITWISE_OR :
    case GRN_OP_BITWISE_XOR :
    case GRN_OP_BITWISE_AND :
      PUSH_N_ARGS_ARITHMETIC_OP(e, op, obj, nargs, code);
      break;
    case GRN_OP_INCR :
    case GRN_OP_DECR :
    case GRN_OP_INCR_POST :
    case GRN_OP_DECR_POST :
      {
        DFI_POP(e, dfi);
        if (dfi) {
          type = dfi->type;
          domain = dfi->domain;
          if (dfi->code) {
            if (dfi->code->op == GRN_OP_GET_VALUE) {
              dfi->code->op = GRN_OP_GET_REF;
            }
            if (dfi->code->value && grn_obj_is_persistent(ctx, dfi->code->value)) {
              e->cacheable = 0;
              e->taintable = 1;
            }
          }
        }
        PUSH_CODE(e, op, obj, nargs, code);
      }
      DFI_PUT(e, type, domain, code);
      break;
    case GRN_OP_GET_VALUE :
      {
        grn_id vdomain = GRN_ID_NIL;
        if (obj) {
          if (nargs == 1) {
            grn_obj *v = grn_expr_get_var_by_offset(ctx, expr, 0);
            if (v) { vdomain = GRN_OBJ_GET_DOMAIN(v); }
          } else {
            DFI_POP(e, dfi);
            vdomain = dfi->domain;
          }
          if (vdomain && CONSTP(obj) && obj->header.type == GRN_BULK) {
            grn_obj *table = grn_ctx_at(ctx, vdomain);
            grn_obj *col = grn_obj_column(ctx, table, GRN_BULK_HEAD(obj), GRN_BULK_VSIZE(obj));
            if (col) {
              obj = col;
              type = col->header.type;
              domain = grn_obj_get_range(ctx, col);
              grn_obj_unlink(ctx, col);
            }
          } else {
            domain = grn_obj_get_range(ctx, obj);
          }
          PUSH_CODE(e, op, obj, nargs, code);
        } else {
          grn_expr_dfi *dfi0;
          DFI_POP(e, dfi0);
          if (nargs == 1) {
            grn_obj *v = grn_expr_get_var_by_offset(ctx, expr, 0);
            if (v) { vdomain = GRN_OBJ_GET_DOMAIN(v); }
          } else {
            DFI_POP(e, dfi);
            vdomain = dfi->domain;
          }
          if (dfi0->code->op == GRN_OP_PUSH) {
            dfi0->code->op = op;
            dfi0->code->nargs = nargs;
            obj = dfi0->code->value;
            if (vdomain && obj && CONSTP(obj) && obj->header.type == GRN_BULK) {
              grn_obj *table = grn_ctx_at(ctx, vdomain);
              grn_obj *col = grn_obj_column(ctx, table, GRN_BULK_HEAD(obj), GRN_BULK_VSIZE(obj));
              if (col) {
                dfi0->code->value = col;
                type = col->header.type;
                domain = grn_obj_get_range(ctx, col);
                grn_obj_unlink(ctx, col);
              }
            } else {
              domain = grn_obj_get_range(ctx, obj);
            }
            code = dfi0->code;
          } else {
            PUSH_CODE(e, op, obj, nargs, code);
          }
        }
      }
      DFI_PUT(e, type, domain, code);
      break;
    case GRN_OP_ASSIGN :
    case GRN_OP_STAR_ASSIGN :
    case GRN_OP_SLASH_ASSIGN :
    case GRN_OP_MOD_ASSIGN :
    case GRN_OP_PLUS_ASSIGN :
    case GRN_OP_MINUS_ASSIGN :
    case GRN_OP_SHIFTL_ASSIGN :
    case GRN_OP_SHIFTR_ASSIGN :
    case GRN_OP_SHIFTRR_ASSIGN :
    case GRN_OP_AND_ASSIGN :
    case GRN_OP_OR_ASSIGN :
    case GRN_OP_XOR_ASSIGN :
      {
        if (obj) {
          type = obj->header.type;
          domain = GRN_OBJ_GET_DOMAIN(obj);
        } else {
          DFI_POP(e, dfi);
          if (dfi) {
            type = dfi->type;
            domain = dfi->domain;
          }
        }
        DFI_POP(e, dfi);
        if (dfi && (dfi->code)) {
          if (dfi->code->op == GRN_OP_GET_VALUE) {
            dfi->code->op = GRN_OP_GET_REF;
          }
          if (dfi->code->value && grn_obj_is_persistent(ctx, dfi->code->value)) {
            e->cacheable = 0;
            e->taintable = 1;
          }
        }
        PUSH_CODE(e, op, obj, nargs, code);
      }
      DFI_PUT(e, type, domain, code);
      break;
    case GRN_OP_JUMP :
      DFI_POP(e, dfi);
      PUSH_CODE(e, op, obj, nargs, code);
      break;
    case GRN_OP_CJUMP :
      DFI_POP(e, dfi);
      PUSH_CODE(e, op, obj, nargs, code);
      break;
    default :
      break;
    }
  }
exit :
  if (!ctx->rc) { res = obj; }
  GRN_API_RETURN(res);
}
#undef PUSH_N_ARGS_ARITHMETIC_OP
#undef APPEND_UNARY_MINUS_OP

grn_obj *
grn_expr_append_const(grn_ctx *ctx, grn_obj *expr, grn_obj *obj,
                      grn_operator op, int nargs)
{
  grn_obj *res = NULL;
  grn_expr *e = (grn_expr *)expr;
  GRN_API_ENTER;
  if (!obj) {
    ERR(GRN_SYNTAX_ERROR, "constant is null");
    goto exit;
  }
  if (GRN_DB_OBJP(obj) || GRN_ACCESSORP(obj)) {
    res = obj;
  } else {
    if ((res = const_new(ctx, e))) {
      switch (obj->header.type) {
      case GRN_VOID :
      case GRN_BULK :
      case GRN_UVECTOR :
        GRN_OBJ_INIT(res, obj->header.type, 0, obj->header.domain);
        grn_bulk_write(ctx, res, GRN_BULK_HEAD(obj), GRN_BULK_VSIZE(obj));
        break;
      default :
        res = NULL;
        ERR(GRN_FUNCTION_NOT_IMPLEMENTED, "unsupported type");
        goto exit;
      }
      res->header.impl_flags |= GRN_OBJ_EXPRCONST;
    }
  }
  grn_expr_append_obj(ctx, expr, res, op, nargs); /* constant */
exit :
  GRN_API_RETURN(res);
}

static grn_obj *
grn_expr_add_str(grn_ctx *ctx, grn_obj *expr, const char *str, unsigned int str_size)
{
  grn_obj *res = NULL;
  grn_expr *e = (grn_expr *)expr;
  if ((res = const_new(ctx, e))) {
    GRN_TEXT_INIT(res, 0);
    grn_bulk_write(ctx, res, str, str_size);
    res->header.impl_flags |= GRN_OBJ_EXPRCONST;
  }
  return res;
}

grn_obj *
grn_expr_append_const_str(grn_ctx *ctx, grn_obj *expr, const char *str, unsigned int str_size,
                          grn_operator op, int nargs)
{
  grn_obj *res;
  GRN_API_ENTER;
  res = grn_expr_add_str(ctx, expr, str, str_size);
  grn_expr_append_obj(ctx, expr, res, op, nargs); /* constant */
  GRN_API_RETURN(res);
}

grn_obj *
grn_expr_append_const_int(grn_ctx *ctx, grn_obj *expr, int i,
                          grn_operator op, int nargs)
{
  grn_obj *res = NULL;
  grn_expr *e = (grn_expr *)expr;
  GRN_API_ENTER;
  if ((res = const_new(ctx, e))) {
    GRN_INT32_INIT(res, 0);
    GRN_INT32_SET(ctx, res, i);
    res->header.impl_flags |= GRN_OBJ_EXPRCONST;
  }
  grn_expr_append_obj(ctx, expr, res, op, nargs); /* constant */
  GRN_API_RETURN(res);
}

grn_rc
grn_expr_append_op(grn_ctx *ctx, grn_obj *expr, grn_operator op, int nargs)
{
  grn_expr_append_obj(ctx, expr, NULL, op, nargs);
  return ctx->rc;
}

grn_rc
grn_expr_compile(grn_ctx *ctx, grn_obj *expr)
{
  grn_obj_spec_save(ctx, DB_OBJ(expr));
  return ctx->rc;
}

#define WITH_SPSAVE(block) {\
  ctx->impl->stack_curr = sp - ctx->impl->stack;\
  e->values_curr = vp - e->values;\
  block\
  vp = e->values + e->values_curr;\
  sp = ctx->impl->stack + ctx->impl->stack_curr;\
  s0 = sp[-1];\
  s1 = sp[-2];\
}

#define DO_COMPARE_SUB_NUMERIC(y,op) {\
  switch ((y)->header.domain) {\
  case GRN_DB_INT8 :\
    r = (x_ op GRN_INT8_VALUE(y));\
    break;\
  case GRN_DB_UINT8 :\
    r = (x_ op GRN_UINT8_VALUE(y));\
    break;\
  case GRN_DB_INT16 :\
    r = (x_ op GRN_INT16_VALUE(y));\
    break;\
  case GRN_DB_UINT16 :\
    r = (x_ op GRN_UINT16_VALUE(y));\
    break;\
  case GRN_DB_INT32 :\
    r = (x_ op GRN_INT32_VALUE(y));\
    break;\
  case GRN_DB_UINT32 :\
    r = (x_ op GRN_UINT32_VALUE(y));\
    break;\
  case GRN_DB_INT64 :\
    r = (x_ op GRN_INT64_VALUE(y));\
    break;\
  case GRN_DB_TIME :\
    r = (GRN_TIME_PACK(x_,0) op GRN_INT64_VALUE(y));\
    break;\
  case GRN_DB_UINT64 :\
    r = (x_ op GRN_UINT64_VALUE(y));\
    break;\
  case GRN_DB_FLOAT :\
    r = (x_ op GRN_FLOAT_VALUE(y));\
    break;\
  default :\
    r = 0;\
    break;\
  }\
}

#define DO_COMPARE_SUB(op) {\
  switch (y->header.domain) {\
  case GRN_DB_SHORT_TEXT :\
  case GRN_DB_TEXT :\
  case GRN_DB_LONG_TEXT :\
    {\
      grn_obj y_;\
      GRN_OBJ_INIT(&y_, GRN_BULK, 0, x->header.domain);\
      if (grn_obj_cast(ctx, y, &y_, 0)) {\
        r = 0;\
      } else {\
        DO_COMPARE_SUB_NUMERIC(&y_, op);\
      }\
      GRN_OBJ_FIN(ctx, &y_);\
    }\
    break;\
  default :\
    DO_COMPARE_SUB_NUMERIC(y,op);\
    break;\
  }\
}

#define DO_COMPARE_BUILTIN(x,y,r,op) {\
  switch (x->header.domain) {\
  case GRN_DB_INT8 :\
    {\
      int8_t x_ = GRN_INT8_VALUE(x);\
      DO_COMPARE_SUB(op);\
    }\
    break;\
  case GRN_DB_UINT8 :\
    {\
      uint8_t x_ = GRN_UINT8_VALUE(x);\
      DO_COMPARE_SUB(op);\
    }\
    break;\
  case GRN_DB_INT16 :\
    {\
      int16_t x_ = GRN_INT16_VALUE(x);\
      DO_COMPARE_SUB(op);\
    }\
    break;\
  case GRN_DB_UINT16 :\
    {\
      uint16_t x_ = GRN_UINT16_VALUE(x);\
      DO_COMPARE_SUB(op);\
    }\
    break;\
  case GRN_DB_INT32 :\
    {\
      int32_t x_ = GRN_INT32_VALUE(x);\
      DO_COMPARE_SUB(op);\
    }\
    break;\
  case GRN_DB_UINT32 :\
    {\
      uint32_t x_ = GRN_UINT32_VALUE(x);\
      DO_COMPARE_SUB(op);\
    }\
    break;\
  case GRN_DB_TIME :\
    {\
      int64_t x_ = GRN_INT64_VALUE(x);\
      switch (y->header.domain) {\
      case GRN_DB_INT32 :\
        r = (x_ op GRN_TIME_PACK(GRN_INT32_VALUE(y), 0));\
        break;\
      case GRN_DB_UINT32 :\
        r = (x_ op GRN_TIME_PACK(GRN_UINT32_VALUE(y), 0));\
        break;\
      case GRN_DB_INT64 :\
      case GRN_DB_TIME :\
        r = (x_ op GRN_INT64_VALUE(y));\
        break;\
      case GRN_DB_UINT64 :\
        r = (x_ op GRN_UINT64_VALUE(y));\
        break;\
      case GRN_DB_FLOAT :\
        r = (x_ op GRN_TIME_PACK(GRN_FLOAT_VALUE(y), 0));\
        break;\
      case GRN_DB_SHORT_TEXT :\
      case GRN_DB_TEXT :\
      case GRN_DB_LONG_TEXT :\
        {\
          const char *p_ = GRN_TEXT_VALUE(y);\
          int i_ = grn_atoi(p_, p_ + GRN_TEXT_LEN(y), NULL);\
          r = (x_ op GRN_TIME_PACK(i_, 0));\
        }\
        break;\
      default :\
        r = 0;\
        break;\
      }\
    }\
    break;\
  case GRN_DB_INT64 :\
    {\
      int64_t x_ = GRN_INT64_VALUE(x);\
      DO_COMPARE_SUB(op);\
    }\
    break;\
  case GRN_DB_UINT64 :\
    {\
      uint64_t x_ = GRN_UINT64_VALUE(x);\
      DO_COMPARE_SUB(op);\
    }\
    break;\
  case GRN_DB_FLOAT :\
    {\
      double x_ = GRN_FLOAT_VALUE(x);\
      DO_COMPARE_SUB(op);\
    }\
    break;\
  case GRN_DB_SHORT_TEXT :\
  case GRN_DB_TEXT :\
  case GRN_DB_LONG_TEXT :\
    if (GRN_DB_SHORT_TEXT <= y->header.domain && y->header.domain <= GRN_DB_LONG_TEXT) {\
      int r_;\
      uint32_t la = GRN_TEXT_LEN(x), lb = GRN_TEXT_LEN(y);\
      if (la > lb) {\
        if (!(r_ = memcmp(GRN_TEXT_VALUE(x), GRN_TEXT_VALUE(y), lb))) {\
          r_ = 1;\
        }\
      } else {\
        if (!(r_ = memcmp(GRN_TEXT_VALUE(x), GRN_TEXT_VALUE(y), la))) {\
          r_ = la == lb ? 0 : -1;\
        }\
      }\
      r = (r_ op 0);\
    } else {\
      const char *q_ = GRN_TEXT_VALUE(x);\
      int x_ = grn_atoi(q_, q_ + GRN_TEXT_LEN(x), NULL);\
      DO_COMPARE_SUB(op);\
    }\
    break;\
  default :\
    r = 0;\
    break;\
  }\
}

#define DO_COMPARE(x, y, r, op) {\
  if (x->header.domain >= GRN_N_RESERVED_TYPES) {\
    grn_obj *table;\
    table = grn_ctx_at(ctx, x->header.domain);\
    switch (table->header.type) {\
    case GRN_TABLE_HASH_KEY :\
    case GRN_TABLE_PAT_KEY :\
      {\
        grn_obj key;\
        int length;\
        GRN_OBJ_INIT(&key, GRN_BULK, 0, table->header.domain);\
        length = grn_table_get_key2(ctx, table, GRN_RECORD_VALUE(x), &key);\
        if (length > 0) {\
          grn_obj *x_original = x;\
          x = &key;\
          DO_COMPARE_BUILTIN((&key), y, r, op);\
          x = x_original;\
        } else {\
          r = 0;\
        }\
        GRN_OBJ_FIN(ctx, &key);\
      }\
      break;\
    default :\
      r = 0;\
      break;\
    }\
    grn_obj_unlink(ctx, table);\
  } else {\
    DO_COMPARE_BUILTIN(x, y, r, op);\
  }\
}

#define DO_EQ_SUB {\
  switch (y->header.domain) {\
  case GRN_DB_INT8 :\
    r = (x_ == GRN_INT8_VALUE(y));\
    break;\
  case GRN_DB_UINT8 :\
    r = (x_ == GRN_UINT8_VALUE(y));\
    break;\
  case GRN_DB_INT16 :\
    r = (x_ == GRN_INT16_VALUE(y));\
    break;\
  case GRN_DB_UINT16 :\
    r = (x_ == GRN_UINT16_VALUE(y));\
    break;\
  case GRN_DB_INT32 :\
    r = (x_ == GRN_INT32_VALUE(y));\
    break;\
  case GRN_DB_UINT32 :\
    r = (x_ == GRN_UINT32_VALUE(y));\
    break;\
  case GRN_DB_INT64 :\
    r = (x_ == GRN_INT64_VALUE(y));\
    break;\
  case GRN_DB_TIME :\
    r = (GRN_TIME_PACK(x_,0) == GRN_INT64_VALUE(y));\
    break;\
  case GRN_DB_UINT64 :\
    r = (x_ == GRN_UINT64_VALUE(y));\
    break;\
  case GRN_DB_FLOAT :\
    r = ((x_ <= GRN_FLOAT_VALUE(y)) && (x_ >= GRN_FLOAT_VALUE(y)));\
    break;\
  case GRN_DB_SHORT_TEXT :\
  case GRN_DB_TEXT :\
  case GRN_DB_LONG_TEXT :\
    {\
      const char *p_ = GRN_TEXT_VALUE(y);\
      int i_ = grn_atoi(p_, p_ + GRN_TEXT_LEN(y), NULL);\
      r = (x_ == i_);\
    }\
    break;\
  default :\
    r = 0;\
    break;\
  }\
}\

#define DO_EQ(x,y,r) {\
  switch (x->header.domain) {\
  case GRN_DB_VOID :\
    r = 0;\
    break;\
  case GRN_DB_INT8 :\
    {\
      int8_t x_ = GRN_INT8_VALUE(x);\
      DO_EQ_SUB;\
    }\
    break;\
  case GRN_DB_UINT8 :\
    {\
      uint8_t x_ = GRN_UINT8_VALUE(x);\
      DO_EQ_SUB;\
    }\
    break;\
  case GRN_DB_INT16 :\
    {\
      int16_t x_ = GRN_INT16_VALUE(x);\
      DO_EQ_SUB;\
    }\
    break;\
  case GRN_DB_UINT16 :\
    {\
      uint16_t x_ = GRN_UINT16_VALUE(x);\
      DO_EQ_SUB;\
    }\
    break;\
  case GRN_DB_INT32 :\
    {\
      int32_t x_ = GRN_INT32_VALUE(x);\
      DO_EQ_SUB;\
    }\
    break;\
  case GRN_DB_UINT32 :\
    {\
      uint32_t x_ = GRN_UINT32_VALUE(x);\
      DO_EQ_SUB;\
    }\
    break;\
  case GRN_DB_INT64 :\
    {\
      int64_t x_ = GRN_INT64_VALUE(x);\
      DO_EQ_SUB;\
    }\
    break;\
  case GRN_DB_TIME :\
    {\
      int64_t x_ = GRN_INT64_VALUE(x);\
      switch (y->header.domain) {\
      case GRN_DB_INT32 :\
        r = (x_ == GRN_TIME_PACK(GRN_INT32_VALUE(y), 0));\
        break;\
      case GRN_DB_UINT32 :\
        r = (x_ == GRN_TIME_PACK(GRN_UINT32_VALUE(y), 0));\
        break;\
      case GRN_DB_INT64 :\
      case GRN_DB_TIME :\
        r = (x_ == GRN_INT64_VALUE(y));\
        break;\
      case GRN_DB_UINT64 :\
        r = (x_ == GRN_UINT64_VALUE(y));\
        break;\
      case GRN_DB_FLOAT :\
        r = (x_ == GRN_TIME_PACK(GRN_FLOAT_VALUE(y), 0));\
        break;\
      case GRN_DB_SHORT_TEXT :\
      case GRN_DB_TEXT :\
      case GRN_DB_LONG_TEXT :\
        {\
          const char *p_ = GRN_TEXT_VALUE(y);\
          int i_ = grn_atoi(p_, p_ + GRN_TEXT_LEN(y), NULL);\
          r = (x_ == GRN_TIME_PACK(i_, 0));\
        }\
        break;\
      default :\
        r = 0;\
        break;\
      }\
    }\
    break;\
  case GRN_DB_UINT64 :\
    {\
      uint64_t x_ = GRN_UINT64_VALUE(x);\
      DO_EQ_SUB;\
    }\
    break;\
  case GRN_DB_FLOAT :\
    {\
      double x_ = GRN_FLOAT_VALUE(x);\
      switch (y->header.domain) {\
      case GRN_DB_INT32 :\
        r = ((x_ <= GRN_INT32_VALUE(y)) && (x_ >= GRN_INT32_VALUE(y)));\
        break;\
      case GRN_DB_UINT32 :\
        r = ((x_ <= GRN_UINT32_VALUE(y)) && (x_ >= GRN_UINT32_VALUE(y)));\
        break;\
      case GRN_DB_INT64 :\
      case GRN_DB_TIME :\
        r = ((x_ <= GRN_INT64_VALUE(y)) && (x_ >= GRN_INT64_VALUE(y)));\
        break;\
      case GRN_DB_UINT64 :\
        r = ((x_ <= GRN_UINT64_VALUE(y)) && (x_ >= GRN_UINT64_VALUE(y)));\
        break;\
      case GRN_DB_FLOAT :\
        r = ((x_ <= GRN_FLOAT_VALUE(y)) && (x_ >= GRN_FLOAT_VALUE(y)));\
        break;\
      case GRN_DB_SHORT_TEXT :\
      case GRN_DB_TEXT :\
      case GRN_DB_LONG_TEXT :\
        {\
          const char *p_ = GRN_TEXT_VALUE(y);\
          int i_ = grn_atoi(p_, p_ + GRN_TEXT_LEN(y), NULL);\
          r = (x_ <= i_ && x_ >= i_);\
        }\
        break;\
      default :\
        r = 0;\
        break;\
      }\
    }\
    break;\
  case GRN_DB_SHORT_TEXT :\
  case GRN_DB_TEXT :\
  case GRN_DB_LONG_TEXT :\
    if (GRN_DB_SHORT_TEXT <= y->header.domain && y->header.domain <= GRN_DB_LONG_TEXT) {\
      uint32_t la = GRN_TEXT_LEN(x), lb = GRN_TEXT_LEN(y);\
      r =  (la == lb && !memcmp(GRN_TEXT_VALUE(x), GRN_TEXT_VALUE(y), lb));\
    } else {\
      const char *q_ = GRN_TEXT_VALUE(x);\
      int x_ = grn_atoi(q_, q_ + GRN_TEXT_LEN(x), NULL);\
      DO_EQ_SUB;\
    }\
    break;\
  default :\
    if ((x->header.domain == y->header.domain)) {\
      r = (GRN_BULK_VSIZE(x) == GRN_BULK_VSIZE(y) &&\
           !(memcmp(GRN_BULK_HEAD(x), GRN_BULK_HEAD(y), GRN_BULK_VSIZE(x))));\
    } else {\
      grn_obj dest;\
      if (x->header.domain < y->header.domain) {\
        GRN_OBJ_INIT(&dest, GRN_BULK, 0, y->header.domain);\
        if (!grn_obj_cast(ctx, x, &dest, 0)) {\
          r = (GRN_BULK_VSIZE(&dest) == GRN_BULK_VSIZE(y) &&\
               !memcmp(GRN_BULK_HEAD(&dest), GRN_BULK_HEAD(y), GRN_BULK_VSIZE(y))); \
        }\
      } else {\
        GRN_OBJ_INIT(&dest, GRN_BULK, 0, x->header.domain);\
        if (!grn_obj_cast(ctx, y, &dest, 0)) {\
          r = (GRN_BULK_VSIZE(&dest) == GRN_BULK_VSIZE(x) &&\
               !memcmp(GRN_BULK_HEAD(&dest), GRN_BULK_HEAD(x), GRN_BULK_VSIZE(x))); \
        }\
      }\
      GRN_OBJ_FIN(ctx, &dest);\
    }\
    break;\
  }\
}

#define GEO_RESOLUTION   3600000
#define GEO_RADIOUS      6357303
#define GEO_BES_C1       6334834
#define GEO_BES_C2       6377397
#define GEO_BES_C3       0.006674
#define GEO_GRS_C1       6335439
#define GEO_GRS_C2       6378137
#define GEO_GRS_C3       0.006694
#define GEO_INT2RAD(x)   ((M_PI * x) / (GEO_RESOLUTION * 180))

#define VAR_SET_VALUE(ctx,var,value) {\
  if (GRN_DB_OBJP(value)) {\
    (var)->header.type = GRN_PTR;\
    (var)->header.domain = DB_OBJ(value)->id;\
    GRN_PTR_SET(ctx, (var), (value));\
  } else {\
    (var)->header.type = (value)->header.type;\
    (var)->header.domain = (value)->header.domain;\
    GRN_TEXT_SET(ctx, (var), GRN_TEXT_VALUE(value), GRN_TEXT_LEN(value));\
  }\
}

grn_rc
grn_proc_call(grn_ctx *ctx, grn_obj *proc, int nargs, grn_obj *caller)
{
  grn_proc_ctx pctx;
  grn_obj *obj = NULL, **args;
  grn_proc *p = (grn_proc *)proc;
  if (nargs > ctx->impl->stack_curr) { return GRN_INVALID_ARGUMENT; }
  GRN_API_ENTER;
  args = ctx->impl->stack + ctx->impl->stack_curr - nargs;
  pctx.proc = p;
  pctx.caller = caller;
  pctx.user_data.ptr = NULL;
  if (p->funcs[PROC_INIT]) {
    obj = p->funcs[PROC_INIT](ctx, nargs, args, &pctx.user_data);
  }
  pctx.phase = PROC_NEXT;
  if (p->funcs[PROC_NEXT]) {
    obj = p->funcs[PROC_NEXT](ctx, nargs, args, &pctx.user_data);
  }
  pctx.phase = PROC_FIN;
  if (p->funcs[PROC_FIN]) {
    obj = p->funcs[PROC_FIN](ctx, nargs, args, &pctx.user_data);
  }
  ctx->impl->stack_curr -= nargs;
  grn_ctx_push(ctx, obj);
  GRN_API_RETURN(ctx->rc);
}

#define PUSH1(v) {\
  if (EXPRVP(v)) { vp++; }\
  s1 = s0;\
  *sp++ = s0 = v;\
}

#define POP1(v) {\
  if (EXPRVP(s0)) { vp--; }\
  v = s0;\
  s0 = s1;\
  sp--;\
  if (sp < s_) { ERR(GRN_INVALID_ARGUMENT, "stack underflow"); goto exit; }\
  s1 = sp[-2];\
}

#define ALLOC1(value) {\
  s1 = s0;\
  *sp++ = s0 = value = vp++;\
  if (vp - e->values > e->values_tail) { e->values_tail = vp - e->values; }\
}

#define POP1ALLOC1(arg,value) {\
  arg = s0;\
  if (EXPRVP(s0)) {\
    value = s0;\
  } else {\
    if (sp < s_ + 1) { ERR(GRN_INVALID_ARGUMENT, "stack underflow"); goto exit; }\
    sp[-1] = s0 = value = vp++;\
    s0->header.impl_flags |= GRN_OBJ_EXPRVALUE;\
  }\
}

#define POP2ALLOC1(arg1,arg2,value) {\
  if (EXPRVP(s0)) { vp--; }\
  if (EXPRVP(s1)) { vp--; }\
  arg2 = s0;\
  arg1 = s1;\
  sp--;\
  if (sp < s_ + 1) { ERR(GRN_INVALID_ARGUMENT, "stack underflow"); goto exit; }\
  s1 = sp[-2];\
  sp[-1] = s0 = value = vp++;\
  s0->header.impl_flags |= GRN_OBJ_EXPRVALUE;\
}

#define INTEGER_ARITHMETIC_OPERATION_PLUS(x, y) ((x) + (y))
#define FLOAT_ARITHMETIC_OPERATION_PLUS(x, y) ((double)(x) + (double)(y))
#define INTEGER_ARITHMETIC_OPERATION_MINUS(x, y) ((x) - (y))
#define FLOAT_ARITHMETIC_OPERATION_MINUS(x, y) ((double)(x) - (double)(y))
#define INTEGER_ARITHMETIC_OPERATION_STAR(x, y) ((x) * (y))
#define FLOAT_ARITHMETIC_OPERATION_STAR(x, y) ((double)(x) * (double)(y))
#define INTEGER_ARITHMETIC_OPERATION_SLASH(x, y) ((x) / (y))
#define FLOAT_ARITHMETIC_OPERATION_SLASH(x, y) ((double)(x) / (double)(y))
#define INTEGER_ARITHMETIC_OPERATION_MOD(x, y) ((x) % (y))
#define FLOAT_ARITHMETIC_OPERATION_MOD(x, y) (fmod((x), (y)))
#define INTEGER_ARITHMETIC_OPERATION_SHIFTL(x, y) ((x) << (y))
#define FLOAT_ARITHMETIC_OPERATION_SHIFTL(x, y)                         \
  ((long long int)(x) << (long long int)(y))
#define INTEGER_ARITHMETIC_OPERATION_SHIFTR(x, y) ((x) >> (y))
#define FLOAT_ARITHMETIC_OPERATION_SHIFTR(x, y)                         \
  ((long long int)(x) >> (long long int)(y))
#define INTEGER32_ARITHMETIC_OPERATION_SHIFTRR(x, y)            \
  ((unsigned int)(x) >> (y))
#define INTEGER64_ARITHMETIC_OPERATION_SHIFTRR(x, y)            \
  ((long long unsigned int)(x) >> (y))
#define FLOAT_ARITHMETIC_OPERATION_SHIFTRR(x, y)                \
  ((long long unsigned int)(x) >> (long long unsigned int)(y))

#define INTEGER_ARITHMETIC_OPERATION_BITWISE_OR(x, y) ((x) | (y))
#define FLOAT_ARITHMETIC_OPERATION_BITWISE_OR(x, y)                \
  ((long long int)(x) | (long long int)(y))
#define INTEGER_ARITHMETIC_OPERATION_BITWISE_XOR(x, y) ((x) ^ (y))
#define FLOAT_ARITHMETIC_OPERATION_BITWISE_XOR(x, y)                \
  ((long long int)(x) ^ (long long int)(y))
#define INTEGER_ARITHMETIC_OPERATION_BITWISE_AND(x, y) ((x) & (y))
#define FLOAT_ARITHMETIC_OPERATION_BITWISE_AND(x, y)                \
  ((long long int)(x) & (long long int)(y))

#define INTEGER_UNARY_ARITHMETIC_OPERATION_MINUS(x) (-(x))
#define FLOAT_UNARY_ARITHMETIC_OPERATION_MINUS(x) (-(x))
#define INTEGER_UNARY_ARITHMETIC_OPERATION_BITWISE_NOT(x) (~(x))
#define FLOAT_UNARY_ARITHMETIC_OPERATION_BITWISE_NOT(x) \
  (~((long long int)(x)))

#define TEXT_ARITHMETIC_OPERATION(operator)                             \
{                                                                       \
  long long int x_;                                                     \
  long long int y_;                                                     \
                                                                        \
  res->header.domain = GRN_DB_INT64;                                    \
                                                                        \
  GRN_INT64_SET(ctx, res, 0);                                           \
  grn_obj_cast(ctx, x, res, GRN_FALSE);                                 \
  x_ = GRN_INT64_VALUE(res);                                            \
                                                                        \
  GRN_INT64_SET(ctx, res, 0);                                           \
  grn_obj_cast(ctx, y, res, GRN_FALSE);                                 \
  y_ = GRN_INT64_VALUE(res);                                            \
                                                                        \
  GRN_INT64_SET(ctx, res, x_ operator y_);                              \
}

#define TEXT_UNARY_ARITHMETIC_OPERATION(unary_operator) \
{                                                       \
  long long int x_;                                     \
                                                        \
  res->header.domain = GRN_DB_INT64;                    \
                                                        \
  GRN_INT64_SET(ctx, res, 0);                           \
  grn_obj_cast(ctx, x, res, GRN_FALSE);                 \
  x_ = GRN_INT64_VALUE(res);                            \
                                                        \
  GRN_INT64_SET(ctx, res, unary_operator x_);           \
}

#define ARITHMETIC_OPERATION_NO_CHECK(y) do {} while (0)
#define ARITHMETIC_OPERATION_ZERO_DIVISION_CHECK(y) do {        \
  if ((long long int)y == 0) {                                  \
    ERR(GRN_INVALID_ARGUMENT, "dividend should not be 0");      \
    goto exit;                                                  \
  }                                                             \
} while (0)


#define NUMERIC_ARITHMETIC_OPERATION_DISPATCH(set, get, x_, y, res,     \
                                              integer_operation,        \
                                              float_operation,          \
                                              right_expression_check,   \
                                              invalid_type_error) {     \
  switch (y->header.domain) {                                           \
  case GRN_DB_INT32 :                                                   \
    {                                                                   \
      int y_;                                                           \
      y_ = GRN_INT32_VALUE(y);                                          \
      right_expression_check(y_);                                       \
      set(ctx, res, integer_operation(x_, y_));                         \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT32 :                                                  \
    {                                                                   \
      unsigned int y_;                                                  \
      y_ = GRN_UINT32_VALUE(y);                                         \
      right_expression_check(y_);                                       \
      set(ctx, res, integer_operation(x_, y_));                         \
    }                                                                   \
    break;                                                              \
  case GRN_DB_TIME :                                                    \
    {                                                                   \
      long long int y_;                                                 \
      y_ = GRN_TIME_VALUE(y);                                           \
      right_expression_check(y_);                                       \
      set(ctx, res, integer_operation(x_, y_));                         \
    }                                                                   \
    break;                                                              \
  case GRN_DB_INT64 :                                                   \
    {                                                                   \
      long long int y_;                                                 \
      y_ = GRN_INT64_VALUE(y);                                          \
      right_expression_check(y_);                                       \
      set(ctx, res, integer_operation(x_, y_));                         \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT64 :                                                  \
    {                                                                   \
      long long unsigned int y_;                                        \
      y_ = GRN_UINT64_VALUE(y);                                         \
      right_expression_check(y_);                                       \
      set(ctx, res, integer_operation(x_, y_));                         \
    }                                                                   \
    break;                                                              \
  case GRN_DB_FLOAT :                                                   \
    {                                                                   \
      double y_;                                                        \
      y_ = GRN_FLOAT_VALUE(y);                                          \
      right_expression_check(y_);                                       \
      res->header.domain = GRN_DB_FLOAT;                                \
      GRN_FLOAT_SET(ctx, res, float_operation(x_, y_));                 \
    }                                                                   \
    break;                                                              \
  case GRN_DB_SHORT_TEXT :                                              \
  case GRN_DB_TEXT :                                                    \
  case GRN_DB_LONG_TEXT :                                               \
    set(ctx, res, 0);                                                   \
    if (grn_obj_cast(ctx, y, res, GRN_FALSE)) {                         \
      ERR(GRN_INVALID_ARGUMENT,                                         \
          "not a numerical format: <%.*s>",                             \
          GRN_TEXT_LEN(y), GRN_TEXT_VALUE(y));                          \
      goto exit;                                                        \
    }                                                                   \
    set(ctx, res, integer_operation(x_, get(res)));                     \
    break;                                                              \
  default :                                                             \
    invalid_type_error;                                                 \
    break;                                                              \
  }                                                                     \
}


#define ARITHMETIC_OPERATION_DISPATCH(x, y, res,                        \
                                      integer32_operation,              \
                                      integer64_operation,              \
                                      float_operation,                  \
                                      left_expression_check,            \
                                      right_expression_check,           \
                                      text_operation,                   \
                                      invalid_type_error) {             \
  switch (x->header.domain) {                                           \
  case GRN_DB_INT32 :                                                   \
    {                                                                   \
      int x_;                                                           \
      x_ = GRN_INT32_VALUE(x);                                          \
      left_expression_check(x_);                                        \
      NUMERIC_ARITHMETIC_OPERATION_DISPATCH(GRN_INT32_SET,              \
                                            GRN_INT32_VALUE,            \
                                            x_, y, res,                 \
                                            integer32_operation,        \
                                            float_operation,            \
                                            right_expression_check,     \
                                            invalid_type_error);        \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT32 :                                                  \
    {                                                                   \
      unsigned int x_;                                                  \
      x_ = GRN_UINT32_VALUE(x);                                         \
      left_expression_check(x_);                                        \
      NUMERIC_ARITHMETIC_OPERATION_DISPATCH(GRN_UINT32_SET,             \
                                            GRN_UINT32_VALUE,           \
                                            x_, y, res,                 \
                                            integer32_operation,        \
                                            float_operation,            \
                                            right_expression_check,     \
                                            invalid_type_error);        \
    }                                                                   \
    break;                                                              \
  case GRN_DB_INT64 :                                                   \
    {                                                                   \
      long long int x_;                                                 \
      x_ = GRN_INT64_VALUE(x);                                          \
      left_expression_check(x_);                                        \
      NUMERIC_ARITHMETIC_OPERATION_DISPATCH(GRN_UINT64_SET,             \
                                            GRN_UINT64_VALUE,           \
                                            x_, y, res,                 \
                                            integer64_operation,        \
                                            float_operation,            \
                                            right_expression_check,     \
                                            invalid_type_error);        \
    }                                                                   \
    break;                                                              \
  case GRN_DB_TIME :                                                    \
    {                                                                   \
      long long int x_;                                                 \
      x_ = GRN_TIME_VALUE(x);                                           \
      left_expression_check(x_);                                        \
      NUMERIC_ARITHMETIC_OPERATION_DISPATCH(GRN_TIME_SET,               \
                                            GRN_TIME_VALUE,             \
                                            x_, y, res,                 \
                                            integer64_operation,        \
                                            float_operation,            \
                                            right_expression_check,     \
                                            invalid_type_error);        \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT64 :                                                  \
    {                                                                   \
      long long unsigned int x_;                                        \
      x_ = GRN_UINT64_VALUE(x);                                         \
      left_expression_check(x_);                                        \
      NUMERIC_ARITHMETIC_OPERATION_DISPATCH(GRN_UINT64_SET,             \
                                            GRN_UINT64_VALUE,           \
                                            x_, y, res,                 \
                                            integer64_operation,        \
                                            float_operation,            \
                                            right_expression_check,     \
                                            invalid_type_error);        \
    }                                                                   \
    break;                                                              \
  case GRN_DB_FLOAT :                                                   \
    {                                                                   \
      double x_;                                                        \
      x_ = GRN_FLOAT_VALUE(x);                                          \
      left_expression_check(x_);                                        \
      NUMERIC_ARITHMETIC_OPERATION_DISPATCH(GRN_FLOAT_SET,              \
                                            GRN_FLOAT_VALUE,            \
                                            x_, y, res,                 \
                                            float_operation,            \
                                            float_operation,            \
                                            right_expression_check,     \
                                            invalid_type_error);        \
    }                                                                   \
    break;                                                              \
  case GRN_DB_SHORT_TEXT :                                              \
  case GRN_DB_TEXT :                                                    \
  case GRN_DB_LONG_TEXT :                                               \
    text_operation;                                                     \
    break;                                                              \
  default:                                                              \
    invalid_type_error;                                                 \
    break;                                                              \
  }                                                                     \
  code++;                                                               \
}

#define ARITHMETIC_BINARY_OPERATION_DISPATCH(integer32_operation,       \
                                             integer64_operation,       \
                                             float_operation,           \
                                             left_expression_check,     \
                                             right_expression_check,    \
                                             text_operation,            \
                                             invalid_type_error) {      \
  grn_obj *x, *y;                                                       \
                                                                        \
  POP2ALLOC1(x, y, res);                                                \
  res->header.domain = x->header.domain;                                \
  ARITHMETIC_OPERATION_DISPATCH(x, y, res,                              \
                                integer32_operation,                    \
                                integer64_operation,                    \
                                float_operation,                        \
                                left_expression_check,                  \
                                right_expression_check,                 \
                                text_operation,                         \
                                invalid_type_error);                    \
}

#define ARITHMETIC_UNARY_OPERATION_DISPATCH(integer_operation,          \
                                            float_operation,            \
                                            left_expression_check,      \
                                            right_expression_check,     \
                                            text_operation,             \
                                            invalid_type_error) {       \
  grn_obj *x;                                                           \
  POP1ALLOC1(x, res);                                                   \
  res->header.domain = x->header.domain;                                \
  switch (x->header.domain) {                                           \
  case GRN_DB_INT32 :                                                   \
    {                                                                   \
      int x_;                                                           \
      x_ = GRN_INT32_VALUE(x);                                          \
      left_expression_check(x_);                                        \
      GRN_INT32_SET(ctx, res, integer_operation(x_));                   \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT32 :                                                  \
    {                                                                   \
      unsigned int x_;                                                  \
      x_ = GRN_UINT32_VALUE(x);                                         \
      left_expression_check(x_);                                        \
      GRN_UINT32_SET(ctx, res, integer_operation(x_));                  \
    }                                                                   \
    break;                                                              \
  case GRN_DB_INT64 :                                                   \
    {                                                                   \
      long long int x_;                                                 \
      x_ = GRN_INT64_VALUE(x);                                          \
      left_expression_check(x_);                                        \
      GRN_INT64_SET(ctx, res, integer_operation(x_));                   \
    }                                                                   \
    break;                                                              \
  case GRN_DB_TIME :                                                    \
    {                                                                   \
      long long int x_;                                                 \
      x_ = GRN_TIME_VALUE(x);                                           \
      left_expression_check(x_);                                        \
      GRN_TIME_SET(ctx, res, integer_operation(x_));                    \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT64 :                                                  \
    {                                                                   \
      long long unsigned int x_;                                        \
      x_ = GRN_UINT64_VALUE(x);                                         \
      left_expression_check(x_);                                        \
      GRN_UINT64_SET(ctx, res, integer_operation(x_));                  \
    }                                                                   \
    break;                                                              \
  case GRN_DB_FLOAT :                                                   \
    {                                                                   \
      double x_;                                                        \
      x_ = GRN_FLOAT_VALUE(x);                                          \
      left_expression_check(x_);                                        \
      GRN_FLOAT_SET(ctx, res, float_operation(x_));                     \
    }                                                                   \
    break;                                                              \
  case GRN_DB_SHORT_TEXT :                                              \
  case GRN_DB_TEXT :                                                    \
  case GRN_DB_LONG_TEXT :                                               \
    text_operation;                                                     \
    break;                                                              \
  default:                                                              \
    invalid_type_error;                                                 \
    break;                                                              \
  }                                                                     \
  code++;                                                               \
}

#define EXEC_OPERATE(operate_sentence, assign_sentence)   \
  operate_sentence                                        \
  assign_sentence

#define EXEC_OPERATE_POST(operate_sentence, assign_sentence)    \
  assign_sentence                                               \
  operate_sentence

#define UNARY_OPERATE_AND_ASSIGN_DISPATCH(exec_operate, delta,          \
                                          set_flags) {                  \
  grn_obj *var, *col, value;                                            \
  grn_id rid;                                                           \
                                                                        \
  POP1ALLOC1(var, res);                                                 \
  if (var->header.type != GRN_PTR) {                                    \
    ERR(GRN_INVALID_ARGUMENT, "invalid variable type: 0x%0x",           \
        var->header.type);                                              \
    goto exit;                                                          \
  }                                                                     \
  if (GRN_BULK_VSIZE(var) != (sizeof(grn_obj *) + sizeof(grn_id))) {    \
    ERR(GRN_INVALID_ARGUMENT,                                           \
        "invalid variable size: expected: %ld, actual: %ld",            \
        (sizeof(grn_obj *) + sizeof(grn_id)), GRN_BULK_VSIZE(var));     \
    goto exit;                                                          \
  }                                                                     \
  col = GRN_PTR_VALUE(var);                                             \
  rid = *(grn_id *)(GRN_BULK_HEAD(var) + sizeof(grn_obj *));            \
  res->header.type = GRN_VOID;                                          \
  res->header.domain = DB_OBJ(col)->range;                              \
  switch (DB_OBJ(col)->range) {                                         \
  case GRN_DB_INT32 :                                                   \
    GRN_INT32_INIT(&value, 0);                                          \
    GRN_INT32_SET(ctx, &value, delta);                                  \
    break;                                                              \
  case GRN_DB_UINT32 :                                                  \
    GRN_UINT32_INIT(&value, 0);                                         \
    GRN_UINT32_SET(ctx, &value, delta);                                 \
    break;                                                              \
  case GRN_DB_INT64 :                                                   \
    GRN_INT64_INIT(&value, 0);                                          \
    GRN_INT64_SET(ctx, &value, delta);                                  \
    break;                                                              \
  case GRN_DB_UINT64 :                                                  \
    GRN_UINT64_INIT(&value, 0);                                         \
    GRN_UINT64_SET(ctx, &value, delta);                                 \
    break;                                                              \
  case GRN_DB_FLOAT :                                                   \
    GRN_FLOAT_INIT(&value, 0);                                          \
    GRN_FLOAT_SET(ctx, &value, delta);                                  \
    break;                                                              \
  case GRN_DB_TIME :                                                    \
    GRN_TIME_INIT(&value, 0);                                           \
    GRN_TIME_SET(ctx, &value, GRN_TIME_PACK(delta, 0));                 \
    break;                                                              \
  default:                                                              \
    ERR(GRN_INVALID_ARGUMENT,                                           \
        "invalid increment target type: %d "                            \
        "(FIXME: type name is needed)", DB_OBJ(col)->range);            \
    goto exit;                                                          \
    break;                                                              \
  }                                                                     \
  exec_operate(grn_obj_set_value(ctx, col, rid, &value, set_flags);,    \
               grn_obj_get_value(ctx, col, rid, res););                 \
  code++;                                                               \
}

#define ARITHMETIC_OPERATION_AND_ASSIGN_DISPATCH(integer32_operation,   \
                                                 integer64_operation,   \
                                                 float_operation,       \
                                                 left_expression_check, \
                                                 right_expression_check,\
                                                 text_operation)        \
{                                                                       \
  grn_obj *value, *var, *res;                                           \
  if (code->value) {                                                    \
    value = code->value;                                                \
    POP1ALLOC1(var, res);                                               \
  } else {                                                              \
    POP2ALLOC1(var, value, res);                                        \
  }                                                                     \
  if (var->header.type == GRN_PTR &&                                    \
      GRN_BULK_VSIZE(var) == (sizeof(grn_obj *) + sizeof(grn_id))) {    \
    grn_obj *col = GRN_PTR_VALUE(var);                                  \
    grn_id rid = *(grn_id *)(GRN_BULK_HEAD(var) + sizeof(grn_obj *));   \
    grn_obj variable_value, casted_value;                               \
    grn_id domain;                                                      \
                                                                        \
    value = GRN_OBJ_RESOLVE(ctx, value);                                \
                                                                        \
    domain = grn_obj_get_range(ctx, col);                               \
    GRN_OBJ_INIT(&variable_value, GRN_BULK, 0, domain);                 \
    grn_obj_get_value(ctx, col, rid, &variable_value);                  \
                                                                        \
    GRN_OBJ_INIT(&casted_value, GRN_BULK, 0, domain);                   \
    if (grn_obj_cast(ctx, value, &casted_value, GRN_FALSE)) {           \
      ERR(GRN_INVALID_ARGUMENT, "invalid value: string");               \
      GRN_OBJ_FIN(ctx, &variable_value);                                \
      GRN_OBJ_FIN(ctx, &casted_value);                                  \
      POP1(res);                                                        \
      goto exit;                                                        \
    }                                                                   \
    grn_obj_reinit(ctx, res, domain, 0);                                \
    ARITHMETIC_OPERATION_DISPATCH((&variable_value), (&casted_value),   \
                                  res,                                  \
                                  integer32_operation,                  \
                                  integer64_operation,                  \
                                  float_operation,                      \
                                  left_expression_check,                \
                                  right_expression_check,               \
                                  text_operation,);                     \
    grn_obj_set_value(ctx, col, rid, res, GRN_OBJ_SET);                 \
    GRN_OBJ_FIN(ctx, (&variable_value));                                \
    GRN_OBJ_FIN(ctx, (&casted_value));                                  \
  } else {                                                              \
    ERR(GRN_INVALID_ARGUMENT, "left hand expression isn't column.");    \
    POP1(res);                                                          \
  }                                                                     \
}

void
pseudo_query_scan(grn_ctx *ctx, grn_obj *x, grn_obj *y, grn_obj *res)
{
  grn_str *a = NULL, *b = NULL;

  switch (x->header.domain) {
  case GRN_DB_SHORT_TEXT:
  case GRN_DB_TEXT:
  case GRN_DB_LONG_TEXT:
    a = grn_str_open(ctx, GRN_TEXT_VALUE(x), GRN_TEXT_LEN(x), GRN_STR_NORMALIZE);
    break;
  default:
    break;
  }

  switch (y->header.domain) {
  case GRN_DB_SHORT_TEXT:
  case GRN_DB_TEXT:
  case GRN_DB_LONG_TEXT:
    b = grn_str_open(ctx, GRN_TEXT_VALUE(y), GRN_TEXT_LEN(y), GRN_STR_NORMALIZE);
    break;
  default:
    break;
  }

  /* normalized str doesn't contain '\0'. */
  if (a && b && strstr(a->norm, b->norm)) {
    GRN_INT32_SET(ctx, res, 1);
  } else {
    GRN_INT32_SET(ctx, res, 0);
  }
  res->header.type = GRN_BULK;
  res->header.domain = GRN_DB_INT32;

  if (a) { grn_str_close(ctx, a); }
  if (b) { grn_str_close(ctx, b); }
}

grn_obj *
grn_expr_exec(grn_ctx *ctx, grn_obj *expr, int nargs)
{
  grn_obj *val = NULL;
  uint32_t stack_curr = ctx->impl->stack_curr;
  GRN_API_ENTER;
  if (expr->header.type == GRN_PROC) {
    grn_proc_call(ctx, expr, nargs, expr);
  } else {
    grn_expr *e = (grn_expr *)expr;
    register grn_obj **s_ = ctx->impl->stack, *s0 = NULL, *s1 = NULL, **sp, *vp = e->values;
    grn_obj *res = NULL, *v0 = grn_expr_get_var_by_offset(ctx, expr, 0);
    grn_expr_code *code = e->codes, *ce = &e->codes[e->codes_curr];
    sp = s_ + stack_curr;
    while (code < ce) {
      switch (code->op) {
      case GRN_OP_NOP :
        code++;
        break;
      case GRN_OP_PUSH :
        PUSH1(code->value);
        code++;
        break;
      case GRN_OP_POP :
        {
          grn_obj *obj;
          POP1(obj);
          code++;
        }
        break;
      case GRN_OP_GET_REF :
        {
          grn_obj *col, *rec;
          if (code->nargs == 1) {
            rec = v0;
            if (code->value) {
              col = code->value;
              ALLOC1(res);
            } else {
              POP1ALLOC1(col, res);
            }
          } else {
            if (code->value) {
              col = code->value;
              POP1ALLOC1(rec, res);
            } else {
              POP2ALLOC1(rec, col, res);
            }
          }
          if (col->header.type == GRN_BULK) {
            grn_obj *table = grn_ctx_at(ctx, GRN_OBJ_GET_DOMAIN(rec));
            col = grn_obj_column(ctx, table, GRN_BULK_HEAD(col), GRN_BULK_VSIZE(col));
            if (col) { GRN_PTR_PUT(ctx, &e->objs, col); }
          }
          if (col) {
            res->header.type = GRN_PTR;
            res->header.domain = GRN_ID_NIL;
            GRN_PTR_SET(ctx, res, col);
            GRN_UINT32_PUT(ctx, res, GRN_RECORD_VALUE(rec));
          } else {
            ERR(GRN_INVALID_ARGUMENT, "col resolve failed");
            goto exit;
          }
          code++;
        }
        break;
      case GRN_OP_CALL :
        {
          grn_obj *proc;
          if (code->value) {
            if (sp < s_ + code->nargs) {
              ERR(GRN_INVALID_ARGUMENT, "stack error");
              goto exit;
            }
            proc = code->value;
            WITH_SPSAVE({
              grn_proc_call(ctx, proc, code->nargs, expr);
            });
          } else {
            int offset = code->nargs + 1;
            if (sp < s_ + offset) {
              ERR(GRN_INVALID_ARGUMENT, "stack error");
              goto exit;
            }
            proc = sp[-offset];
            WITH_SPSAVE({
              grn_proc_call(ctx, proc, code->nargs, expr);
            });
            POP1(res);
            {
              grn_obj *proc_;
              POP1(proc_);
              if (proc != proc_) {
                GRN_LOG(ctx, GRN_LOG_WARNING, "stack may be corrupt");
              }
            }
            PUSH1(res);
          }
        }
        code++;
        break;
      case GRN_OP_INTERN :
        {
          grn_obj *obj;
          POP1(obj);
          obj = GRN_OBJ_RESOLVE(ctx, obj);
          res = grn_expr_get_var(ctx, expr, GRN_TEXT_VALUE(obj), GRN_TEXT_LEN(obj));
          if (!res) { res = grn_ctx_get(ctx, GRN_TEXT_VALUE(obj), GRN_TEXT_LEN(obj)); }
          if (!res) {
            ERR(GRN_INVALID_ARGUMENT, "intern failed");
            goto exit;
          }
          PUSH1(res);
        }
        code++;
        break;
      case GRN_OP_TABLE_CREATE :
        {
          grn_obj *value_type, *key_type, *flags, *name;
          POP1(value_type);
          value_type = GRN_OBJ_RESOLVE(ctx, value_type);
          POP1(key_type);
          key_type = GRN_OBJ_RESOLVE(ctx, key_type);
          POP1(flags);
          flags = GRN_OBJ_RESOLVE(ctx, flags);
          POP1(name);
          name = GRN_OBJ_RESOLVE(ctx, name);
          res = grn_table_create(ctx, GRN_TEXT_VALUE(name), GRN_TEXT_LEN(name),
                                 NULL, GRN_UINT32_VALUE(flags),
                                 key_type, value_type);
          PUSH1(res);
        }
        code++;
        break;
      case GRN_OP_EXPR_GET_VAR :
        {
          grn_obj *name, *expr;
          POP1(name);
          name = GRN_OBJ_RESOLVE(ctx, name);
          POP1(expr);
          expr = GRN_OBJ_RESOLVE(ctx, expr);
          switch (name->header.domain) {
          case GRN_DB_INT32 :
            res = grn_expr_get_var_by_offset(ctx, expr, (unsigned int) GRN_INT32_VALUE(name));
            break;
          case GRN_DB_UINT32 :
            res = grn_expr_get_var_by_offset(ctx, expr, (unsigned int) GRN_UINT32_VALUE(name));
            break;
          case GRN_DB_INT64 :
            res = grn_expr_get_var_by_offset(ctx, expr, (unsigned int) GRN_INT64_VALUE(name));
            break;
          case GRN_DB_UINT64 :
            res = grn_expr_get_var_by_offset(ctx, expr, (unsigned int) GRN_UINT64_VALUE(name));
            break;
          case GRN_DB_SHORT_TEXT :
          case GRN_DB_TEXT :
          case GRN_DB_LONG_TEXT :
            res = grn_expr_get_var(ctx, expr, GRN_TEXT_VALUE(name), GRN_TEXT_LEN(name));
            break;
          default :
            ERR(GRN_INVALID_ARGUMENT, "invalid type");
            goto exit;
          }
          PUSH1(res);
        }
        code++;
        break;
      case GRN_OP_ASSIGN :
        {
          grn_obj *value, *var;
          if (code->value) {
            value = code->value;
          } else {
            POP1(value);
          }
          value = GRN_OBJ_RESOLVE(ctx, value);
          POP1(var);
          // var = GRN_OBJ_RESOLVE(ctx, var);
          if (var->header.type == GRN_PTR &&
              GRN_BULK_VSIZE(var) == (sizeof(grn_obj *) + sizeof(grn_id))) {
            grn_obj *col = GRN_PTR_VALUE(var);
            grn_id rid = *(grn_id *)(GRN_BULK_HEAD(var) + sizeof(grn_obj *));
            grn_obj_set_value(ctx, col, rid, value, GRN_OBJ_SET);
          } else {
            VAR_SET_VALUE(ctx, var, value);
          }
          PUSH1(value);
        }
        code++;
        break;
      case GRN_OP_STAR_ASSIGN :
        ARITHMETIC_OPERATION_AND_ASSIGN_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_STAR,
          INTEGER_ARITHMETIC_OPERATION_STAR,
          FLOAT_ARITHMETIC_OPERATION_STAR,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT, "variable *= \"string\" isn't supported");
            goto exit;
          });
        break;
      case GRN_OP_SLASH_ASSIGN :
        ARITHMETIC_OPERATION_AND_ASSIGN_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_SLASH,
          INTEGER_ARITHMETIC_OPERATION_SLASH,
          FLOAT_ARITHMETIC_OPERATION_SLASH,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT, "variable /= \"string\" isn't supported");
            goto exit;
          });
        break;
      case GRN_OP_MOD_ASSIGN :
        ARITHMETIC_OPERATION_AND_ASSIGN_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_MOD,
          INTEGER_ARITHMETIC_OPERATION_MOD,
          FLOAT_ARITHMETIC_OPERATION_MOD,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT, "variable %= \"string\" isn't supported");
            goto exit;
          });
        break;
      case GRN_OP_PLUS_ASSIGN :
        ARITHMETIC_OPERATION_AND_ASSIGN_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_PLUS,
          INTEGER_ARITHMETIC_OPERATION_PLUS,
          FLOAT_ARITHMETIC_OPERATION_PLUS,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT, "variable += \"string\" isn't supported");
            goto exit;
          });
        break;
      case GRN_OP_MINUS_ASSIGN :
        ARITHMETIC_OPERATION_AND_ASSIGN_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_MINUS,
          INTEGER_ARITHMETIC_OPERATION_MINUS,
          FLOAT_ARITHMETIC_OPERATION_MINUS,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT, "variable -= \"string\" isn't supported");
            goto exit;
          });
        break;
      case GRN_OP_SHIFTL_ASSIGN :
        ARITHMETIC_OPERATION_AND_ASSIGN_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_SHIFTL,
          INTEGER_ARITHMETIC_OPERATION_SHIFTL,
          FLOAT_ARITHMETIC_OPERATION_SHIFTL,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT, "variable <<= \"string\" isn't supported");
            goto exit;
          });
        break;
      case GRN_OP_SHIFTR_ASSIGN :
        ARITHMETIC_OPERATION_AND_ASSIGN_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_SHIFTR,
          INTEGER_ARITHMETIC_OPERATION_SHIFTR,
          FLOAT_ARITHMETIC_OPERATION_SHIFTR,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT, "variable >>= \"string\" isn't supported");
            goto exit;
          });
        break;
      case GRN_OP_SHIFTRR_ASSIGN :
        ARITHMETIC_OPERATION_AND_ASSIGN_DISPATCH(
          INTEGER32_ARITHMETIC_OPERATION_SHIFTRR,
          INTEGER64_ARITHMETIC_OPERATION_SHIFTRR,
          FLOAT_ARITHMETIC_OPERATION_SHIFTRR,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT,
                "variable >>>= \"string\" isn't supported");
            goto exit;
          });
        break;
      case GRN_OP_AND_ASSIGN :
        ARITHMETIC_OPERATION_AND_ASSIGN_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_BITWISE_AND,
          INTEGER_ARITHMETIC_OPERATION_BITWISE_AND,
          FLOAT_ARITHMETIC_OPERATION_BITWISE_AND,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT, "variable &= \"string\" isn't supported");
            goto exit;
          });
        break;
      case GRN_OP_OR_ASSIGN :
        ARITHMETIC_OPERATION_AND_ASSIGN_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_BITWISE_OR,
          INTEGER_ARITHMETIC_OPERATION_BITWISE_OR,
          FLOAT_ARITHMETIC_OPERATION_BITWISE_OR,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT, "variable |= \"string\" isn't supported");
            goto exit;
          });
        break;
      case GRN_OP_XOR_ASSIGN :
        ARITHMETIC_OPERATION_AND_ASSIGN_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_BITWISE_XOR,
          INTEGER_ARITHMETIC_OPERATION_BITWISE_XOR,
          FLOAT_ARITHMETIC_OPERATION_BITWISE_XOR,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT, "variable ^= \"string\" isn't supported");
            goto exit;
          });
        break;
      case GRN_OP_JUMP :
        code += code->nargs + 1;
        break;
      case GRN_OP_CJUMP :
        {
          grn_obj *v;
          unsigned int v_boolean;
          POP1(v);
          GRN_TRUEP(ctx, v, v_boolean);
          if (!v_boolean) { code += code->nargs; }
        }
        code++;
        break;
      case GRN_OP_GET_VALUE :
        {
          grn_obj *col, *rec;
          do {
            uint32_t size;
            const char *value;
            if (code->nargs == 1) {
              rec = v0;
              if (code->value) {
                col = code->value;
                ALLOC1(res);
              } else {
                POP1ALLOC1(col, res);
              }
            } else {
              if (code->value) {
                col = code->value;
                POP1ALLOC1(rec, res);
              } else {
                POP2ALLOC1(rec, col, res);
              }
            }
            if (col->header.type == GRN_BULK) {
              grn_obj *table = grn_ctx_at(ctx, GRN_OBJ_GET_DOMAIN(rec));
              col = grn_obj_column(ctx, table, GRN_BULK_HEAD(col), GRN_BULK_VSIZE(col));
              if (col) { GRN_PTR_PUT(ctx, &e->objs, col); }
            }
            if (col) {
              value = grn_obj_get_value_(ctx, col, GRN_RECORD_VALUE(rec), &size);
            } else {
              ERR(GRN_INVALID_ARGUMENT, "col resolve failed");
              goto exit;
            }
            if (size == GRN_OBJ_GET_VALUE_IMD) {
              GRN_RECORD_SET(ctx, res, (uintptr_t)value);
            } else {
              grn_bulk_write_from(ctx, res, value, 0, size);
            }
            res->header.domain = grn_obj_get_range(ctx, col);
            code++;
          } while (code < ce && code->op == GRN_OP_GET_VALUE);
        }
        break;
      case GRN_OP_OBJ_SEARCH :
        {
          grn_obj *op, *query, *index;
          // todo : grn_search_optarg optarg;
          POP1(op);
          op = GRN_OBJ_RESOLVE(ctx, op);
          POP1(res);
          res = GRN_OBJ_RESOLVE(ctx, res);
          POP1(query);
          query = GRN_OBJ_RESOLVE(ctx, query);
          POP1(index);
          index = GRN_OBJ_RESOLVE(ctx, index);
          grn_obj_search(ctx, index, query, res,
                         (grn_operator)GRN_UINT32_VALUE(op), NULL);
        }
        code++;
        break;
      case GRN_OP_TABLE_SELECT :
        {
          grn_obj *op, *res, *expr, *table;
          POP1(op);
          op = GRN_OBJ_RESOLVE(ctx, op);
          POP1(res);
          res = GRN_OBJ_RESOLVE(ctx, res);
          POP1(expr);
          expr = GRN_OBJ_RESOLVE(ctx, expr);
          POP1(table);
          table = GRN_OBJ_RESOLVE(ctx, table);
          WITH_SPSAVE({
            grn_table_select(ctx, table, expr, res, (grn_operator)GRN_UINT32_VALUE(op));
          });
          PUSH1(res);
        }
        code++;
        break;
      case GRN_OP_TABLE_SORT :
        {
          grn_obj *keys_, *res, *limit, *table;
          POP1(keys_);
          keys_ = GRN_OBJ_RESOLVE(ctx, keys_);
          POP1(res);
          res = GRN_OBJ_RESOLVE(ctx, res);
          POP1(limit);
          limit = GRN_OBJ_RESOLVE(ctx, limit);
          POP1(table);
          table = GRN_OBJ_RESOLVE(ctx, table);
          {
            grn_table_sort_key *keys;
            const char *p = GRN_BULK_HEAD(keys_), *tokbuf[256];
            int n = grn_str_tok(p, GRN_BULK_VSIZE(keys_), ' ', tokbuf, 256, NULL);
            if ((keys = GRN_MALLOCN(grn_table_sort_key, n))) {
              int i, n_keys = 0;
              for (i = 0; i < n; i++) {
                uint32_t len = (uint32_t) (tokbuf[i] - p);
                grn_obj *col = grn_obj_column(ctx, table, p, len);
                if (col) {
                  keys[n_keys].key = col;
                  keys[n_keys].flags = GRN_TABLE_SORT_ASC;
                  keys[n_keys].offset = 0;
                  n_keys++;
                } else {
                  if (p[0] == ':' && p[1] == 'd' && len == 2 && n_keys) {
                    keys[n_keys - 1].flags |= GRN_TABLE_SORT_DESC;
                  }
                }
                p = tokbuf[i] + 1;
              }
              WITH_SPSAVE({
                grn_table_sort(ctx, table, 0, GRN_INT32_VALUE(limit), res, keys, n_keys);
              });
              for (i = 0; i < n_keys; i++) {
                grn_obj_unlink(ctx, keys[i].key);
              }
              GRN_FREE(keys);
            }
          }
        }
        code++;
        break;
      case GRN_OP_TABLE_GROUP :
        {
          grn_obj *res, *keys_, *table;
          POP1(res);
          res = GRN_OBJ_RESOLVE(ctx, res);
          POP1(keys_);
          keys_ = GRN_OBJ_RESOLVE(ctx, keys_);
          POP1(table);
          table = GRN_OBJ_RESOLVE(ctx, table);
          {
            grn_table_sort_key *keys;
            grn_table_group_result results;
            const char *p = GRN_BULK_HEAD(keys_), *tokbuf[256];
            int n = grn_str_tok(p, GRN_BULK_VSIZE(keys_), ' ', tokbuf, 256, NULL);
            if ((keys = GRN_MALLOCN(grn_table_sort_key, n))) {
              int i, n_keys = 0;
              for (i = 0; i < n; i++) {
                uint32_t len = (uint32_t) (tokbuf[i] - p);
                grn_obj *col = grn_obj_column(ctx, table, p, len);
                if (col) {
                  keys[n_keys].key = col;
                  keys[n_keys].flags = GRN_TABLE_SORT_ASC;
                  keys[n_keys].offset = 0;
                  n_keys++;
                } else if (n_keys) {
                  if (p[0] == ':' && p[1] == 'd' && len == 2) {
                    keys[n_keys - 1].flags |= GRN_TABLE_SORT_DESC;
                  } else {
                    keys[n_keys - 1].offset = grn_atoi(p, p + len, NULL);
                  }
                }
                p = tokbuf[i] + 1;
              }
              /* todo : support multi-results */
              results.table = res;
              results.key_begin = 0;
              results.key_end = 0;
              results.limit = 0;
              results.flags = 0;
              results.op = GRN_OP_OR;
              WITH_SPSAVE({
                grn_table_group(ctx, table, keys, n_keys, &results, 1);
              });
              for (i = 0; i < n_keys; i++) {
                grn_obj_unlink(ctx, keys[i].key);
              }
              GRN_FREE(keys);
            }
          }
        }
        code++;
        break;
      case GRN_OP_JSON_PUT :
        {
          grn_obj_format format;
          grn_obj *str, *table, *res;
          POP1(res);
          res = GRN_OBJ_RESOLVE(ctx, res);
          POP1(str);
          str = GRN_OBJ_RESOLVE(ctx, str);
          POP1(table);
          table = GRN_OBJ_RESOLVE(ctx, table);
          GRN_OBJ_FORMAT_INIT(&format, grn_table_size(ctx, table), 0, -1, 0);
          format.flags = 0;
          grn_obj_columns(ctx, table,
                          GRN_TEXT_VALUE(str), GRN_TEXT_LEN(str), &format.columns);
          grn_text_otoj(ctx, res, table, &format);
          GRN_OBJ_FORMAT_FIN(ctx, &format);
        }
        code++;
        break;
      case GRN_OP_AND :
        {
          grn_obj *x, *y;
          unsigned int x_boolean, y_boolean;
          POP2ALLOC1(x, y, res);
          GRN_TRUEP(ctx, x, x_boolean);
          GRN_TRUEP(ctx, y, y_boolean);
          if (x_boolean && y_boolean) {
            GRN_INT32_SET(ctx, res, 1);
          } else {
            GRN_INT32_SET(ctx, res, 0);
          }
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_INT32;
        }
        code++;
        break;
      case GRN_OP_OR :
        {
          grn_obj *x, *y;
          POP2ALLOC1(x, y, res);
          if (GRN_INT32_VALUE(x) == 0 && GRN_INT32_VALUE(y) == 0) {
            GRN_INT32_SET(ctx, res, 0);
          } else {
            GRN_INT32_SET(ctx, res, 1);
          }
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_INT32;
        }
        code++;
        break;
      case GRN_OP_BUT :
        {
          grn_obj *x, *y;
          POP2ALLOC1(x, y, res);
          if (GRN_INT32_VALUE(x) == 0 || GRN_INT32_VALUE(y) == 1) {
            GRN_INT32_SET(ctx, res, 0);
          } else {
            GRN_INT32_SET(ctx, res, 1);
          }
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_INT32;
        }
        code++;
        break;
      case GRN_OP_ADJUST :
        {
          /* todo */
        }
        code++;
        break;
      case GRN_OP_MATCH :
        {
          grn_obj *x, *y;
          POP2ALLOC1(x, y, res);
          pseudo_query_scan(ctx, x, y, res);
        }
        code++;
        break;
      case GRN_OP_EQUAL :
        {
          int r = GRN_FALSE;
          grn_obj *x, *y;
          POP2ALLOC1(x, y, res);
          DO_EQ(x, y, r);
          GRN_INT32_SET(ctx, res, r);
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_INT32;
        }
        code++;
        break;
      case GRN_OP_NOT_EQUAL :
        {
          int r = GRN_FALSE;
          grn_obj *x, *y;
          POP2ALLOC1(x, y, res);
          DO_EQ(x, y, r);
          GRN_INT32_SET(ctx, res, 1 - r);
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_INT32;
        }
        code++;
        break;
      case GRN_OP_PREFIX :
        {
          grn_obj *x, *y;
          POP2ALLOC1(x, y, res);
          GRN_INT32_SET(ctx, res,
                        (GRN_TEXT_LEN(x) >= GRN_TEXT_LEN(y) &&
                         !memcmp(GRN_TEXT_VALUE(x), GRN_TEXT_VALUE(y), GRN_TEXT_LEN(y))));
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_INT32;
        }
        code++;
        break;
      case GRN_OP_SUFFIX :
        {
          grn_obj *x, *y;
          POP2ALLOC1(x, y, res);
          GRN_INT32_SET(ctx, res,
                        (GRN_TEXT_LEN(x) >= GRN_TEXT_LEN(y) &&
                         !memcmp(GRN_TEXT_VALUE(x) + GRN_TEXT_LEN(x) - GRN_TEXT_LEN(y),
                                 GRN_TEXT_VALUE(y), GRN_TEXT_LEN(y))));
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_INT32;
        }
        code++;
        break;
      case GRN_OP_LESS :
        {
          int r;
          grn_obj *x, *y;
          POP2ALLOC1(x, y, res);
          DO_COMPARE(x, y, r, <);
          GRN_INT32_SET(ctx, res, r);
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_INT32;
        }
        code++;
        break;
      case GRN_OP_GREATER :
        {
          int r;
          grn_obj *x, *y;
          POP2ALLOC1(x, y, res);
          DO_COMPARE(x, y, r, >);
          GRN_INT32_SET(ctx, res, r);
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_INT32;
        }
        code++;
        break;
      case GRN_OP_LESS_EQUAL :
        {
          int r;
          grn_obj *x, *y;
          POP2ALLOC1(x, y, res);
          DO_COMPARE(x, y, r, <=);
          GRN_INT32_SET(ctx, res, r);
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_INT32;
        }
        code++;
        break;
      case GRN_OP_GREATER_EQUAL :
        {
          int r;
          grn_obj *x, *y;
          POP2ALLOC1(x, y, res);
          DO_COMPARE(x, y, r, >=);
          GRN_INT32_SET(ctx, res, r);
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_INT32;
        }
        code++;
        break;
      case GRN_OP_GEO_DISTANCE1 :
        {
          grn_obj *e;
          double lng1, lat1, lng2, lat2, x, y, d;
          POP1(e);
          lng1 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          POP1(e);
          lat1 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          POP1(e);
          lng2 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          POP1ALLOC1(e, res);
          lat2 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          x = (lng2 - lng1) * cos((lat1 + lat2) * 0.5);
          y = (lat2 - lat1);
          d = sqrt((x * x) + (y * y)) * GEO_RADIOUS;
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_FLOAT;
          GRN_FLOAT_SET(ctx, res, d);
        }
        code++;
        break;
      case GRN_OP_GEO_DISTANCE2 :
        {
          grn_obj *e;
          double lng1, lat1, lng2, lat2, x, y, d;
          POP1(e);
          lng1 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          POP1(e);
          lat1 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          POP1(e);
          lng2 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          POP1ALLOC1(e, res);
          lat2 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          x = sin(fabs(lng2 - lng1) * 0.5);
          y = sin(fabs(lat2 - lat1) * 0.5);
          d = asin(sqrt((y * y) + cos(lat1) * cos(lat2) * x * x)) * 2 * GEO_RADIOUS;
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_FLOAT;
          GRN_FLOAT_SET(ctx, res, d);
        }
        code++;
        break;
      case GRN_OP_GEO_DISTANCE3 :
        {
          grn_obj *e;
          double lng1, lat1, lng2, lat2, p, q, m, n, x, y, d;
          POP1(e);
          lng1 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          POP1(e);
          lat1 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          POP1(e);
          lng2 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          POP1ALLOC1(e, res);
          lat2 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          p = (lat1 + lat2) * 0.5;
          q = (1 - GEO_BES_C3 * sin(p) * sin(p));
          m = GEO_BES_C1 / sqrt(q * q * q);
          n = GEO_BES_C2 / sqrt(q);
          x = n * cos(p) * fabs(lng1 - lng2);
          y = m * fabs(lat1 - lat2);
          d = sqrt((x * x) + (y * y));
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_FLOAT;
          GRN_FLOAT_SET(ctx, res, d);
        }
        code++;
        break;
      case GRN_OP_GEO_DISTANCE4 :
        {
          grn_obj *e;
          double lng1, lat1, lng2, lat2, p, q, m, n, x, y, d;
          POP1(e);
          lng1 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          POP1(e);
          lat1 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          POP1(e);
          lng2 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          POP1ALLOC1(e, res);
          lat2 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          p = (lat1 + lat2) * 0.5;
          q = (1 - GEO_GRS_C3 * sin(p) * sin(p));
          m = GEO_GRS_C1 / sqrt(q * q * q);
          n = GEO_GRS_C2 / sqrt(q);
          x = n * cos(p) * fabs(lng1 - lng2);
          y = m * fabs(lat1 - lat2);
          d = sqrt((x * x) + (y * y));
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_FLOAT;
          GRN_FLOAT_SET(ctx, res, d);
        }
        code++;
        break;
      case GRN_OP_GEO_WITHINP5 :
        {
          int r;
          grn_obj *e;
          double lng0, lat0, lng1, lat1, x, y, d;
          POP1(e);
          lng0 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          POP1(e);
          lat0 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          POP1(e);
          lng1 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          POP1(e);
          lat1 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          POP1ALLOC1(e, res);
          x = (lng1 - lng0) * cos((lat0 + lat1) * 0.5);
          y = (lat1 - lat0);
          d = sqrt((x * x) + (y * y)) * GEO_RADIOUS;
          switch (e->header.domain) {
          case GRN_DB_INT32 :
            r = d <= GRN_INT32_VALUE(e);
            break;
          case GRN_DB_FLOAT :
            r = d <= GRN_FLOAT_VALUE(e);
            break;
          default :
            r = 0;
            break;
          }
          GRN_INT32_SET(ctx, res, r);
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_INT32;
        }
        code++;
        break;
      case GRN_OP_GEO_WITHINP6 :
        {
          int r;
          grn_obj *e;
          double lng0, lat0, lng1, lat1, lng2, lat2, x, y, d;
          POP1(e);
          lng0 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          POP1(e);
          lat0 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          POP1(e);
          lng1 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          POP1(e);
          lat1 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          POP1(e);
          lng2 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          POP1ALLOC1(e, res);
          lat2 = GEO_INT2RAD(GRN_INT32_VALUE(e));
          x = (lng1 - lng0) * cos((lat0 + lat1) * 0.5);
          y = (lat1 - lat0);
          d = (x * x) + (y * y);
          x = (lng2 - lng1) * cos((lat1 + lat2) * 0.5);
          y = (lat2 - lat1);
          r = d <= (x * x) + (y * y);
          GRN_INT32_SET(ctx, res, r);
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_INT32;
        }
        code++;
        break;
      case GRN_OP_GEO_WITHINP8 :
        {
          int r;
          grn_obj *e;
          int64_t ln0, la0, ln1, la1, ln2, la2, ln3, la3;
          POP1(e);
          ln0 = GRN_INT32_VALUE(e);
          POP1(e);
          la0 = GRN_INT32_VALUE(e);
          POP1(e);
          ln1 = GRN_INT32_VALUE(e);
          POP1(e);
          la1 = GRN_INT32_VALUE(e);
          POP1(e);
          ln2 = GRN_INT32_VALUE(e);
          POP1(e);
          la2 = GRN_INT32_VALUE(e);
          POP1(e);
          ln3 = GRN_INT32_VALUE(e);
          POP1ALLOC1(e, res);
          la3 = GRN_INT32_VALUE(e);
          r = ((ln2 <= ln0) && (ln0 <= ln3) && (la2 <= la0) && (la0 <= la3));
          GRN_INT32_SET(ctx, res, r);
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_INT32;
        }
        code++;
        break;
      case GRN_OP_PLUS :
        ARITHMETIC_BINARY_OPERATION_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_PLUS,
          INTEGER_ARITHMETIC_OPERATION_PLUS,
          FLOAT_ARITHMETIC_OPERATION_PLUS,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            GRN_BULK_REWIND(res);
            grn_obj_cast(ctx, x, res, GRN_FALSE);
            grn_obj_cast(ctx, y, res, GRN_FALSE);
          }
          ,);
        break;
      case GRN_OP_MINUS :
        if (code->nargs == 1) {
          ARITHMETIC_UNARY_OPERATION_DISPATCH(
            INTEGER_UNARY_ARITHMETIC_OPERATION_MINUS,
            FLOAT_UNARY_ARITHMETIC_OPERATION_MINUS,
            ARITHMETIC_OPERATION_NO_CHECK,
            ARITHMETIC_OPERATION_NO_CHECK,
            {
              long long int x_;

              res->header.type = GRN_BULK;
              res->header.domain = GRN_DB_INT64;

              GRN_INT64_SET(ctx, res, 0);
              grn_obj_cast(ctx, x, res, GRN_FALSE);
              x_ = GRN_INT64_VALUE(res);

              GRN_INT64_SET(ctx, res, -x_);
            }
            ,);
        } else {
          ARITHMETIC_BINARY_OPERATION_DISPATCH(
            INTEGER_ARITHMETIC_OPERATION_MINUS,
            INTEGER_ARITHMETIC_OPERATION_MINUS,
            FLOAT_ARITHMETIC_OPERATION_MINUS,
            ARITHMETIC_OPERATION_NO_CHECK,
            ARITHMETIC_OPERATION_NO_CHECK,
            {
              ERR(GRN_INVALID_ARGUMENT,
                  "\"string\" - \"string\" "
                  "isn't supported");
              goto exit;
            }
            ,);
        }
        break;
      case GRN_OP_STAR :
        ARITHMETIC_BINARY_OPERATION_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_STAR,
          INTEGER_ARITHMETIC_OPERATION_STAR,
          FLOAT_ARITHMETIC_OPERATION_STAR,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT,
                "\"string\" * \"string\" "
                "isn't supported");
            goto exit;
          }
          ,);
        break;
      case GRN_OP_SLASH :
        ARITHMETIC_BINARY_OPERATION_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_SLASH,
          INTEGER_ARITHMETIC_OPERATION_SLASH,
          FLOAT_ARITHMETIC_OPERATION_SLASH,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_ZERO_DIVISION_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT,
                "\"string\" / \"string\" "
                "isn't supported");
            goto exit;
          }
          ,);
        break;
      case GRN_OP_MOD :
        ARITHMETIC_BINARY_OPERATION_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_MOD,
          INTEGER_ARITHMETIC_OPERATION_MOD,
          FLOAT_ARITHMETIC_OPERATION_MOD,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_ZERO_DIVISION_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT,
                "\"string\" % \"string\" "
                "isn't supported");
            goto exit;
          }
          ,);
        break;
      case GRN_OP_BITWISE_NOT :
        ARITHMETIC_UNARY_OPERATION_DISPATCH(
          INTEGER_UNARY_ARITHMETIC_OPERATION_BITWISE_NOT,
          FLOAT_UNARY_ARITHMETIC_OPERATION_BITWISE_NOT,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          TEXT_UNARY_ARITHMETIC_OPERATION(~),);
        break;
      case GRN_OP_BITWISE_OR :
        ARITHMETIC_BINARY_OPERATION_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_BITWISE_OR,
          INTEGER_ARITHMETIC_OPERATION_BITWISE_OR,
          FLOAT_ARITHMETIC_OPERATION_BITWISE_OR,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          TEXT_ARITHMETIC_OPERATION(|),);
        break;
      case GRN_OP_BITWISE_XOR :
        ARITHMETIC_BINARY_OPERATION_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_BITWISE_XOR,
          INTEGER_ARITHMETIC_OPERATION_BITWISE_XOR,
          FLOAT_ARITHMETIC_OPERATION_BITWISE_XOR,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          TEXT_ARITHMETIC_OPERATION(^),);
        break;
      case GRN_OP_BITWISE_AND :
        ARITHMETIC_BINARY_OPERATION_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_BITWISE_AND,
          INTEGER_ARITHMETIC_OPERATION_BITWISE_AND,
          FLOAT_ARITHMETIC_OPERATION_BITWISE_AND,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          TEXT_ARITHMETIC_OPERATION(&),);
        break;
      case GRN_OP_SHIFTL :
        ARITHMETIC_BINARY_OPERATION_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_SHIFTL,
          INTEGER_ARITHMETIC_OPERATION_SHIFTL,
          FLOAT_ARITHMETIC_OPERATION_SHIFTL,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          TEXT_ARITHMETIC_OPERATION(<<),);
        break;
      case GRN_OP_SHIFTR :
        ARITHMETIC_BINARY_OPERATION_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_SHIFTR,
          INTEGER_ARITHMETIC_OPERATION_SHIFTR,
          FLOAT_ARITHMETIC_OPERATION_SHIFTR,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          TEXT_ARITHMETIC_OPERATION(>>),);
        break;
      case GRN_OP_SHIFTRR :
        ARITHMETIC_BINARY_OPERATION_DISPATCH(
          INTEGER32_ARITHMETIC_OPERATION_SHIFTRR,
          INTEGER64_ARITHMETIC_OPERATION_SHIFTRR,
          FLOAT_ARITHMETIC_OPERATION_SHIFTRR,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            long long unsigned int x_;
            long long unsigned int y_;

            res->header.type = GRN_BULK;
            res->header.domain = GRN_DB_INT64;

            GRN_INT64_SET(ctx, res, 0);
            grn_obj_cast(ctx, x, res, GRN_FALSE);
            x_ = GRN_INT64_VALUE(res);

            GRN_INT64_SET(ctx, res, 0);
            grn_obj_cast(ctx, y, res, GRN_FALSE);
            y_ = GRN_INT64_VALUE(res);

            GRN_INT64_SET(ctx, res, x_ >> y_);
          }
          ,);
        break;
      case GRN_OP_INCR :
        UNARY_OPERATE_AND_ASSIGN_DISPATCH(EXEC_OPERATE, 1, GRN_OBJ_INCR);
        break;
      case GRN_OP_DECR :
        UNARY_OPERATE_AND_ASSIGN_DISPATCH(EXEC_OPERATE, 1, GRN_OBJ_DECR);
        break;
      case GRN_OP_INCR_POST :
        UNARY_OPERATE_AND_ASSIGN_DISPATCH(EXEC_OPERATE_POST, 1, GRN_OBJ_INCR);
        break;
      case GRN_OP_DECR_POST :
        UNARY_OPERATE_AND_ASSIGN_DISPATCH(EXEC_OPERATE_POST, 1, GRN_OBJ_DECR);
        break;
      default :
        ERR(GRN_FUNCTION_NOT_IMPLEMENTED, "not implemented operator assigned");
        break;
      }
    }
    ctx->impl->stack_curr = sp - s_;
  }
  if (ctx->impl->stack_curr + nargs > stack_curr) {
    val = grn_ctx_pop(ctx);
    if (ctx->impl->stack_curr + nargs > stack_curr) {
      /*
        GRN_LOG(ctx, GRN_LOG_WARNING, "nargs=%d stack balance=%d",
        nargs, stack_curr - ctx->impl->stack_curr);
      */
      ctx->impl->stack_curr = stack_curr - nargs;
    }
  }
exit :
  GRN_API_RETURN(val);
}

grn_obj *
grn_expr_get_value(grn_ctx *ctx, grn_obj *expr, int offset)
{
  grn_obj *res = NULL;
  grn_expr *e = (grn_expr *)expr;
  GRN_API_ENTER;
  if (0 <= offset && offset < e->values_size) {
    res = &e->values[offset];
  }
  GRN_API_RETURN(res);
}

inline static void
res_add(grn_ctx *ctx, grn_hash *s, grn_rset_posinfo *pi, uint32_t score,
        grn_operator op)
{
  grn_rset_recinfo *ri;
  switch (op) {
  case GRN_OP_OR :
    if (grn_hash_add(ctx, s, pi, s->key_size, (void **)&ri, NULL)) {
      grn_table_add_subrec((grn_obj *)s, ri, score, pi, 1);
    }
    break;
  case GRN_OP_AND :
    if (grn_hash_get(ctx, s, pi, s->key_size, (void **)&ri)) {
      ri->n_subrecs |= GRN_RSET_UTIL_BIT;
      grn_table_add_subrec((grn_obj *)s, ri, score, pi, 1);
    }
    break;
  case GRN_OP_BUT :
    {
      grn_id id;
      if ((id = grn_hash_get(ctx, s, pi, s->key_size, (void **)&ri))) {
        grn_hash_delete_by_id(ctx, s, id, NULL);
      }
    }
    break;
  case GRN_OP_ADJUST :
    if (grn_hash_get(ctx, s, pi, s->key_size, (void **)&ri)) {
      ri->score += score;
    }
    break;
  default :
    break;
  }
}

#define SCAN_ACCESSOR                  (0x01)
#define SCAN_PUSH                      (0x02)
#define SCAN_POP                       (0x04)
#define SCAN_PRE_CONST                 (0x08)

typedef struct {
  uint32_t start;
  uint32_t end;
  int32_t nargs;
  int flags;
  grn_operator op;
  grn_operator logical_op;
  grn_obj wv;
  grn_obj index;
  grn_obj *query;
  grn_obj *args[8];
} scan_info;

typedef enum {
  SCAN_START = 0,
  SCAN_VAR,
  SCAN_COL1,
  SCAN_COL2,
  SCAN_CONST
} scan_stat;

#define SI_FREE(si) {\
  GRN_OBJ_FIN(ctx, &(si)->wv);\
  GRN_OBJ_FIN(ctx, &(si)->index);\
  GRN_FREE(si);\
}

#define SI_ALLOC(si, i, st) {\
  if (!((si) = GRN_MALLOCN(scan_info, 1))) {\
    int j;\
    for (j = 0; j < i; j++) { SI_FREE(sis[j]); }\
    GRN_FREE(sis);\
    return NULL;\
  }\
  GRN_INT32_INIT(&(si)->wv, GRN_OBJ_VECTOR);\
  GRN_PTR_INIT(&(si)->index, GRN_OBJ_VECTOR, GRN_ID_NIL);\
  (si)->logical_op = GRN_OP_OR;\
  (si)->flags = SCAN_PUSH;\
  (si)->nargs = 0;\
  (si)->start = (st);\
}

static scan_info **
put_logical_op(grn_ctx *ctx, scan_info **sis, int *ip, grn_operator op, int start)
{
  int nparens = 1, ndifops = 0, i = *ip, j = i, r = 0;
  grn_operator op_ = op == GRN_OP_BUT ? GRN_OP_AND : op;
  while (j--) {
    scan_info *s_ = sis[j];
    if (s_->flags & SCAN_POP) {
      ndifops++;
      nparens++;
    } else {
      if (s_->flags & SCAN_PUSH) {
        if (!(--nparens)) {
          if (!r) {
            if (ndifops) {
              if (j) {
                nparens = 1;
                ndifops = 0;
                r = j;
              } else {
                SI_ALLOC(s_, i, start);
                s_->flags = SCAN_POP;
                s_->logical_op = op;
                sis[i++] = s_;
                *ip = i;
                break;
              }
            } else {
              s_->flags &= ~SCAN_PUSH;
              s_->logical_op = op;
              break;
            }
          } else {
            if (ndifops) {
              SI_ALLOC(s_, i, start);
              s_->flags = SCAN_POP;
              s_->logical_op = op;
              sis[i++] = s_;
              *ip = i;
            } else {
              s_->flags &= ~SCAN_PUSH;
              s_->logical_op = op;
              memcpy(&sis[i], &sis[j], sizeof(scan_info *) * (r - j));
              memmove(&sis[j], &sis[r], sizeof(scan_info *) * (i - r));
              memcpy(&sis[i + j - r], &sis[i], sizeof(scan_info *) * (r - j));
            }
            break;
          }
        }
      } else {
        if (op_ != (s_->logical_op == GRN_OP_BUT ? GRN_OP_AND : s_->logical_op)) {
          ndifops++;
        }
      }
    }
  }
  if (j < 0) {
    ERR(GRN_INVALID_ARGUMENT, "unmatched nesting level");
    for (j = 0; j < i; j++) { SI_FREE(sis[j]); }
    GRN_FREE(sis);
    return NULL;
  }
  return sis;
}


#define EXPRLOG(name,expr) {\
  grn_obj strbuf;\
  GRN_TEXT_INIT(&strbuf, 0);\
  grn_expr_inspect(ctx, &strbuf, (expr));\
  GRN_TEXT_PUTC(ctx, &strbuf, '\0');\
  GRN_LOG(ctx, GRN_LOG_NOTICE, "%s=(%s)", (name), GRN_TEXT_VALUE(&strbuf));\
  GRN_OBJ_FIN(ctx, &strbuf);\
}

static void
scan_info_put_index(grn_ctx *ctx, scan_info *si, grn_obj *index, uint32_t sid, int32_t weight)
{
  GRN_PTR_PUT(ctx, &si->index, index);
  GRN_UINT32_PUT(ctx, &si->wv, sid);
  GRN_INT32_PUT(ctx, &si->wv, weight);
  {
    int i, ni = (GRN_BULK_VSIZE(&si->index) / sizeof(grn_obj *)) - 1;
    grn_obj **pi = &GRN_PTR_VALUE_AT(&si->index, ni);
    for (i = 0; i < ni; i++, pi--) {
      if (index == pi[-1]) {
        if (i) {
          int32_t *pw = &GRN_INT32_VALUE_AT(&si->wv, (ni - i) * 2);
          memmove(pw + 2, pw, sizeof(int32_t) * 2 * i);
          pw[0] = (int32_t) sid;
          pw[1] = weight;
          memmove(pi + 1, pi, sizeof(grn_obj *) * i);
          pi[0] = index;
        }
        return;
      }
    }
  }
}

static int32_t
get_weight(grn_ctx *ctx, grn_expr_code *ec)
{
  if (ec->modify == 2 && ec[2].op == GRN_OP_STAR &&
      ec[1].value && ec[1].value->header.type == GRN_BULK) {
    if (ec[1].value->header.domain == GRN_DB_INT32 ||
        ec[1].value->header.domain == GRN_DB_UINT32) {
      return GRN_INT32_VALUE(ec[1].value);
    } else {
      int32_t weight = 1;
      grn_obj weight_buffer;
      GRN_INT32_INIT(&weight_buffer, 0);
      if (!grn_obj_cast(ctx, ec[1].value, &weight_buffer, 0)) {
        weight = GRN_INT32_VALUE(&weight_buffer);
      }
      grn_obj_unlink(ctx, &weight_buffer);
      return weight;
    }
  } else {
    return 1;
  }
}

static scan_info **
scan_info_build(grn_ctx *ctx, grn_obj *expr, int *n,
                grn_operator op, uint32_t size)
{
  grn_obj *var;
  scan_stat stat;
  int i, m = 0, o = 0;
  scan_info **sis, *si = NULL;
  grn_expr_code *c, *ce;
  grn_expr *e = (grn_expr *)expr;
  if (!(var = grn_expr_get_var_by_offset(ctx, expr, 0))) { return NULL; }
  for (stat = SCAN_START, c = e->codes, ce = &e->codes[e->codes_curr]; c < ce; c++) {
    switch (c->op) {
    case GRN_OP_MATCH :
    case GRN_OP_PREFIX :
    case GRN_OP_SUFFIX :
    case GRN_OP_EQUAL :
    case GRN_OP_NOT_EQUAL :
    case GRN_OP_LESS :
    case GRN_OP_GREATER :
    case GRN_OP_LESS_EQUAL :
    case GRN_OP_GREATER_EQUAL :
    case GRN_OP_GEO_WITHINP5 :
    case GRN_OP_GEO_WITHINP6 :
    case GRN_OP_GEO_WITHINP8 :
    case GRN_OP_TERM_EXTRACT :
      if (stat < SCAN_COL1 || SCAN_CONST < stat) { return NULL; }
      stat = SCAN_START;
      m++;
      break;
    case GRN_OP_AND :
    case GRN_OP_OR :
    case GRN_OP_BUT :
    case GRN_OP_ADJUST :
      if (stat != SCAN_START) { return NULL; }
      o++;
      if (o >= m) { return NULL; }
      break;
    case GRN_OP_PUSH :
      stat = (c->value == var) ? SCAN_VAR : SCAN_CONST;
      break;
    case GRN_OP_GET_VALUE :
      switch (stat) {
      case SCAN_START :
      case SCAN_CONST :
      case SCAN_VAR :
        stat = SCAN_COL1;
        break;
      case SCAN_COL1 :
        stat = SCAN_COL2;
        break;
      case SCAN_COL2 :
        break;
      default :
        return NULL;
        break;
      }
      break;
    case GRN_OP_CALL :
      if ((c->flags & GRN_EXPR_CODE_RELATIONAL_EXPRESSION) || c + 1 == ce) {
        stat = SCAN_START;
        m++;
      } else {
        stat = SCAN_COL2;
      }
      break;
    default :
      return NULL;
      break;
    }
  }
  if (stat || m != o + 1) { return NULL; }
  if (!(sis = GRN_MALLOCN(scan_info *, m + m + o))) { return NULL; }
  for (i = 0, stat = SCAN_START, c = e->codes, ce = &e->codes[e->codes_curr]; c < ce; c++) {
    switch (c->op) {
    case GRN_OP_MATCH :
    case GRN_OP_PREFIX :
    case GRN_OP_SUFFIX :
    case GRN_OP_EQUAL :
    case GRN_OP_NOT_EQUAL :
    case GRN_OP_LESS :
    case GRN_OP_GREATER :
    case GRN_OP_LESS_EQUAL :
    case GRN_OP_GREATER_EQUAL :
    case GRN_OP_GEO_WITHINP5 :
    case GRN_OP_GEO_WITHINP6 :
    case GRN_OP_GEO_WITHINP8 :
    case GRN_OP_TERM_EXTRACT :
      stat = SCAN_START;
      si->op = c->op;
      si->end = c - e->codes;
      sis[i++] = si;
      {
        uint32_t sid;
        grn_obj *index, **p = si->args, **pe = si->args + si->nargs;
        for (; p < pe; p++) {
          if ((*p)->header.type == GRN_EXPR) {
            uint32_t j;
            grn_expr_code *ec;
            grn_expr *e = (grn_expr *)(*p);
            for (j = e->codes_curr, ec = e->codes; j--; ec++) {
              if (ec->value) {
                switch (ec->value->header.type) {
                case GRN_ACCESSOR :
                case GRN_ACCESSOR_VIEW :
                  if (grn_column_index(ctx, ec->value, c->op, &index, 1, &sid)) {
                    si->flags |= SCAN_ACCESSOR;
                    scan_info_put_index(ctx, si, index, sid, get_weight(ctx, ec));
                  }
                  break;
                case GRN_COLUMN_FIX_SIZE :
                case GRN_COLUMN_VAR_SIZE :
                  if (grn_column_index(ctx, ec->value, c->op, &index, 1, &sid)) {
                    scan_info_put_index(ctx, si, index, sid, get_weight(ctx, ec));
                  }
                  break;
                case GRN_COLUMN_INDEX :
                  scan_info_put_index(ctx, si, ec->value, 0, get_weight(ctx, ec));
                  break;
                }
              }
            }
          } else if (GRN_DB_OBJP(*p)) {
            if (grn_column_index(ctx, *p, c->op, &index, 1, &sid)) {
              scan_info_put_index(ctx, si, index, sid, 1);
            }
          } else if (GRN_ACCESSORP(*p)) {
            si->flags |= SCAN_ACCESSOR;
            if (grn_column_index(ctx, *p, c->op, &index, 1, &sid)) {
              scan_info_put_index(ctx, si, index, sid, 1);
            }
          } else {
            si->query = *p;
          }
        }
      }
      si = NULL;
      break;
    case GRN_OP_AND :
    case GRN_OP_OR :
    case GRN_OP_BUT :
    case GRN_OP_ADJUST :
      if (!put_logical_op(ctx, sis, &i, c->op, c - e->codes)) { return NULL; }
      stat = SCAN_START;
      break;
    case GRN_OP_PUSH :
      if (!si) { SI_ALLOC(si, i, c - e->codes); }
      if (c->value == var) {
        stat = SCAN_VAR;
      } else {
        if (si->nargs < 8) {
          si->args[si->nargs++] = c->value;
        }
        if (stat == SCAN_START) { si->flags |= SCAN_PRE_CONST; }
        stat = SCAN_CONST;
      }
      break;
    case GRN_OP_GET_VALUE :
      switch (stat) {
      case SCAN_START :
        if (!si) { SI_ALLOC(si, i, c - e->codes); }
        // fallthru
      case SCAN_CONST :
      case SCAN_VAR :
        stat = SCAN_COL1;
        if (si->nargs < 8) {
          si->args[si->nargs++] = c->value;
        }
        break;
      case SCAN_COL1 :
        {
          int j;
          grn_obj inspected;
          GRN_TEXT_INIT(&inspected, 0);
          GRN_TEXT_PUTS(ctx, &inspected, "<");
          grn_inspect_name(ctx, &inspected, c->value);
          GRN_TEXT_PUTS(ctx, &inspected, ">: <");
          grn_inspect(ctx, &inspected, expr);
          GRN_TEXT_PUTS(ctx, &inspected, ">");
          ERR(GRN_INVALID_ARGUMENT,
              "invalid expression: can't use column as a value: %.*s",
              GRN_TEXT_LEN(&inspected), GRN_TEXT_VALUE(&inspected));
          GRN_OBJ_FIN(ctx, &inspected);
          for (j = 0; j < i; j++) { SI_FREE(sis[j]); }
          GRN_FREE(sis);
          return NULL;
        }
        stat = SCAN_COL2;
        break;
      case SCAN_COL2 :
        break;
      default :
        break;
      }
      break;
    case GRN_OP_CALL :
      if (!si) { SI_ALLOC(si, i, c - e->codes); }
      if ((c->flags & GRN_EXPR_CODE_RELATIONAL_EXPRESSION) || c + 1 == ce) {
        stat = SCAN_START;
        si->op = c->op;
        si->end = c - e->codes;
        sis[i++] = si;
        /* better index resolving framework for functions should be implemented */
        {
          uint32_t sid;
          grn_obj *index, **p = si->args, **pe = si->args + si->nargs;
          for (; p < pe; p++) {
            if (GRN_DB_OBJP(*p)) {
              if (grn_column_index(ctx, *p, c->op, &index, 1, &sid)) {
                scan_info_put_index(ctx, si, index, sid, 1);
              }
            } else if (GRN_ACCESSORP(*p)) {
              si->flags |= SCAN_ACCESSOR;
              if (grn_column_index(ctx, *p, c->op, &index, 1, &sid)) {
                scan_info_put_index(ctx, si, index, sid, 1);
              }
            } else {
              si->query = *p;
            }
          }
        }
        si = NULL;
      } else {
        stat = SCAN_COL2;
      }
      break;
    default :
      break;
    }
  }
  if (op == GRN_OP_OR && !size) {
    // for debug
    if (!(sis[0]->flags & SCAN_PUSH) || (sis[0]->logical_op != op)) {
      int j;
      ERR(GRN_INVALID_ARGUMENT, "invalid expr");
      for (j = 0; j < i; j++) { SI_FREE(sis[j]); }
      GRN_FREE(sis);
      return NULL;
    } else {
      sis[0]->flags &= ~SCAN_PUSH;
      sis[0]->logical_op = op;
    }
  } else {
    if (!put_logical_op(ctx, sis, &i, op, c - e->codes)) { return NULL; }
  }
  *n = i;
  return sis;
}

static void
grn_table_select_(grn_ctx *ctx, grn_obj *table, grn_obj *expr, grn_obj *v,
                  grn_obj *res, grn_operator op)
{
  int32_t score;
  grn_id id, *idp;
  grn_table_cursor *tc;
  grn_hash_cursor *hc;
  grn_hash *s = (grn_hash *)res;
  grn_obj *r;
  GRN_RECORD_INIT(v, 0, grn_obj_id(ctx, table));
  switch (op) {
  case GRN_OP_OR :
    if ((tc = grn_table_cursor_open(ctx, table, NULL, 0, NULL, 0, 0, -1, 0))) {
      while ((id = grn_table_cursor_next(ctx, tc))) {
        GRN_RECORD_SET(ctx, v, id);
        r = grn_expr_exec(ctx, expr, 0);
        if (r && (score = GRN_UINT32_VALUE(r))) {
          grn_rset_recinfo *ri;
          if (grn_hash_add(ctx, s, &id, s->key_size, (void **)&ri, NULL)) {
            grn_table_add_subrec(res, ri, score, (grn_rset_posinfo *)&id, 1);
          }
        }
      }
      grn_table_cursor_close(ctx, tc);
    }
    break;
  case GRN_OP_AND :
    if ((hc = grn_hash_cursor_open(ctx, s, NULL, 0, NULL, 0, 0, -1, 0))) {
      while (grn_hash_cursor_next(ctx, hc)) {
        grn_hash_cursor_get_key(ctx, hc, (void **) &idp);
        GRN_RECORD_SET(ctx, v, *idp);
        r = grn_expr_exec(ctx, expr, 0);
        if (r && (score = GRN_UINT32_VALUE(r))) {
          grn_rset_recinfo *ri;
          grn_hash_cursor_get_value(ctx, hc, (void **) &ri);
          grn_table_add_subrec(res, ri, score, (grn_rset_posinfo *)idp, 1);
        } else {
          grn_hash_cursor_delete(ctx, hc, NULL);
        }
      }
      grn_hash_cursor_close(ctx, hc);
    }
    break;
  case GRN_OP_BUT :
    if ((hc = grn_hash_cursor_open(ctx, s, NULL, 0, NULL, 0, 0, -1, 0))) {
      while (grn_hash_cursor_next(ctx, hc)) {
        grn_hash_cursor_get_key(ctx, hc, (void **) &idp);
        GRN_RECORD_SET(ctx, v, *idp);
        r = grn_expr_exec(ctx, expr, 0);
        if (r && (score = GRN_UINT32_VALUE(r))) {
          grn_hash_cursor_delete(ctx, hc, NULL);
        }
      }
      grn_hash_cursor_close(ctx, hc);
    }
    break;
  case GRN_OP_ADJUST :
    if ((hc = grn_hash_cursor_open(ctx, s, NULL, 0, NULL, 0, 0, -1, 0))) {
      while (grn_hash_cursor_next(ctx, hc)) {
        grn_hash_cursor_get_key(ctx, hc, (void **) &idp);
        GRN_RECORD_SET(ctx, v, *idp);
        r = grn_expr_exec(ctx, expr, 0);
        if (r && (score = GRN_UINT32_VALUE(r))) {
          grn_rset_recinfo *ri;
          grn_hash_cursor_get_value(ctx, hc, (void **) &ri);
          grn_table_add_subrec(res, ri, score, (grn_rset_posinfo *)idp, 1);
        }
      }
      grn_hash_cursor_close(ctx, hc);
    }
    break;
  default :
    break;
  }
}

grn_obj *
grn_view_select(grn_ctx *ctx, grn_obj *table, grn_obj *expr,
                grn_obj *res, grn_operator op)
{
  if (res) {
    if (res->header.type != GRN_TABLE_VIEW ||
        (res->header.domain != DB_OBJ(table)->id)) {
      ERR(GRN_INVALID_ARGUMENT, "view table required");
      return NULL;
    }
  } else {
    if (!(res = grn_table_create(ctx, NULL, 0, NULL,
                                 GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC, table, NULL))) {
      return NULL;
    }
  }
  {
    grn_obj *t, *r;
    grn_id *tp, rid;
    grn_view *tv = (grn_view *)table;
    grn_view *rv = (grn_view *)res;
    grn_hash *th = tv->hash;
    grn_hash *rh = rv->hash;
    grn_expr *e = (grn_expr *)expr;
    grn_expr_code *cs, *cd, *c0 = e->codes, *ce = e->codes + e->codes_curr;
    if ((e->codes = GRN_MALLOCN(grn_expr_code, e->codes_curr))) {
      memcpy(e->codes, c0, sizeof(grn_expr_code) * e->codes_curr);
      GRN_HASH_EACH(ctx, th, id, &tp, NULL, NULL, {
        grn_hash_get_key(ctx, rh, id, &rid, sizeof(grn_id));
        t = grn_ctx_at(ctx, *tp);
        r = grn_ctx_at(ctx, rid);
        for (cs = c0, cd = e->codes; cs < ce; cs++, cd++) {
          if (cs->value && cs->value->header.type == GRN_ACCESSOR_VIEW) {
            grn_accessor_view *a = (grn_accessor_view *)cs->value;
            if (!(cd->value = a->accessors[id - 1])) {
              cd->value = grn_null;
            }
          }
        }
        grn_table_select(ctx, t, expr, r, op);
      });

      GRN_FREE(e->codes);
    }
    e->codes = c0;
  }
  return res;
}

grn_obj *
grn_table_select(grn_ctx *ctx, grn_obj *table, grn_obj *expr,
                 grn_obj *res, grn_operator op)
{
  grn_obj *v;
  unsigned int res_size;
  if (table->header.type == GRN_TABLE_VIEW) {
    return grn_view_select(ctx, table, expr, res, op);
  }
  if (res) {
    if (res->header.type != GRN_TABLE_HASH_KEY ||
        (res->header.domain != DB_OBJ(table)->id)) {
      ERR(GRN_INVALID_ARGUMENT, "hash table required");
      return NULL;
    }
  } else {
    if (!(res = grn_table_create(ctx, NULL, 0, NULL,
                                 GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC, table, NULL))) {
      return NULL;
    }
  }
  if (!(v = grn_expr_get_var_by_offset(ctx, expr, 0))) {
    ERR(GRN_INVALID_ARGUMENT, "at least one variable must be defined");
    return NULL;
  }
  GRN_API_ENTER;
  res_size = GRN_HASH_SIZE((grn_hash *)res);
  if (op == GRN_OP_OR || res_size) {
    int i, n;
    scan_info **sis;
    if ((sis = scan_info_build(ctx, expr, &n, op, res_size))) {
      grn_obj res_stack;
      grn_expr *e = (grn_expr *)expr;
      grn_expr_code *codes = e->codes;
      uint32_t codes_curr = e->codes_curr;
      GRN_PTR_INIT(&res_stack, GRN_OBJ_VECTOR, GRN_ID_NIL);
      for (i = 0; i < n; i++) {
        int done = 0;
        scan_info *si = sis[i];
        if (si->flags & SCAN_POP) {
          grn_obj *res_;
          GRN_PTR_POP(&res_stack, res_);
          grn_table_setoperation(ctx, res_, res, res_, si->logical_op);
          grn_obj_close(ctx, res);
          res = res_;
        } else {
          if (si->flags & SCAN_PUSH) {
            grn_obj *res_;
            res_ = grn_table_create(ctx, NULL, 0, NULL,
                                    GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC, table, NULL);
            if (res_) {
              GRN_PTR_PUT(ctx, &res_stack, res);
              res = res_;
            }
          }
          if (GRN_BULK_VSIZE(&si->index)) {
            grn_obj *index = GRN_PTR_VALUE(&si->index);
            switch (si->op) {
            case GRN_OP_EQUAL :
              if (si->flags & SCAN_ACCESSOR) {
                if (index->header.type == GRN_ACCESSOR &&
                    !((grn_accessor *)index)->next) {
                  grn_obj dest;
                  grn_accessor *a = (grn_accessor *)index;
                  grn_rset_posinfo pi;
                  switch (a->action) {
                  case GRN_ACCESSOR_GET_ID :
                    GRN_UINT32_INIT(&dest, 0);
                    if (!grn_obj_cast(ctx, si->query, &dest, 0)) {
                      memcpy(&pi, GRN_BULK_HEAD(&dest), GRN_BULK_VSIZE(&dest));
                      if (pi.rid) {
                        if (pi.rid == grn_table_at(ctx, table, pi.rid)) {
                          res_add(ctx, (grn_hash *)res, &pi, 1, si->logical_op);
                        }
                      }
                      done++;
                    }
                    grn_ii_resolve_sel_and(ctx, (grn_hash *)res, si->logical_op);
                    GRN_OBJ_FIN(ctx, &dest);
                    break;
                  case GRN_ACCESSOR_GET_KEY :
                    GRN_OBJ_INIT(&dest, GRN_BULK, 0, table->header.domain);
                    if (!grn_obj_cast(ctx, si->query, &dest, 0)) {
                      if ((pi.rid = grn_table_get(ctx, table,
                                                  GRN_BULK_HEAD(&dest),
                                                  GRN_BULK_VSIZE(&dest)))) {
                        res_add(ctx, (grn_hash *)res, &pi, 1, si->logical_op);
                      }
                      done++;
                    }
                    grn_ii_resolve_sel_and(ctx, (grn_hash *)res, si->logical_op);
                    GRN_OBJ_FIN(ctx, &dest);
                    break;
                  }
                }
              } else {
                grn_obj *domain = grn_ctx_at(ctx, index->header.domain);
                if (domain) {
                  grn_id tid;
                  if (GRN_OBJ_GET_DOMAIN(si->query) == DB_OBJ(domain)->id) {
                    tid = GRN_RECORD_VALUE(si->query);
                  } else {
                    tid = grn_table_get(ctx, domain,
                                        GRN_BULK_HEAD(si->query),
                                        GRN_BULK_VSIZE(si->query));
                  }
                  grn_ii_at(ctx, (grn_ii *)index, tid, (grn_hash *)res, si->logical_op);
                }
                grn_ii_resolve_sel_and(ctx, (grn_hash *)res, si->logical_op);
                done++;
              }
              break;
            case GRN_OP_PREFIX :
              if (si->flags & SCAN_ACCESSOR) {
                if (index->header.type == GRN_ACCESSOR &&
                    !((grn_accessor *)index)->next) {
                  grn_obj dest;
                  grn_accessor *a = (grn_accessor *)index;
                  grn_rset_posinfo pi;
                  switch (a->action) {
                  case GRN_ACCESSOR_GET_ID :
                    /* todo */
                    break;
                  case GRN_ACCESSOR_GET_KEY :
                    GRN_OBJ_INIT(&dest, GRN_BULK, 0, table->header.domain);
                    if (!grn_obj_cast(ctx, si->query, &dest, 0)) {
                      grn_hash *pres;
                      if ((pres = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                                                  GRN_OBJ_TABLE_HASH_KEY))) {
                        grn_id *key;
                        grn_table_search(ctx, table,
                                         GRN_BULK_HEAD(&dest), GRN_BULK_VSIZE(&dest),
                                         si->op, (grn_obj *)pres, GRN_OP_OR);
                        GRN_HASH_EACH(ctx, pres, id, &key, NULL, NULL, {
                          pi.rid = *key;
                          res_add(ctx, (grn_hash *)res, &pi, 1, si->logical_op);
                        });
                        grn_hash_close(ctx, pres);
                      }
                      done++;
                    }
                    grn_ii_resolve_sel_and(ctx, (grn_hash *)res, si->logical_op);
                    GRN_OBJ_FIN(ctx, &dest);
                    break;
                  }
                }
              } else {
                grn_obj *i = GRN_PTR_VALUE(&si->index);
                grn_obj *domain = grn_ctx_at(ctx, i->header.domain);
                if (domain) {
                  grn_hash *pres;
                  if ((pres = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                                              GRN_OBJ_TABLE_HASH_KEY))) {
                    grn_id *key;
                    grn_table_search(ctx, domain,
                                     GRN_BULK_HEAD(si->query),
                                     GRN_BULK_VSIZE(si->query),
                                     si->op, (grn_obj *)pres, GRN_OP_OR);
                    grn_obj_unlink(ctx, domain);
                    GRN_HASH_EACH(ctx, pres, id, &key, NULL, NULL, {
                        grn_ii_at(ctx, (grn_ii *)index, *key, (grn_hash *)res, si->logical_op);
                      });
                    grn_hash_close(ctx, pres);
                  }
                  grn_obj_unlink(ctx, domain);
                }
                grn_ii_resolve_sel_and(ctx, (grn_hash *)res, si->logical_op);
                done++;
              }
              break;
            case GRN_OP_MATCH :
              {
                grn_obj wv, **ip = &GRN_PTR_VALUE(&si->index);
                int j = GRN_BULK_VSIZE(&si->index)/sizeof(grn_obj *);
                int32_t *wp = &GRN_INT32_VALUE(&si->wv);
                grn_search_optarg optarg;
                GRN_INT32_INIT(&wv, GRN_OBJ_VECTOR);
                optarg.mode = GRN_OP_EXACT;
                optarg.similarity_threshold = 0;
                optarg.max_interval = 0;
                optarg.weight_vector = (int *)GRN_BULK_HEAD(&wv);
                /* optarg.vector_size = GRN_BULK_VSIZE(&si->wv); */
                optarg.vector_size = 1;
                optarg.proc = NULL;
                optarg.max_size = 0;
                ctx->flags |= GRN_CTX_TEMPORARY_DISABLE_II_RESOLVE_SEL_AND;
                for (; j--; ip++, wp += 2) {
                  uint32_t sid = (uint32_t) wp[0];
                  int32_t weight = wp[1];
                  if (sid) {
                    GRN_INT32_SET_AT(ctx, &wv, sid - 1, weight);
                    optarg.weight_vector = &GRN_INT32_VALUE(&wv);
                    optarg.vector_size = GRN_BULK_VSIZE(&wv)/sizeof(int32_t);
                  } else {
                    optarg.weight_vector = NULL;
                    optarg.vector_size = weight;
                  }
                  if (j) {
                    if (sid && ip[0] == ip[1]) { continue; }
                  } else {
                    ctx->flags &= ~GRN_CTX_TEMPORARY_DISABLE_II_RESOLVE_SEL_AND;
                  }
                  grn_obj_search(ctx, ip[0], si->query, res, si->logical_op, &optarg);
                  GRN_BULK_REWIND(&wv);
                }
                GRN_OBJ_FIN(ctx, &wv);
              }
              done++;
              break;
            case GRN_OP_TERM_EXTRACT :
              if (si->flags & SCAN_ACCESSOR) {
                if (index->header.type == GRN_ACCESSOR &&
                    !((grn_accessor *)index)->next) {
                  grn_accessor *a = (grn_accessor *)index;
                  switch (a->action) {
                  case GRN_ACCESSOR_GET_KEY :
                    grn_table_search(ctx, table,
                                     GRN_TEXT_VALUE(si->query), GRN_TEXT_LEN(si->query),
                                     GRN_OP_TERM_EXTRACT, res, si->logical_op);
                    done++;
                    break;
                  }
                }
              }
              break;
            case GRN_OP_CALL :
              if (si->flags & SCAN_ACCESSOR) {
              } else {
                char buf[GRN_TABLE_MAX_KEY_SIZE];
                int len = grn_obj_name(ctx, si->args[0], buf,
                                       GRN_TABLE_MAX_KEY_SIZE);
                /* geo_in_circle and geo_in_rectangle only */
                if (len == 13 && !memcmp(buf, "geo_in_circle", 13)) {
                  /* TODO: error check */
                  grn_selector_geo_in_circle(ctx, index, si->args, si->nargs,
                                             res, si->logical_op);
                  done++;
                } else if (len == 16 && !memcmp(buf, "geo_in_rectangle", 16)) {
                  /* TODO: error check */
                  grn_selector_geo_in_rectangle(ctx, index, si->args, si->nargs,
                                                res, si->logical_op);
                  done++;
                }
              }
              break;
            default :
              /* todo : implement */
              /* todo : handle SCAN_PRE_CONST */
              break;
            }
          }
          if (!done) {
            e->codes = codes + si->start;
            e->codes_curr = si->end - si->start + 1;
            grn_table_select_(ctx, table, expr, v, res, si->logical_op);
          }
        }
        SI_FREE(si);
        LAP(":", "filter(%d)", grn_table_size(ctx, res));
      }
      GRN_OBJ_FIN(ctx, &res_stack);
      GRN_FREE(sis);
      e->codes = codes;
      e->codes_curr = codes_curr;
    } else {
      if (!ctx->rc) {
        grn_table_select_(ctx, table, expr, v, res, op);
      }
    }
  }
  GRN_API_RETURN(res);
}

/* grn_expr_parse */

grn_obj *
grn_ptr_value_at(grn_obj *obj, int offset)
{
  int size = GRN_BULK_VSIZE(obj) / sizeof(grn_obj *);
  if (offset < 0) { offset = size + offset; }
  return (0 <= offset && offset < size)
    ? (((grn_obj **)GRN_BULK_HEAD(obj))[offset])
    : NULL;
}

int32_t
grn_int32_value_at(grn_obj *obj, int offset)
{
  int size = GRN_BULK_VSIZE(obj) / sizeof(int32_t);
  if (offset < 0) { offset = size + offset; }
  return (0 <= offset && offset < size)
    ? (((int32_t *)GRN_BULK_HEAD(obj))[offset])
    : 0;
}

/* grn_expr_create_from_str */

#include "snip.h"

#define DEFAULT_WEIGHT 5
#define DEFAULT_DECAYSTEP 2
#define DEFAULT_MAX_INTERVAL 10
#define DEFAULT_SIMILARITY_THRESHOLD 10
#define DEFAULT_TERM_EXTRACT_POLICY 0
#define DEFAULT_WEIGHT_VECTOR_SIZE 4096

typedef struct {
  grn_ctx *ctx;
  grn_obj *e;
  grn_obj *v;
  const char *str;
  const char *cur;
  const char *str_end;
  grn_obj *table;
  grn_obj *default_column;
  grn_obj buf;
  grn_obj token_stack;
  grn_obj column_stack;
  grn_obj op_stack;
  grn_obj mode_stack;
  grn_operator default_op;
  grn_select_optarg opt;
  grn_operator default_mode;
  grn_expr_flags flags;
  grn_expr_flags default_flags;
  int escalation_threshold;
  int escalation_decaystep;
  int weight_offset;
  grn_hash *weight_set;
  snip_cond *snip_conds;
} efs_info;

typedef struct {
  grn_operator op;
  int weight;
} efs_op;

inline static void
skip_space(grn_ctx *ctx, efs_info *q)
{
  unsigned int len;
  while (q->cur < q->str_end && grn_isspace(q->cur, ctx->encoding)) {
    /* null check and length check */
    if (!(len = grn_charlen(ctx, q->cur, q->str_end))) {
      q->cur = q->str_end;
      break;
    }
    q->cur += len;
  }
}

static grn_rc get_expr(grn_ctx *ctx, efs_info *q, grn_obj *column, grn_operator mode);
static grn_rc get_token(grn_ctx *ctx, efs_info *q, efs_op *op, grn_obj *column, grn_operator mode);

static grn_rc
get_phrase(grn_ctx *ctx, efs_info *q, grn_obj *column, int mode, int option)
{
  const char *start, *s;
  start = s = q->cur;
  GRN_BULK_REWIND(&q->buf);
  while (1) {
    unsigned int len;
    if (s >= q->str_end) {
      q->cur = s;
      break;
    }
    len = grn_charlen(ctx, s, q->str_end);
    if (len == 0) {
      /* invalid string containing malformed multibyte char */
      return GRN_END_OF_DATA;
    } else if (len == 1) {
      if (*s == GRN_QUERY_QUOTER) {
        q->cur = s + 1;
        break;
      } else if (*s == GRN_QUERY_ESCAPE && s + 1 < q->str_end) {
        s++;
        len = grn_charlen(ctx, s, q->str_end);
      }
    }
    GRN_TEXT_PUT(ctx, &q->buf, s, len);
    s += len;
  }
  grn_expr_append_obj(ctx, q->e, q->v, GRN_OP_PUSH, 1);
  grn_expr_append_const(ctx, q->e, column, GRN_OP_PUSH, 1);
  grn_expr_append_op(ctx, q->e, GRN_OP_GET_VALUE, 2);
  grn_expr_append_const(ctx, q->e, &q->buf, GRN_OP_PUSH, 1);
  if (mode == GRN_OP_MATCH || mode == GRN_OP_EXACT) {
    grn_expr_append_op(ctx, q->e, mode, 2);
  } else {
    grn_expr_append_const_int(ctx, q->e, option, GRN_OP_PUSH, 1);
    grn_expr_append_op(ctx, q->e, mode, 3);
  }
  return GRN_SUCCESS;
}

static grn_rc
get_geocond(grn_ctx *ctx, efs_info *q, grn_obj *longitude, grn_obj *latitude)
{
  unsigned int len;
  const char *start = q->cur, *end;
  for (end = q->cur;; ) {
    /* null check and length check */
    if (!(len = grn_charlen(ctx, end, q->str_end))) {
      q->cur = q->str_end;
      break;
    }
    if (grn_isspace(end, ctx->encoding) ||
        *end == GRN_QUERY_PARENR) {
      q->cur = end;
      break;
    }
  }
  {
    const char *tokbuf[8];
    int32_t lng0, lat0, lng1, lat1, lng2, lat2, r;
    int32_t n = grn_str_tok((char *)start, end - start, ',', tokbuf, 8, NULL);
    switch (n) {
    case 3 :
      lng0 = grn_atoi(start, tokbuf[0], NULL);
      lat0 = grn_atoi(tokbuf[0] + 1, tokbuf[1], NULL);
      r = grn_atoi(tokbuf[1] + 1, tokbuf[2], NULL);
      grn_expr_append_obj(ctx, q->e, q->v, GRN_OP_PUSH, 1);
      grn_expr_append_const(ctx, q->e, longitude, GRN_OP_PUSH, 1);
      grn_expr_append_op(ctx, q->e, GRN_OP_GET_VALUE, 2);
      grn_expr_append_obj(ctx, q->e, q->v, GRN_OP_PUSH, 1);
      grn_expr_append_const(ctx, q->e, latitude, GRN_OP_PUSH, 1);
      grn_expr_append_op(ctx, q->e, GRN_OP_GET_VALUE, 2);
      grn_expr_append_const_int(ctx, q->e, lng0, GRN_OP_PUSH, 1);
      grn_expr_append_const_int(ctx, q->e, lat0, GRN_OP_PUSH, 1);
      grn_expr_append_const_int(ctx, q->e, r, GRN_OP_PUSH, 1);
      grn_expr_append_op(ctx, q->e, GRN_OP_GEO_WITHINP5, 5);
      break;
    case 4 :
      lng0 = grn_atoi(start, tokbuf[0], NULL);
      lat0 = grn_atoi(tokbuf[0] + 1, tokbuf[1], NULL);
      lng1 = grn_atoi(tokbuf[1] + 1, tokbuf[2], NULL);
      lat1 = grn_atoi(tokbuf[2] + 1, tokbuf[3], NULL);
      grn_expr_append_obj(ctx, q->e, q->v, GRN_OP_PUSH, 1);
      grn_expr_append_const(ctx, q->e, longitude, GRN_OP_PUSH, 1);
      grn_expr_append_op(ctx, q->e, GRN_OP_GET_VALUE, 2);
      grn_expr_append_obj(ctx, q->e, q->v, GRN_OP_PUSH, 1);
      grn_expr_append_const(ctx, q->e, latitude, GRN_OP_PUSH, 1);
      grn_expr_append_op(ctx, q->e, GRN_OP_GET_VALUE, 2);
      grn_expr_append_const_int(ctx, q->e, lng0, GRN_OP_PUSH, 1);
      grn_expr_append_const_int(ctx, q->e, lat0, GRN_OP_PUSH, 1);
      grn_expr_append_const_int(ctx, q->e, lng1, GRN_OP_PUSH, 1);
      grn_expr_append_const_int(ctx, q->e, lat1, GRN_OP_PUSH, 1);
      grn_expr_append_op(ctx, q->e, GRN_OP_GEO_WITHINP6, 6);
      break;
    case 6 :
      lng0 = grn_atoi(start, tokbuf[0], NULL);
      lat0 = grn_atoi(tokbuf[0] + 1, tokbuf[1], NULL);
      lng1 = grn_atoi(tokbuf[1] + 1, tokbuf[2], NULL);
      lat1 = grn_atoi(tokbuf[2] + 1, tokbuf[3], NULL);
      lng2 = grn_atoi(tokbuf[3] + 1, tokbuf[4], NULL);
      lat2 = grn_atoi(tokbuf[4] + 1, tokbuf[5], NULL);
      grn_expr_append_obj(ctx, q->e, q->v, GRN_OP_PUSH, 1);
      grn_expr_append_const(ctx, q->e, longitude, GRN_OP_PUSH, 1);
      grn_expr_append_op(ctx, q->e, GRN_OP_GET_VALUE, 2);
      grn_expr_append_obj(ctx, q->e, q->v, GRN_OP_PUSH, 1);
      grn_expr_append_const(ctx, q->e, latitude, GRN_OP_PUSH, 1);
      grn_expr_append_op(ctx, q->e, GRN_OP_GET_VALUE, 2);
      grn_expr_append_const_int(ctx, q->e, lng0, GRN_OP_PUSH, 1);
      grn_expr_append_const_int(ctx, q->e, lat0, GRN_OP_PUSH, 1);
      grn_expr_append_const_int(ctx, q->e, lng1, GRN_OP_PUSH, 1);
      grn_expr_append_const_int(ctx, q->e, lat1, GRN_OP_PUSH, 1);
      grn_expr_append_const_int(ctx, q->e, lng2, GRN_OP_PUSH, 1);
      grn_expr_append_const_int(ctx, q->e, lat2, GRN_OP_PUSH, 1);
      grn_expr_append_op(ctx, q->e, GRN_OP_GEO_WITHINP8, 8);
      break;
    default :
      ERR(GRN_INVALID_ARGUMENT, "invalid geocond");
      break;
    }
  }
  return ctx->rc;
}

static grn_rc
get_word(grn_ctx *ctx, efs_info *q, grn_obj *column, int mode, int option)
{
  const char *start = q->cur, *end;
  unsigned int len;
  for (end = q->cur;; ) {
    /* null check and length check */
    if (!(len = grn_charlen(ctx, end, q->str_end))) {
      q->cur = q->str_end;
      break;
    }
    if (grn_isspace(end, ctx->encoding) ||
        *end == GRN_QUERY_PARENR) {
      q->cur = end;
      break;
    }
    if (*end == GRN_QUERY_COLUMN) {
      grn_obj *c = grn_obj_column(ctx, q->table, start, end - start);
      if (c && end + 1 < q->str_end) {
        efs_op op;
        switch (end[1]) {
        case '!' :
          mode = GRN_OP_NOT_EQUAL;
          q->cur = end + 2;
          break;
        case '=' :
          if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
            mode = GRN_OP_ASSIGN;
            q->cur = end + 2;
          } else {
            get_token(ctx, q, &op, c, mode);
          }
          break;
        case '<' :
          if (end + 2 < q->str_end && end[2] == '=') {
            mode = GRN_OP_LESS_EQUAL;
            q->cur = end + 3;
          } else {
            mode = GRN_OP_LESS;
            q->cur = end + 2;
          }
          break;
        case '>' :
          if (end + 2 < q->str_end && end[2] == '=') {
            mode = GRN_OP_GREATER_EQUAL;
            q->cur = end + 3;
          } else {
            mode = GRN_OP_GREATER;
            q->cur = end + 2;
          }
          break;
        case '%' :
          mode = GRN_OP_MATCH;
          q->cur = end + 2;
          break;
        case '@' :
          q->cur = end + 2;
          return get_geocond(ctx, q, column, c);
          break;
        default :
          mode = GRN_OP_EQUAL;
          q->cur = end + 1;
          break;
        }
        return get_token(ctx, q, &op, c, mode);
      } else {
        ERR(GRN_INVALID_ARGUMENT, "column lookup failed");
        return ctx->rc;
      }
    } else if (*end == GRN_QUERY_PREFIX) {
      mode = GRN_OP_PREFIX;
      q->cur = end + 1;
      break;
    }
    end += len;
  }
  if (!column) {
    ERR(GRN_INVALID_ARGUMENT, "column missing");
    return ctx->rc;
  }
  if (mode == GRN_OP_ASSIGN) {
    grn_expr_append_obj(ctx, q->e, q->v, GRN_OP_PUSH, 1);
    grn_expr_append_const(ctx, q->e, column, GRN_OP_PUSH, 1);
    grn_expr_append_const_str(ctx, q->e, start, end - start, GRN_OP_PUSH, 1);
    grn_expr_append_op(ctx, q->e, GRN_OP_ASSIGN, 2);
  } else {
    grn_expr_append_obj(ctx, q->e, q->v, GRN_OP_PUSH, 1);
    grn_expr_append_const(ctx, q->e, column, GRN_OP_PUSH, 1);
    grn_expr_append_op(ctx, q->e, GRN_OP_GET_VALUE, 2);
    grn_expr_append_const_str(ctx, q->e, start, end - start, GRN_OP_PUSH, 1);
    switch (mode) {
    case GRN_OP_NEAR :
    case GRN_OP_NEAR2 :
    case GRN_OP_SIMILAR :
    case GRN_OP_TERM_EXTRACT :
      grn_expr_append_const_int(ctx, q->e, option, GRN_OP_PUSH, 1);
      grn_expr_append_op(ctx, q->e, mode, 3);
      break;
    default :
      grn_expr_append_op(ctx, q->e, mode, 2);
      break;
    }
  }
  return GRN_SUCCESS;
}

static void
get_op(efs_info *q, efs_op *op, grn_operator *mode, int *option)
{
  const char *start, *end = q->cur;
  switch (*end) {
  case 'S' :
    *mode = GRN_OP_SIMILAR;
    start = ++end;
    *option = grn_atoi(start, q->str_end, (const char **)&end);
    if (start == end) { *option = DEFAULT_SIMILARITY_THRESHOLD; }
    q->cur = end;
    break;
  case 'N' :
    *mode = GRN_OP_NEAR;
    start = ++end;
    *option = grn_atoi(start, q->str_end, (const char **)&end);
    if (start == end) { *option = DEFAULT_MAX_INTERVAL; }
    q->cur = end;
    break;
  case 'n' :
    *mode = GRN_OP_NEAR2;
    start = ++end;
    *option = grn_atoi(start, q->str_end, (const char **)&end);
    if (start == end) { *option = DEFAULT_MAX_INTERVAL; }
    q->cur = end;
    break;
  case 'T' :
    *mode = GRN_OP_TERM_EXTRACT;
    start = ++end;
    *option = grn_atoi(start, q->str_end, (const char **)&end);
    if (start == end) { *option = DEFAULT_TERM_EXTRACT_POLICY; }
    q->cur = end;
    break;
  case 'X' : /* force exact mode */
    op->op = GRN_OP_AND;
    *mode = GRN_OP_EXACT;
    *option = 0;
    start = ++end;
    q->cur = end;
    break;
  default :
    break;
  }
}

static grn_rc
get_token(grn_ctx *ctx, efs_info *q, efs_op *op, grn_obj *column, grn_operator mode)
{
  int option = 0;
  op->op = q->default_op;
  op->weight = DEFAULT_WEIGHT;
  for (;;) {
    skip_space(ctx, q);
    if (q->cur >= q->str_end) { return GRN_END_OF_DATA; }
    switch (*q->cur) {
    case '\0' :
      return GRN_END_OF_DATA;
      break;
    case GRN_QUERY_PARENR :
      q->cur++;
      return GRN_END_OF_DATA;
      break;
    case GRN_QUERY_QUOTEL :
      q->cur++;
      return get_phrase(ctx, q, column, mode, option);
      break;
    case GRN_QUERY_PREFIX :
      q->cur++;
      get_op(q, op, &mode, &option);
      break;
    case GRN_QUERY_AND :
      q->cur++;
      op->op = GRN_OP_AND;
      break;
    case GRN_QUERY_BUT :
      q->cur++;
      op->op = GRN_OP_BUT;
      break;
    case GRN_QUERY_ADJ_INC :
      q->cur++;
      if (op->weight < 127) { op->weight++; }
      op->op = GRN_OP_ADJUST;
      break;
    case GRN_QUERY_ADJ_DEC :
      q->cur++;
      if (op->weight > -128) { op->weight--; }
      op->op = GRN_OP_ADJUST;
      break;
    case GRN_QUERY_ADJ_NEG :
      q->cur++;
      op->op = GRN_OP_ADJUST;
      op->weight = -1;
      break;
    case GRN_QUERY_PARENL :
      q->cur++;
      return get_expr(ctx, q, column, mode);
      break;
    case 'O' :
      if (q->cur[1] == 'R' && q->cur[2] == ' ') {
        q->cur += 2;
        op->op = GRN_OP_OR;
        break;
      }
      /* fallthru */
    default :
      return get_word(ctx, q, column, mode, option);
      break;
    }
  }
  return GRN_SUCCESS;
}

static grn_rc
get_expr(grn_ctx *ctx, efs_info *q, grn_obj *column, grn_operator mode)
{
  efs_op op;
  grn_rc rc = get_token(ctx, q, &op, column, mode);
  if (rc) { return rc; }
  while (!(rc = get_token(ctx, q, &op, column, mode))) {
    if (op.op == GRN_OP_ADJUST) {
      grn_expr_append_const_int(ctx, q->e, op.weight, GRN_OP_PUSH, 1);
      grn_expr_append_op(ctx, q->e, op.op, 3);
    } else {
      grn_expr_append_op(ctx, q->e, op.op, 2);
    }
  }
  return rc;
}

#define DISABLE_UNUSED_CODE 1
#ifndef DISABLE_UNUSED_CODE
static const char *
get_weight_vector(grn_ctx *ctx, efs_info *query, const char *source)
{
  const char *p;

  if (!query->opt.weight_vector &&
      !query->weight_set &&
      !(query->opt.weight_vector = GRN_CALLOC(sizeof(int) * DEFAULT_WEIGHT_VECTOR_SIZE))) {
    GRN_LOG(ctx, GRN_LOG_ALERT, "get_weight_vector malloc fail");
    return source;
  }
  for (p = source; p < query->str_end; ) {
    unsigned int key;
    int value;

    /* key, key is not zero */
    key = grn_atoui(p, query->str_end, &p);
    if (!key || key > GRN_ID_MAX) { break; }

    /* value */
    if (*p == ':') {
      p++;
      value = grn_atoi(p, query->str_end, &p);
    } else {
      value = 1;
    }

    if (query->weight_set) {
      int *pval;
      if (grn_hash_add(ctx, query->weight_set, &key, sizeof(unsigned int), (void **)&pval, NULL)) {
        *pval = value;
      }
    } else if (key < DEFAULT_WEIGHT_VECTOR_SIZE) {
      query->opt.weight_vector[key - 1] = value;
    } else {
      GRN_FREE(query->opt.weight_vector);
      query->opt.weight_vector = NULL;
      if (!(query->weight_set = grn_hash_create(ctx, NULL, sizeof(unsigned int), sizeof(int),
                                                0))) {
        return source;
      }
      p = source;           /* reparse */
      continue;
    }
    if (*p != ',') { break; }
    p++;
  }
  return p;
}

static void
get_pragma(grn_ctx *ctx, efs_info *q)
{
  const char *start, *end = q->cur;
  while (end < q->str_end && *end == GRN_QUERY_PREFIX) {
    if (++end >= q->str_end) { break; }
    switch (*end) {
    case 'E' :
      start = ++end;
      q->escalation_threshold = grn_atoi(start, q->str_end, (const char **)&end);
      while (end < q->str_end && (('0' <= *end && *end <= '9') || *end == '-')) { end++; }
      if (*end == ',') {
        start = ++end;
        q->escalation_decaystep = grn_atoi(start, q->str_end, (const char **)&end);
      }
      q->cur = end;
      break;
    case 'D' :
      start = ++end;
      while (end < q->str_end && *end != GRN_QUERY_PREFIX && !grn_isspace(end, ctx->encoding)) {
        end++;
      }
      if (end > start) {
        switch (*start) {
        case 'O' :
          q->default_op = GRN_OP_OR;
          break;
        case GRN_QUERY_AND :
          q->default_op = GRN_OP_AND;
          break;
        case GRN_QUERY_BUT :
          q->default_op = GRN_OP_BUT;
          break;
        case GRN_QUERY_ADJ_INC :
          q->default_op = GRN_OP_ADJUST;
          break;
        }
      }
      q->cur = end;
      break;
    case 'W' :
      start = ++end;
      end = (char *)get_weight_vector(ctx, q, start);
      q->cur = end;
      break;
    }
  }
}

static int
section_weight_cb(grn_ctx *ctx, grn_hash *r, const void *rid, int sid, void *arg)
{
  int *w;
  grn_hash *s = (grn_hash *)arg;
  if (s && grn_hash_get(ctx, s, &sid, sizeof(grn_id), (void **)&w)) {
    return *w;
  } else {
    return 0;
  }
}
#endif

#include "ecmascript.h"
#include "ecmascript.c"

static grn_rc
grn_expr_parser_open(grn_ctx *ctx)
{
  if (!ctx->impl->parser) {
    yyParser *pParser = GRN_MALLOCN(yyParser, 1);
    if (pParser) {
      pParser->yyidx = -1;
#if YYSTACKDEPTH<=0
      yyGrowStack(pParser);
#endif
      ctx->impl->parser = pParser;
    }
  }
  return ctx->rc;
}

#define PARSE(token) grn_expr_parser(ctx->impl->parser, (token), 0, q)

static grn_rc
get_word_(grn_ctx *ctx, efs_info *q)
{
  const char *start = q->cur, *end;
  unsigned int len;
  for (end = q->cur;; ) {
    /* null check and length check */
    if (!(len = grn_charlen(ctx, end, q->str_end))) {
      q->cur = q->str_end;
      break;
    }
    if (grn_isspace(end, ctx->encoding) ||
        *end == GRN_QUERY_PARENL || *end == GRN_QUERY_PARENR) {
      q->cur = end;
      break;
    }
    if (q->flags & GRN_EXPR_ALLOW_COLUMN && *end == GRN_QUERY_COLUMN) {
      grn_operator mode;
      grn_obj *c = grn_obj_column(ctx, q->table, start, end - start);
      if (c && end + 1 < q->str_end) {
        //        efs_op op;
        switch (end[1]) {
        case '!' :
          mode = GRN_OP_NOT_EQUAL;
          q->cur = end + 2;
          break;
        case '=' :
          if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
            mode = GRN_OP_ASSIGN;
            q->cur = end + 2;
          } else {
            mode = GRN_OP_EQUAL;
            q->cur = end + 1;
          }
          break;
        case '<' :
          if (end + 2 < q->str_end && end[2] == '=') {
            mode = GRN_OP_LESS_EQUAL;
            q->cur = end + 3;
          } else {
            mode = GRN_OP_LESS;
            q->cur = end + 2;
          }
          break;
        case '>' :
          if (end + 2 < q->str_end && end[2] == '=') {
            mode = GRN_OP_GREATER_EQUAL;
            q->cur = end + 3;
          } else {
            mode = GRN_OP_GREATER;
            q->cur = end + 2;
          }
          break;
        case '@' :
          mode = GRN_OP_MATCH;
          q->cur = end + 2;
          break;
        case '^' :
          mode = GRN_OP_PREFIX;
          q->cur = end + 2;
          break;
        case '$' :
          mode = GRN_OP_SUFFIX;
          q->cur = end + 2;
          break;
        default :
          mode = GRN_OP_EQUAL;
          q->cur = end + 1;
          break;
        }
      } else {
        ERR(GRN_INVALID_ARGUMENT, "column lookup failed");
        q->cur = q->str_end;
        return ctx->rc;
      }
      PARSE(GRN_EXPR_TOKEN_IDENTIFIER);
      PARSE(GRN_EXPR_TOKEN_RELATIVE_OP);

      GRN_PTR_PUT(ctx, &((grn_expr *)(q->e))->objs, c);
      GRN_PTR_PUT(ctx, &q->column_stack, c);
      GRN_INT32_PUT(ctx, &q->mode_stack, mode);

      return GRN_SUCCESS;
    } else if (*end == GRN_QUERY_PREFIX) {
      q->cur = end + 1;
      GRN_INT32_PUT(ctx, &q->mode_stack, GRN_OP_PREFIX);
      break;
    }
    end += len;
  }
  GRN_PTR_PUT(ctx, &q->token_stack, grn_expr_add_str(ctx, q->e, start, end - start));

  PARSE(GRN_EXPR_TOKEN_QSTRING);
{
  grn_obj *column, *token;
  efs_info *efsi = q;
  GRN_PTR_POP(&efsi->token_stack, token);
  column = grn_ptr_value_at(&efsi->column_stack, -1);
  grn_expr_append_const(efsi->ctx, efsi->e, column, GRN_OP_GET_VALUE, 1);
  grn_expr_append_obj(efsi->ctx, efsi->e, token, GRN_OP_PUSH, 1);
  grn_expr_append_op(efsi->ctx, efsi->e, grn_int32_value_at(&efsi->mode_stack, -1), 2);
}
  return GRN_SUCCESS;
}

static grn_rc
parse_query(grn_ctx *ctx, efs_info *q)
{
  int option = 0;
  grn_operator mode;
  efs_op op_, *op = &op_;
  grn_bool first_token = GRN_TRUE;
  grn_bool block_started = GRN_FALSE;

  op->op = q->default_op;
  op->weight = DEFAULT_WEIGHT;
  while (!ctx->rc) {
    skip_space(ctx, q);
    if (q->cur >= q->str_end) { goto exit; }
    switch (*q->cur) {
    case '\0' :
      goto exit;
      break;
    case GRN_QUERY_PARENR :
      q->cur++;
      PARSE(GRN_EXPR_TOKEN_PARENR);
      break;
    case GRN_QUERY_QUOTEL :
      q->cur++;

      {
        const char *start, *s;
        start = s = q->cur;
        GRN_BULK_REWIND(&q->buf);
        while (1) {
          unsigned int len;
          if (s >= q->str_end) {
            q->cur = s;
            break;
          }
          len = grn_charlen(ctx, s, q->str_end);
          if (len == 0) {
            /* invalid string containing malformed multibyte char */
            goto exit;
          } else if (len == 1) {
            if (*s == GRN_QUERY_QUOTER) {
              q->cur = s + 1;
              break;
            } else if (*s == GRN_QUERY_ESCAPE && s + 1 < q->str_end) {
              s++;
              len = grn_charlen(ctx, s, q->str_end);
            }
          }
          GRN_TEXT_PUT(ctx, &q->buf, s, len);
          s += len;

        }
        GRN_PTR_PUT(ctx, &q->token_stack, grn_expr_add_str(ctx, q->e,
                                                           GRN_TEXT_VALUE(&q->buf),
                                                           GRN_TEXT_LEN(&q->buf)));
          PARSE(GRN_EXPR_TOKEN_QSTRING);
{
  grn_obj *column, *token;
  efs_info *efsi = q;
  GRN_PTR_POP(&efsi->token_stack, token);
  column = grn_ptr_value_at(&efsi->column_stack, -1);
  grn_expr_append_const(efsi->ctx, efsi->e, column, GRN_OP_GET_VALUE, 1);
  grn_expr_append_obj(efsi->ctx, efsi->e, token, GRN_OP_PUSH, 1);
  grn_expr_append_op(efsi->ctx, efsi->e, grn_int32_value_at(&efsi->mode_stack, -1), 2);
}

      }

      break;
    case GRN_QUERY_PREFIX :
      q->cur++;
      get_op(q, op, &mode, &option);
      PARSE(GRN_EXPR_TOKEN_MATCH);
      break;
    case GRN_QUERY_AND :
      q->cur++;
      if (!first_token) {
        op->op = GRN_OP_AND;
        PARSE(GRN_EXPR_TOKEN_LOGICAL_AND);
      }
      break;
    case GRN_QUERY_BUT :
      q->cur++;
      op->op = GRN_OP_BUT;
      PARSE(GRN_EXPR_TOKEN_LOGICAL_BUT);
      break;
    case GRN_QUERY_ADJ_INC :
      q->cur++;
      if (op->weight < 127) { op->weight++; }
      op->op = GRN_OP_ADJUST;
      PARSE(GRN_EXPR_TOKEN_ADJUST);
      break;
    case GRN_QUERY_ADJ_DEC :
      q->cur++;
      if (op->weight > -128) { op->weight--; }
      op->op = GRN_OP_ADJUST;
      PARSE(GRN_EXPR_TOKEN_ADJUST);
      break;
    case GRN_QUERY_ADJ_NEG :
      q->cur++;
      op->op = GRN_OP_ADJUST;
      op->weight = -1;
      PARSE(GRN_EXPR_TOKEN_ADJUST);
      break;
    case GRN_QUERY_PARENL :
      q->cur++;
      PARSE(GRN_EXPR_TOKEN_PARENL);
      block_started = GRN_TRUE;
      break;
    case '=' :
    case 'O' :
      if (q->cur[1] == 'R' && q->cur[2] == ' ') {
        q->cur += 2;
        PARSE(GRN_EXPR_TOKEN_LOGICAL_OR);
        break;
      }
      /* fallthru */
    default :
      get_word_(ctx, q);
      break;
    }
    first_token = block_started;
    block_started = GRN_FALSE;
  }
exit :
  PARSE(0);
  return GRN_SUCCESS;
}

static grn_rc
get_string(grn_ctx *ctx, efs_info *q)
{
  const char *s;
  unsigned int len;
  grn_rc rc = GRN_END_OF_DATA;
  GRN_BULK_REWIND(&q->buf);
  for (s = q->cur + 1; s < q->str_end; s += len) {
    if (!(len = grn_charlen(ctx, s, q->str_end))) { break; }
    if (len == 1) {
      if (*s == GRN_QUERY_QUOTER) {
        s++;
        rc = GRN_SUCCESS;
        break;
      }
      if (*s == GRN_QUERY_ESCAPE && s + 1 < q->str_end) {
        s++;
        if (!(len = grn_charlen(ctx, s, q->str_end))) { break; }
      }
    }
    GRN_TEXT_PUT(ctx, &q->buf, s, len);
  }
  q->cur = s;
  return rc;
}

static grn_rc
get_identifier(grn_ctx *ctx, efs_info *q)
{
  const char *s;
  unsigned int len;
  grn_rc rc = GRN_SUCCESS;
  for (s = q->cur; s < q->str_end; s += len) {
    if (!(len = grn_charlen(ctx, s, q->str_end))) {
      rc = GRN_END_OF_DATA;
      goto exit;
    }
    if (grn_isspace(s, ctx->encoding)) { goto done; }
    if (len == 1) {
      switch (*s) {
      case '\0' : case '(' : case ')' : case '{' : case '}' :
      case '[' : case ']' : case ',' : case ':' : case '@' :
      case '?' : case '"' : case '*' : case '+' : case '-' :
      case '|' : case '/' : case '%' : case '!' : case '^' :
      case '&' : case '>' : case '<' : case '=' : case '~' :
        /* case '.' : */
        goto done;
        break;
      }
    }
  }
done :
  len = s - q->cur;
  switch (*q->cur) {
  case 'd' :
    if (len == 6 && !memcmp(q->cur, "delete", 6)) {
      PARSE(GRN_EXPR_TOKEN_DELETE);
      goto exit;
    }
    break;
  case 'f' :
    if (len == 5 && !memcmp(q->cur, "false", 5)) {
      grn_obj buf;
      PARSE(GRN_EXPR_TOKEN_BOOLEAN);
      GRN_BOOL_INIT(&buf, 0);
      GRN_BOOL_SET(ctx, &buf, 0);
      grn_expr_append_const(ctx, q->e, &buf, GRN_OP_PUSH, 1);
      GRN_OBJ_FIN(ctx, &buf);
      goto exit;
    }
    break;
  case 'i' :
    if (len == 2 && !memcmp(q->cur, "in", 2)) {
      PARSE(GRN_EXPR_TOKEN_IN);
      goto exit;
    }
    break;
  case 'n' :
    if (len == 4 && !memcmp(q->cur, "null", 4)) {
      grn_obj buf;
      PARSE(GRN_EXPR_TOKEN_NULL);
      GRN_VOID_INIT(&buf);
      grn_expr_append_const(ctx, q->e, &buf, GRN_OP_PUSH, 1);
      GRN_OBJ_FIN(ctx, &buf);
      goto exit;
    }
    break;
  case 't' :
    if (len == 4 && !memcmp(q->cur, "true", 4)) {
      grn_obj buf;
      PARSE(GRN_EXPR_TOKEN_BOOLEAN);
      GRN_BOOL_INIT(&buf, 0);
      GRN_BOOL_SET(ctx, &buf, 1);
      grn_expr_append_const(ctx, q->e, &buf, GRN_OP_PUSH, 1);
      GRN_OBJ_FIN(ctx, &buf);
      goto exit;
    }
    break;
  }
  {
    grn_obj *obj;
    const char *name = q->cur;
    unsigned int name_size = s - q->cur;
    if ((obj = grn_expr_get_var(ctx, q->e, name, name_size))) {
      PARSE(GRN_EXPR_TOKEN_IDENTIFIER);
      grn_expr_append_obj(ctx, q->e, obj, GRN_OP_PUSH, 1);
      goto exit;
    }
    if ((obj = grn_obj_column(ctx, q->table, name, name_size))) {
      GRN_PTR_PUT(ctx, &((grn_expr *)q->e)->objs, obj);
      PARSE(GRN_EXPR_TOKEN_IDENTIFIER);
      grn_expr_append_obj(ctx, q->e, obj, GRN_OP_GET_VALUE, 1);
      goto exit;
    }
    if ((obj = grn_ctx_get(ctx, name, name_size))) {
      PARSE(GRN_EXPR_TOKEN_IDENTIFIER);
      grn_expr_append_obj(ctx, q->e, obj, GRN_OP_PUSH, 1);
      goto exit;
    }
    rc = GRN_SYNTAX_ERROR;
  }
exit :
  q->cur = s;
  return rc;
}

static void
set_tos_minor_to_curr(grn_ctx *ctx, efs_info *q)
{
  yyParser *pParser = ctx->impl->parser;
  yyStackEntry *yytos = &pParser->yystack[pParser->yyidx];
  yytos->minor.yy0 = ((grn_expr *)(q->e))->codes_curr;
}

static grn_rc
parse_script(grn_ctx *ctx, efs_info *q)
{
  grn_rc rc = GRN_SUCCESS;
  for (;;) {
    skip_space(ctx, q);
    if (q->cur >= q->str_end) { rc = GRN_END_OF_DATA; goto exit; }
    switch (*q->cur) {
    case '\0' :
      rc = GRN_END_OF_DATA;
      goto exit;
      break;
    case '(' :
      q->cur++;
      PARSE(GRN_EXPR_TOKEN_PARENL);
      break;
    case ')' :
      q->cur++;
      PARSE(GRN_EXPR_TOKEN_PARENR);
      break;
    case '{' :
      q->cur++;
      PARSE(GRN_EXPR_TOKEN_BRACEL);
      break;
    case '}' :
      q->cur++;
      PARSE(GRN_EXPR_TOKEN_BRACER);
      break;
    case '[' :
      q->cur++;
      PARSE(GRN_EXPR_TOKEN_BRACKETL);
      break;
    case ']' :
      q->cur++;
      PARSE(GRN_EXPR_TOKEN_BRACKETR);
      break;
    case ',' :
      q->cur++;
      PARSE(GRN_EXPR_TOKEN_COMMA);
      break;
    case '.' :
      q->cur++;
      PARSE(GRN_EXPR_TOKEN_DOT);
      break;
    case ':' :
      q->cur++;
      PARSE(GRN_EXPR_TOKEN_COLON);
      set_tos_minor_to_curr(ctx, q);
      grn_expr_append_op(ctx, q->e, GRN_OP_JUMP, 0);
      break;
    case '@' :
      q->cur++;
      switch (*q->cur) {
      case '^' :
        q->cur++;
        PARSE(GRN_EXPR_TOKEN_PREFIX);
        break;
      case '$' :
        q->cur++;
        PARSE(GRN_EXPR_TOKEN_SUFFIX);
        break;
      default :
        PARSE(GRN_EXPR_TOKEN_MATCH);
        break;
      }
      break;
    case '~' :
      q->cur++;
      PARSE(GRN_EXPR_TOKEN_BITWISE_NOT);
      break;
    case '?' :
      q->cur++;
      PARSE(GRN_EXPR_TOKEN_QUESTION);
      set_tos_minor_to_curr(ctx, q);
      grn_expr_append_op(ctx, q->e, GRN_OP_CJUMP, 0);
      break;
    case '"' :
      if ((rc = get_string(ctx, q))) { goto exit; }
      PARSE(GRN_EXPR_TOKEN_STRING);
      grn_expr_append_const(ctx, q->e, &q->buf, GRN_OP_PUSH, 1);
      break;
    case '*' :
      q->cur++;
      switch (*q->cur) {
      case 'N' :
        q->cur++;
        PARSE(GRN_EXPR_TOKEN_NEAR);
        break;
      case 'S' :
        q->cur++;
        PARSE(GRN_EXPR_TOKEN_SIMILAR);
        break;
      case 'T' :
        q->cur++;
        PARSE(GRN_EXPR_TOKEN_TERM_EXTRACT);
        break;
      case '>' :
        q->cur++;
        PARSE(GRN_EXPR_TOKEN_ADJUST);
        break;
      case '<' :
        q->cur++;
        PARSE(GRN_EXPR_TOKEN_ADJUST);
        break;
      case '~' :
        q->cur++;
        PARSE(GRN_EXPR_TOKEN_ADJUST);
        break;
      case '=' :
        if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
          q->cur++;
          PARSE(GRN_EXPR_TOKEN_STAR_ASSIGN);
        } else {
          ERR(GRN_UPDATE_NOT_ALLOWED,
              "'*=' is not allowed (%.*s)", q->str_end - q->str, q->str);
        }
        break;
      default :
        PARSE(GRN_EXPR_TOKEN_STAR);
        break;
      }
      break;
    case '+' :
      q->cur++;
      switch (*q->cur) {
      case '+' :
        if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
          q->cur++;
          PARSE(GRN_EXPR_TOKEN_INCR);
        } else {
          ERR(GRN_UPDATE_NOT_ALLOWED,
              "'++' is not allowed (%.*s)", q->str_end - q->str, q->str);
        }
        break;
      case '=' :
        if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
          q->cur++;
          PARSE(GRN_EXPR_TOKEN_PLUS_ASSIGN);
        } else {
          ERR(GRN_UPDATE_NOT_ALLOWED,
              "'+=' is not allowed (%.*s)", q->str_end - q->str, q->str);
          goto exit;
        }
        break;
      default :
        PARSE(GRN_EXPR_TOKEN_PLUS);
        break;
      }
      break;
    case '-' :
      q->cur++;
      switch (*q->cur) {
      case '-' :
        if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
          q->cur++;
          PARSE(GRN_EXPR_TOKEN_DECR);
        } else {
          ERR(GRN_UPDATE_NOT_ALLOWED,
              "'--' is not allowed (%.*s)", q->str_end - q->str, q->str);
        }
        break;
      case '=' :
        if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
          q->cur++;
          PARSE(GRN_EXPR_TOKEN_MINUS_ASSIGN);
        } else {
          ERR(GRN_UPDATE_NOT_ALLOWED,
              "'-=' is not allowed (%.*s)", q->str_end - q->str, q->str);
        }
        break;
      default :
        PARSE(GRN_EXPR_TOKEN_MINUS);
        break;
      }
      break;
    case '|' :
      q->cur++;
      switch (*q->cur) {
      case '|' :
        q->cur++;
        PARSE(GRN_EXPR_TOKEN_LOGICAL_OR);
        break;
      case '=' :
        if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
          q->cur++;
          PARSE(GRN_EXPR_TOKEN_OR_ASSIGN);
        } else {
          ERR(GRN_UPDATE_NOT_ALLOWED,
              "'|=' is not allowed (%.*s)", q->str_end - q->str, q->str);
        }
        break;
      default :
        PARSE(GRN_EXPR_TOKEN_BITWISE_OR);
        break;
      }
      break;
    case '/' :
      q->cur++;
      switch (*q->cur) {
      case '=' :
        if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
          q->cur++;
          PARSE(GRN_EXPR_TOKEN_SLASH_ASSIGN);
        } else {
          ERR(GRN_UPDATE_NOT_ALLOWED,
              "'/=' is not allowed (%.*s)", q->str_end - q->str, q->str);
        }
        break;
      default :
        PARSE(GRN_EXPR_TOKEN_SLASH);
        break;
      }
      break;
    case '%' :
      q->cur++;
      switch (*q->cur) {
      case '=' :
        if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
          q->cur++;
          PARSE(GRN_EXPR_TOKEN_MOD_ASSIGN);
        } else {
          ERR(GRN_UPDATE_NOT_ALLOWED,
              "'%%=' is not allowed (%.*s)", q->str_end - q->str, q->str);
        }
        break;
      default :
        PARSE(GRN_EXPR_TOKEN_MOD);
        break;
      }
      break;
    case '!' :
      q->cur++;
      switch (*q->cur) {
      case '=' :
        q->cur++;
        PARSE(GRN_EXPR_TOKEN_NOT_EQUAL);
        break;
      default :
        PARSE(GRN_EXPR_TOKEN_NOT);
        break;
      }
      break;
    case '^' :
      q->cur++;
      switch (*q->cur) {
      case '=' :
        if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
          q->cur++;
          PARSE(GRN_EXPR_TOKEN_XOR_ASSIGN);
        } else {
          ERR(GRN_UPDATE_NOT_ALLOWED,
              "'^=' is not allowed (%.*s)", q->str_end - q->str, q->str);
        }
        break;
      default :
        PARSE(GRN_EXPR_TOKEN_BITWISE_XOR);
        break;
      }
      break;
    case '&' :
      q->cur++;
      switch (*q->cur) {
      case '&' :
        q->cur++;
        PARSE(GRN_EXPR_TOKEN_LOGICAL_AND);
        break;
      case '=' :
        if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
          q->cur++;
          PARSE(GRN_EXPR_TOKEN_AND_ASSIGN);
        } else {
          ERR(GRN_UPDATE_NOT_ALLOWED,
              "'&=' is not allowed (%.*s)", q->str_end - q->str, q->str);
        }
        break;
      case '!' :
        q->cur++;
        PARSE(GRN_EXPR_TOKEN_LOGICAL_BUT);
        break;
      default :
        PARSE(GRN_EXPR_TOKEN_BITWISE_AND);
        break;
      }
      break;
    case '>' :
      q->cur++;
      switch (*q->cur) {
      case '>' :
        q->cur++;
        switch (*q->cur) {
        case '>' :
          q->cur++;
          switch (*q->cur) {
          case '=' :
            if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
              q->cur++;
              PARSE(GRN_EXPR_TOKEN_SHIFTRR_ASSIGN);
            } else {
              ERR(GRN_UPDATE_NOT_ALLOWED,
                  "'>>>=' is not allowed (%.*s)", q->str_end - q->str, q->str);
            }
            break;
          default :
            PARSE(GRN_EXPR_TOKEN_SHIFTRR);
            break;
          }
          break;
        case '=' :
          if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
            q->cur++;
            PARSE(GRN_EXPR_TOKEN_SHIFTR_ASSIGN);
          } else {
            ERR(GRN_UPDATE_NOT_ALLOWED,
                "'>>=' is not allowed (%.*s)", q->str_end - q->str, q->str);
          }
          break;
        default :
          PARSE(GRN_EXPR_TOKEN_SHIFTR);
          break;
        }
        break;
      case '=' :
        q->cur++;
        PARSE(GRN_EXPR_TOKEN_GREATER_EQUAL);
        break;
      default :
        PARSE(GRN_EXPR_TOKEN_GREATER);
        break;
      }
      break;
    case '<' :
      q->cur++;
      switch (*q->cur) {
      case '<' :
        q->cur++;
        switch (*q->cur) {
        case '=' :
          if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
            q->cur++;
            PARSE(GRN_EXPR_TOKEN_SHIFTL_ASSIGN);
          } else {
            ERR(GRN_UPDATE_NOT_ALLOWED,
                "'<<=' is not allowed (%.*s)", q->str_end - q->str, q->str);
          }
          break;
        default :
          PARSE(GRN_EXPR_TOKEN_SHIFTL);
          break;
        }
        break;
      case '=' :
        q->cur++;
        PARSE(GRN_EXPR_TOKEN_LESS_EQUAL);
        break;
      default :
        PARSE(GRN_EXPR_TOKEN_LESS);
        break;
      }
      break;
    case '=' :
      q->cur++;
      switch (*q->cur) {
      case '=' :
        q->cur++;
        PARSE(GRN_EXPR_TOKEN_EQUAL);
        break;
      default :
        if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
          PARSE(GRN_EXPR_TOKEN_ASSIGN);
        } else {
          ERR(GRN_UPDATE_NOT_ALLOWED,
              "'=' is not allowed (%.*s)", q->str_end - q->str, q->str);
        }
        break;
      }
      break;
    case '0' : case '1' : case '2' : case '3' : case '4' :
    case '5' : case '6' : case '7' : case '8' : case '9' :
      {
        const char *rest;
        int64_t int64 = grn_atoll(q->cur, q->str_end, &rest);
        // checks to see grn_atoll was appropriate
        // (NOTE: *q->cur begins with a digit. Thus, grn_atoll parses at leaset
        //        one char.)
        if (q->str_end != rest &&
            (*rest == '.' || *rest == 'e' || *rest == 'E' ||
             (*rest >= '0' && *rest <= '9'))) {
          char *rest_float;
          double d = strtod(q->cur, &rest_float);
          grn_obj floatbuf;
          GRN_FLOAT_INIT(&floatbuf, 0);
          GRN_FLOAT_SET(ctx, &floatbuf, d);
          grn_expr_append_const(ctx, q->e, &floatbuf, GRN_OP_PUSH, 1);
          rest = rest_float;
        } else {
          const char *rest64 = rest;
          unsigned int uint32 = grn_atoui(q->cur, q->str_end, &rest);
          // checks to see grn_atoi failed (see above NOTE)
          if (q->str_end != rest && *rest >= '0' && *rest <= '9') {
            grn_obj int64buf;
            GRN_INT64_INIT(&int64buf, 0);
            GRN_INT64_SET(ctx, &int64buf, int64);
            grn_expr_append_const(ctx, q->e, &int64buf, GRN_OP_PUSH, 1);
            rest = rest64;
          } else {
            grn_obj uint32buf;
            GRN_UINT32_INIT(&uint32buf, 0);
            GRN_UINT32_SET(ctx, &uint32buf, uint32);
            grn_expr_append_const(ctx, q->e, &uint32buf, GRN_OP_PUSH, 1);
          }
        }
        q->cur = rest;
        PARSE(GRN_EXPR_TOKEN_DECIMAL);
      }
      break;
    default :
      if ((rc = get_identifier(ctx, q))) { goto exit; }
      break;
    }
    if (ctx->rc) { rc = ctx->rc; break; }
  }
exit :
  PARSE(0);
  return rc;
}

grn_rc
grn_expr_parse(grn_ctx *ctx, grn_obj *expr,
               const char *str, unsigned int str_size,
               grn_obj *default_column, grn_operator default_mode,
               grn_operator default_op, grn_expr_flags flags)
{
  efs_info efsi;
  if (grn_expr_parser_open(ctx)) { return ctx->rc; }
  GRN_API_ENTER;
  efsi.ctx = ctx;
  efsi.str = str;
  if ((efsi.v = grn_expr_get_var_by_offset(ctx, expr, 0)) &&
      (efsi.table = grn_ctx_at(ctx, efsi.v->header.domain))) {
    GRN_TEXT_INIT(&efsi.buf, 0);
    GRN_UINT32_INIT(&efsi.op_stack, GRN_OBJ_VECTOR);
    GRN_UINT32_INIT(&efsi.mode_stack, GRN_OBJ_VECTOR);
    GRN_PTR_INIT(&efsi.column_stack, GRN_OBJ_VECTOR, GRN_ID_NIL);
    GRN_PTR_INIT(&efsi.token_stack, GRN_OBJ_VECTOR, GRN_ID_NIL);
    efsi.e = expr;
    efsi.str = str;
    efsi.cur = str;
    efsi.str_end = str + str_size;
    efsi.default_column = default_column;
    GRN_PTR_PUT(ctx, &efsi.column_stack, default_column);
    GRN_INT32_PUT(ctx, &efsi.op_stack, default_op);
    GRN_INT32_PUT(ctx, &efsi.mode_stack, default_mode);
    efsi.default_flags = efsi.flags = flags;
    efsi.escalation_threshold = GRN_DEFAULT_MATCH_ESCALATION_THRESHOLD;
    efsi.escalation_decaystep = DEFAULT_DECAYSTEP;
    efsi.weight_offset = 0;
    efsi.opt.weight_vector = NULL;
    efsi.weight_set = NULL;

    if (flags & GRN_EXPR_SYNTAX_SCRIPT) {
      parse_script(ctx, &efsi);
    } else {
      parse_query(ctx, &efsi);
    }

    /*
        grn_obj strbuf;
        GRN_TEXT_INIT(&strbuf, 0);
        grn_expr_inspect(ctx, &strbuf, expr);
        GRN_TEXT_PUTC(ctx, &strbuf, '\0');
        GRN_LOG(ctx, GRN_LOG_NOTICE, "query=(%s)", GRN_TEXT_VALUE(&strbuf));
        GRN_OBJ_FIN(ctx, &strbuf);
    */

    /*
    efsi.opt.vector_size = DEFAULT_WEIGHT_VECTOR_SIZE;
    efsi.opt.func = efsi.weight_set ? section_weight_cb : NULL;
    efsi.opt.func_arg = efsi.weight_set;
    efsi.snip_conds = NULL;
    */
    GRN_OBJ_FIN(ctx, &efsi.op_stack);
    GRN_OBJ_FIN(ctx, &efsi.mode_stack);
    GRN_OBJ_FIN(ctx, &efsi.column_stack);
    GRN_OBJ_FIN(ctx, &efsi.token_stack);
    GRN_OBJ_FIN(ctx, &efsi.buf);
  } else {
    ERR(GRN_INVALID_ARGUMENT, "variable is not defined correctly");
  }
  GRN_API_RETURN(ctx->rc);
}

grn_rc
grn_expr_parser_close(grn_ctx *ctx)
{
  if (ctx->impl->parser) {
    yyParser *pParser = (yyParser*)ctx->impl->parser;
    while (pParser->yyidx >= 0) yy_pop_parser_stack(pParser);
#if YYSTACKDEPTH<=0
    free(pParser->yystack);
#endif
    GRN_FREE(pParser);
    ctx->impl->parser = NULL;
  }
  return ctx->rc;
}

grn_snip *
grn_expr_snip(grn_ctx *ctx, grn_obj *expr, int flags,
              unsigned int width, unsigned int max_results,
              unsigned int n_tags,
              const char **opentags, unsigned int *opentag_lens,
              const char **closetags, unsigned int *closetag_lens,
              grn_snip_mapping *mapping)
{
  int i, n;
  scan_info **sis, *si;
  grn_snip *res = NULL;
  GRN_API_ENTER;
  if ((sis = scan_info_build(ctx, expr, &n, GRN_OP_OR, 0))) {
    if ((res = grn_snip_open(ctx, flags, width, max_results,
                           NULL, 0, NULL, 0, mapping))) {
      int butp = 0, nparens = 0, npbut = 0;
      grn_obj but_stack;
      grn_obj snip_stack;
      GRN_UINT32_INIT(&but_stack, GRN_OBJ_VECTOR);
      GRN_PTR_INIT(&snip_stack, GRN_OBJ_VECTOR, GRN_ID_NIL);
      for (i = n; i--;) {
        si = sis[i];
        if (si->flags & SCAN_POP) {
          nparens++;
          if (si->logical_op == GRN_OP_BUT) {
            GRN_UINT32_PUT(ctx, &but_stack, npbut);
            npbut = nparens;
            butp = 1 - butp;
          }
        } else {
          if (si->op == GRN_OP_MATCH && si->query) {
            if (butp == (si->logical_op == GRN_OP_BUT)) {
              GRN_PTR_PUT(ctx, &snip_stack, si->query);
            }
          }
          if (si->flags & SCAN_PUSH) {
            if (nparens == npbut) {
              butp = 1 - butp;
              GRN_UINT32_POP(&but_stack, npbut);
            }
            nparens--;
          }
        }
      }
      if (n_tags) {
        for (i = 0;; i = (i + 1) % n_tags) {
          grn_obj *q;
          GRN_PTR_POP(&snip_stack, q);
          if (!q) { break; }
          grn_snip_add_cond(ctx, res, GRN_TEXT_VALUE(q), GRN_TEXT_LEN(q),
                            opentags[i], opentag_lens[i], closetags[i], closetag_lens[i]);
        }
      } else {
        for (;;) {
          grn_obj *q;
          GRN_PTR_POP(&snip_stack, q);
          if (!q) { break; }
          grn_snip_add_cond(ctx, res, GRN_TEXT_VALUE(q), GRN_TEXT_LEN(q),
                            NULL, 0, NULL, 0);
        }
      }
      GRN_OBJ_FIN(ctx, &but_stack);
      GRN_OBJ_FIN(ctx, &snip_stack);
    }
    for (i = n; i--;) { SI_FREE(sis[i]); }
    GRN_FREE(sis);
  }
  GRN_API_RETURN(res);
}
