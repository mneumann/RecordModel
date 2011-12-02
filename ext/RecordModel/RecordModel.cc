#include "../../include/RecordModel.h"
#include <assert.h> // assert
#include "ruby.h"

/*
 * C extension
 */

static VALUE cRecordModel;
static VALUE cRecordModelInstance;

static void RecordModel__free(void *ptr)
{
  RecordModel *m = (RecordModel*) ptr;
  if (m)
  {
    delete(m);
  }
}

static VALUE RecordModel__allocate(VALUE klass)
{
  VALUE obj;
  obj = Data_Wrap_Struct(klass, NULL, RecordModel__free, new RecordModel());
  return obj;
}

static RecordModel& RecordModel__get(VALUE self) {
  RecordModel *ptr;
  Data_Get_Struct(self, RecordModel, ptr);
  return *ptr;
}

static VALUE RecordModel_initialize(VALUE self, VALUE keys, VALUE values)
{
  Check_Type(keys, T_ARRAY);
  Check_Type(values, T_ARRAY);

  RecordModel &m = RecordModel__get(self);

  assert(m.items == NULL);
  assert(m.keys == NULL);
  assert(m.values == NULL);

  m.items = new uint32_t[RARRAY_LEN(keys) + RARRAY_LEN(values) + 2];
  m.keys = &m.items[0];
  m.values = &m.items[RARRAY_LEN(keys) + 1];

  uint32_t offset = 0;

  int i;

  for (i = 0; i < RARRAY_LEN(keys); ++i)
  {
    uint32_t desc = NUM2UINT(RARRAY_PTR(keys)[i]);
    assert(RecordModelOffset(desc) == 0 || RecordModelOffset(desc) == offset);

    m.keys[i] = (offset << 16) | RecordModelType(desc);
    offset += RecordModelTypeSize(desc);
  }
  m.keys[i] = 0x00;
  m.keysize = offset;

  for (i = 0; i < RARRAY_LEN(values); ++i)
  {
    uint32_t desc = NUM2UINT(RARRAY_PTR(values)[i]);
    assert(RecordModelOffset(desc) == 0 || RecordModelOffset(desc) == offset);

    m.values[i] = (offset << 16) | RecordModelType(desc);
    offset += RecordModelTypeSize(desc);
  }
  m.values[i] = 0x00;

  m.size = offset;

  return Qnil;
}

static VALUE RecordModel_size(VALUE self)
{
  RecordModel &m = RecordModel__get(self);
  return UINT2NUM(m.size);
}

static void RecordModelInstance__free(void *ptr)
{
  free(ptr);
}

static VALUE RecordModelInstance__allocate(VALUE klass)
{
  VALUE model = rb_cvar_get(klass, rb_intern("@@__model"));
  RecordModel &m = RecordModel__get(model);

  VALUE obj;
  // XXX: gc!
  obj = Data_Wrap_Struct(klass, NULL, RecordModelInstance__free, m.create_instance());
  return obj;
}

static RecordModelInstance& RecordModelInstance__get(VALUE self) {
  RecordModelInstance *ptr;
  Data_Get_Struct(self, RecordModelInstance, ptr);
  return *ptr;
}

static VALUE RecordModelInstance__model(VALUE klass)
{
  return rb_cvar_get(klass, rb_intern("@@__model"));
}

static VALUE RecordModelInstance_get(VALUE self, VALUE _desc)
{
  RecordModelInstance *mi;
  Data_Get_Struct(self, RecordModelInstance, mi);
  const RecordModel &model = *mi->model;

  uint32_t desc = NUM2UINT(_desc);

  assert(RecordModelOffset(desc) + RecordModelTypeSize(desc) <= model.size);

  const char *ptr = model.ptr_to_field(mi, desc);

  if (RecordModelType(desc) == RMT_UINT64)
  {
    return ULONG2NUM( *((uint64_t*)ptr) );
  }
  else if (RecordModelType(desc) == RMT_UINT32)
  {
    return UINT2NUM( *((uint32_t*)ptr) );
  }
  else if (RecordModelType(desc) == RMT_DOUBLE)
  {
    return rb_float_new( *((double*)ptr) );
  }
  else
  {
    // XXX: raise
    return Qnil;
  }
}

static VALUE RecordModelInstance_set(VALUE self, VALUE _desc, VALUE _val)
{
  RecordModelInstance *mi;
  Data_Get_Struct(self, RecordModelInstance, mi);
  const RecordModel &model = *mi->model;

  uint32_t desc = NUM2UINT(_desc);

  assert(RecordModelOffset(desc) + RecordModelTypeSize(desc) <= model.size);

  const char *ptr = model.ptr_to_field(mi, desc);

  if (RecordModelType(desc) == RMT_UINT64)
  {
    *((uint64_t*)ptr) = (uint64_t)NUM2ULONG(_val);
  }
  else if (RecordModelType(desc) == RMT_UINT32)
  {
    *((uint32_t*)ptr) = (uint32_t)NUM2UINT(_val);
  }
  else if (RecordModelType(desc) == RMT_DOUBLE)
  {
    *((double*)ptr) = (double)NUM2DBL(_val);
  }
  else
  {
    // XXX: raise
    return Qnil;
  }

  return Qnil;
}

static VALUE RecordModelInstance_zero(VALUE self)
{
  RecordModelInstance *mi;
  Data_Get_Struct(self, RecordModelInstance, mi);
  mi->model->zero_instance(mi);
  return self;
}

static VALUE RecordModelInstance_dup(VALUE self)
{
  VALUE obj = RecordModelInstance__allocate(rb_obj_class(self));

  RecordModelInstance *oldi;
  RecordModelInstance *newi;

  Data_Get_Struct(self, RecordModelInstance, oldi);
  Data_Get_Struct(obj, RecordModelInstance, newi);

  oldi->model->copy_instance(newi, oldi);

  return obj;
}

static VALUE RecordModelInstance_sum_values(VALUE self, VALUE other)
{
  RecordModelInstance *a;
  RecordModelInstance *b;
  Data_Get_Struct(self, RecordModelInstance, a);
  Data_Get_Struct(other, RecordModelInstance, b);

  a->model->sum_instance(a, b);
  return self;
}

static VALUE RecordModel_to_class(VALUE self)
{
  VALUE klass = rb_class_new(cRecordModelInstance);
  rb_cvar_set(klass, rb_intern("@@__model"), self);
  rb_define_alloc_func(klass, RecordModelInstance__allocate);
  rb_define_singleton_method(klass, "model", (VALUE (*)(...)) RecordModelInstance__model, 0);
  rb_define_method(klass, "[]", (VALUE (*)(...)) RecordModelInstance_get, 1);
  rb_define_method(klass, "[]=", (VALUE (*)(...)) RecordModelInstance_set, 2);
  rb_define_method(klass, "zero!", (VALUE (*)(...)) RecordModelInstance_zero, 0);
  rb_define_method(klass, "dup", (VALUE (*)(...)) RecordModelInstance_dup, 0);
  rb_define_method(klass, "sum_values!", (VALUE (*)(...)) RecordModelInstance_sum_values, 1);

  return klass;
}

extern "C"
void Init_RecordModelExt()
{
  cRecordModel = rb_define_class("RecordModel", rb_cObject);
  rb_define_alloc_func(cRecordModel, RecordModel__allocate);
  rb_define_method(cRecordModel, "initialize", (VALUE (*)(...)) RecordModel_initialize, 2);
  rb_define_method(cRecordModel, "to_class", (VALUE (*)(...)) RecordModel_to_class, 0);
  rb_define_method(cRecordModel, "size", (VALUE (*)(...)) RecordModel_size, 0);

  cRecordModelInstance = rb_define_class("RecordModelInstance", rb_cObject);
}
