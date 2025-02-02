#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include "Python.h"
#include "datetime.h"
#include "structmember.h"

#include "common.h"
#include "ryu.h"
#include "atof.h"

#ifdef __GNUC__
#define ms_popcount(i) __builtin_popcount(i)
#else
static int
ms_popcount(uint32_t i) {
     i = i - ((i >> 1) & 0x55555555);        // add pairs of bits
     i = (i & 0x33333333) + ((i >> 2) & 0x33333333);  // quads
     i = (i + (i >> 4)) & 0x0F0F0F0F;        // groups of 8
     return (i * 0x01010101) >> 24;          // horizontal sum of bytes
}
#endif

#if PY_VERSION_HEX < 0x03090000
#define IS_TRACKED(o) _PyObject_GC_IS_TRACKED(o)
#define CALL_ONE_ARG(f, a) PyObject_CallFunctionObjArgs(f, a, NULL)
#define CALL_METHOD_ONE_ARG(o, n, a) PyObject_CallMethodObjArgs(o, n, a, NULL)
#define SET_SIZE(obj, size) (((PyVarObject *)obj)->ob_size = size)
#else
#define IS_TRACKED(o)  PyObject_GC_IsTracked(o)
#define CALL_ONE_ARG(f, a) PyObject_CallOneArg(f, a)
#define CALL_METHOD_ONE_ARG(o, n, a) PyObject_CallMethodOneArg(o, n, a)
#define SET_SIZE(obj, size) Py_SET_SIZE(obj, size)
#endif

#define is_digit(c) (c >= '0' && c <= '9')

/* Easy access to NoneType object */
#define NONE_TYPE ((PyObject *)(Py_TYPE(Py_None)))

/* Is this object something that is/could be GC tracked? True if
 * - the value supports GC
 * - the value isn't a tuple or the object is tracked (skip tracked checks for non-tuples)
 */
#define OBJ_IS_GC(x) \
    (PyType_IS_GC(Py_TYPE(x)) && \
     (!PyTuple_CheckExact(x) || IS_TRACKED(x)))

/* Fast shrink of bytes & bytearray objects. This doesn't do any memory
 * allocations, it just shrinks the size of the view presented to Python. Since
 * outputs of `encode` should be short lived (immediately written to a
 * socket/file then dropped), this shouldn't result in increased application
 * memory usage. */
# define FAST_BYTES_SHRINK(obj, size) \
    do { \
    SET_SIZE(obj, size); \
    PyBytes_AS_STRING(obj)[size] = '\0'; \
    } while (0);
# define FAST_BYTEARRAY_SHRINK(obj, size) \
    do { \
    SET_SIZE(obj, size); \
    PyByteArray_AS_STRING(obj)[size] = '\0'; \
    } while (0);

/* XXX: Optimized `PyUnicode_AsUTF8AndSize`, fastpath for ascii strings. */
static inline const char *
unicode_str_and_size(PyObject *str, Py_ssize_t *size) {
    if (PyUnicode_IS_COMPACT_ASCII(str)) {
        *size = ((PyASCIIObject *)str)->length;
        return (char *)(((PyASCIIObject *)str) + 1);
    }
    return PyUnicode_AsUTF8AndSize(str, size);
}

/*************************************************************************
 * Datetime utilities                                                    *
 *************************************************************************/

#define MS_HAS_TZINFO(o)  (((_PyDateTime_BaseTZInfo *)(o))->hastzinfo)
#if PY_VERSION_HEX < 0x030a00f0
#define MS_GET_TZINFO(o)      (MS_HAS_TZINFO(o) ? \
    ((PyDateTime_DateTime *)(o))->tzinfo : Py_None)
#else
#define MS_GET_TZINFO(o) PyDateTime_DATE_GET_TZINFO(o)
#endif

static bool
is_leap_year(int year)
{
    unsigned int y = (unsigned int)year;
    return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
}

static int
days_in_month(int year, int month) {
    static const uint8_t ndays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year))
        return 29;
    else
        return ndays[month - 1];
}

static inline int
divmod(int x, int y, int *r) {
    int quo = x / y;
    *r = x - quo * y;
    if (*r < 0) {
        --quo;
        *r += y;
    }
    return quo;
}

/* Convert a *valid* datetime with a tz offset (in minutes) to UTC time.
 * Returns -1 on error, but no error indicator set */
static int
datetime_apply_tz_offset(
    int *year, int *month, int *day, int *hour,
    int *minute, int tz_offset
) {
    *minute -= tz_offset;
    if (*minute < 0 || *minute >= 60) {
        *hour += divmod(*minute, 60, minute);
    }
    if (*hour < 0 || *hour >= 24) {
        *day += divmod(*hour, 24, hour);
    }
    /* days can only be off by +/- day */
    if (*day == 0) {
        --*month;
        if (*month > 0)
            *day = days_in_month(*year, *month);
        else {
            --*year;
            *month = 12;
            *day = 31;
        }
    }
    else if (*day == days_in_month(*year, *month) + 1) {
        ++*month;
        *day = 1;
        if (*month > 12) {
            *month = 1;
            ++*year;
        }
    }
    if (1 <= *year && *year <= 9999)
        return 0;
    return -1;
}

/* Days since 0001-01-01, the min value for python's datetime objects */
static int
days_since_min_datetime(int year, int month, int day)
{
    int out = day;
    static const int _days_before_month[] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };
    out += _days_before_month[month - 1];
    if (month > 2 && is_leap_year(year)) out++;

    year--; /* makes math easier */
    out += year*365 + year/4 - year/100 + year/400;

    return out;
}

static void
datetime_to_epoch(PyObject *obj, int64_t *seconds, int32_t *nanoseconds) {
    int64_t d = days_since_min_datetime(
        PyDateTime_GET_YEAR(obj),
        PyDateTime_GET_MONTH(obj),
        PyDateTime_GET_DAY(obj)
    ) - 719163;  /* days_since_min_datetime(1970, 1, 1) */
    int64_t s = (
        PyDateTime_DATE_GET_HOUR(obj) * 3600
        + PyDateTime_DATE_GET_MINUTE(obj) * 60
        + PyDateTime_DATE_GET_SECOND(obj)
    );
    int64_t us = PyDateTime_DATE_GET_MICROSECOND(obj);

    *seconds = 86400 * d + s;
    *nanoseconds = us * 1000;
}

/* Python datetimes bounded between (inclusive)
 * [0001-01-01T00:00:00.000000, 9999-12-31T23:59:59.999999] UTC */
#define MS_EPOCH_SECS_MAX 253402300800
#define MS_EPOCH_SECS_MIN -62135596800
#define MS_DAYS_PER_400Y (365*400 + 97)
#define MS_DAYS_PER_100Y (365*100 + 24)
#define MS_DAYS_PER_4Y   (365*4 + 1)

/* Epoch -> datetime conversion borrowed and modified from the implementation
 * in musl, found at
 * http://git.musl-libc.org/cgit/musl/tree/src/time/__secs_to_tm.c. musl is
 * copyright Rich Felker et. al, and is licensed under the standard MIT
 * license.  */
static PyObject *
datetime_from_epoch(int64_t epoch_secs, uint32_t epoch_nanos) {
    int64_t days, secs, years;
    int months, remdays, remsecs, remyears;
    int qc_cycles, c_cycles, q_cycles;
    /* Start in Mar not Jan, so leap day is on end */
    static const char days_in_month[] = {31, 30, 31, 30, 31, 31, 30, 31, 30, 31, 31, 29};

    /* Offset to 2000-03-01, a mod 400 year, immediately after feb 29 */
    secs = epoch_secs - (946684800LL + 86400 * (31 + 29));
    days = secs / 86400;
    remsecs = secs % 86400;
    if (remsecs < 0) {
        remsecs += 86400;
        days--;
    }

    qc_cycles = days / MS_DAYS_PER_400Y;
    remdays = days % MS_DAYS_PER_400Y;
    if (remdays < 0) {
        remdays += MS_DAYS_PER_400Y;
        qc_cycles--;
    }

    c_cycles = remdays / MS_DAYS_PER_100Y;
    if (c_cycles == 4) c_cycles--;
    remdays -= c_cycles * MS_DAYS_PER_100Y;

    q_cycles = remdays / MS_DAYS_PER_4Y;
    if (q_cycles == 25) q_cycles--;
    remdays -= q_cycles * MS_DAYS_PER_4Y;

    remyears = remdays / 365;
    if (remyears == 4) remyears--;
    remdays -= remyears * 365;

    years = remyears + 4*q_cycles + 100*c_cycles + 400LL*qc_cycles;

    for (months = 0; days_in_month[months] <= remdays; months++)
        remdays -= days_in_month[months];

    if (months >= 10) {
        months -= 12;
        years++;
    }

    return PyDateTimeAPI->DateTime_FromDateAndTime(
        years + 2000,
        months + 3,
        remdays + 1,
        remsecs / 3600,
        remsecs / 60 % 60,
        remsecs % 60,
        epoch_nanos / 1000,
        PyDateTime_TimeZone_UTC,
        PyDateTimeAPI->DateTimeType
    );
}


/*************************************************************************
 * Endian handling macros                                                *
 *************************************************************************/

#define _msgspec_store16(to, x) do { \
    ((uint8_t*)to)[0] = (uint8_t)((x >> 8) & 0xff); \
    ((uint8_t*)to)[1] = (uint8_t)(x & 0xff); \
} while (0);

#define _msgspec_store32(to, x) do { \
    ((uint8_t*)to)[0] = (uint8_t)((x >> 24) & 0xff); \
    ((uint8_t*)to)[1] = (uint8_t)((x >> 16) & 0xff); \
    ((uint8_t*)to)[2] = (uint8_t)((x >> 8) & 0xff); \
    ((uint8_t*)to)[3] = (uint8_t)(x & 0xff); \
} while (0);

#define _msgspec_store64(to, x) do { \
    ((uint8_t*)to)[0] = (uint8_t)((x >> 56) & 0xff); \
    ((uint8_t*)to)[1] = (uint8_t)((x >> 48) & 0xff); \
    ((uint8_t*)to)[2] = (uint8_t)((x >> 40) & 0xff); \
    ((uint8_t*)to)[3] = (uint8_t)((x >> 32) & 0xff); \
    ((uint8_t*)to)[4] = (uint8_t)((x >> 24) & 0xff); \
    ((uint8_t*)to)[5] = (uint8_t)((x >> 16) & 0xff); \
    ((uint8_t*)to)[6] = (uint8_t)((x >> 8) & 0xff); \
    ((uint8_t*)to)[7] = (uint8_t)(x & 0xff); \
} while (0);

#define _msgspec_load16(cast, from) ((cast)( \
    (((uint16_t)((uint8_t*)from)[0]) << 8) | \
    (((uint16_t)((uint8_t*)from)[1])     ) ))

#define _msgspec_load32(cast, from) ((cast)( \
    (((uint32_t)((uint8_t*)from)[0]) << 24) | \
    (((uint32_t)((uint8_t*)from)[1]) << 16) | \
    (((uint32_t)((uint8_t*)from)[2]) <<  8) | \
    (((uint32_t)((uint8_t*)from)[3])      ) ))

#define _msgspec_load64(cast, from) ((cast)( \
    (((uint64_t)((uint8_t*)from)[0]) << 56) | \
    (((uint64_t)((uint8_t*)from)[1]) << 48) | \
    (((uint64_t)((uint8_t*)from)[2]) << 40) | \
    (((uint64_t)((uint8_t*)from)[3]) << 32) | \
    (((uint64_t)((uint8_t*)from)[4]) << 24) | \
    (((uint64_t)((uint8_t*)from)[5]) << 16) | \
    (((uint64_t)((uint8_t*)from)[6]) << 8)  | \
    (((uint64_t)((uint8_t*)from)[7])     )  ))

/*************************************************************************
 * Module level state                                                    *
 *************************************************************************/

/* State of the msgspec module */
typedef struct {
    PyObject *MsgspecError;
    PyObject *EncodeError;
    PyObject *DecodeError;
    PyObject *StructType;
    PyTypeObject *EnumType;
    PyObject *str__name_;
    PyObject *str__value2member_map_;
    PyObject *str_name;
    PyObject *str_type;
    PyObject *str_enc_hook;
    PyObject *str_dec_hook;
    PyObject *str_ext_hook;
    PyObject *str_utcoffset;
    PyObject *str___origin__;
    PyObject *str___args__;
    PyObject *typing_list;
    PyObject *typing_set;
    PyObject *typing_tuple;
    PyObject *typing_dict;
    PyObject *typing_union;
    PyObject *typing_any;
    PyObject *get_type_hints;
    PyObject *astimezone;
} MsgspecState;

/* Forward declaration of the msgspec module definition. */
static struct PyModuleDef msgspecmodule;

/* Given a module object, get its per-module state. */
static MsgspecState *
msgspec_get_state(PyObject *module)
{
    return (MsgspecState *)PyModule_GetState(module);
}

/* Find the module instance imported in the currently running sub-interpreter
   and get its state. */
static MsgspecState *
msgspec_get_global_state(void)
{
    return msgspec_get_state(PyState_FindModule(&msgspecmodule));
}

static int
ms_err_truncated(void)
{
    PyErr_SetString(msgspec_get_global_state()->DecodeError, "Input data was truncated");
    return -1;
}

/*************************************************************************
 * Utilities                                                             *
 *************************************************************************/

static PyObject*
find_keyword(PyObject *kwnames, PyObject *const *kwstack, PyObject *key)
{
    Py_ssize_t i, nkwargs;

    nkwargs = PyTuple_GET_SIZE(kwnames);
    for (i = 0; i < nkwargs; i++) {
        PyObject *kwname = PyTuple_GET_ITEM(kwnames, i);

        /* kwname == key will normally find a match in since keyword keys
           should be interned strings; if not retry below in a new loop. */
        if (kwname == key) {
            return kwstack[i];
        }
    }

    for (i = 0; i < nkwargs; i++) {
        PyObject *kwname = PyTuple_GET_ITEM(kwnames, i);
        assert(PyUnicode_Check(kwname));
        if (_PyUnicode_EQ(kwname, key)) {
            return kwstack[i];
        }
    }
    return NULL;
}

static int
check_positional_nargs(Py_ssize_t nargs, Py_ssize_t min, Py_ssize_t max) {
    if (nargs > max) {
        PyErr_SetString(
            PyExc_TypeError,
            "Extra positional arguments provided"
        );
        return 0;
    }
    else if (nargs < min) {
        PyErr_Format(
            PyExc_TypeError,
            "Missing %zd required arguments",
            min - nargs
        );
        return 0;
    }
    return 1;
}

/* A utility incrementally building strings */
typedef struct strbuilder {
    char *sep;
    Py_ssize_t sep_size;
    char *buffer;
    Py_ssize_t size;  /* How many bytes have been written */
    Py_ssize_t capacity;  /* How many bytes can be written */
} strbuilder;

static bool strbuilder_extend(strbuilder *self, const char *buf, Py_ssize_t nbytes) {
    /* Optimization - store first write by reference.
     *
     * This is only possible because the appended buffers are assumed to be
     * string constants (and thus their lifetime will exist after this call */
    if (self->buffer == NULL) {
        self->buffer = (char *)buf;
        self->size = nbytes;
        return true;
    }

    Py_ssize_t required = self->capacity + nbytes + self->sep_size;

    if (!self->capacity) {
        char *temp = self->buffer;
        self->capacity = Py_MAX(16, required);
        self->buffer = PyMem_Malloc(self->capacity);
        if (self->buffer == NULL) return false;
        memcpy(self->buffer, temp, self->size);
    }
    else if (required > self->capacity) {
        self->capacity = required * 1.5;
        char *new_buf = PyMem_Realloc(self->buffer, self->capacity);
        if (new_buf == NULL) {
            PyMem_Free(self->buffer);
            self->buffer = NULL;
            return false;
        }
        self->buffer = new_buf;
    }
    if (self->sep_size) {
        memcpy(self->buffer + self->size, self->sep, self->sep_size);
        self->size += self->sep_size;
    }
    memcpy(self->buffer + self->size, buf, nbytes);
    self->size += nbytes;
    return true;
}

static bool
strbuilder_extend_unicode(strbuilder *self, PyObject *obj) {
    Py_ssize_t size;
    const char* buf = unicode_str_and_size(obj, &size);
    return strbuilder_extend(self, buf, size);
}

static void
strbuilder_reset(strbuilder *self) {
    if (self->capacity != 0 && self->buffer != NULL) {
        PyMem_Free(self->buffer);
    }
    self->buffer = NULL;
    self->size = 0;
    self->capacity = 0;
}

static PyObject *
strbuilder_build(strbuilder *self) {
    PyObject *out = PyUnicode_FromStringAndSize(self->buffer, self->size);
    strbuilder_reset(self);
    return out;
}

/*************************************************************************
 * Struct, PathNode, and TypeNode Types                                  *
 *************************************************************************/

#define MS_TYPE_ANY                 (1u << 0)
#define MS_TYPE_NONE                (1u << 1)
#define MS_TYPE_BOOL                (1u << 2)
#define MS_TYPE_INT                 (1u << 3)
#define MS_TYPE_FLOAT               (1u << 4)
#define MS_TYPE_STR                 (1u << 5)
#define MS_TYPE_BYTES               (1u << 6)
#define MS_TYPE_BYTEARRAY           (1u << 7)
#define MS_TYPE_DATETIME            (1u << 8)
#define MS_TYPE_EXT                 (1u << 9)
#define MS_TYPE_STRUCT              (1u << 10)
#define MS_TYPE_ENUM                (1u << 11)
#define MS_TYPE_INTENUM             (1u << 12)
#define MS_TYPE_CUSTOM              (1u << 13)
#define MS_TYPE_CUSTOM_GENERIC      (1u << 14)
#define MS_TYPE_DICT                (1u << 15)
#define MS_TYPE_LIST                (1u << 16)
#define MS_TYPE_SET                 (1u << 17)
#define MS_TYPE_VARTUPLE            (1u << 18)
#define MS_TYPE_FIXTUPLE            (1u << 19)

typedef struct TypeNode {
    uint32_t types;
} TypeNode;

typedef struct TypeNodeExtra {
    TypeNode type;
    Py_ssize_t fixtuple_size;
    void* extra[];
} TypeNodeExtra;

typedef struct {
    PyHeapTypeObject base;
    PyObject *struct_fields;
    PyObject *struct_defaults;
    Py_ssize_t *struct_offsets;
    TypeNode **struct_types;
    bool json_compatible;
    bool traversing;
    char frozen;
    char asarray;
} StructMetaObject;

static PyTypeObject StructMetaType;
static PyTypeObject Ext_Type;
static int StructMeta_prep_types(PyObject *self, bool err_not_json, bool *json_compatible);

#define StructMeta_GET_FIELDS(s) (((StructMetaObject *)(s))->struct_fields);
#define StructMeta_GET_NFIELDS(s) (PyTuple_GET_SIZE((((StructMetaObject *)(s))->struct_fields)));
#define StructMeta_GET_DEFAULTS(s) (((StructMetaObject *)(s))->struct_defaults);
#define StructMeta_GET_OFFSETS(s) (((StructMetaObject *)(s))->struct_offsets);

#define OPT_UNSET -1
#define OPT_FALSE 0
#define OPT_TRUE 1
#define STRUCT_MERGE_OPTIONS(opt1, opt2) (((opt2) != OPT_UNSET) ? (opt2) : (opt1))

static Py_ssize_t
TypeNode_get_size(TypeNode *type, Py_ssize_t *n_typenode) {
    Py_ssize_t n_obj = 0, n_type = 0;
    /* Custom types cannot share a union with anything except `None` */
    if (type->types & (MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC)) {
        n_obj = 1;
    }
    else if (!(type->types & MS_TYPE_ANY)) {
        n_obj = ms_popcount(
            type->types & (MS_TYPE_STRUCT | MS_TYPE_INTENUM | MS_TYPE_ENUM)
        );

        if (type->types & MS_TYPE_DICT) n_type += 2;
        /* Only one array generic is allowed in a union */
        if (type->types & (MS_TYPE_LIST | MS_TYPE_SET | MS_TYPE_VARTUPLE)) n_type++;
        if (type->types & MS_TYPE_FIXTUPLE) n_type += ((TypeNodeExtra *)type)->fixtuple_size;
    }
    *n_typenode = n_type;
    return n_obj;
}

static MS_INLINE StructMetaObject *
TypeNode_get_struct(TypeNode *type) {
    /* Struct types are always first */
    return ((TypeNodeExtra *)type)->extra[0];
}

static MS_INLINE PyObject *
TypeNode_get_custom(TypeNode *type) {
    /* Custom types can't be mixed with anything */
    return ((TypeNodeExtra *)type)->extra[0];
}

static MS_INLINE PyObject *
TypeNode_get_intenum(TypeNode *type) {
    Py_ssize_t i = (type->types & MS_TYPE_STRUCT) != 0;
    return ((TypeNodeExtra *)type)->extra[i];
}

static MS_INLINE PyObject *
TypeNode_get_enum(TypeNode *type) {
    Py_ssize_t i = ms_popcount(type->types & (MS_TYPE_STRUCT | MS_TYPE_INTENUM));
    return ((TypeNodeExtra *)type)->extra[i];
}

static MS_INLINE PyObject *
TypeNode_get_dict_obj(TypeNode *type) {
    Py_ssize_t i = ms_popcount(type->types & (MS_TYPE_STRUCT | MS_TYPE_INTENUM | MS_TYPE_ENUM));
    return ((TypeNodeExtra *)type)->extra[i];
}

static MS_INLINE void
TypeNode_get_dict(TypeNode *type, TypeNode **key, TypeNode **val) {
    Py_ssize_t i = ms_popcount(type->types & (MS_TYPE_STRUCT | MS_TYPE_INTENUM | MS_TYPE_ENUM));
    *key = ((TypeNodeExtra *)type)->extra[i];
    *val = ((TypeNodeExtra *)type)->extra[i + 1];
}

static MS_INLINE Py_ssize_t
TypeNode_get_array_offset(TypeNode *type) {
    Py_ssize_t i = ms_popcount(type->types & (MS_TYPE_STRUCT | MS_TYPE_INTENUM | MS_TYPE_ENUM));
    if (type->types & MS_TYPE_DICT) i += 2;
    return i;
}

static MS_INLINE TypeNode *
TypeNode_get_array(TypeNode *type) {
    return ((TypeNodeExtra *)type)->extra[TypeNode_get_array_offset(type)];
}

static void
TypeNode_Free(TypeNode *self) {
    if (self == NULL) return;
    Py_ssize_t n_obj, n_typenode, i;
    n_obj = TypeNode_get_size(self, &n_typenode);
    TypeNodeExtra *tex = (TypeNodeExtra *)self;

    for (i = 0; i < n_obj; i++) {
        PyObject *obj = (PyObject *)(tex->extra[i]);
        Py_XDECREF(obj);
    }
    for (i = n_obj; i < (n_obj + n_typenode); i++) {
        TypeNode *node = (TypeNode *)(tex->extra[i]);
        TypeNode_Free(node);
    }
    PyMem_Free(self);
}

static int
TypeNode_traverse(TypeNode *self, visitproc visit, void *arg) {
    if (self == NULL) return 0;
    Py_ssize_t n_obj, n_typenode, i;
    n_obj = TypeNode_get_size(self, &n_typenode);
    TypeNodeExtra *tex = (TypeNodeExtra *)self;

    for (i = 0; i < n_obj; i++) {
        PyObject *obj = (PyObject *)(tex->extra[i]);
        Py_VISIT(obj);
    }
    for (i = n_obj; i < (n_obj + n_typenode); i++) {
        int out;
        TypeNode *node = (TypeNode *)(tex->extra[i]);
        if ((out = TypeNode_traverse(node, visit, arg)) != 0) return out;
    }
    return 0;
}

static PyObject *
typenode_simple_repr(TypeNode *self) {
    strbuilder builder = {" | ", 3};

    if (self->types & (MS_TYPE_ANY | MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC)) {
        return PyUnicode_FromString("any");
    }

    if (self->types & MS_TYPE_BOOL) {
        if (!strbuilder_extend(&builder, "bool", 4)) return NULL;
    }
    if (self->types & (MS_TYPE_INT | MS_TYPE_INTENUM)) {
        if (!strbuilder_extend(&builder, "int", 3)) return NULL;
    }
    if (self->types & MS_TYPE_FLOAT) {
        if (!strbuilder_extend(&builder, "float", 5)) return NULL;
    }
    if (self->types & (MS_TYPE_STR | MS_TYPE_ENUM)) {
        if (!strbuilder_extend(&builder, "str", 3)) return NULL;
    }
    if (self->types & (MS_TYPE_BYTES | MS_TYPE_BYTEARRAY)) {
        if (!strbuilder_extend(&builder, "bytes", 5)) return NULL;
    }
    if (self->types & MS_TYPE_DATETIME) {
        if (!strbuilder_extend(&builder, "datetime", 8)) return NULL;
    }
    if (self->types & MS_TYPE_EXT) {
        if (!strbuilder_extend(&builder, "ext", 3)) return NULL;
    }
    if (self->types & MS_TYPE_STRUCT) {
        if (TypeNode_get_struct(self)->asarray == OPT_TRUE) {
            if (!strbuilder_extend(&builder, "array", 5)) return NULL;
        }
        else {
            if (!strbuilder_extend(&builder, "object", 6)) return NULL;
        }
    }
    if (self->types & MS_TYPE_DICT) {
        if (!strbuilder_extend(&builder, "object", 6)) return NULL;
    }
    if (self->types & (MS_TYPE_LIST | MS_TYPE_SET | MS_TYPE_VARTUPLE | MS_TYPE_FIXTUPLE)) {
        if (!strbuilder_extend(&builder, "array", 5)) return NULL;
    }
    if (self->types & MS_TYPE_NONE) {
        if (!strbuilder_extend(&builder, "null", 4)) return NULL;
    }

    return strbuilder_build(&builder);
}

typedef struct {
    PyObject *context;
    uint32_t types;
    PyObject *struct_obj;
    PyObject *intenum_obj;
    PyObject *enum_obj;
    PyObject *custom_obj;
    PyObject *array_el_obj;
    PyObject *dict_key_obj;
    PyObject *dict_val_obj;
} TypeNodeCollectState;

static TypeNode * TypeNode_Convert(PyObject *type, bool err_not_json, bool *json_compatible);

static TypeNode *
typenode_from_collect_state(TypeNodeCollectState *state, bool err_not_json, bool *json_compatible) {
    Py_ssize_t e_ind, n_extra = 0, fixtuple_size = 0;
    bool has_fixtuple = false;

    if (state->struct_obj != NULL) n_extra++;
    if (state->intenum_obj != NULL) n_extra++;
    if (state->enum_obj != NULL) n_extra++;
    if (state->custom_obj != NULL) n_extra++;
    if (state->dict_key_obj != NULL) n_extra += 2;
    if (state->array_el_obj != NULL) {
        if (PyTuple_Check(state->array_el_obj)) {
            has_fixtuple = true;
            fixtuple_size = PyTuple_Size(state->array_el_obj);
            n_extra += fixtuple_size;
        }
        else {
            n_extra++;
        }
    }

    if (n_extra == 0) {
        TypeNode *out = (TypeNode *)PyMem_Malloc(sizeof(TypeNode));
        if (out == NULL) {
            PyErr_NoMemory();
            return NULL;
        }
        out->types = state->types;
        return out;
    }

    /* Use calloc so that `out->extra` is initialized, easing cleanup on error */
    TypeNodeExtra *out = (TypeNodeExtra *)PyMem_Calloc(
        1, sizeof(TypeNodeExtra) + n_extra * sizeof(void *)
    );
    if (out == NULL) {
        PyErr_NoMemory();
        return NULL;
    }

    out->type.types = state->types;
    out->fixtuple_size = fixtuple_size;

    /* Populate `extra` fields in order */
    e_ind = 0;
    if (state->struct_obj != NULL) {
        if (StructMeta_prep_types(state->struct_obj, err_not_json, json_compatible) < 0) {
            goto error;
        }
        Py_INCREF(state->struct_obj);
        out->extra[e_ind++] = state->struct_obj;
    }
    if (state->intenum_obj != NULL) {
        Py_INCREF(state->intenum_obj);
        out->extra[e_ind++] = state->intenum_obj;
    }
    if (state->enum_obj != NULL) {
        Py_INCREF(state->enum_obj);
        out->extra[e_ind++] = state->enum_obj;
    }
    if (state->custom_obj != NULL) {
        Py_INCREF(state->custom_obj);
        /* Add `Any` to the type node, so the individual decode functions can
         * check for `Any` alone, and only have to handle custom types in one
         * location  (e.g. `mpack_decode`). */
        out->type.types |= MS_TYPE_ANY;
        out->extra[e_ind++] = state->custom_obj;
    }
    if (state->dict_key_obj != NULL) {
        TypeNode *temp = TypeNode_Convert(state->dict_key_obj, err_not_json, json_compatible);
        if (temp == NULL) goto error;
        out->extra[e_ind++] = temp;
        /* Check that JSON dict keys are strings */
        if (temp->types & ~(MS_TYPE_ANY | MS_TYPE_STR)) {
            if (err_not_json) {
                PyErr_Format(
                    PyExc_TypeError,
                    "JSON doesn't support dicts with non-string keys "
                    "- type `%R` is not supported",
                    state->context
                );
                goto error;  /* temp already added to `out`, gets freed below */
            }
            if (json_compatible != NULL)
                *json_compatible = false;
        }
        temp = TypeNode_Convert(state->dict_val_obj, err_not_json, json_compatible);
        if (temp == NULL) goto error;
        out->extra[e_ind++] = temp;
    }
    if (state->array_el_obj != NULL) {
        if (has_fixtuple) {
            for (Py_ssize_t i = 0; i < fixtuple_size; i++) {
                TypeNode *temp = TypeNode_Convert(
                    PyTuple_GET_ITEM(state->array_el_obj, i),
                    err_not_json,
                    json_compatible
                );
                if (temp == NULL) goto error;
                out->extra[e_ind++] = temp;
            }
        }
        else {
            TypeNode *temp = TypeNode_Convert(
                state->array_el_obj, err_not_json, json_compatible
            );
            if (temp == NULL) goto error;
            out->extra[e_ind++] = temp;
        }
    }
    return (TypeNode *)out;

error:
    TypeNode_Free((TypeNode *)out);
    return NULL;
}

static int
typenode_collect_err_unique(TypeNodeCollectState *state, const char *kind) {
    PyErr_Format(
        PyExc_TypeError,
        "Type unions may not contain more than one %s type - "
        "type `%R` is not supported",
        kind,
        state->context
    );
    return -1;
}

static int
typenode_collect_check_invariants(
    TypeNodeCollectState *state, bool err_not_json, bool *json_compatible
) {
    /* Ensure at least one type is set */
    if (state->types == 0) {
        PyErr_Format(PyExc_RuntimeError, "No types found, this is likely a bug!");
    }

    /* If a custom type is used, this node may only contain that type and `None */
    if (
        state->custom_obj != NULL &&
        state->types & ~(MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC | MS_TYPE_NONE)
    ) {
        PyErr_Format(
            PyExc_TypeError,
            "Type unions containing a custom type may not contain any "
            "additional types other than `None` - type `%R` is not supported",
            state->context
        );
        return -1;
    }

    /* Ensure structs don't conflict with dict/array types */
    if (state->struct_obj) {
        bool asarray = (((StructMetaObject *)(state->struct_obj))->asarray == OPT_TRUE);
        if (asarray && state->array_el_obj) {
            PyErr_Format(
                PyExc_TypeError,
                "Type unions containing a Struct type with `asarray=True` may "
                "not contain other array-like types - type `%R` is not supported",
                state->context
            );
            return -1;
        }
        else if (!asarray && state->dict_key_obj) {
            PyErr_Format(
                PyExc_TypeError,
                "Type unions may not contain both a Struct type and a dict type "
                "- type `%R` is not supported",
                state->context
            );
            return -1;
        }
    }

    /* Ensure IntEnum doesn't conflict with int */
    if (state->intenum_obj && state->types & MS_TYPE_INT) {
        PyErr_Format(
            PyExc_TypeError,
            "Type unions may not contain both int and an IntEnum "
            "- type `%R` is not supported",
            state->context
        );
        return -1;
    }

    /* Ensure Enum doesn't conflict with str */
    if (state->enum_obj && state->types & MS_TYPE_STR) {
        PyErr_Format(
            PyExc_TypeError,
            "Type unions may not contain both str and an Enum "
            "- type `%R` is not supported",
            state->context
        );
        return -1;
    }

    /* JSON serializes more types as strings, ensure they don't conflict in a union */
    if (
        ms_popcount(
            state->types & (MS_TYPE_STR | MS_TYPE_BYTES | MS_TYPE_BYTEARRAY | MS_TYPE_DATETIME)
        ) > 1
    ) {
        if (err_not_json) {
            PyErr_Format(
                PyExc_TypeError,
                "JSON type unions may contain at most one of `str`, `bytes`, "
                "`bytearray` or `datetime` - type `%R` is not supported",
                state->context
            );
            return -1;
        }
        if (json_compatible != NULL)
            *json_compatible = false;
    }
    return 0;
}

static int
typenode_collect_dict(TypeNodeCollectState *state, PyObject *obj, PyObject *key, PyObject *val) {
    if (state->dict_key_obj != NULL) {
        return typenode_collect_err_unique(state, "dict");
    }
    state->types |= MS_TYPE_DICT;
    Py_INCREF(key);
    state->dict_key_obj = key;
    Py_INCREF(val);
    state->dict_val_obj = val;
    return 0;
}

static int
typenode_collect_array(TypeNodeCollectState *state, uint32_t type, PyObject *obj) {
    if (state->array_el_obj != NULL) {
        return typenode_collect_err_unique(
            state, "array-like (list, set, tuple)"
        );
    }
    state->types |= type;
    Py_INCREF(obj);
    state->array_el_obj = obj;
    return 0;
}

static int
typenode_collect_custom(TypeNodeCollectState *state, uint32_t type, PyObject *obj) {
    if (state->custom_obj != NULL) {
        return typenode_collect_err_unique(state, "custom");
    }
    state->types |= type;
    Py_INCREF(obj);
    state->custom_obj = obj;
    return 0;
}

static void
typenode_collect_clear_state(TypeNodeCollectState *state) {
    Py_CLEAR(state->struct_obj);
    Py_CLEAR(state->struct_obj);
    Py_CLEAR(state->intenum_obj);
    Py_CLEAR(state->enum_obj);
    Py_CLEAR(state->custom_obj);
    Py_CLEAR(state->array_el_obj);
    Py_CLEAR(state->dict_key_obj);
    Py_CLEAR(state->dict_val_obj);
}

static int
typenode_collect_type(TypeNodeCollectState *state, PyObject *obj) {
    int out = -1;
    PyObject *origin = NULL, *args = NULL;
    MsgspecState *st = msgspec_get_global_state();

    /* If `Any` type already encountered, nothing to do */
    if (state->types & MS_TYPE_ANY) return 0;
    if (obj == st->typing_any) {
        /* Any takes precedence, drop all existing and update type flags */
        typenode_collect_clear_state(state);
        state->types = MS_TYPE_ANY;
        return 0;
    }

    /* Handle Scalar types */
    if (obj == Py_None || obj == NONE_TYPE) {
        state->types |= MS_TYPE_NONE;
        return 0;
    }
    else if (obj == (PyObject *)(&PyBool_Type)) {
        state->types |= MS_TYPE_BOOL;
        return 0;
    }
    else if (obj == (PyObject *)(&PyLong_Type)) {
        state->types |= MS_TYPE_INT;
        return 0;
    }
    else if (obj == (PyObject *)(&PyFloat_Type)) {
        state->types |= MS_TYPE_FLOAT;
        return 0;
    }
    else if (obj == (PyObject *)(&PyUnicode_Type)) {
        state->types |= MS_TYPE_STR;
        return 0;
    }
    else if (obj == (PyObject *)(&PyBytes_Type)) {
        state->types |= MS_TYPE_BYTES;
        return 0;
    }
    else if (obj == (PyObject *)(&PyByteArray_Type)) {
        state->types |= MS_TYPE_BYTEARRAY;
        return 0;
    }
    else if (obj == (PyObject *)(PyDateTimeAPI->DateTimeType)) {
        state->types |= MS_TYPE_DATETIME;
        return 0;
    }
    else if (obj == (PyObject *)(&Ext_Type)) {
        state->types |= MS_TYPE_EXT;
        return 0;
    }

    /* Struct types */
    if (Py_TYPE(obj) == &StructMetaType) {
        /* May only have one Struct type in a union */
        if (state->struct_obj != NULL) {
            return typenode_collect_err_unique(state, "Struct");
        }
        state->types |= MS_TYPE_STRUCT;
        Py_INCREF(obj);
        state->struct_obj = obj;
        return 0;
    }

    /* Enum types */
    if (PyType_Check(obj) && PyType_IsSubtype((PyTypeObject *)obj, st->EnumType)) {
        if (PyType_IsSubtype((PyTypeObject *)obj, &PyLong_Type)) {
            /* IntEnum */
            if (state->intenum_obj != NULL) {
                return typenode_collect_err_unique(state, "IntEnum");
            }
            state->types |= MS_TYPE_INTENUM;
            Py_INCREF(obj);
            state->intenum_obj = obj;
            return 0;
        }
        else {
            /* Enum */
            if (state->enum_obj != NULL) {
                return typenode_collect_err_unique(state, "Enum");
            }
            state->types |= MS_TYPE_ENUM;
            Py_INCREF(obj);
            state->enum_obj = obj;
            return 0;
        }
    }

    /* Generic collections can be spelled a few different ways, so the below
     * logic is a bit split up as we discover what type of thing `obj` is. */
    if (obj == (PyObject*)(&PyDict_Type) || obj == st->typing_dict) {
        return typenode_collect_dict(state, obj, st->typing_any, st->typing_any);
    }
    else if (obj == (PyObject*)(&PyList_Type) || obj == st->typing_list) {
        return typenode_collect_array(state, MS_TYPE_LIST, st->typing_any);
    }
    else if (obj == (PyObject*)(&PySet_Type) || obj == st->typing_set) {
        return typenode_collect_array(state, MS_TYPE_SET, st->typing_any);
    }
    else if (obj == (PyObject*)(&PyTuple_Type) || obj == st->typing_tuple) {
        return typenode_collect_array(state, MS_TYPE_VARTUPLE, st->typing_any);
    }

    /* Attempt to extract __origin__/__args__ from the obj as a typing object */
    if ((origin = PyObject_GetAttr(obj, st->str___origin__)) == NULL ||
            (args = PyObject_GetAttr(obj, st->str___args__)) == NULL) {
        /* Not a parametrized generic, must be a custom type */
        PyErr_Clear();
        if (!PyType_Check(origin != NULL ? origin : obj)) goto invalid;
        out = typenode_collect_custom(
            state,
            origin != NULL ? MS_TYPE_CUSTOM_GENERIC : MS_TYPE_CUSTOM,
            obj
        );
        goto done;
    }

    if (origin == (PyObject*)(&PyDict_Type)) {
        if (PyTuple_Size(args) != 2) goto invalid;
        out = typenode_collect_dict(
            state, obj, PyTuple_GET_ITEM(args, 0), PyTuple_GET_ITEM(args, 1)
        );
        goto done;
    }
    else if (origin == (PyObject*)(&PyList_Type)) {
        if (PyTuple_Size(args) != 1) goto invalid;
        out = typenode_collect_array(
            state, MS_TYPE_LIST, PyTuple_GET_ITEM(args, 0)
        );
        goto done;
    }
    else if (origin == (PyObject*)(&PySet_Type)) {
        if (PyTuple_Size(args) != 1) goto invalid;
        out = typenode_collect_array(
            state, MS_TYPE_SET, PyTuple_GET_ITEM(args, 0)
        );
        goto done;
    }
    else if (origin == (PyObject*)(&PyTuple_Type)) {
        if (PyTuple_Size(args) == 2 && PyTuple_GET_ITEM(args, 1) == Py_Ellipsis) {
            out = typenode_collect_array(
                state, MS_TYPE_VARTUPLE, PyTuple_GET_ITEM(args, 0)
            );
        }
        else {
            out = typenode_collect_array(state, MS_TYPE_FIXTUPLE, args);
        }
        goto done;
    }
    else if (origin == st->typing_union) {
        for (Py_ssize_t i = 0; i < PyTuple_Size(args); i++) {
            out = typenode_collect_type(state, PyTuple_GET_ITEM(args, i));
            if (out < 0) break;
        }
        goto done;
    }
    else {
        if (!PyType_Check(origin)) goto invalid;
        /* A parametrized type, but not one we natively support. */
        out = typenode_collect_custom(state, MS_TYPE_CUSTOM_GENERIC, obj);
        goto done;
    }

invalid:
    PyErr_Format(PyExc_TypeError, "Type '%R' is not supported", obj);

done:
    Py_XDECREF(origin);
    Py_XDECREF(args);
    return out;
}

static TypeNode *
TypeNode_Convert(PyObject *obj, bool err_not_json, bool *json_compatible) {
    TypeNode *out = NULL;
    TypeNodeCollectState state = {0};
    state.context = obj;

    /* Traverse `obj` to collect all type annotations at this level */
    if (typenode_collect_type(&state, obj) < 0) goto done;
    /* Check type invariants to ensure Union types are valid */
    if (typenode_collect_check_invariants(&state, err_not_json, json_compatible) < 0) goto done;
    /* Populate a new TypeNode, recursing into subtypes as needed */
    out = typenode_from_collect_state(&state, err_not_json, json_compatible);
done:
    typenode_collect_clear_state(&state);
    return out;
}

#define PATH_ELLIPSIS -1
#define PATH_KEY -2

typedef struct PathNode {
    struct PathNode *parent;
    Py_ssize_t index;
    StructMetaObject *struct_type;
} PathNode;

/* reverse the parent pointers in the path linked list */
static PathNode *
pathnode_reverse(PathNode *path) {
    PathNode *current = path, *prev = NULL, *next = NULL;
    while (current != NULL) {
        next = current->parent;
        current->parent = prev;
        prev = current;
        current = next;
    }
    return prev;
}

static PyObject *
PathNode_ErrSuffix(PathNode *path) {
    strbuilder parts = {0};
    PathNode *path_orig;
    PyObject *out = NULL, *path_repr = NULL, *groups = NULL, *group = NULL;

    if (path == NULL) {
        return PyUnicode_FromString("");
    }

    /* Reverse the parent pointers for easier printing */
    path = pathnode_reverse(path);

    /* Cache the original path to reset the parent pointers later */
    path_orig = path;

    /* Start with the root element */
    if (!strbuilder_extend(&parts, "`$", 2)) goto cleanup;

    while (path != NULL) {
        if (path->struct_type != NULL) {
            PyObject *name = PyTuple_GET_ITEM(
                path->struct_type->struct_fields, path->index
            );
            if (!strbuilder_extend(&parts, ".", 1)) goto cleanup;
            if (!strbuilder_extend_unicode(&parts, name)) goto cleanup;
        }
        else if (path->index == PATH_ELLIPSIS) {
            if (!strbuilder_extend(&parts, "[...]", 5)) goto cleanup;
        }
        else if (path->index == PATH_KEY) {
            if (groups == NULL) {
                groups = PyList_New(0);
                if (groups == NULL) goto cleanup;
            }
            if (!strbuilder_extend(&parts, "`", 1)) goto cleanup;
            group = strbuilder_build(&parts);
            if (group == NULL) goto cleanup;
            if (PyList_Append(groups, group) < 0) goto cleanup;
            Py_CLEAR(group);
            strbuilder_extend(&parts, "`key", 4);
        }
        else {
            char buf[20];
            char *p = &buf[20];
            Py_ssize_t x = path->index;
            if (!strbuilder_extend(&parts, "[", 1)) goto cleanup;
            while (x >= 100) {
                const int64_t old = x;
                p -= 2;
                x /= 100;
                memcpy(p, DIGIT_TABLE + ((old - (x * 100)) << 1), 2);
            }
            if (x >= 10) {
                p -= 2;
                memcpy(p, DIGIT_TABLE + (x << 1), 2);
            }
            else {
                *--p = x + '0';
            }
            if (!strbuilder_extend(&parts, p, &buf[20] - p)) goto cleanup;
            if (!strbuilder_extend(&parts, "]", 1)) goto cleanup;
        }
        path = path->parent;
    }
    if (!strbuilder_extend(&parts, "`", 1)) goto cleanup;

    if (groups == NULL) {
        path_repr = strbuilder_build(&parts);
    }
    else {
        group = strbuilder_build(&parts);
        if (group == NULL) goto cleanup;
        if (PyList_Append(groups, group) < 0) goto cleanup;
        PyObject *sep = PyUnicode_FromString(" in ");
        if (sep == NULL) goto cleanup;
        if (PyList_Reverse(groups) < 0) goto cleanup;
        path_repr = PyUnicode_Join(sep, groups);
        Py_DECREF(sep);
    }

    out = PyUnicode_FromFormat(" - at %U", path_repr);

cleanup:
    Py_XDECREF(path_repr);
    Py_XDECREF(group);
    Py_XDECREF(groups);
    pathnode_reverse(path_orig);
    strbuilder_reset(&parts);
    return out;
}

#define ms_raise_validation_error(path, format, ...) \
    do { \
        MsgspecState *st = msgspec_get_global_state(); \
        PyObject *suffix = PathNode_ErrSuffix(path); \
        if (suffix != NULL) { \
            PyErr_Format(st->DecodeError, format, __VA_ARGS__, suffix); \
            Py_DECREF(suffix); \
        } \
    } while (0)

static MS_NOINLINE PyObject *
ms_validation_error(char *got, TypeNode *type, PathNode *path) {
    PyObject *type_repr = typenode_simple_repr(type);
    if (type_repr != NULL) {
        ms_raise_validation_error(path, "Expected `%U`, got `%s`%U", type_repr, got);
        Py_DECREF(type_repr);
    }
    return NULL;
}

static PyTypeObject StructMixinType;

/* To reduce overhead of repeatedly allocating & freeing messages (in e.g. a
 * server), we keep Struct objects below a certain size around in a freelist.
 * This freelist is cleared during major GC collections (as part of traversing
 * the msgspec module).
 *
 * Set STRUCT_FREELIST_MAX_SIZE to 0 to disable the freelist entirely.
 */
#ifndef STRUCT_FREELIST_MAX_SIZE
#define STRUCT_FREELIST_MAX_SIZE 10
#endif
#ifndef STRUCT_FREELIST_MAX_PER_SIZE
#define STRUCT_FREELIST_MAX_PER_SIZE 2000
#endif

#if STRUCT_FREELIST_MAX_SIZE > 0
static PyObject *struct_freelist[STRUCT_FREELIST_MAX_SIZE];
static int struct_freelist_len[STRUCT_FREELIST_MAX_SIZE];

static void
Struct_freelist_clear(void) {
    Py_ssize_t i;
    PyObject *obj;
    for (i = 0; i < STRUCT_FREELIST_MAX_SIZE; i++) {
        while ((obj = struct_freelist[i]) != NULL) {
            struct_freelist[i] = (PyObject *)obj->ob_type;
            PyObject_GC_Del(obj);
        }
        struct_freelist_len[i] = 0;
    }
}

static PyObject *
Struct_alloc(PyTypeObject *type) {
    Py_ssize_t size;
    PyObject *obj;

    size = (type->tp_basicsize - sizeof(PyObject)) / sizeof(void *);

    if (size > 0 &&
        size <= STRUCT_FREELIST_MAX_SIZE &&
        struct_freelist[size - 1] != NULL)
    {
        /* Pop object off freelist */
        obj = struct_freelist[size - 1];
        struct_freelist[size - 1] = (PyObject *)obj->ob_type;
        struct_freelist_len[size - 1]--;
        /* Initialize the object. This is mirrored from within `PyObject_Init`,
         * as well as PyType_GenericAlloc */
        obj->ob_type = type;
        Py_INCREF(type);
        _Py_NewReference(obj);
        PyObject_GC_Track(obj);
    }
    else {
        obj = PyType_GenericAlloc(type, 0);
    }
    return obj;
}

/* Mirrored from cpython Objects/typeobject.c */
static void
clear_slots(PyTypeObject *type, PyObject *self)
{
    Py_ssize_t i, n;
    PyMemberDef *mp;

    n = Py_SIZE(type);
    mp = PyHeapType_GET_MEMBERS((PyHeapTypeObject *)type);
    for (i = 0; i < n; i++, mp++) {
        if (mp->type == T_OBJECT_EX && !(mp->flags & READONLY)) {
            char *addr = (char *)self + mp->offset;
            PyObject *obj = *(PyObject **)addr;
            if (obj != NULL) {
                *(PyObject **)addr = NULL;
                Py_DECREF(obj);
            }
        }
    }
}

static void
Struct_dealloc(PyObject *self) {
    Py_ssize_t size;
    PyTypeObject *type, *base;

    type = Py_TYPE(self);

    PyObject_GC_UnTrack(self);

    size = (type->tp_basicsize - sizeof(PyObject)) / sizeof(void *);

    Py_TRASHCAN_BEGIN(self, Struct_dealloc)
    base = type;
    while (base != NULL) {
        if (Py_SIZE(base))
            clear_slots(base, self);
        base = base->tp_base;
    }
    Py_TRASHCAN_END

    if (size > 0 &&
        size <= STRUCT_FREELIST_MAX_SIZE &&
        struct_freelist_len[size - 1] < STRUCT_FREELIST_MAX_PER_SIZE)
    {
        /* Push object onto freelist */
        self->ob_type = (PyTypeObject *)(struct_freelist[size - 1]);
        struct_freelist_len[size - 1]++;
        struct_freelist[size - 1] = self;
    }
    else {
        type->tp_free(self);
    }
    Py_DECREF(type);
}

#else

static inline PyObject *
Struct_alloc(PyTypeObject *type) {
    return type->tp_alloc(type, 0);
}

#endif /* Struct freelist */

static Py_ssize_t
StructMeta_get_field_index(
    StructMetaObject *self, const char * key, Py_ssize_t key_size, Py_ssize_t *pos
) {
    const char *field;
    Py_ssize_t nfields, field_size, i, ind, offset = *pos;
    nfields = PyTuple_GET_SIZE(self->struct_fields);
    for (i = 0; i < nfields; i++) {
        ind = (i + offset) % nfields;
        field = unicode_str_and_size(
            PyTuple_GET_ITEM(self->struct_fields, ind), &field_size
        );
        if (field == NULL) return -1;
        if (key_size == field_size && memcmp(key, field, key_size) == 0) {
            *pos = (ind + 1) % nfields;
            return ind;
        }
    }
    return -1;
}

static int
dict_discard(PyObject *dict, PyObject *key) {
    int status = PyDict_Contains(dict, key);
    if (status < 0)
        return status;
    return (status == 1) ? PyDict_DelItem(dict, key) : 0;
}

static PyObject *
Struct_vectorcall(PyTypeObject *cls, PyObject *const *args, size_t nargsf, PyObject *kwnames);

static PyObject *
StructMeta_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    StructMetaObject *cls = NULL;
    PyObject *name = NULL, *bases = NULL, *orig_dict = NULL;
    PyObject *arg_fields = NULL, *kwarg_fields = NULL, *new_dict = NULL, *new_args = NULL;
    PyObject *fields = NULL, *defaults = NULL, *offsets_lk = NULL, *offset = NULL, *slots = NULL, *slots_list = NULL;
    PyObject *base, *base_fields, *base_defaults, *annotations;
    PyObject *default_val, *field;
    Py_ssize_t nfields, ndefaults, i, j, k;
    Py_ssize_t *offsets = NULL, *base_offsets;
    int arg_frozen = -1, arg_asarray = -1, frozen = -1, asarray = -1;

    static char *kwlist[] = {"name", "bases", "dict", "frozen", "asarray", NULL};

    /* Parse arguments: (name, bases, dict) */
    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs, "UO!O!|$pp:StructMeta.__new__", kwlist,
            &name, &PyTuple_Type, &bases, &PyDict_Type, &orig_dict,
            &arg_frozen, &arg_asarray))
        return NULL;

    if (PyDict_GetItemString(orig_dict, "__init__") != NULL) {
        PyErr_SetString(PyExc_TypeError, "Struct types cannot define __init__");
        return NULL;
    }
    if (PyDict_GetItemString(orig_dict, "__new__") != NULL) {
        PyErr_SetString(PyExc_TypeError, "Struct types cannot define __new__");
        return NULL;
    }
    if (PyDict_GetItemString(orig_dict, "__slots__") != NULL) {
        PyErr_SetString(PyExc_TypeError, "Struct types cannot define __slots__");
        return NULL;
    }

    arg_fields = PyDict_New();
    if (arg_fields == NULL)
        goto error;
    kwarg_fields = PyDict_New();
    if (kwarg_fields == NULL)
        goto error;
    offsets_lk = PyDict_New();
    if (offsets_lk == NULL)
        goto error;

    for (i = PyTuple_GET_SIZE(bases) - 1; i >= 0; i--) {
        base = PyTuple_GET_ITEM(bases, i);
        if ((PyTypeObject *)base == &StructMixinType) {
            continue;
        }

        if (!(PyType_Check(base) && (Py_TYPE(base) == &StructMetaType))) {
            PyErr_SetString(
                PyExc_TypeError,
                "All base classes must be subclasses of msgspec.Struct"
            );
            goto error;
        }
        frozen = STRUCT_MERGE_OPTIONS(frozen, ((StructMetaObject *)base)->frozen);
        asarray = STRUCT_MERGE_OPTIONS(asarray, ((StructMetaObject *)base)->asarray);
        base_fields = StructMeta_GET_FIELDS(base);
        base_defaults = StructMeta_GET_DEFAULTS(base);
        base_offsets = StructMeta_GET_OFFSETS(base);
        nfields = PyTuple_GET_SIZE(base_fields);
        ndefaults = PyTuple_GET_SIZE(base_defaults);
        for (j = 0; j < nfields; j++) {
            field = PyTuple_GET_ITEM(base_fields, j);
            if (j < (nfields - ndefaults)) {
                if (PyDict_SetItem(arg_fields, field, Py_None) < 0)
                    goto error;
                if (dict_discard(kwarg_fields, field) < 0)
                    goto error;
            }
            else {
                default_val = PyTuple_GET_ITEM(base_defaults, (j + ndefaults - nfields));
                if (PyDict_SetItem(kwarg_fields, field, default_val) < 0)
                    goto error;
                if (dict_discard(arg_fields, field) < 0)
                    goto error;
            }
            offset = PyLong_FromSsize_t(base_offsets[j]);
            if (offset == NULL)
                goto error;
            if (PyDict_SetItem(offsets_lk, field, offset) < 0)
                goto error;
            Py_DECREF(offset);
        }
    }
    frozen = STRUCT_MERGE_OPTIONS(frozen, arg_frozen);
    asarray = STRUCT_MERGE_OPTIONS(asarray, arg_asarray);

    new_dict = PyDict_Copy(orig_dict);
    if (new_dict == NULL)
        goto error;
    slots_list = PyList_New(0);
    if (slots_list == NULL)
        goto error;

    annotations = PyDict_GetItemString(orig_dict, "__annotations__");
    if (annotations != NULL) {
        if (!PyDict_Check(annotations)) {
            PyErr_SetString(PyExc_TypeError, "__annotations__ must be a dict");
            goto error;
        }

        i = 0;
        while (PyDict_Next(annotations, &i, &field, NULL)) {
            if (!PyUnicode_CheckExact(field)) {
                PyErr_SetString(
                    PyExc_TypeError,
                    "__annotations__ keys must be strings"
                );
                goto error;
            }

            /* If the field is new, add it to slots */
            if (PyDict_GetItem(arg_fields, field) == NULL && PyDict_GetItem(kwarg_fields, field) == NULL) {
                if (PyList_Append(slots_list, field) < 0)
                    goto error;
            }

            default_val = PyDict_GetItem(new_dict, field);
            if (default_val != NULL) {
                if (dict_discard(arg_fields, field) < 0)
                    goto error;
                if (PyDict_SetItem(kwarg_fields, field, default_val) < 0)
                    goto error;
                if (dict_discard(new_dict, field) < 0)
                    goto error;
            }
            else {
                if (dict_discard(kwarg_fields, field) < 0)
                    goto error;
                if (PyDict_SetItem(arg_fields, field, Py_None) < 0)
                    goto error;
            }
        }
    }

    fields = PyTuple_New(PyDict_Size(arg_fields) + PyDict_Size(kwarg_fields));
    if (fields == NULL)
        goto error;
    defaults = PyTuple_New(PyDict_Size(kwarg_fields));
    if (defaults == NULL)
        goto error;

    i = 0;
    j = 0;
    while (PyDict_Next(arg_fields, &i, &field, NULL)) {
        Py_INCREF(field);
        PyTuple_SET_ITEM(fields, j, field);
        j++;
    }
    i = 0;
    k = 0;
    while (PyDict_Next(kwarg_fields, &i, &field, &default_val)) {
        Py_INCREF(field);
        PyTuple_SET_ITEM(fields, j, field);
        Py_INCREF(default_val);
        PyTuple_SET_ITEM(defaults, k, default_val);
        j++;
        k++;
    }
    Py_CLEAR(arg_fields);
    Py_CLEAR(kwarg_fields);

    if (PyList_Sort(slots_list) < 0)
        goto error;

    slots = PyList_AsTuple(slots_list);
    if (slots == NULL)
        goto error;
    Py_CLEAR(slots_list);

    if (PyDict_SetItemString(new_dict, "__slots__", slots) < 0)
        goto error;
    Py_CLEAR(slots);

    new_args = Py_BuildValue("(OOO)", name, bases, new_dict);
    if (new_args == NULL)
        goto error;

    cls = (StructMetaObject *) PyType_Type.tp_new(type, new_args, NULL);
    if (cls == NULL)
        goto error;
    ((PyTypeObject *)cls)->tp_vectorcall = (vectorcallfunc)Struct_vectorcall;
#if STRUCT_FREELIST_MAX_SIZE > 0
    ((PyTypeObject *)cls)->tp_dealloc = Struct_dealloc;
#endif
    Py_CLEAR(new_args);

    PyMemberDef *mp = PyHeapType_GET_MEMBERS(cls);
    for (i = 0; i < Py_SIZE(cls); i++, mp++) {
        offset = PyLong_FromSsize_t(mp->offset);
        if (offset == NULL)
            goto error;
        if (PyDict_SetItemString(offsets_lk, mp->name, offset) < 0)
            goto error;
    }
    offsets = PyMem_New(Py_ssize_t, PyTuple_GET_SIZE(fields));
    if (offsets == NULL)
        goto error;
    for (i = 0; i < PyTuple_GET_SIZE(fields); i++) {
        field = PyTuple_GET_ITEM(fields, i);
        offset = PyDict_GetItem(offsets_lk, field);
        if (offset == NULL) {
            PyErr_Format(PyExc_RuntimeError, "Failed to get offset for %R", field);
            goto error;
        }
        offsets[i] = PyLong_AsSsize_t(offset);
    }
    Py_CLEAR(offsets_lk);

    cls->struct_fields = fields;
    cls->struct_defaults = defaults;
    cls->struct_offsets = offsets;
    cls->frozen = frozen;
    cls->asarray = asarray;
    return (PyObject *) cls;
error:
    Py_XDECREF(arg_fields);
    Py_XDECREF(kwarg_fields);
    Py_XDECREF(fields);
    Py_XDECREF(defaults);
    Py_XDECREF(new_dict);
    Py_XDECREF(slots_list);
    Py_XDECREF(new_args);
    Py_XDECREF(offsets_lk);
    Py_XDECREF(offset);
    if (offsets != NULL)
        PyMem_Free(offsets);
    return NULL;
}

static int
StructMeta_prep_types(PyObject *py_self, bool err_not_json, bool *json_compatible) {
    StructMetaObject *self = (StructMetaObject *)py_self;
    MsgspecState *st;
    TypeNode *type;
    TypeNode **struct_types = NULL;
    Py_ssize_t i, nfields;
    PyObject *obj, *field, *annotations = NULL;
    bool struct_is_json_compatible = true;

    /* Types are currently being prepped, recursive type */
    if (self->traversing) return 0;

    if (self->struct_types) {
        if (!self->json_compatible) {
            if (json_compatible != NULL) {
                *json_compatible = false;
            }
            if (!err_not_json) return 0;
            /* If we want to error, we need to recurse again here. This won't
             * modify any internal state, since it will error too early. */
        }
        else {
            return 0;
        }
    }

    /* Prevent recursion, clear on return */
    self->traversing = true;

    nfields = PyTuple_GET_SIZE(self->struct_fields);

    st = msgspec_get_global_state();
    annotations = CALL_ONE_ARG(st->get_type_hints, py_self);
    if (annotations == NULL) goto error;

    struct_types = PyMem_Calloc(nfields, sizeof(TypeNode*));
    if (struct_types == NULL)  {
        PyErr_NoMemory();
        goto error;
    }

    for (i = 0; i < nfields; i++) {
        bool field_is_json_compatible = true;
        field = PyTuple_GET_ITEM(self->struct_fields, i);
        obj = PyDict_GetItem(annotations, field);
        if (obj == NULL) goto error;
        type = TypeNode_Convert(obj, err_not_json, &field_is_json_compatible);
        if (type == NULL) goto error;
        struct_types[i] = type;
        struct_is_json_compatible &= field_is_json_compatible;
    }

    self->traversing = false;
    self->struct_types = struct_types;
    self->json_compatible = struct_is_json_compatible;
    if (!struct_is_json_compatible && json_compatible != NULL)
        *json_compatible = false;

    Py_DECREF(annotations);
    return 0;

error:
    self->traversing = false;
    Py_XDECREF(annotations);
    if (struct_types != NULL) {
        for (i = 0; i < nfields; i++) {
            TypeNode_Free(struct_types[i]);
        }
    }
    PyMem_Free(struct_types);
    return -1;
}

static int
StructMeta_traverse(StructMetaObject *self, visitproc visit, void *arg)
{
    int out;
    Py_ssize_t i, nfields;
    Py_VISIT(self->struct_fields);
    Py_VISIT(self->struct_defaults);
    if (self->struct_types != NULL) {
        nfields = PyTuple_GET_SIZE(self->struct_fields);
        for (i = 0; i < nfields; i++) {
            out = TypeNode_traverse(self->struct_types[i], visit, arg);
            if (out != 0) return out;
        }
    }
    return PyType_Type.tp_traverse((PyObject *)self, visit, arg);
}

static int
StructMeta_clear(StructMetaObject *self)
{
    Py_ssize_t i, nfields;
    /* skip if clear already invoked */
    if (self->struct_fields == NULL) return 0;

    nfields = PyTuple_GET_SIZE(self->struct_fields);
    Py_CLEAR(self->struct_fields);
    Py_CLEAR(self->struct_defaults);
    PyMem_Free(self->struct_offsets);
    if (self->struct_types != NULL) {
        for (i = 0; i < nfields; i++) {
            TypeNode_Free(self->struct_types[i]);
        }
    }
    return PyType_Type.tp_clear((PyObject *)self);
}

static void
StructMeta_dealloc(StructMetaObject *self)
{
    StructMeta_clear(self);
    PyType_Type.tp_dealloc((PyObject *)self);
}

static PyObject*
StructMeta_frozen(StructMetaObject *self, void *closure)
{
    if (self->frozen == OPT_TRUE) { Py_RETURN_TRUE; }
    else { Py_RETURN_FALSE; }
}

static PyObject*
StructMeta_asarray(StructMetaObject *self, void *closure)
{
    if (self->asarray == OPT_TRUE) { Py_RETURN_TRUE; }
    else { Py_RETURN_FALSE; }
}

static PyObject*
StructMeta_signature(StructMetaObject *self, void *closure)
{
    Py_ssize_t nfields, ndefaults, npos, i;
    MsgspecState *st;
    PyObject *res = NULL;
    PyObject *inspect = NULL;
    PyObject *parameter_cls = NULL;
    PyObject *parameter_empty = NULL;
    PyObject *parameter_kind = NULL;
    PyObject *signature_cls = NULL;
    PyObject *annotations = NULL;
    PyObject *parameters = NULL;
    PyObject *temp_args = NULL, *temp_kwargs = NULL;
    PyObject *field, *default_val, *parameter, *annotation;

    st = msgspec_get_global_state();

    nfields = PyTuple_GET_SIZE(self->struct_fields);
    ndefaults = PyTuple_GET_SIZE(self->struct_defaults);
    npos = nfields - ndefaults;

    inspect = PyImport_ImportModule("inspect");
    if (inspect == NULL)
        goto cleanup;
    parameter_cls = PyObject_GetAttrString(inspect, "Parameter");
    if (parameter_cls == NULL)
        goto cleanup;
    parameter_empty = PyObject_GetAttrString(parameter_cls, "empty");
    if (parameter_empty == NULL)
        goto cleanup;
    parameter_kind = PyObject_GetAttrString(parameter_cls, "POSITIONAL_OR_KEYWORD");
    if (parameter_kind == NULL)
        goto cleanup;
    signature_cls = PyObject_GetAttrString(inspect, "Signature");
    if (signature_cls == NULL)
        goto cleanup;

    annotations = CALL_ONE_ARG(st->get_type_hints, (PyObject *)self);
    if (annotations == NULL)
        goto cleanup;

    parameters = PyList_New(nfields);
    if (parameters == NULL)
        return NULL;

    temp_args = PyTuple_New(0);
    if (temp_args == NULL)
        goto cleanup;
    temp_kwargs = PyDict_New();
    if (temp_kwargs == NULL)
        goto cleanup;
    if (PyDict_SetItemString(temp_kwargs, "kind", parameter_kind) < 0)
        goto cleanup;

    for (i = 0; i < nfields; i++) {
        field = PyTuple_GET_ITEM(self->struct_fields, i);
        if (i < npos) {
            default_val = parameter_empty;
        } else {
            default_val = PyTuple_GET_ITEM(self->struct_defaults, i - npos);
        }
        annotation = PyDict_GetItem(annotations, field);
        if (annotation == NULL) {
            annotation = parameter_empty;
        }
        if (PyDict_SetItemString(temp_kwargs, "name", field) < 0)
            goto cleanup;
        if (PyDict_SetItemString(temp_kwargs, "default", default_val) < 0)
            goto cleanup;
        if (PyDict_SetItemString(temp_kwargs, "annotation", annotation) < 0)
            goto cleanup;
        parameter = PyObject_Call(parameter_cls, temp_args, temp_kwargs);
        if (parameter == NULL)
            goto cleanup;
        PyList_SET_ITEM(parameters, i, parameter);
    }
    res = CALL_ONE_ARG(signature_cls, parameters);
cleanup:
    Py_XDECREF(inspect);
    Py_XDECREF(parameter_cls);
    Py_XDECREF(parameter_empty);
    Py_XDECREF(parameter_kind);
    Py_XDECREF(signature_cls);
    Py_XDECREF(annotations);
    Py_XDECREF(parameters);
    Py_XDECREF(temp_args);
    Py_XDECREF(temp_kwargs);
    return res;
}

static PyMemberDef StructMeta_members[] = {
    {"__struct_fields__", T_OBJECT_EX, offsetof(StructMetaObject, struct_fields), READONLY, "Struct fields"},
    {"__struct_defaults__", T_OBJECT_EX, offsetof(StructMetaObject, struct_defaults), READONLY, "Struct defaults"},
    {"__match_args__", T_OBJECT_EX, offsetof(StructMetaObject, struct_fields), READONLY, "Positional match args"},
    {NULL},
};

static PyGetSetDef StructMeta_getset[] = {
    {"__signature__", (getter) StructMeta_signature, NULL, NULL, NULL},
    {"frozen", (getter) StructMeta_frozen, NULL, NULL, NULL},
    {"asarray", (getter) StructMeta_asarray, NULL, NULL, NULL},
    {NULL},
};

static PyTypeObject StructMetaType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec._core.StructMeta",
    .tp_basicsize = sizeof(StructMetaObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_TYPE_SUBCLASS | Py_TPFLAGS_HAVE_GC | _Py_TPFLAGS_HAVE_VECTORCALL,
    .tp_new = StructMeta_new,
    .tp_dealloc = (destructor) StructMeta_dealloc,
    .tp_clear = (inquiry) StructMeta_clear,
    .tp_traverse = (traverseproc) StructMeta_traverse,
    .tp_members = StructMeta_members,
    .tp_getset = StructMeta_getset,
    .tp_call = PyVectorcall_Call,
    .tp_vectorcall_offset = offsetof(PyTypeObject, tp_vectorcall),
};


static PyObject *
maybe_deepcopy_default(PyObject *obj) {
    MsgspecState *st;
    PyObject *copy = NULL, *deepcopy = NULL, *res = NULL;
    PyTypeObject *type = Py_TYPE(obj);

    /* Known non-collection or recursively immutable types */
    if (obj == Py_None || obj == Py_False || obj == Py_True ||
        type == &PyLong_Type || type == &PyFloat_Type ||
        type == &PyBytes_Type || type == &PyUnicode_Type ||
        type == &PyByteArray_Type || type == &PyFrozenSet_Type
    ) {
        Py_INCREF(obj);
        return obj;
    }
    else if (type == &PyTuple_Type && (PyTuple_GET_SIZE(obj) == 0)) {
        Py_INCREF(obj);
        return obj;
    }
    else if (type == PyDateTimeAPI->DateTimeType ||
             type == PyDateTimeAPI->DeltaType ||
             type == PyDateTimeAPI->DateType ||
             type == PyDateTimeAPI->TimeType
    ) {
        Py_INCREF(obj);
        return obj;
    }

    st = msgspec_get_global_state();
    if (PyType_IsSubtype(type, st->EnumType)) {
        Py_INCREF(obj);
        return obj;
    }

    /* Fast paths for known empty collections */
    if (type == &PyDict_Type && PyDict_Size(obj) == 0) {
        return PyDict_New();
    }
    else if (type == &PyList_Type && PyList_GET_SIZE(obj) == 0) {
        return PyList_New(0);
    }
    else if (type == &PySet_Type && PySet_GET_SIZE(obj) == 0) {
        return PySet_New(NULL);
    }
    /* More complicated, invoke full deepcopy */
    copy = PyImport_ImportModule("copy");
    if (copy == NULL)
        goto cleanup;
    deepcopy = PyObject_GetAttrString(copy, "deepcopy");
    if (deepcopy == NULL)
        goto cleanup;
    res = CALL_ONE_ARG(deepcopy, obj);
cleanup:
    Py_XDECREF(copy);
    Py_XDECREF(deepcopy);
    return res;
}


/* Set field #index on obj. Steals a reference to val */
static inline void
Struct_set_index(PyObject *obj, Py_ssize_t index, PyObject *val) {
    StructMetaObject *cls;
    char *addr;
    PyObject *old;

    cls = (StructMetaObject *)Py_TYPE(obj);
    addr = (char *)obj + cls->struct_offsets[index];
    old = *(PyObject **)addr;
    Py_XDECREF(old);
    *(PyObject **)addr = val;
}

/* Get field #index or NULL on obj. Returns a borrowed reference */
static inline PyObject*
Struct_get_index_noerror(PyObject *obj, Py_ssize_t index) {
    StructMetaObject *cls = (StructMetaObject *)Py_TYPE(obj);
    char *addr = (char *)obj + cls->struct_offsets[index];
    return *(PyObject **)addr;
}

/* Get field #index on obj. Returns a borrowed reference */
static inline PyObject*
Struct_get_index(PyObject *obj, Py_ssize_t index) {
    PyObject *val = Struct_get_index_noerror(obj, index);
    if (val == NULL) {
        StructMetaObject *cls = (StructMetaObject *)Py_TYPE(obj);
        PyErr_Format(PyExc_AttributeError,
                     "Struct field %R is unset",
                     PyTuple_GET_ITEM(cls->struct_fields, index));
    }
    return val;
}

static int
Struct_fill_in_defaults(StructMetaObject *st_type, PyObject *obj, PathNode *path) {
    Py_ssize_t nfields, ndefaults, i;
    bool should_untrack;

    nfields = PyTuple_GET_SIZE(st_type->struct_fields);
    ndefaults = PyTuple_GET_SIZE(st_type->struct_defaults);
    should_untrack = PyObject_IS_GC(obj);

    for (i = 0; i < nfields; i++) {
        PyObject *val = Struct_get_index_noerror(obj, i);
        if (val == NULL) {
            if (i < (nfields - ndefaults)) {
                ms_raise_validation_error(
                    path,
                    "Object missing required field `%U`%U",
                    PyTuple_GET_ITEM(st_type->struct_fields, i)
                );
                return -1;
            }
            else {
                /* Fill in default */
                val = maybe_deepcopy_default(
                    PyTuple_GET_ITEM(st_type->struct_defaults, i - (nfields - ndefaults))
                );
                if (val == NULL) return -1;
                Struct_set_index(obj, i, val);
            }
        }
        if (should_untrack) {
            should_untrack = !OBJ_IS_GC(val);
        }
    }
    if (should_untrack)
        PyObject_GC_UnTrack(obj);
    return 0;
}

static PyObject *
Struct_vectorcall(PyTypeObject *cls, PyObject *const *args, size_t nargsf, PyObject *kwnames) {
    PyObject *self, *fields, *defaults, *field, *val;
    Py_ssize_t nargs, nkwargs, nfields, ndefaults, npos, i;
    int should_untrack;

    self = Struct_alloc(cls);
    if (self == NULL)
        return NULL;

    fields = StructMeta_GET_FIELDS(Py_TYPE(self));
    defaults = StructMeta_GET_DEFAULTS(Py_TYPE(self));

    nargs = PyVectorcall_NARGS(nargsf);
    nkwargs = (kwnames == NULL) ? 0 : PyTuple_GET_SIZE(kwnames);
    ndefaults = PyTuple_GET_SIZE(defaults);
    nfields = PyTuple_GET_SIZE(fields);
    npos = nfields - ndefaults;

    if (nargs > nfields) {
        PyErr_SetString(
            PyExc_TypeError,
            "Extra positional arguments provided"
        );
        goto error;
    }

    should_untrack = PyObject_IS_GC(self);

    for (i = 0; i < nfields; i++) {
        field = PyTuple_GET_ITEM(fields, i);
        val = (nkwargs == 0) ? NULL : find_keyword(kwnames, args + nargs, field);
        if (val != NULL) {
            if (i < nargs) {
                PyErr_Format(
                    PyExc_TypeError,
                    "Argument '%U' given by name and position",
                    field
                );
                goto error;
            }
            Py_INCREF(val);
            nkwargs -= 1;
        }
        else if (i < nargs) {
            val = args[i];
            Py_INCREF(val);
        }
        else if (i < npos) {
            PyErr_Format(
                PyExc_TypeError,
                "Missing required argument '%U'",
                field
            );
            goto error;
        }
        else {
            val = maybe_deepcopy_default(PyTuple_GET_ITEM(defaults, i - npos));
            if (val == NULL)
                goto error;
        }
        Struct_set_index(self, i, val);
        if (should_untrack) {
            should_untrack = !OBJ_IS_GC(val);
        }
    }
    if (nkwargs > 0) {
        PyErr_SetString(
            PyExc_TypeError,
            "Extra keyword arguments provided"
        );
        goto error;
    }
    if (should_untrack)
        PyObject_GC_UnTrack(self);
    return self;

error:
    Py_DECREF(self);
    return NULL;
}

static int
Struct_setattro(PyObject *self, PyObject *key, PyObject *value) {
    if (((StructMetaObject *)Py_TYPE(self))->frozen == OPT_TRUE) {
        PyErr_Format(
            PyExc_AttributeError,
            "immutable type: '%s'",
            Py_TYPE(self)->tp_name
        );
        return -1;
    }
    if (PyObject_GenericSetAttr(self, key, value) < 0)
        return -1;
    if (value != NULL && OBJ_IS_GC(value) && !IS_TRACKED(self))
        PyObject_GC_Track(self);
    return 0;
}

static PyObject *
Struct_repr(PyObject *self) {
    int recursive;
    Py_ssize_t nfields, i;
    PyObject *parts = NULL, *empty = NULL, *out = NULL;
    PyObject *part, *fields, *field, *val;

    recursive = Py_ReprEnter(self);
    if (recursive != 0) {
        out = (recursive < 0) ? NULL : PyUnicode_FromString("...");
        goto cleanup;
    }

    fields = StructMeta_GET_FIELDS(Py_TYPE(self));
    nfields = PyTuple_GET_SIZE(fields);
    if (nfields == 0) {
        out = PyUnicode_FromFormat("%s()", Py_TYPE(self)->tp_name);
        goto cleanup;
    }

    parts = PyList_New(nfields + 1);
    if (parts == NULL)
        goto cleanup;

    part = PyUnicode_FromFormat("%s(", Py_TYPE(self)->tp_name);
    if (part == NULL)
        goto cleanup;
    PyList_SET_ITEM(parts, 0, part);

    for (i = 0; i < nfields; i++) {
        field = PyTuple_GET_ITEM(fields, i);
        val = Struct_get_index(self, i);
        if (val == NULL)
            goto cleanup;

        if (i == (nfields - 1)) {
            part = PyUnicode_FromFormat("%U=%R)", field, val);
        } else {
            part = PyUnicode_FromFormat("%U=%R, ", field, val);
        }
        if (part == NULL)
            goto cleanup;
        PyList_SET_ITEM(parts, i + 1, part);
    }
    empty = PyUnicode_FromString("");
    if (empty == NULL)
        goto cleanup;
    out = PyUnicode_Join(empty, parts);

cleanup:
    Py_XDECREF(parts);
    Py_XDECREF(empty);
    Py_ReprLeave(self);
    return out;
}

/* Hash algorithm borrowed from cpython 3.10's hashing algorithm for tuples.
 * See https://github.com/python/cpython/blob/4bcef2bb48b3fd82011a89c1c716421b789f1442/Objects/tupleobject.c#L386-L424
 */
#if SIZEOF_PY_UHASH_T > 4
#define MS_HASH_XXPRIME_1 ((Py_uhash_t)11400714785074694791ULL)
#define MS_HASH_XXPRIME_2 ((Py_uhash_t)14029467366897019727ULL)
#define MS_HASH_XXPRIME_5 ((Py_uhash_t)2870177450012600261ULL)
#define MS_HASH_XXROTATE(x) ((x << 31) | (x >> 33))  /* Rotate left 31 bits */
#else
#define MS_HASH_XXPRIME_1 ((Py_uhash_t)2654435761UL)
#define MS_HASH_XXPRIME_2 ((Py_uhash_t)2246822519UL)
#define MS_HASH_XXPRIME_5 ((Py_uhash_t)374761393UL)
#define MS_HASH_XXROTATE(x) ((x << 13) | (x >> 19))  /* Rotate left 13 bits */
#endif

static Py_hash_t
Struct_hash(PyObject *self) {
    PyObject *val;
    Py_ssize_t i, nfields;
    Py_uhash_t acc = MS_HASH_XXPRIME_5;

    if (((StructMetaObject *)Py_TYPE(self))->frozen != OPT_TRUE) {
        PyErr_Format(PyExc_TypeError, "unhashable type: '%s'", Py_TYPE(self)->tp_name);
        return -1;
    }

    nfields = StructMeta_GET_NFIELDS(Py_TYPE(self));

    for (i = 0; i < nfields; i++) {
        Py_uhash_t lane;
        val = Struct_get_index(self, i);
        if (val == NULL) return -1;
        lane = PyObject_Hash(val);
        if (lane == (Py_uhash_t)-1) return -1;
        acc += lane * MS_HASH_XXPRIME_2;
        acc = MS_HASH_XXROTATE(acc);
        acc *= MS_HASH_XXPRIME_1;
    }
    acc += nfields ^ (MS_HASH_XXPRIME_5 ^ 3527539UL);
    return (acc == (Py_uhash_t)-1) ?  1546275796 : acc;
}

static PyObject *
Struct_richcompare(PyObject *self, PyObject *other, int op) {
    int status;
    PyObject *left, *right;
    Py_ssize_t nfields, i;

    if (!(Py_TYPE(Py_TYPE(other)) == &StructMetaType)) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    if (op != Py_EQ && op != Py_NE) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    status = Py_TYPE(self) == Py_TYPE(other);
    if (status == 0)
        goto done;

    nfields = StructMeta_GET_NFIELDS(Py_TYPE(self));

    for (i = 0; i < nfields; i++) {
        left = Struct_get_index(self, i);
        if (left == NULL)
            return NULL;
        right = Struct_get_index(other, i);
        if (right == NULL)
            return NULL;
        Py_INCREF(left);
        Py_INCREF(right);
        status = PyObject_RichCompareBool(left, right, Py_EQ);
        Py_DECREF(left);
        Py_DECREF(right);
        if (status < 0)
            return NULL;
        if (status == 0)
            goto done;
    }
done:
    if (status == ((op == Py_EQ) ? 1 : 0)) {
        Py_RETURN_TRUE;
    } else {
        Py_RETURN_FALSE;
    }
}

static PyObject *
Struct_copy(PyObject *self, PyObject *args)
{
    Py_ssize_t i, nfields;
    PyObject *val, *res = NULL;

    res = Struct_alloc(Py_TYPE(self));
    if (res == NULL)
        return NULL;

    nfields = StructMeta_GET_NFIELDS(Py_TYPE(self));
    for (i = 0; i < nfields; i++) {
        val = Struct_get_index(self, i);
        if (val == NULL)
            goto error;
        Py_INCREF(val);
        Struct_set_index(res, i, val);
    }
    /* If self is untracked, then copy is untracked */
    if (PyObject_IS_GC(self) && !IS_TRACKED(self))
        PyObject_GC_UnTrack(res);
    return res;
error:
    Py_DECREF(res);
    return NULL;
}

static PyObject *
Struct_reduce(PyObject *self, PyObject *args)
{
    Py_ssize_t i, nfields;
    PyObject *values, *val;

    nfields = StructMeta_GET_NFIELDS(Py_TYPE(self));
    values = PyTuple_New(nfields);
    if (values == NULL)
        return NULL;
    for (i = 0; i < nfields; i++) {
        val = Struct_get_index(self, i);
        if (val == NULL)
            goto error;
        Py_INCREF(val);
        PyTuple_SET_ITEM(values, i, val);
    }
    return PyTuple_Pack(2, Py_TYPE(self), values);
error:
    Py_XDECREF(values);
    return NULL;
}

static PyObject *
StructMixin_fields(PyObject *self, void *closure) {
    PyObject *out;
    out = StructMeta_GET_FIELDS(Py_TYPE(self));
    Py_INCREF(out);
    return out;
}

static PyObject *
StructMixin_defaults(PyObject *self, void *closure) {
    PyObject *out;
    out = StructMeta_GET_DEFAULTS(Py_TYPE(self));
    Py_INCREF(out);
    return out;
}

static PyMethodDef Struct_methods[] = {
    {"__copy__", Struct_copy, METH_NOARGS, "copy a struct"},
    {"__reduce__", Struct_reduce, METH_NOARGS, "reduce a struct"},
    {NULL, NULL},
};

static PyGetSetDef StructMixin_getset[] = {
    {"__struct_fields__", (getter) StructMixin_fields, NULL, "Struct fields", NULL},
    {"__struct_defaults__", (getter) StructMixin_defaults, NULL, "Struct defaults", NULL},
    {NULL},
};

static PyTypeObject StructMixinType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec._core._StructMixin",
    .tp_basicsize = 0,
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_setattro = Struct_setattro,
    .tp_repr = Struct_repr,
    .tp_richcompare = Struct_richcompare,
    .tp_hash = Struct_hash,
    .tp_methods = Struct_methods,
    .tp_getset = StructMixin_getset,
};

PyDoc_STRVAR(Struct__doc__,
"A base class for defining efficient serializable objects.\n"
"\n"
"Fields are defined using type annotations. Fields may optionally have\n"
"default values, which result in keyword parameters to the constructor.\n"
"Note that mutable default values are deepcopied in the constructor to\n"
"prevent accidental sharing.\n"
"\n"
"Additional class options can be enabled by passing keywords to the class\n"
"definition (see example below). The following options exist:\n"
"\n"
"- ``frozen``: whether instances of the class are pseudo-immutable. If true,\n"
"  attribute assignment is disabled and a corresponding ``__hash__`` is defined.\n"
"- ``asarray``: whether instances of the class should be serialized as\n"
"  arrays rather than dicts (the default).\n"
"\n"
"Structs automatically define ``__init__``, ``__eq__``, ``__repr__``, and\n"
"``__copy__`` methods. Additional methods can be defined on the class as\n"
"needed. Note that ``__init__``/``__new__`` cannot be overridden, but other\n"
"methods can. A tuple of the field names is available on the class via the\n"
"``__struct_fields__`` attribute if needed.\n"
"\n"
"Examples\n"
"--------\n"
"Here we define a new `Struct` type for describing a dog. It has three fields;\n"
"two required and one optional.\n"
"\n"
">>> class Dog(Struct):\n"
"...     name: str\n"
"...     breed: str\n"
"...     is_good_boy: bool = True\n"
"...\n"
">>> Dog('snickers', breed='corgi')\n"
"Dog(name='snickers', breed='corgi', is_good_boy=True)\n"
"\n"
"Additional struct options can be set as part of the class definition. Here\n"
"we define a new `Struct` type for a frozen `Point` object.\n"
"\n"
">>> class Point(Struct, frozen=True):\n"
"...     x: float\n"
"...     y: float\n"
"...\n"
">>> {Point(1.5, 2.0): 1}  # frozen structs are hashable\n"
"{Point(1.5, 2.0): 1}"
);

/*************************************************************************
 * Ext                                                               *
 *************************************************************************/

typedef struct Ext {
    PyObject_HEAD
    char code;
    PyObject *data;
} Ext;

static PyObject *
Ext_New(char code, PyObject *data) {
    Ext *out = (Ext *)Ext_Type.tp_alloc(&Ext_Type, 0);
    if (out == NULL)
        return NULL;

    out->code = code;
    Py_INCREF(data);
    out->data = data;
    return (PyObject *)out;
}

PyDoc_STRVAR(Ext__doc__,
"Ext(code, data)\n"
"--\n"
"\n"
"A record representing a MessagePack Extension Type.\n"
"\n"
"Parameters\n"
"----------\n"
"code : int\n"
"    The integer type code for this extension. Must be between -128 and 127.\n"
"data : bytes, bytearray, or memoryview\n"
"    The byte buffer for this extension. One of bytes, bytearray, memoryview,\n"
"    or any object that implements the buffer protocol."
);
static PyObject *
Ext_new(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
    PyObject *pycode, *data;
    char code;
    Py_ssize_t nargs, nkwargs;

    nargs = PyTuple_GET_SIZE(args);
    nkwargs = (kwargs == NULL) ? 0 : PyDict_GET_SIZE(kwargs);

    if (nkwargs != 0) {
        PyErr_SetString(
            PyExc_TypeError,
            "Ext takes no keyword arguments"
        );
        return NULL;
    }
    else if (nargs != 2) {
        PyErr_Format(
            PyExc_TypeError,
            "Ext expected 2 arguments, got %zd",
            nargs
        );
        return NULL;
    }

    pycode = PyTuple_GET_ITEM(args, 0);
    data = PyTuple_GET_ITEM(args, 1);

    if (PyLong_CheckExact(pycode)) {
        long val = PyLong_AsLong(pycode);
        if ((val == -1 && PyErr_Occurred()) || val > 127 || val < -128) {
            PyErr_SetString(
                PyExc_ValueError,
                "code must be an int between -128 and 127"
            );
            return NULL;
        }
        else {
            code = val;
        }
    }
    else {
        PyErr_Format(
            PyExc_TypeError,
            "code must be an int, got %.200s",
            Py_TYPE(pycode)->tp_name
        );
        return NULL;
    }
    if (!(PyBytes_CheckExact(data) || PyByteArray_CheckExact(data) || PyObject_CheckBuffer(data))) {
        PyErr_Format(
            PyExc_TypeError,
            "data must be a bytes, bytearray, or buffer-like object, got %.200s",
            Py_TYPE(data)->tp_name
        );
        return NULL;
    }
    return Ext_New(code, data);
}

static void
Ext_dealloc(Ext *self)
{
    Py_XDECREF(self->data);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyMemberDef Ext_members[] = {
    {"code", T_BYTE, offsetof(Ext, code), READONLY, "The extension type code"},
    {"data", T_OBJECT_EX, offsetof(Ext, data), READONLY, "The extension data payload"},
    {NULL},
};

static PyObject *
Ext_reduce(PyObject *self, PyObject *unused)
{
    return Py_BuildValue("O(bO)", Py_TYPE(self), ((Ext*)self)->code, ((Ext*)self)->data);
}

static PyObject *
Ext_richcompare(PyObject *self, PyObject *other, int op) {
    int status;
    PyObject *out;
    Ext *ex_self, *ex_other;

    if (Py_TYPE(other) != &Ext_Type) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    if (op != Py_EQ && op != Py_NE) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    ex_self = (Ext *)self;
    ex_other = (Ext *)other;

    status = ex_self->code == ex_other->code;
    if (!status) {
        out = (op == Py_EQ) ? Py_False : Py_True;
    }
    else {
        status = PyObject_RichCompareBool(ex_self->data, ex_other->data, op);
        if (status == -1) return NULL;
        out = status ? Py_True : Py_False;
    }
    Py_INCREF(out);
    return out;
}

static PyMethodDef Ext_methods[] = {
    {"__reduce__", Ext_reduce, METH_NOARGS, "reduce an Ext"},
    {NULL, NULL},
};

static PyTypeObject Ext_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec.msgpack.Ext",
    .tp_doc = Ext__doc__,
    .tp_basicsize = sizeof(Ext),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | _Py_TPFLAGS_HAVE_VECTORCALL,
    .tp_new = Ext_new,
    .tp_dealloc = (destructor) Ext_dealloc,
    .tp_call = PyVectorcall_Call,
    .tp_richcompare = Ext_richcompare,
    .tp_members = Ext_members,
    .tp_methods = Ext_methods
};


/*************************************************************************
 * Shared Encoder structs/methods                                        *
 *************************************************************************/

typedef struct EncoderState {
    PyObject *enc_hook;     /* `enc_hook` callback */
    Py_ssize_t write_buffer_size;  /* Configured internal buffer size */

    PyObject *output_buffer;    /* bytes or bytearray storing the output */
    char *output_buffer_raw;    /* raw pointer to output_buffer internal buffer */
    Py_ssize_t output_len;      /* Length of output_buffer */
    Py_ssize_t max_output_len;  /* Allocation size of output_buffer */
    char* (*resize_buffer)(PyObject**, Py_ssize_t);  /* callback for resizing buffer */
} EncoderState;

typedef struct Encoder {
    PyObject_HEAD
    EncoderState state;
} Encoder;

static char*
ms_resize_bytes(PyObject** output_buffer, Py_ssize_t size)
{
    int status = _PyBytes_Resize(output_buffer, size);
    if (status < 0) return NULL;
    return PyBytes_AS_STRING(*output_buffer);
}

static char*
ms_resize_bytearray(PyObject** output_buffer, Py_ssize_t size)
{
    int status = PyByteArray_Resize(*output_buffer, size);
    if (status < 0) return NULL;
    return PyByteArray_AS_STRING(*output_buffer);
}

static MS_NOINLINE int
ms_resize(EncoderState *self, Py_ssize_t size)
{
    self->max_output_len = Py_MAX(8, 1.5 * size);
    char *new_buf = self->resize_buffer(&self->output_buffer, self->max_output_len);
    if (new_buf == NULL) return -1;
    self->output_buffer_raw = new_buf;
    return 0;
}

static MS_INLINE int
ms_ensure_space(EncoderState *self, Py_ssize_t size) {
    Py_ssize_t required = self->output_len + size;
    if (required > self->max_output_len) {
        return ms_resize(self, required);
    }
    return 0;
}

static MS_INLINE int
ms_write(EncoderState *self, const char *s, Py_ssize_t n)
{
    Py_ssize_t required = self->output_len + n;
    if (MS_UNLIKELY(required > self->max_output_len)) {
        if (ms_resize(self, required) < 0) return -1;
    }
    memcpy(self->output_buffer_raw + self->output_len, s, n);
    self->output_len += n;
    return 0;
}

static int
Encoder_init(Encoder *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"enc_hook", "write_buffer_size", NULL};
    Py_ssize_t write_buffer_size = 512;
    PyObject *enc_hook = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|$On", kwlist,
                                     &enc_hook, &write_buffer_size)) {
        return -1;
    }

    if (enc_hook == Py_None) {
        enc_hook = NULL;
    }
    if (enc_hook != NULL) {
        if (!PyCallable_Check(enc_hook)) {
            PyErr_SetString(PyExc_TypeError, "enc_hook must be callable");
            return -1;
        }
        Py_INCREF(enc_hook);
    }
    self->state.enc_hook = enc_hook;
    self->state.write_buffer_size = Py_MAX(write_buffer_size, 32);
    self->state.max_output_len = self->state.write_buffer_size;
    self->state.output_len = 0;
    self->state.output_buffer = NULL;
    self->state.resize_buffer = &ms_resize_bytes;
    return 0;
}

static int
Encoder_clear(Encoder *self)
{
    Py_CLEAR(self->state.output_buffer);
    Py_CLEAR(self->state.enc_hook);
    return 0;
}

static void
Encoder_dealloc(Encoder *self)
{
    Encoder_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
Encoder_traverse(Encoder *self, visitproc visit, void *arg)
{
    Py_VISIT(self->state.enc_hook);
    return 0;
}

static PyObject*
Encoder_sizeof(Encoder *self)
{
    Py_ssize_t res;

    res = sizeof(Encoder);
    if (self->state.output_buffer != NULL) {
        res += self->state.max_output_len;
    }
    return PyLong_FromSsize_t(res);
}

PyDoc_STRVAR(Encoder_encode_into__doc__,
"encode_into(self, obj, buffer, offset=0, /)\n"
"--\n"
"\n"
"Serialize an object into an existing bytearray buffer.\n"
"\n"
"Upon success, the buffer will be truncated to the end of the serialized\n"
"message. Note that the underlying memory buffer *won't* be truncated,\n"
"allowing for efficiently appending additional bytes later.\n"
"\n"
"Parameters\n"
"----------\n"
"obj : Any\n"
"    The object to serialize.\n"
"buffer : bytearray\n"
"    The buffer to serialize into.\n"
"offset : int, optional\n"
"    The offset into the buffer to start writing at. Defaults to 0. Set to -1\n"
"    to start writing at the end of the buffer.\n"
"\n"
"Returns\n"
"-------\n"
"None"
);
static PyObject*
encoder_encode_into_common(
    EncoderState *state,
    PyObject *const *args,
    Py_ssize_t nargs,
    int(*encode)(EncoderState*, PyObject*)
)
{
    int status;
    PyObject *obj, *old_buf, *buf;
    Py_ssize_t buf_size, offset = 0;

    if (!check_positional_nargs(nargs, 2, 3)) {
        return NULL;
    }
    obj = args[0];
    buf = args[1];
    if (!PyByteArray_CheckExact(buf)) {
        PyErr_SetString(PyExc_TypeError, "buffer must be a `bytearray`");
        return NULL;
    }
    buf_size = PyByteArray_GET_SIZE(buf);

    if (nargs == 3) {
        offset = PyLong_AsSsize_t(args[2]);
        if (offset == -1) {
            if (PyErr_Occurred()) return NULL;
            offset = buf_size;
        }
        if (offset < 0) {
            PyErr_SetString(PyExc_ValueError, "offset must be >= -1");
            return NULL;
        }
        if (offset > buf_size) {
            offset = buf_size;
        }
    }

    /* Setup buffer */
    old_buf = state->output_buffer;
    state->output_buffer = buf;
    state->output_buffer_raw = PyByteArray_AS_STRING(buf);
    state->resize_buffer = &ms_resize_bytearray;
    state->output_len = offset;
    state->max_output_len = buf_size;

    status = encode(state, obj);

    /* Reset buffer */
    state->output_buffer = old_buf;
    state->resize_buffer = &ms_resize_bytes;
    if (old_buf != NULL) {
        state->output_buffer_raw = PyBytes_AS_STRING(old_buf);
    }

    if (status == 0) {
        FAST_BYTEARRAY_SHRINK(buf, state->output_len);
        Py_RETURN_NONE;
    }
    return NULL;
}

PyDoc_STRVAR(Encoder_encode__doc__,
"encode(self, obj)\n"
"--\n"
"\n"
"Serialize an object to bytes.\n"
"\n"
"Parameters\n"
"----------\n"
"obj : Any\n"
"    The object to serialize.\n"
"\n"
"Returns\n"
"-------\n"
"data : bytes\n"
"    The serialized object.\n"
);
static PyObject*
encoder_encode_common(
    EncoderState *state,
    PyObject *const *args,
    Py_ssize_t nargs,
    int(*encode)(EncoderState*, PyObject*)
)
{
    int status;
    PyObject *res = NULL;

    if (!check_positional_nargs(nargs, 1, 1)) {
        return NULL;
    }

    /* reset buffer */
    state->output_len = 0;
    if (state->output_buffer == NULL) {
        state->max_output_len = state->write_buffer_size;
        state->output_buffer = PyBytes_FromStringAndSize(NULL, state->max_output_len);
        if (state->output_buffer == NULL) return NULL;
        state->output_buffer_raw = PyBytes_AS_STRING(state->output_buffer);
    }

    status = encode(state, args[0]);

    if (status == 0) {
        if (state->max_output_len > state->write_buffer_size) {
            /* Buffer was resized, trim to length */
            res = state->output_buffer;
            state->output_buffer = NULL;
            FAST_BYTES_SHRINK(res, state->output_len);
        }
        else {
            /* Only constant buffer used, copy to output */
            res = PyBytes_FromStringAndSize(
                PyBytes_AS_STRING(state->output_buffer),
                state->output_len
            );
        }
    } else {
        /* Error in encode, drop buffer if necessary */
        if (state->max_output_len > state->write_buffer_size) {
            Py_DECREF(state->output_buffer);
            state->output_buffer = NULL;
        }
    }
    return res;
}

static PyObject*
encode_common(
    PyObject *const *args,
    Py_ssize_t nargs,
    PyObject *kwnames,
    int(*encode)(EncoderState*, PyObject*)
)
{
    int status;
    PyObject *enc_hook = NULL, *res = NULL;
    EncoderState state;

    /* Parse arguments */
    if (!check_positional_nargs(nargs, 1, 1)) return NULL;
    if (kwnames != NULL) {
        Py_ssize_t nkwargs = PyTuple_GET_SIZE(kwnames);
        MsgspecState *st = msgspec_get_global_state();
        if ((enc_hook = find_keyword(kwnames, args + nargs, st->str_enc_hook)) != NULL) nkwargs--;
        if (nkwargs > 0) {
            PyErr_SetString(
                PyExc_TypeError,
                "Extra keyword arguments provided"
            );
            return NULL;
        }
    }

    if (enc_hook == Py_None) {
        enc_hook = NULL;
    }
    if (enc_hook != NULL) {
        if (!PyCallable_Check(enc_hook)) {
            PyErr_SetString(PyExc_TypeError, "enc_hook must be callable");
            return NULL;
        }
    }
    state.enc_hook = enc_hook;

    /* use a smaller buffer size here to reduce chance of over allocating for one-off calls */
    state.write_buffer_size = 32;
    state.max_output_len = state.write_buffer_size;
    state.output_len = 0;
    state.output_buffer = PyBytes_FromStringAndSize(NULL, state.max_output_len);
    if (state.output_buffer == NULL) return NULL;
    state.output_buffer_raw = PyBytes_AS_STRING(state.output_buffer);
    state.resize_buffer = &ms_resize_bytes;

    status = encode(&state, args[0]);

    if (status == 0) {
        /* Trim output to length */
        res = state.output_buffer;
        FAST_BYTES_SHRINK(res, state.output_len);
    } else {
        /* Error in encode, drop buffer */
        Py_CLEAR(state.output_buffer);
    }
    return res;
}

static PyMemberDef Encoder_members[] = {
    {"enc_hook", T_OBJECT, offsetof(Encoder, state.enc_hook), READONLY, "The encoder enc_hook"},
    {"write_buffer_size", T_PYSSIZET, offsetof(Encoder, state.write_buffer_size),
        READONLY, "The encoder write buffer size"},
    {NULL},
};

/*************************************************************************
 * Shared Decoding Utilities                                             *
 *************************************************************************/

static MS_NOINLINE PyObject *
ms_decode_enum(PyObject *val, TypeNode *type, PathNode *path) {
    if (val == NULL) return NULL;
    PyObject *enum_obj = TypeNode_get_enum(type);
    PyObject *out = PyObject_GetAttr(enum_obj, val);
    Py_DECREF(val);
    if (MS_UNLIKELY(out == NULL)) {
        PyErr_Clear();
        ms_raise_validation_error(path, "Invalid enum value '%U'%U", val);
    }
    return out;
}

static PyObject *
ms_decode_intenum(PyObject *val, TypeNode *type, PathNode *path) {
    if (val == NULL) return NULL;

    PyObject *out = NULL;
    MsgspecState *st = msgspec_get_global_state();
    PyObject *intenum = TypeNode_get_intenum(type);

    /* Fast path for common case. This accesses a non-public member of the
    * enum class to speedup lookups. If this fails, we clear errors and
    * use the slower-but-more-public method instead. */
    PyObject *member_table = PyObject_GetAttr(intenum, st->str__value2member_map_);
    if (MS_LIKELY(member_table != NULL)) {
        out = PyDict_GetItem(member_table, val);
        Py_DECREF(member_table);
        if (MS_LIKELY(out != NULL)) {
            Py_DECREF(val);
            Py_INCREF(out);
            return out;
        }
    }
    PyErr_Clear();
    out = CALL_ONE_ARG(intenum, val);
    if (MS_UNLIKELY(out == NULL)) {
        PyErr_Clear();
        ms_raise_validation_error(path, "Invalid enum value `%S`%U", val);
    }
    Py_DECREF(val);
    return out;
}

static PyObject *
ms_decode_custom(PyObject *obj, PyObject *dec_hook, bool generic, TypeNode* type, PathNode *path) {
    PyObject *custom_cls = NULL, *custom_obj, *out = NULL;
    int status;

    if (obj == NULL) return NULL;

    custom_obj = TypeNode_get_custom(type);

    if (dec_hook != NULL) {
        out = PyObject_CallFunctionObjArgs(dec_hook, custom_obj, obj, NULL);
        Py_DECREF(obj);
        if (out == NULL)
            return NULL;
    }
    else {
        out = obj;
    }

    /* Generic classes must be checked based on __origin__ */
    if (generic) {
        MsgspecState *st = msgspec_get_global_state();
        custom_cls = PyObject_GetAttr(custom_obj, st->str___origin__);
        if (custom_cls == NULL) {
            Py_DECREF(out);
            return NULL;
        }
    }
    else {
        custom_cls = custom_obj;
    }

    /* Check that the decoded value matches the expected type */
    status = PyObject_IsInstance(out, custom_cls);
    if (status == 0) {
        ms_raise_validation_error(
            path,
            "Expected `%s`, got `%s`%U",
            ((PyTypeObject *)custom_cls)->tp_name,
            Py_TYPE(out)->tp_name
        );
        Py_CLEAR(out);
    }
    else if (status == -1) {
        Py_CLEAR(out);
    }

    if (generic) {
        Py_DECREF(custom_cls);
    }
    return out;
}


/*************************************************************************
 * MessagePack Encoder                                                   *
 *************************************************************************/

PyDoc_STRVAR(Encoder__doc__,
"Encoder(*, enc_hook=None, write_buffer_size=512)\n"
"--\n"
"\n"
"A MessagePack encoder.\n"
"\n"
"Parameters\n"
"----------\n"
"enc_hook : callable, optional\n"
"    A callable to call for objects that aren't supported msgspec types. Takes the\n"
"    unsupported object and should return a supported object, or raise a TypeError.\n"
"write_buffer_size : int, optional\n"
"    The size of the internal static write buffer."
);

enum mpack_code {
    MP_NIL = '\xc0',
    MP_FALSE = '\xc2',
    MP_TRUE = '\xc3',
    MP_FLOAT32 = '\xca',
    MP_FLOAT64 = '\xcb',
    MP_UINT8 = '\xcc',
    MP_UINT16 = '\xcd',
    MP_UINT32 = '\xce',
    MP_UINT64 = '\xcf',
    MP_INT8 = '\xd0',
    MP_INT16 = '\xd1',
    MP_INT32 = '\xd2',
    MP_INT64 = '\xd3',
    MP_FIXSTR = '\xa0',
    MP_STR8 = '\xd9',
    MP_STR16 = '\xda',
    MP_STR32 = '\xdb',
    MP_BIN8 = '\xc4',
    MP_BIN16 = '\xc5',
    MP_BIN32 = '\xc6',
    MP_FIXARRAY = '\x90',
    MP_ARRAY16 = '\xdc',
    MP_ARRAY32 = '\xdd',
    MP_FIXMAP = '\x80',
    MP_MAP16 = '\xde',
    MP_MAP32 = '\xdf',
    MP_FIXEXT1 = '\xd4',
    MP_FIXEXT2 = '\xd5',
    MP_FIXEXT4 = '\xd6',
    MP_FIXEXT8 = '\xd7',
    MP_FIXEXT16 = '\xd8',
    MP_EXT8 = '\xc7',
    MP_EXT16 = '\xc8',
    MP_EXT32 = '\xc9',
};

static int mpack_encode(EncoderState *self, PyObject *obj);

static int
mpack_encode_none(EncoderState *self)
{
    const char op = MP_NIL;
    return ms_write(self, &op, 1);
}

static int
mpack_encode_bool(EncoderState *self, PyObject *obj)
{
    const char op = (obj == Py_True) ? MP_TRUE : MP_FALSE;
    return ms_write(self, &op, 1);
}

static int
mpack_encode_long(EncoderState *self, PyObject *obj)
{
    int overflow;
    int64_t x = PyLong_AsLongLongAndOverflow(obj, &overflow);
    uint64_t ux = x;
    if (overflow != 0) {
        if (overflow > 0) {
            ux = PyLong_AsUnsignedLongLong(obj);
            x = (1ULL << 63) - 1ULL;
            if (ux == ((uint64_t)(-1)) && PyErr_Occurred()) {
                return -1;
            }
        } else {
            PyErr_SetString(PyExc_OverflowError, "can't serialize ints < -2**63");
            return -1;
        }
    }
    else if (x == -1 && PyErr_Occurred()) {
        return -1;
    }

    if(x < -(1LL<<5)) {
        if(x < -(1LL<<15)) {
            if(x < -(1LL<<31)) {
                char buf[9];
                buf[0] = MP_INT64;
                _msgspec_store64(&buf[1], x);
                return ms_write(self, buf, 9);
            } else {
                char buf[5];
                buf[0] = MP_INT32;
                _msgspec_store32(&buf[1], (int32_t)x);
                return ms_write(self, buf, 5);
            }
        } else {
            if(x < -(1<<7)) {
                char buf[3];
                buf[0] = MP_INT16;
                _msgspec_store16(&buf[1], (int16_t)x);
                return ms_write(self, buf, 3);
            } else {
                char buf[2] = {MP_INT8, (x & 0xff)};
                return ms_write(self, buf, 2);
            }
        }
    } else if(x < (1<<7)) {
        char buf[1] = {(x & 0xff)};
        return ms_write(self, buf, 1);
    } else {
        if(x < (1<<16)) {
            if(x < (1<<8)) {
                char buf[2] = {MP_UINT8, (x & 0xff)};
                return ms_write(self, buf, 2);
            } else {
                char buf[3];
                buf[0] = MP_UINT16;
                _msgspec_store16(&buf[1], (uint16_t)x);
                return ms_write(self, buf, 3);
            }
        } else {
            if(x < (1LL<<32)) {
                char buf[5];
                buf[0] = MP_UINT32;
                _msgspec_store32(&buf[1], (uint32_t)x);
                return ms_write(self, buf, 5);
            } else {
                char buf[9];
                buf[0] = MP_UINT64;
                _msgspec_store64(&buf[1], ux);
                return ms_write(self, buf, 9);
            }
        }
    }
}

static int
mpack_encode_float(EncoderState *self, PyObject *obj)
{
    char buf[9];
    double x = PyFloat_AS_DOUBLE(obj);
    uint64_t ux = 0;
    memcpy(&ux, &x, sizeof(double));
    buf[0] = MP_FLOAT64;
    _msgspec_store64(&buf[1], ux);
    return ms_write(self, buf, 9);
}

static int
mpack_encode_str(EncoderState *self, PyObject *obj)
{
    Py_ssize_t len;
    const char* buf = unicode_str_and_size(obj, &len);
    if (buf == NULL) {
        return -1;
    }
    if (len < 32) {
        char header[1] = {MP_FIXSTR | (uint8_t)len};
        if (ms_write(self, header, 1) < 0)
            return -1;
    } else if (len < (1 << 8)) {
        char header[2] = {MP_STR8, (uint8_t)len};
        if (ms_write(self, header, 2) < 0)
            return -1;
    } else if (len < (1 << 16)) {
        char header[3];
        header[0] = MP_STR16;
        _msgspec_store16(&header[1], (uint16_t)len);
        if (ms_write(self, header, 3) < 0)
            return -1;
    } else if (len < (1LL << 32)) {
        char header[5];
        header[0] = MP_STR32;
        _msgspec_store32(&header[1], (uint32_t)len);
        if (ms_write(self, header, 5) < 0)
            return -1;
    } else {
        PyErr_SetString(
            msgspec_get_global_state()->EncodeError,
            "Can't encode strings longer than 2**32 - 1"
        );
        return -1;
    }
    return len > 0 ? ms_write(self, buf, len) : 0;
}

static int
mpack_encode_bin(EncoderState *self, const char* buf, Py_ssize_t len) {
    if (buf == NULL) {
        return -1;
    }
    if (len < (1 << 8)) {
        char header[2] = {MP_BIN8, (uint8_t)len};
        if (ms_write(self, header, 2) < 0)
            return -1;
    } else if (len < (1 << 16)) {
        char header[3];
        header[0] = MP_BIN16;
        _msgspec_store16(&header[1], (uint16_t)len);
        if (ms_write(self, header, 3) < 0)
            return -1;
    } else if (len < (1LL << 32)) {
        char header[5];
        header[0] = MP_BIN32;
        _msgspec_store32(&header[1], (uint32_t)len);
        if (ms_write(self, header, 5) < 0)
            return -1;
    } else {
        PyErr_SetString(
            msgspec_get_global_state()->EncodeError,
            "Can't encode bytes-like objects longer than 2**32 - 1"
        );
        return -1;
    }
    return len > 0 ? ms_write(self, buf, len) : 0;
}

static int
mpack_encode_bytes(EncoderState *self, PyObject *obj)
{
    Py_ssize_t len = PyBytes_GET_SIZE(obj);
    const char* buf = PyBytes_AS_STRING(obj);
    return mpack_encode_bin(self, buf, len);
}

static int
mpack_encode_bytearray(EncoderState *self, PyObject *obj)
{
    Py_ssize_t len = PyByteArray_GET_SIZE(obj);
    const char* buf = PyByteArray_AS_STRING(obj);
    return mpack_encode_bin(self, buf, len);
}

static int
mpack_encode_memoryview(EncoderState *self, PyObject *obj)
{
    int out;
    Py_buffer buffer;
    if (PyObject_GetBuffer(obj, &buffer, PyBUF_CONTIG_RO) < 0) return -1;
    out = mpack_encode_bin(self, buffer.buf, buffer.len);
    PyBuffer_Release(&buffer);
    return out;
}

static int
mpack_encode_array_header(EncoderState *self, Py_ssize_t len, const char* typname)
{
    if (len < 16) {
        char header[1] = {MP_FIXARRAY | len};
        if (ms_write(self, header, 1) < 0)
            return -1;
    } else if (len < (1 << 16)) {
        char header[3];
        header[0] = MP_ARRAY16;
        _msgspec_store16(&header[1], (uint16_t)len);
        if (ms_write(self, header, 3) < 0)
            return -1;
    } else if (len < (1LL << 32)) {
        char header[5];
        header[0] = MP_ARRAY32;
        _msgspec_store32(&header[1], (uint32_t)len);
        if (ms_write(self, header, 5) < 0)
            return -1;
    } else {
        PyErr_Format(
            msgspec_get_global_state()->EncodeError,
            "Can't encode %s longer than 2**32 - 1",
            typname
        );
        return -1;
    }
    return 0;
}

static int
mpack_encode_list(EncoderState *self, PyObject *obj)
{
    Py_ssize_t i, len;
    int status = 0;

    len = PyList_GET_SIZE(obj);
    if (mpack_encode_array_header(self, len, "list") < 0)
        return -1;
    if (len == 0)
        return 0;
    if (Py_EnterRecursiveCall(" while serializing an object"))
        return -1;
    for (i = 0; i < len; i++) {
        if (mpack_encode(self, PyList_GET_ITEM(obj, i)) < 0) {
            status = -1;
            break;
        }
    }
    Py_LeaveRecursiveCall();
    return status;
}

static int
mpack_encode_set(EncoderState *self, PyObject *obj)
{
    Py_ssize_t len, ppos = 0;
    Py_hash_t hash;
    PyObject *item;
    int status = 0;

    len = PySet_GET_SIZE(obj);
    if (mpack_encode_array_header(self, len, "set") < 0)
        return -1;
    if (len == 0)
        return 0;
    if (Py_EnterRecursiveCall(" while serializing an object"))
        return -1;
    while (_PySet_NextEntry(obj, &ppos, &item, &hash)) {
        if (mpack_encode(self, item) < 0) {
            status = -1;
            break;
        }
    }
    Py_LeaveRecursiveCall();
    return status;
}

static int
mpack_encode_tuple(EncoderState *self, PyObject *obj)
{
    Py_ssize_t i, len;
    int status = 0;

    len = PyTuple_GET_SIZE(obj);
    if (mpack_encode_array_header(self, len, "tuples") < 0)
        return -1;
    if (len == 0)
        return 0;
    if (Py_EnterRecursiveCall(" while serializing an object"))
        return -1;
    for (i = 0; i < len; i++) {
        if (mpack_encode(self, PyTuple_GET_ITEM(obj, i)) < 0) {
            status = -1;
            break;
        }
    }
    Py_LeaveRecursiveCall();
    return status;
}

static int
mpack_encode_map_header(EncoderState *self, Py_ssize_t len, const char* typname)
{
    if (len < 16) {
        char header[1] = {MP_FIXMAP | len};
        if (ms_write(self, header, 1) < 0)
            return -1;
    } else if (len < (1 << 16)) {
        char header[3];
        header[0] = MP_MAP16;
        _msgspec_store16(&header[1], (uint16_t)len);
        if (ms_write(self, header, 3) < 0)
            return -1;
    } else if (len < (1LL << 32)) {
        char header[5];
        header[0] = MP_MAP32;
        _msgspec_store32(&header[1], (uint32_t)len);
        if (ms_write(self, header, 5) < 0)
            return -1;
    } else {
        PyErr_Format(
            msgspec_get_global_state()->EncodeError,
            "Can't encode %s longer than 2**32 - 1",
            typname
        );
        return -1;
    }
    return 0;
}

static int
mpack_encode_dict(EncoderState *self, PyObject *obj)
{
    PyObject *key, *val;
    Py_ssize_t len, pos = 0;
    int status = 0;

    len = PyDict_GET_SIZE(obj);
    if (mpack_encode_map_header(self, len, "dicts") < 0)
        return -1;
    if (len == 0)
        return 0;
    if (Py_EnterRecursiveCall(" while serializing an object"))
        return -1;
    while (PyDict_Next(obj, &pos, &key, &val)) {
        if (mpack_encode(self, key) < 0 || mpack_encode(self, val) < 0) {
            status = -1;
            break;
        }
    }
    Py_LeaveRecursiveCall();
    return status;
}

static int
mpack_encode_struct(EncoderState *self, PyObject *obj)
{
    PyObject *key, *val, *fields;
    Py_ssize_t i, len;
    int status = -1;
    bool asarray = ((StructMetaObject *)Py_TYPE(obj))->asarray == OPT_TRUE;

    fields = StructMeta_GET_FIELDS(Py_TYPE(obj));
    len = PyTuple_GET_SIZE(fields);

    if (asarray) {
        if (mpack_encode_array_header(self, len, "structs") < 0) return -1;
    }
    else {
        if (mpack_encode_map_header(self, len, "structs") < 0) return -1;
    }
    if (len == 0) return 0;
    if (Py_EnterRecursiveCall(" while serializing an object"))
        return -1;
    if (asarray) {
        for (i = 0; i < len; i++) {
            val = Struct_get_index(obj, i);
            if (val == NULL || mpack_encode(self, val) < 0) {
                goto cleanup;
            }
        }
    }
    else {
        for (i = 0; i < len; i++) {
            key = PyTuple_GET_ITEM(fields, i);
            val = Struct_get_index(obj, i);
            if (val == NULL || mpack_encode_str(self, key) < 0 || mpack_encode(self, val) < 0) {
                goto cleanup;
            }
        }
    }

    status = 0;
cleanup:
    Py_LeaveRecursiveCall();
    return status;
}

static int
mpack_encode_ext(EncoderState *self, PyObject *obj)
{
    Ext *ex = (Ext *)obj;
    Py_ssize_t len;
    int status = -1, header_len = 2;
    char header[6];
    const char* data;
    Py_buffer buffer;
    buffer.buf = NULL;

    if (PyBytes_CheckExact(ex->data)) {
        len = PyBytes_GET_SIZE(ex->data);
        data = PyBytes_AS_STRING(ex->data);
    }
    else if (PyByteArray_CheckExact(ex->data)) {
        len = PyByteArray_GET_SIZE(ex->data);
        data = PyByteArray_AS_STRING(ex->data);
    }
    else {
        if (PyObject_GetBuffer(ex->data, &buffer, PyBUF_CONTIG_RO) < 0)
            return -1;
        len = buffer.len;
        data = buffer.buf;
    }
    if (len == 1) {
        header[0] = MP_FIXEXT1;
        header[1] = ex->code;
    }
    else if (len == 2) {
        header[0] = MP_FIXEXT2;
        header[1] = ex->code;
    }
    else if (len == 4) {
        header[0] = MP_FIXEXT4;
        header[1] = ex->code;
    }
    else if (len == 8) {
        header[0] = MP_FIXEXT8;
        header[1] = ex->code;
    }
    else if (len == 16) {
        header[0] = MP_FIXEXT16;
        header[1] = ex->code;
    }
    else if (len < (1<<8)) {
        header[0] = MP_EXT8;
        header[1] = len;
        header[2] = ex->code;
        header_len = 3;
    }
    else if (len < (1<<16)) {
        header[0] = MP_EXT16;
        _msgspec_store16(&header[1], (uint16_t)len);
        header[3] = ex->code;
        header_len = 4;
    }
    else if (len < (1LL<<32)) {
        header[0] = MP_EXT32;
        _msgspec_store32(&header[1], (uint32_t)len);
        header[5] = ex->code;
        header_len = 6;
    }
    else {
        PyErr_SetString(
            msgspec_get_global_state()->EncodeError,
            "Can't encode Ext objects with data longer than 2**32 - 1"
        );
        goto done;
    }
    if (ms_write(self, header, header_len) < 0)
        goto done;
    status = len > 0 ? ms_write(self, data, len) : 0;
done:
    if (buffer.buf != NULL)
        PyBuffer_Release(&buffer);
    return status;
}

static int
mpack_encode_enum(EncoderState *self, PyObject *obj)
{
    if (PyLong_Check(obj))
        return mpack_encode_long(self, obj);

    int status;
    PyObject *name = NULL;
    MsgspecState *st = msgspec_get_global_state();
    /* Try the private variable first for speed, fall back to the public
     * interface if not available */
    name = PyObject_GetAttr(obj, st->str__name_);
    if (name == NULL) {
        PyErr_Clear();
        name = PyObject_GetAttr(obj, st->str_name);
        if (name == NULL)
            return -1;
    }
    if (PyUnicode_CheckExact(name)) {
        status = mpack_encode_str(self, name);
    } else {
        PyErr_SetString(
            msgspec_get_global_state()->EncodeError,
            "Enum's with non-str names aren't supported"
        );
        status = -1;
    }
    Py_DECREF(name);
    return status;
}

static int
mpack_encode_datetime(EncoderState *self, PyObject *obj)
{
    int64_t seconds;
    int32_t nanoseconds;
    PyObject *tzinfo;

    if (!MS_HAS_TZINFO(obj)) {
        PyErr_SetString(
            msgspec_get_global_state()->EncodeError,
            "Can't encode naive datetime objects"
        );
        return -1;
    }

    tzinfo = MS_GET_TZINFO(obj);
    if (tzinfo == PyDateTime_TimeZone_UTC) {
        datetime_to_epoch(obj, &seconds, &nanoseconds);
    }
    else {
        PyObject *temp = PyObject_CallFunctionObjArgs(
            msgspec_get_global_state()->astimezone,
            obj, PyDateTime_TimeZone_UTC, NULL
        );
        if (temp == NULL) return -1;
        datetime_to_epoch(temp, &seconds, &nanoseconds);
        Py_DECREF(temp);
    }

    if ((seconds >> 34) == 0) {
        uint64_t data64 = ((uint64_t)nanoseconds << 34) | (uint64_t)seconds;
        if ((data64 & 0xffffffff00000000L) == 0) {
            /* timestamp 32 */
            char buf[6];
            buf[0] = MP_FIXEXT4;
            buf[1] = -1;
            uint32_t data32 = (uint32_t)data64;
            _msgspec_store32(&buf[2], data32);
            if (ms_write(self, buf, 6) < 0) return -1;
        } else {
            /* timestamp 64 */
            char buf[10];
            buf[0] = MP_FIXEXT8;
            buf[1] = -1;
            _msgspec_store64(&buf[2], data64);
            if (ms_write(self, buf, 10) < 0) return -1;
        }
    } else {
        /* timestamp 96 */
        char buf[15];
        buf[0] = MP_EXT8;
        buf[1] = 12;
        buf[2] = -1;
        _msgspec_store32(&buf[3], nanoseconds);
        _msgspec_store64(&buf[7], seconds);
        if (ms_write(self, buf, 15) < 0) return -1;
    }
    return 0;
}

static int
mpack_encode(EncoderState *self, PyObject *obj)
{
    PyTypeObject *type;
    MsgspecState *st;

    type = Py_TYPE(obj);

    if (obj == Py_None) {
        return mpack_encode_none(self);
    }
    else if (obj == Py_False || obj == Py_True) {
        return mpack_encode_bool(self, obj);
    }
    else if (type == &PyLong_Type) {
        return mpack_encode_long(self, obj);
    }
    else if (type == &PyFloat_Type) {
        return mpack_encode_float(self, obj);
    }
    else if (type == &PyUnicode_Type) {
        return mpack_encode_str(self, obj);
    }
    else if (type == &PyBytes_Type) {
        return mpack_encode_bytes(self, obj);
    }
    else if (type == &PyByteArray_Type) {
        return mpack_encode_bytearray(self, obj);
    }
    else if (type == &PyMemoryView_Type) {
        return mpack_encode_memoryview(self, obj);
    }
    else if (type == &PyList_Type) {
        return mpack_encode_list(self, obj);
    }
    else if (type == &PySet_Type) {
        return mpack_encode_set(self, obj);
    }
    else if (type == &PyTuple_Type) {
        return mpack_encode_tuple(self, obj);
    }
    else if (type == &PyDict_Type) {
        return mpack_encode_dict(self, obj);
    }
    else if (Py_TYPE(type) == &StructMetaType) {
        return mpack_encode_struct(self, obj);
    }
    else if (type == PyDateTimeAPI->DateTimeType) {
        return mpack_encode_datetime(self, obj);
    }
    else if (type == &Ext_Type) {
        return mpack_encode_ext(self, obj);
    }
    st = msgspec_get_global_state();
    if (PyType_IsSubtype(type, st->EnumType)) {
        return mpack_encode_enum(self, obj);
    }
    if (self->enc_hook != NULL) {
        int status = -1;
        PyObject *temp;
        temp = CALL_ONE_ARG(self->enc_hook, obj);
        if (temp == NULL) return -1;
        if (!Py_EnterRecursiveCall(" while serializing an object")) {
            status = mpack_encode(self, temp);
            Py_LeaveRecursiveCall();
        }
        Py_DECREF(temp);
        return status;
    }
    else {
        PyErr_Format(PyExc_TypeError,
                     "Encoding objects of type %.200s is unsupported",
                     type->tp_name);
        return -1;
    }
}

static PyObject*
Encoder_encode_into(Encoder *self, PyObject *const *args, Py_ssize_t nargs)
{
    return encoder_encode_into_common(&(self->state), args, nargs, &mpack_encode);
}

static PyObject*
Encoder_encode(Encoder *self, PyObject *const *args, Py_ssize_t nargs)
{
    return encoder_encode_common(&(self->state), args, nargs, &mpack_encode);
}

static struct PyMethodDef Encoder_methods[] = {
    {
        "encode", (PyCFunction) Encoder_encode, METH_FASTCALL,
        Encoder_encode__doc__,
    },
    {
        "encode_into", (PyCFunction) Encoder_encode_into, METH_FASTCALL,
        Encoder_encode_into__doc__,
    },
    {
        "__sizeof__", (PyCFunction) Encoder_sizeof, METH_NOARGS,
        PyDoc_STR("Size in bytes")
    },
    {NULL, NULL}                /* sentinel */
};

static PyTypeObject Encoder_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec.msgpack.Encoder",
    .tp_doc = Encoder__doc__,
    .tp_basicsize = sizeof(Encoder),
    .tp_dealloc = (destructor)Encoder_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)Encoder_traverse,
    .tp_clear = (inquiry)Encoder_clear,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc)Encoder_init,
    .tp_methods = Encoder_methods,
    .tp_members = Encoder_members,
};

PyDoc_STRVAR(msgspec_msgpack_encode__doc__,
"msgpack_encode(obj, *, enc_hook=None)\n"
"--\n"
"\n"
"Serialize an object to bytes.\n"
"\n"
"Parameters\n"
"----------\n"
"obj : Any\n"
"    The object to serialize.\n"
"enc_hook : callable, optional\n"
"    A callable to call for objects that aren't supported msgspec types. Takes the\n"
"    unsupported object and should return a supported object, or raise a TypeError.\n"
"\n"
"Returns\n"
"-------\n"
"data : bytes\n"
"    The serialized object.\n"
"\n"
"See Also\n"
"--------\n"
"Encoder.encode"
);
static PyObject*
msgspec_msgpack_encode(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    return encode_common(args, nargs, kwnames, &mpack_encode);
}

/*************************************************************************
 * JSON Encoder                                                          *
 *************************************************************************/

PyDoc_STRVAR(JSONEncoder__doc__,
"Encoder(*, enc_hook=None, write_buffer_size=512)\n"
"--\n"
"\n"
"A JSON encoder.\n"
"\n"
"Parameters\n"
"----------\n"
"enc_hook : callable, optional\n"
"    A callable to call for objects that aren't supported msgspec types. Takes the\n"
"    unsupported object and should return a supported object, or raise a TypeError.\n"
"write_buffer_size : int, optional\n"
"    The size of the internal static write buffer."
);

static int json_encode(EncoderState*, PyObject*);

static MS_INLINE int
json_encode_none(EncoderState *self)
{
    const char *buf = "null";
    return ms_write(self, buf, 4);
}

static MS_INLINE int
json_encode_true(EncoderState *self)
{
    const char *buf = "true";
    return ms_write(self, buf, 4);
}

static MS_INLINE int
json_encode_false(EncoderState *self)
{
    const char *buf = "false";
    return ms_write(self, buf, 5);
}

static int
json_encode_long(EncoderState *self, PyObject *obj) {
    int neg, overflow;
    char buf[20];
    char *p = &buf[20];
    int64_t xsigned = PyLong_AsLongLongAndOverflow(obj, &overflow);
    uint64_t x;
    if (overflow) {
        if (overflow > 0) {
            neg = false;
            x = PyLong_AsUnsignedLongLong(obj);
            if (x == ((uint64_t)(-1)) && PyErr_Occurred()) return -1;
        } else {
            PyErr_SetString(PyExc_OverflowError, "can't serialize ints < -2**63");
            return -1;
        }
    }
    else if (xsigned == -1 && PyErr_Occurred()) {
        return -1;
    }
    else {
        neg = xsigned < 0;
        x = neg ? -xsigned : xsigned;
    }
    while (x >= 100) {
        int64_t const old = x;
        p -= 2;
        x /= 100;
        memcpy(p, DIGIT_TABLE + ((old - (x * 100)) << 1), 2);
    }
    if (x >= 10) {
        p -= 2;
        memcpy(p, DIGIT_TABLE + (x << 1), 2);
    }
    else {
        *--p = x + '0';
    }
    if (neg) {
        *--p = '-';
    }
    return ms_write(self, p, &buf[20] - p);
}

static int
json_encode_float(EncoderState *self, PyObject *obj) {
    char buf[24];
    double x = PyFloat_AS_DOUBLE(obj);
    int n = format_double(x, buf);
    return ms_write(self, buf, n);
}

static inline int
json_encode_str_nocheck(EncoderState *self, PyObject *obj) {
    Py_ssize_t len;
    const char* buf = unicode_str_and_size(obj, &len);
    if (buf == NULL) return -1;
    if (ms_ensure_space(self, len + 2) < 0) return -1;
    char *p = self->output_buffer_raw + self->output_len;
    *p++ = '"';
    memcpy(p, buf, len);
    *(p + len) = '"';
    self->output_len += len + 2;
    return 0;
}

/* A table of escape characters to use for each byte (0 if no escape needed) */
static const char escape_table[256] = {
    'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'b', 't', 'n', 'u', 'f', 'r', 'u', 'u',
    'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u',
    0, 0, '"', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '\\', 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

/* GCC generates better code if the uncommon path in the str encoding loop is
 * pulled out into a separate function. Clang generates the same code either
 * way. */
static int
json_write_str_fragment(
    EncoderState *self, const char *buf, Py_ssize_t start, Py_ssize_t i, char c, char escape
) {
    if (MS_LIKELY(start < i)) {
        if (MS_UNLIKELY(ms_write(self, buf + start, i - start) < 0)) return -1;
    }

    /* Write the escaped character */
    char escaped[6] = {'\\', escape, '0', '0'};
    if (MS_UNLIKELY(escape == 'u')) {
        static const char* const hex = "0123456789abcdef";
        escaped[4] = hex[c >> 4];
        escaped[5] = hex[c & 0xF];
        if (MS_UNLIKELY(ms_write(self, escaped, 6) < 0)) return -1;
    }
    else {
        if (MS_UNLIKELY(ms_write(self, escaped, 2) < 0)) return -1;
    }
    return i + 1;
}

static int
json_encode_str(EncoderState *self, PyObject *obj) {
    Py_ssize_t i, len, start = 0;
    const char* buf = unicode_str_and_size(obj, &len);
    if (buf == NULL) return -1;

    if (ms_write(self, "\"", 1) < 0) return -1;

    for (i = 0; i < len; i++) {
        /* Scan through until a character needs to be escaped */
        char c = buf[i];
        char escape = escape_table[(uint8_t)c];
        if (MS_UNLIKELY(escape != 0)) {
            if (MS_UNLIKELY((start = json_write_str_fragment(self, buf, start, i, c, escape)) < 0)) {
                return -1;
            }
        }
    }
    /* Write the last unescaped fragment (if any) */
    if (start != len) {
        if (ms_write(self, buf + start, i - start) < 0) return -1;
    }

    return ms_write(self, "\"", 1);
}

static const char base64_encode_table[] =
"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int
json_encode_bin(EncoderState *self, const char* buf, Py_ssize_t len) {
    char *out;
    int nbits = 0, charbuf = 0;
    Py_ssize_t encoded_len;

    if (len >= (1LL << 32)) {
        PyErr_SetString(
            msgspec_get_global_state()->EncodeError,
            "Can't encode bytes-like objects longer than 2**32 - 1"
        );
        return -1;
    }

    /* Preallocate the buffer (ceil(4/3 * len) + 2) */
    encoded_len = 4 * ((len + 2) / 3) + 2;
    if (ms_ensure_space(self, encoded_len) < 0) return -1;

    /* Write to the buffer directly */
    out = self->output_buffer_raw + self->output_len;

    *out++ = '"';
    for (; len > 0; len--, buf++) {
        charbuf = (charbuf << 8) | *buf;
        nbits += 8;
        while (nbits >= 6) {
            unsigned char ind = (charbuf >> (nbits - 6)) & 0x3f;
            nbits -= 6;
            *out++ = base64_encode_table[ind];
        }
    }
    if (nbits == 2) {
        *out++ = base64_encode_table[(charbuf & 3) << 4];
        *out++ = '=';
        *out++ = '=';
    }
    else if (nbits == 4) {
        *out++ = base64_encode_table[(charbuf & 0xf) << 2];
        *out++ = '=';
    }
    *out++ = '"';

    self->output_len += encoded_len;
    return 0;
}

static int
json_encode_bytes(EncoderState *self, PyObject *obj)
{
    Py_ssize_t len = PyBytes_GET_SIZE(obj);
    const char* buf = PyBytes_AS_STRING(obj);
    return json_encode_bin(self, buf, len);
}

static int
json_encode_bytearray(EncoderState *self, PyObject *obj)
{
    Py_ssize_t len = PyByteArray_GET_SIZE(obj);
    const char* buf = PyByteArray_AS_STRING(obj);
    return json_encode_bin(self, buf, len);
}

static int
json_encode_memoryview(EncoderState *self, PyObject *obj)
{
    int out;
    Py_buffer buffer;
    if (PyObject_GetBuffer(obj, &buffer, PyBUF_CONTIG_RO) < 0) return -1;
    out = json_encode_bin(self, buffer.buf, buffer.len);
    PyBuffer_Release(&buffer);
    return out;
}

static int
json_encode_enum(EncoderState *self, PyObject *obj)
{
    if (PyLong_Check(obj))
        return json_encode_long(self, obj);

    int status;
    PyObject *name = NULL;
    MsgspecState *st = msgspec_get_global_state();
    /* Try the private variable first for speed, fall back to the public
     * interface if not available */
    name = PyObject_GetAttr(obj, st->str__name_);
    if (name == NULL) {
        PyErr_Clear();
        name = PyObject_GetAttr(obj, st->str_name);
        if (name == NULL)
            return -1;
    }
    if (PyUnicode_CheckExact(name)) {
        status = json_encode_str(self, name);
    } else {
        PyErr_SetString(
            msgspec_get_global_state()->EncodeError,
            "Enum's with non-str names aren't supported"
        );
        status = -1;
    }
    Py_DECREF(name);
    return status;
}

static MS_INLINE char *
json_write_fixint(char *p, uint32_t x, int width) {
    p += width;
    for (int i = 0; i < width; i++) {
        *--p = (x % 10) + '0';
        x = x / 10;
    }
    return p + width;
}

static int
json_encode_datetime(EncoderState *self, PyObject *obj)
{
    uint32_t year = PyDateTime_GET_YEAR(obj);
    uint8_t month = PyDateTime_GET_MONTH(obj);
    uint8_t day = PyDateTime_GET_DAY(obj);

    uint8_t hour = PyDateTime_DATE_GET_HOUR(obj);
    uint8_t minute = PyDateTime_DATE_GET_MINUTE(obj);
    uint8_t second = PyDateTime_DATE_GET_SECOND(obj);
    uint32_t microsecond = PyDateTime_DATE_GET_MICROSECOND(obj);
    int32_t offset_days = 0, offset_secs = 0;
    PyObject *tzinfo;

    if (!MS_HAS_TZINFO(obj)) {
        PyErr_SetString(
            msgspec_get_global_state()->EncodeError,
            "Can't encode naive datetime objects"
        );
        return -1;
    }

    tzinfo = MS_GET_TZINFO(obj);
    if (tzinfo != PyDateTime_TimeZone_UTC) {
        MsgspecState *st = msgspec_get_global_state();
        PyObject *offset = CALL_METHOD_ONE_ARG(tzinfo, st->str_utcoffset, obj);
        if (offset == NULL) return -1;
        if (PyDelta_Check(offset)) {
            offset_days = PyDateTime_DELTA_GET_DAYS(offset);
            offset_secs = PyDateTime_DELTA_GET_SECONDS(offset);
        }
        else if (offset != Py_None) {
            PyErr_SetString(
                PyExc_TypeError,
                "datetime.tzinfo.utcoffset returned a non-timedelta object"
            );
            Py_DECREF(offset);
            return -1;
        }
        Py_DECREF(offset);
    }

    char buf[34];
    char *p = buf;
    memset(p, '0', 34);

    *p++ = '"';
    p = json_write_fixint(p, year, 4);
    *p++ = '-';
    p = json_write_fixint(p, month, 2);
    *p++ = '-';
    p = json_write_fixint(p, day, 2);
    *p++ = 'T';
    p = json_write_fixint(p, hour, 2);
    *p++ = ':';
    p = json_write_fixint(p, minute, 2);
    *p++ = ':';
    p = json_write_fixint(p, second, 2);
    if (microsecond) {
        *p++ = '.';
        p = json_write_fixint(p, microsecond, 6);
    }
    if (MS_LIKELY(offset_secs == 0)) {
        *p++ = 'Z';
    }
    else {
        char sign = '+';
        if (offset_days == -1) {
            sign = '-';
            offset_secs = 86400 - offset_secs;
        }
        uint8_t offset_hour = offset_secs / 3600;
        uint8_t offset_min = (offset_secs / 60) % 60;
        /* If the offset isn't an even number of minutes, RFC 3339
        * indicates that the offset should be rounded to the nearest
        * possible hour:min pair */
        bool round_up = (offset_secs - (offset_hour * 3600 + offset_min * 60)) > 30;
        if (MS_UNLIKELY(round_up)) {
            offset_min++;
            if (offset_min == 60) {
                offset_min = 0;
                offset_hour++;
                if (offset_hour == 24) {
                    offset_hour = 0;
                }
            }
        }
        if (offset_hour == 0 && offset_min == 0) {
            *p++ = 'Z';
        }
        else {
            *p++ = sign;
            p = json_write_fixint(p, offset_hour, 2);
            *p++ = ':';
            p = json_write_fixint(p, offset_min, 2);
        }
    }
    *p++ = '"';
    return ms_write(self, buf, p - buf);
}

static int
json_encode_list(EncoderState *self, PyObject *obj)
{
    Py_ssize_t i, len;
    int status = -1;

    len = PyList_GET_SIZE(obj);
    if (len == 0) return ms_write(self, "[]", 2);

    if (ms_write(self, "[", 1) < 0) return -1;
    if (Py_EnterRecursiveCall(" while serializing an object"))
        return -1;
    for (i = 0; i < len; i++) {
        if (json_encode(self, PyList_GET_ITEM(obj, i)) < 0) goto cleanup;
        if (ms_write(self, ",", 1) < 0) goto cleanup;
    }
    /* Overwrite trailing comma with ] */
    *(self->output_buffer_raw + self->output_len - 1) = ']';
    status = 0;
cleanup:
    Py_LeaveRecursiveCall();
    return status;
}

static int
json_encode_set(EncoderState *self, PyObject *obj)
{
    Py_ssize_t len, ppos = 0;
    Py_hash_t hash;
    PyObject *item;
    int status = -1;

    len = PySet_GET_SIZE(obj);
    if (len == 0) return ms_write(self, "[]", 2);

    if (ms_write(self, "[", 1) < 0) return -1;
    if (Py_EnterRecursiveCall(" while serializing an object"))
        return -1;
    while (_PySet_NextEntry(obj, &ppos, &item, &hash)) {
        if (json_encode(self, item) < 0) goto cleanup;
        if (ms_write(self, ",", 1) < 0) goto cleanup;
    }
    /* Overwrite trailing comma with ] */
    *(self->output_buffer_raw + self->output_len - 1) = ']';
    status = 0;
cleanup:
    Py_LeaveRecursiveCall();
    return status;
}

static int
json_encode_tuple(EncoderState *self, PyObject *obj)
{
    Py_ssize_t i, len;
    int status = -1;

    len = PyTuple_GET_SIZE(obj);
    if (len == 0) return ms_write(self, "[]", 2);

    if (ms_write(self, "[", 1) < 0) return -1;
    if (Py_EnterRecursiveCall(" while serializing an object"))
        return -1;
    for (i = 0; i < len; i++) {
        if (json_encode(self, PyTuple_GET_ITEM(obj, i)) < 0) goto cleanup;
        if (ms_write(self, ",", 1) < 0) goto cleanup;
    }
    /* Overwrite trailing comma with ] */
    *(self->output_buffer_raw + self->output_len - 1) = ']';
    status = 0;
cleanup:
    Py_LeaveRecursiveCall();
    return status;
}

static int
json_encode_dict(EncoderState *self, PyObject *obj)
{
    PyObject *key, *val;
    Py_ssize_t len, pos = 0;
    int status = -1;

    len = PyDict_GET_SIZE(obj);
    if (len == 0) return ms_write(self, "{}", 2);
    if (ms_write(self, "{", 1) < 0) return -1;
    if (Py_EnterRecursiveCall(" while serializing an object"))
        return -1;
    while (PyDict_Next(obj, &pos, &key, &val)) {
        if (!PyUnicode_CheckExact(key)) {
            PyErr_SetString(PyExc_TypeError, "dict keys must be strings");
            goto cleanup;
        }
        if (json_encode_str(self, key) < 0) goto cleanup;
        if (ms_write(self, ":", 1) < 0) goto cleanup;
        if (json_encode(self, val) < 0) goto cleanup;
        if (ms_write(self, ",", 1) < 0) goto cleanup;
    }
    /* Overwrite trailing comma with } */
    *(self->output_buffer_raw + self->output_len - 1) = '}';
    status = 0;
cleanup:
    Py_LeaveRecursiveCall();
    return status;
}

static int
json_encode_struct(EncoderState *self, PyObject *obj)
{
    PyObject *key, *val, *fields;
    Py_ssize_t i, len;
    int status = -1;
    bool asarray = ((StructMetaObject *)Py_TYPE(obj))->asarray == OPT_TRUE;

    fields = StructMeta_GET_FIELDS(Py_TYPE(obj));
    len = PyTuple_GET_SIZE(fields);

    if (asarray) {
        if (len == 0) return ms_write(self, "[]", 2);
        if (ms_write(self, "[", 1) < 0) return -1;
        if (Py_EnterRecursiveCall(" while serializing an object")) return -1;
        for (i = 0; i < len; i++) {
            val = Struct_get_index(obj, i);
            if (val == NULL) goto cleanup;
            if (json_encode(self, val) < 0) goto cleanup;
            if (ms_write(self, ",", 1) < 0) goto cleanup;
        }
        /* Overwrite trailing comma with ] */
        *(self->output_buffer_raw + self->output_len - 1) = ']';
    }
    else {
        if (len == 0) return ms_write(self, "{}", 2);
        if (ms_write(self, "{", 1) < 0) return -1;
        if (Py_EnterRecursiveCall(" while serializing an object")) return -1;
        for (i = 0; i < len; i++) {
            key = PyTuple_GET_ITEM(fields, i);
            val = Struct_get_index(obj, i);
            if (val == NULL) goto cleanup;
            if (json_encode_str_nocheck(self, key) < 0) goto cleanup;
            if (ms_write(self, ":", 1) < 0) goto cleanup;
            if (json_encode(self, val) < 0) goto cleanup;
            if (ms_write(self, ",", 1) < 0) goto cleanup;
        }
        /* Overwrite trailing comma with } */
        *(self->output_buffer_raw + self->output_len - 1) = '}';
    }
    status = 0;
cleanup:
    Py_LeaveRecursiveCall();
    return status;
}

static int
json_encode(EncoderState *self, PyObject *obj)
{
    MsgspecState *st;
    PyTypeObject *type = Py_TYPE(obj);

    if (obj == Py_None) {
        return json_encode_none(self);
    }
    else if (obj == Py_True) {
        return json_encode_true(self);
    }
    else if (obj == Py_False) {
        return json_encode_false(self);
    }
    else if (type == &PyLong_Type) {
        return json_encode_long(self, obj);
    }
    else if (type == &PyFloat_Type) {
        return json_encode_float(self, obj);
    }
    else if (type == &PyUnicode_Type) {
        return json_encode_str(self, obj);
    }
    else if (type == &PyList_Type) {
        return json_encode_list(self, obj);
    }
    else if (type == &PyTuple_Type) {
        return json_encode_tuple(self, obj);
    }
    else if (type == &PySet_Type) {
        return json_encode_set(self, obj);
    }
    else if (type == &PyDict_Type) {
        return json_encode_dict(self, obj);
    }
    else if (Py_TYPE(type) == &StructMetaType) {
        return json_encode_struct(self, obj);
    }
    else if (type == PyDateTimeAPI->DateTimeType) {
        return json_encode_datetime(self, obj);
    }
    else if (type == &PyBytes_Type) {
        return json_encode_bytes(self, obj);
    }
    else if (type == &PyByteArray_Type) {
        return json_encode_bytearray(self, obj);
    }
    else if (type == &PyMemoryView_Type) {
        return json_encode_memoryview(self, obj);
    }
    st = msgspec_get_global_state();
    if (PyType_IsSubtype(type, st->EnumType)) {
        return json_encode_enum(self, obj);
    }

    if (self->enc_hook != NULL) {
        int status = -1;
        PyObject *temp;
        temp = CALL_ONE_ARG(self->enc_hook, obj);
        if (temp == NULL) return -1;
        if (!Py_EnterRecursiveCall(" while serializing an object")) {
            status = json_encode(self, temp);
            Py_LeaveRecursiveCall();
        }
        Py_DECREF(temp);
        return status;
    }
    else {
        PyErr_Format(PyExc_TypeError,
                     "Encoding objects of type %.200s is unsupported",
                     type->tp_name);
        return -1;
    }
}

static PyObject*
JSONEncoder_encode_into(Encoder *self, PyObject *const *args, Py_ssize_t nargs)
{
    return encoder_encode_into_common(&(self->state), args, nargs, &json_encode);
}

static PyObject*
JSONEncoder_encode(Encoder *self, PyObject *const *args, Py_ssize_t nargs)
{
    return encoder_encode_common(&(self->state), args, nargs, &json_encode);
}

static struct PyMethodDef JSONEncoder_methods[] = {
    {
        "encode", (PyCFunction) JSONEncoder_encode, METH_FASTCALL,
        Encoder_encode__doc__,
    },
    {
        "encode_into", (PyCFunction) JSONEncoder_encode_into, METH_FASTCALL,
        Encoder_encode_into__doc__,
    },
    {
        "__sizeof__", (PyCFunction) Encoder_sizeof, METH_NOARGS,
        PyDoc_STR("Size in bytes")
    },
    {NULL, NULL}                /* sentinel */
};

static PyTypeObject JSONEncoder_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec.json.Encoder",
    .tp_doc = JSONEncoder__doc__,
    .tp_basicsize = sizeof(Encoder),
    .tp_dealloc = (destructor)Encoder_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)Encoder_traverse,
    .tp_clear = (inquiry)Encoder_clear,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc)Encoder_init,
    .tp_methods = JSONEncoder_methods,
    .tp_members = Encoder_members,
};

PyDoc_STRVAR(msgspec_json_encode__doc__,
"json_encode(obj, *, enc_hook=None)\n"
"--\n"
"\n"
"Serialize an object to bytes.\n"
"\n"
"Parameters\n"
"----------\n"
"obj : Any\n"
"    The object to serialize.\n"
"enc_hook : callable, optional\n"
"    A callable to call for objects that aren't supported msgspec types. Takes the\n"
"    unsupported object and should return a supported object, or raise a TypeError.\n"
"\n"
"Returns\n"
"-------\n"
"data : bytes\n"
"    The serialized object.\n"
"\n"
"See Also\n"
"--------\n"
"Encoder.encode"
);
static PyObject*
msgspec_json_encode(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    return encode_common(args, nargs, kwnames, &json_encode);
}

/*************************************************************************
 * MessagePack Decoder                                                   *
 *************************************************************************/

typedef struct DecoderState {
    /* Configuration */
    TypeNode *type;
    PyObject *dec_hook;
    PyObject *ext_hook;

    /* Per-message attributes */
    PyObject *buffer_obj;
    char *input_start;
    char *input_pos;
    char *input_end;
} DecoderState;

typedef struct Decoder {
    PyObject_HEAD
    PyObject *orig_type;
    DecoderState state;
} Decoder;

PyDoc_STRVAR(Decoder__doc__,
"Decoder(type='Any', *, dec_hook=None, ext_hook=None)\n"
"--\n"
"\n"
"A MessagePack decoder.\n"
"\n"
"Parameters\n"
"----------\n"
"type : Type, optional\n"
"    A Python type (in type annotation form) to decode the object as. If\n"
"    provided, the message will be type checked and decoded as the specified\n"
"    type. Defaults to `Any`, in which case the message will be decoded using\n"
"    the default MessagePack types.\n"
"dec_hook : Callable, optional\n"
"    An optional callback for handling decoding custom types. Should have the\n"
"    signature ``dec_hook(type: Type, obj: Any) -> Any``, where ``type`` is the\n"
"    expected message type, and ``obj`` is the decoded representation composed\n"
"    of only basic MessagePack types. This hook should transform ``obj`` into\n"
"    type ``type``, or raise a ``TypeError`` if unsupported.\n"
"ext_hook : Callable, optional\n"
"    An optional callback for decoding MessagePack extensions. Should have the\n"
"    signature ``ext_hook(code: int, data: memoryview) -> Any``. If provided,\n"
"    this will be called to deserialize all extension types found in the\n"
"    message. Note that ``data`` is a memoryview into the larger message\n"
"    buffer - any references created to the underlying buffer without copying\n"
"    the data out will cause the full message buffer to persist in memory.\n"
"    If not provided, extension types will decode as ``msgspec.Ext`` objects."
);
static int
Decoder_init(Decoder *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"type", "dec_hook", "ext_hook", NULL};
    MsgspecState *st = msgspec_get_global_state();
    PyObject *type = st->typing_any;
    PyObject *ext_hook = NULL;
    PyObject *dec_hook = NULL;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "|O$OO", kwlist, &type, &dec_hook, &ext_hook
        )) {
        return -1;
    }

    /* Handle dec_hook */
    if (dec_hook == Py_None) {
        dec_hook = NULL;
    }
    if (dec_hook != NULL) {
        if (!PyCallable_Check(dec_hook)) {
            PyErr_SetString(PyExc_TypeError, "dec_hook must be callable");
            return -1;
        }
        Py_INCREF(dec_hook);
    }
    self->state.dec_hook = dec_hook;

    /* Handle ext_hook */
    if (ext_hook == Py_None) {
        ext_hook = NULL;
    }
    if (ext_hook != NULL) {
        if (!PyCallable_Check(ext_hook)) {
            PyErr_SetString(PyExc_TypeError, "ext_hook must be callable");
            return -1;
        }
        Py_INCREF(ext_hook);
    }
    self->state.ext_hook = ext_hook;

    /* Handle type */
    self->state.type = TypeNode_Convert(type, false, NULL);
    if (self->state.type == NULL) {
        return -1;
    }
    Py_INCREF(type);
    self->orig_type = type;
    return 0;
}

static int
Decoder_traverse(Decoder *self, visitproc visit, void *arg)
{
    int out = TypeNode_traverse(self->state.type, visit, arg);
    if (out != 0) return out;
    Py_VISIT(self->orig_type);
    Py_VISIT(self->state.dec_hook);
    Py_VISIT(self->state.ext_hook);
    return 0;
}

static void
Decoder_dealloc(Decoder *self)
{
    TypeNode_Free(self->state.type);
    Py_XDECREF(self->orig_type);
    Py_XDECREF(self->state.dec_hook);
    Py_XDECREF(self->state.ext_hook);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
Decoder_repr(Decoder *self) {
    int recursive;
    PyObject *typstr, *out = NULL;

    recursive = Py_ReprEnter((PyObject *)self);
    if (recursive != 0) {
        return (recursive < 0) ? NULL : PyUnicode_FromString("...");
    }
    typstr = PyObject_Repr(self->orig_type);
    if (typstr != NULL) {
        out = PyUnicode_FromFormat("msgspec.msgpack.Decoder(%U)", typstr);
    }
    Py_XDECREF(typstr);
    Py_ReprLeave((PyObject *)self);
    return out;
}

static MS_INLINE int
mpack_read1(DecoderState *self, char *s)
{
    if (MS_UNLIKELY(self->input_pos == self->input_end)) {
        return ms_err_truncated();
    }
    *s = *self->input_pos++;
    return 0;
}

static MS_INLINE int
mpack_read(DecoderState *self, char **s, Py_ssize_t n)
{
    if (MS_LIKELY(n <= self->input_end - self->input_pos)) {
        *s = self->input_pos;
        self->input_pos += n;
        return 0;
    }
    return ms_err_truncated();
}

static MS_INLINE bool
mpack_has_trailing_characters(DecoderState *self)
{
    if (self->input_pos != self->input_end) {
        PyErr_Format(
            msgspec_get_global_state()->DecodeError,
            "MessagePack data is malformed: trailing characters (byte %zd)",
            (Py_ssize_t)(self->input_pos - self->input_start)
        );
        return true;
    }
    return false;
}

static MS_INLINE Py_ssize_t
mpack_decode_size1(DecoderState *self) {
    char s = 0;
    if (mpack_read1(self, &s) < 0) return -1;
    return (Py_ssize_t)((unsigned char)s);
}

static MS_INLINE Py_ssize_t
mpack_decode_size2(DecoderState *self) {
    char *s = NULL;
    if (mpack_read(self, &s, 2) < 0) return -1;
    return (Py_ssize_t)(_msgspec_load16(uint16_t, s));
}

static MS_INLINE Py_ssize_t
mpack_decode_size4(DecoderState *self) {
    char *s = NULL;
    if (mpack_read(self, &s, 4) < 0) return -1;
    return (Py_ssize_t)(_msgspec_load32(uint32_t, s));
}

static PyObject *
mpack_decode_datetime(
    DecoderState *self, const char *data_buf, Py_ssize_t size, PathNode *path
) {
    uint64_t data64;
    uint32_t nanoseconds;
    int64_t seconds;
    MsgspecState *st;
    PyObject *suffix;
    char *err_msg;

    switch (size) {
        case 4:
            seconds = _msgspec_load32(uint32_t, data_buf);
            nanoseconds = 0;
            break;
        case 8:
            data64 = _msgspec_load64(uint64_t, data_buf);
            seconds = data64 & 0x00000003ffffffffL;
            nanoseconds = data64 >> 34;
            break;
        case 12:
            nanoseconds = _msgspec_load32(uint32_t, data_buf);
            seconds = _msgspec_load64(uint64_t, data_buf + 4);
            break;
        default:
            err_msg = "Invalid MessagePack timestamp%U";
            goto invalid;
    }

    if (nanoseconds > 999999999) {
        err_msg = "Invalid MessagePack timestamp: nanoseconds out of range%U";
        goto invalid;
    }
    /* Error on out-of-bounds datetimes. This leaves ample space in an int, so
     * no need to check for overflow later. */
    if (seconds < MS_EPOCH_SECS_MIN || seconds > MS_EPOCH_SECS_MAX) {
        err_msg = "Timestamp is out of range%U";
        goto invalid;
    }
    return datetime_from_epoch(seconds, nanoseconds);

invalid:
    st = msgspec_get_global_state();
    suffix = PathNode_ErrSuffix(path);
    if (suffix != NULL) {
        PyErr_Format(st->DecodeError, err_msg, suffix);
        Py_DECREF(suffix);
    }
    return NULL;
}

static int mpack_skip(DecoderState *self);

static int
mpack_skip_array(DecoderState *self, Py_ssize_t size) {
    int status = -1;
    Py_ssize_t i;
    if (size < 0) return -1;
    if (size == 0) return 0;

    if (Py_EnterRecursiveCall(" while deserializing an object")) return -1;
    for (i = 0; i < size; i++) {
        if (mpack_skip(self) < 0) break;
    }
    status = 0;
    Py_LeaveRecursiveCall();
    return status;
}

static int
mpack_skip_map(DecoderState *self, Py_ssize_t size) {
    return mpack_skip_array(self, size * 2);
}

static int
mpack_skip_ext(DecoderState *self, Py_ssize_t size) {
    char *s;
    if (size < 0) return -1;
    return mpack_read(self, &s, size + 1);
}

static int
mpack_skip(DecoderState *self) {
    char *s = NULL;
    char op = 0;
    Py_ssize_t size;

    if (mpack_read1(self, &op) < 0) return -1;

    if (-32 <= op && op <= 127) {
        return 0;
    }
    else if ('\xa0' <= op && op <= '\xbf') {
        return mpack_read(self, &s, op & 0x1f);
    }
    else if ('\x90' <= op && op <= '\x9f') {
        return mpack_skip_array(self, op & 0x0f);
    }
    else if ('\x80' <= op && op <= '\x8f') {
        return mpack_skip_map(self, op & 0x0f);
    }
    switch ((enum mpack_code)op) {
        case MP_NIL:
        case MP_TRUE:
        case MP_FALSE:
            return 0;
        case MP_UINT8:
        case MP_INT8:
            return mpack_read1(self, &op);
        case MP_UINT16:
        case MP_INT16:
            return mpack_read(self, &s, 2);
        case MP_UINT32:
        case MP_INT32:
        case MP_FLOAT32:
            return mpack_read(self, &s, 4);
        case MP_UINT64:
        case MP_INT64:
        case MP_FLOAT64:
            return mpack_read(self, &s, 8);
        case MP_STR8:
        case MP_BIN8:
            if ((size = mpack_decode_size1(self)) < 0) return -1;
            return mpack_read(self, &s, size);
        case MP_STR16:
        case MP_BIN16:
            if ((size = mpack_decode_size2(self)) < 0) return -1;
            return mpack_read(self, &s, size);
        case MP_STR32:
        case MP_BIN32:
            if ((size = mpack_decode_size4(self)) < 0) return -1;
            return mpack_read(self, &s, size);
        case MP_ARRAY16:
            return mpack_skip_array(self, mpack_decode_size2(self));
        case MP_ARRAY32:
            return mpack_skip_array(self, mpack_decode_size4(self));
        case MP_MAP16:
            return mpack_skip_map(self, mpack_decode_size2(self));
        case MP_MAP32:
            return mpack_skip_map(self, mpack_decode_size4(self));
        case MP_FIXEXT1:
            return mpack_skip_ext(self, 1);
        case MP_FIXEXT2:
            return mpack_skip_ext(self, 2);
        case MP_FIXEXT4:
            return mpack_skip_ext(self, 4);
        case MP_FIXEXT8:
            return mpack_skip_ext(self, 8);
        case MP_FIXEXT16:
            return mpack_skip_ext(self, 16);
        case MP_EXT8:
            return mpack_skip_ext(self, mpack_decode_size1(self));
        case MP_EXT16:
            return mpack_skip_ext(self, mpack_decode_size2(self));
        case MP_EXT32:
            return mpack_skip_ext(self, mpack_decode_size4(self));
        default:
            PyErr_Format(
                msgspec_get_global_state()->DecodeError,
                "MessagePack data is malformed: invalid opcode '\\x%02x' (byte %zd)",
                op,
                (Py_ssize_t)(self->input_pos - self->input_start - 1)
            );
            return -1;
    }
}

static PyObject * mpack_decode(
    DecoderState *self, TypeNode *type, PathNode *path, bool is_key
);

static MS_INLINE PyObject *
mpack_decode_int(DecoderState *self, int64_t x, TypeNode *type, PathNode *path) {
    if (type->types & MS_TYPE_INTENUM) {
        return ms_decode_intenum(PyLong_FromLongLong(x), type, path);
    }
    else if (type->types & (MS_TYPE_ANY | MS_TYPE_INT)) {
        return PyLong_FromLongLong(x);
    }
    else if (type->types & MS_TYPE_FLOAT) {
        return PyFloat_FromDouble(x);
    }
    return ms_validation_error("int", type, path);
}

static MS_INLINE PyObject *
mpack_decode_uint(DecoderState *self, uint64_t x, TypeNode *type, PathNode *path) {
    if (type->types & MS_TYPE_INTENUM) {
        return ms_decode_intenum(PyLong_FromUnsignedLongLong(x), type, path);
    }
    else if (type->types & (MS_TYPE_ANY | MS_TYPE_INT)) {
        return PyLong_FromUnsignedLongLong(x);
    }
    else if (type->types & MS_TYPE_FLOAT) {
        return PyFloat_FromDouble(x);
    }
    return ms_validation_error("int", type, path);
}

static MS_INLINE PyObject *
mpack_decode_none(DecoderState *self, TypeNode *type, PathNode *path) {
    if (type->types & (MS_TYPE_ANY | MS_TYPE_NONE)) {
        Py_INCREF(Py_None);
        return Py_None;
    }
    return ms_validation_error("None", type, path);
}

static MS_INLINE PyObject *
mpack_decode_bool(DecoderState *self, PyObject *val, TypeNode *type, PathNode *path) {
    if (type->types & (MS_TYPE_ANY | MS_TYPE_BOOL)) {
        Py_INCREF(val);
        return val;
    }
    return ms_validation_error("bool", type, path);
}

static PyObject *
mpack_decode_float(DecoderState *self, double val, TypeNode *type, PathNode *path) {
    if (type->types & (MS_TYPE_ANY | MS_TYPE_FLOAT)) {
        return PyFloat_FromDouble(val);
    }
    return ms_validation_error("float", type, path);
}

static MS_INLINE PyObject *
mpack_decode_str(DecoderState *self, Py_ssize_t size, TypeNode *type, PathNode *path) {
    if (MS_LIKELY(type->types & (MS_TYPE_ANY | MS_TYPE_STR | MS_TYPE_ENUM))) {
        char *s = NULL;
        PyObject *val;
        if (MS_UNLIKELY(mpack_read(self, &s, size) < 0)) return NULL;
        val = PyUnicode_DecodeUTF8(s, size, NULL);
        if (MS_UNLIKELY(type->types & MS_TYPE_ENUM)) {
            return ms_decode_enum(val, type, path);
        }
        return val;
    }
    return ms_validation_error("str", type, path);
}

static PyObject *
mpack_decode_bin(
    DecoderState *self, Py_ssize_t size, TypeNode *type, PathNode *path
) {
    if (MS_UNLIKELY(size < 0)) return NULL;

    char *s = NULL;
    if (MS_UNLIKELY(mpack_read(self, &s, size) < 0)) return NULL;

    if (type->types & (MS_TYPE_ANY | MS_TYPE_BYTES)) {
        return PyBytes_FromStringAndSize(s, size);
    }
    else if (type->types & MS_TYPE_BYTEARRAY) {
        return PyByteArray_FromStringAndSize(s, size);
    }
    return ms_validation_error("bytes", type, path);
}

static PyObject *
mpack_decode_list(
    DecoderState *self, Py_ssize_t size, TypeNode *el_type, PathNode *path
) {
    Py_ssize_t i;
    PyObject *res, *item;

    res = PyList_New(size);
    if (res == NULL) return NULL;
    if (size == 0) return res;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(res);
        return NULL;
    }
    for (i = 0; i < size; i++) {
        PathNode el_path = {path, i};
        item = mpack_decode(self, el_type, &el_path, false);
        if (MS_UNLIKELY(item == NULL)) {
            Py_CLEAR(res);
            break;
        }
        PyList_SET_ITEM(res, i, item);
    }
    Py_LeaveRecursiveCall();
    return res;
}

static PyObject *
mpack_decode_set(
    DecoderState *self, Py_ssize_t size, TypeNode *el_type, PathNode *path
) {
    Py_ssize_t i;
    PyObject *res, *item;

    res = PySet_New(NULL);
    if (res == NULL) return NULL;
    if (size == 0) return res;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(res);
        return NULL;
    }
    for (i = 0; i < size; i++) {
        PathNode el_path = {path, i};
        item = mpack_decode(self, el_type, &el_path, true);
        if (MS_UNLIKELY(item == NULL || PySet_Add(res, item) < 0)) {
            Py_CLEAR(res);
            break;
        }
        Py_DECREF(item);
    }
    Py_LeaveRecursiveCall();
    return res;
}

static PyObject *
mpack_decode_vartuple(
    DecoderState *self, Py_ssize_t size, TypeNode *el_type, PathNode *path, bool is_key
) {
    Py_ssize_t i;
    PyObject *res, *item;

    res = PyTuple_New(size);
    if (res == NULL) return NULL;
    if (size == 0) return res;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(res);
        return NULL;
    }
    for (i = 0; i < size; i++) {
        PathNode el_path = {path, i};
        item = mpack_decode(self, el_type, &el_path, is_key);
        if (MS_UNLIKELY(item == NULL)) {
            Py_CLEAR(res);
            break;
        }
        PyTuple_SET_ITEM(res, i, item);
    }
    Py_LeaveRecursiveCall();
    return res;
}

static PyObject *
mpack_decode_fixtuple(
    DecoderState *self, Py_ssize_t size, TypeNode *type, PathNode *path, bool is_key
) {
    Py_ssize_t i, offset;
    PyObject *res, *item;
    TypeNodeExtra *tex = (TypeNodeExtra *)type;

    if (size != tex->fixtuple_size) {
        /* tuple is the incorrect size, raise and return */
        ms_raise_validation_error(
            path,
            "Expected `array` of length %zd, got %zd%U",
            tex->fixtuple_size,
            size
        );
        return NULL;
    }

    res = PyTuple_New(size);
    if (res == NULL) return NULL;
    if (size == 0) return res;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(res);
        return NULL;
    }

    offset = TypeNode_get_array_offset(type);
    for (i = 0; i < tex->fixtuple_size; i++) {
        PathNode el_path = {path, i};
        item = mpack_decode(self, tex->extra[offset + i], &el_path, is_key);
        if (MS_UNLIKELY(item == NULL)) {
            Py_CLEAR(res);
            break;
        }
        PyTuple_SET_ITEM(res, i, item);
    }
    Py_LeaveRecursiveCall();
    return res;
}

static PyObject *
mpack_decode_struct_array(
    DecoderState *self, Py_ssize_t size, StructMetaObject *st_type,
    TypeNode *type, PathNode *path, bool is_key
) {
    Py_ssize_t i, nfields, ndefaults, npos;
    PyObject *res, *val = NULL;
    int should_untrack;

    res = Struct_alloc((PyTypeObject *)(st_type));
    if (res == NULL) return NULL;

    nfields = PyTuple_GET_SIZE(st_type->struct_fields);
    ndefaults = PyTuple_GET_SIZE(st_type->struct_defaults);
    npos = nfields - ndefaults;
    should_untrack = PyObject_IS_GC(res);

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(res);
        return NULL;
    }
    for (i = 0; i < nfields; i++) {
        if (size > 0) {
            PathNode el_path = {path, i};
            val = mpack_decode(self, st_type->struct_types[i], &el_path, is_key);
            if (MS_UNLIKELY(val == NULL)) goto error;
            size--;
        }
        else if (i < npos) {
            ms_raise_validation_error(
                path,
                "Object missing required field `%U`%U",
                PyTuple_GET_ITEM(st_type->struct_fields, i)
            );
            goto error;
        }
        else {
            val = maybe_deepcopy_default(
                PyTuple_GET_ITEM(st_type->struct_defaults, i - npos)
            );
            if (val == NULL)
                goto error;
        }
        Struct_set_index(res, i, val);
        if (should_untrack) {
            should_untrack = !OBJ_IS_GC(val);
        }
    }
    /* Ignore all trailing fields */
    while (size > 0) {
        if (mpack_skip(self) < 0)
            goto error;
        size--;
    }
    Py_LeaveRecursiveCall();
    if (should_untrack)
        PyObject_GC_UnTrack(res);
    return res;
error:
    Py_LeaveRecursiveCall();
    Py_DECREF(res);
    return NULL;
}


static PyObject *
mpack_decode_array(
    DecoderState *self, Py_ssize_t size, TypeNode *type, PathNode *path, bool is_key
) {
    if (type->types & MS_TYPE_ANY) {
        TypeNode type_any = {MS_TYPE_ANY};
        if (is_key) {
            return mpack_decode_vartuple(self, size, &type_any, path, is_key);
        }
        else {
            return mpack_decode_list(self, size, &type_any, path);
        }
    }
    else if (type->types & MS_TYPE_LIST) {
        return mpack_decode_list(self, size, TypeNode_get_array(type), path);
    }
    else if (type->types & MS_TYPE_SET) {
        return mpack_decode_set(self, size, TypeNode_get_array(type), path);
    }
    else if (type->types & MS_TYPE_VARTUPLE) {
        return mpack_decode_vartuple(self, size, TypeNode_get_array(type), path, is_key);
    }
    else if (type->types & MS_TYPE_FIXTUPLE) {
        return mpack_decode_fixtuple(self, size, type, path, is_key);
    }
    else if (type->types & MS_TYPE_STRUCT) {
        StructMetaObject *struct_type = TypeNode_get_struct(type);
        if (struct_type->asarray == OPT_TRUE) {
            return mpack_decode_struct_array(self, size, struct_type, type, path, is_key);
        }
    }
    return ms_validation_error("array", type, path);
}

static PyObject *
mpack_decode_dict(
    DecoderState *self, Py_ssize_t size, TypeNode *key_type,
    TypeNode *val_type, PathNode *path
) {
    Py_ssize_t i;
    PyObject *res, *key = NULL, *val = NULL;
    PathNode key_path = {path, PATH_KEY, NULL};
    PathNode val_path = {path, PATH_ELLIPSIS, NULL};

    res = PyDict_New();
    if (res == NULL) return NULL;
    if (size == 0) return res;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(res);
        return NULL;
    }
    for (i = 0; i < size; i++) {
        key = mpack_decode(self, key_type, &key_path, true);
        if (MS_UNLIKELY(key == NULL))
            goto error;
        val = mpack_decode(self, val_type, &val_path, false);
        if (MS_UNLIKELY(val == NULL))
            goto error;
        if (MS_UNLIKELY(PyDict_SetItem(res, key, val) < 0))
            goto error;
        Py_CLEAR(key);
        Py_CLEAR(val);
    }
    Py_LeaveRecursiveCall();
    return res;
error:
    Py_LeaveRecursiveCall();
    Py_XDECREF(key);
    Py_XDECREF(val);
    Py_DECREF(res);
    return NULL;
}

static PyObject *
mpack_error_expected(char op, char *expected, PathNode *path) {
    char *got;
    if (-32 <= op && op <= 127) {
        got = "int";
    }
    else if ('\xa0' <= op && op <= '\xbf') {
        got = "str";
    }
    else if ('\x90' <= op && op <= '\x9f') {
        got = "array";
    }
    else if ('\x80' <= op && op <= '\x8f') {
        got = "object";
    }
    else {
        switch ((enum mpack_code)op) {
            case MP_NIL:
                got = "null";
                break;
            case MP_TRUE:
            case MP_FALSE:
                got = "bool";
                break;
            case MP_UINT8:
            case MP_UINT16:
            case MP_UINT32:
            case MP_UINT64:
            case MP_INT8:
            case MP_INT16:
            case MP_INT32:
            case MP_INT64:
                got = "int";
                break;
            case MP_FLOAT32:
            case MP_FLOAT64:
                got = "float";
                break;
            case MP_STR8:
            case MP_STR16:
            case MP_STR32:
                got = "str";
                break;
            case MP_BIN8:
            case MP_BIN16:
            case MP_BIN32:
                got = "bytes";
                break;
            case MP_ARRAY16:
            case MP_ARRAY32:
                got = "array";
                break;
            case MP_MAP16:
            case MP_MAP32:
                got = "object";
                break;
            case MP_FIXEXT1:
            case MP_FIXEXT2:
            case MP_FIXEXT4:
            case MP_FIXEXT8:
            case MP_FIXEXT16:
            case MP_EXT8:
            case MP_EXT16:
            case MP_EXT32:
                got = "ext";
                break;
            default:
                got = "unknown";
                break;
        }
    }
    ms_raise_validation_error(path, "Expected `str`, got `%s`%U", got);
    return NULL;
}

static MS_INLINE Py_ssize_t
mpack_decode_cstr(DecoderState *self, char ** out, PathNode *path) {
    char op = 0;
    Py_ssize_t size;
    if (mpack_read1(self, &op) < 0) return -1;

    if ('\xa0' <= op && op <= '\xbf') {
        size = op & 0x1f;
    }
    else if (op == MP_STR8) {
        size = mpack_decode_size1(self);
    }
    else if (op == MP_STR16) {
        size = mpack_decode_size2(self);
    }
    else if (op == MP_STR32) {
        size = mpack_decode_size4(self);
    }
    else {
        mpack_error_expected(op, "str", path);
        return -1;
    }

    if (mpack_read(self, out, size) < 0) return -1;
    return size;
}

static PyObject *
mpack_decode_struct_map(
    DecoderState *self, Py_ssize_t size, StructMetaObject *st_type,
    TypeNode *type, PathNode *path, bool is_key
) {
    Py_ssize_t i, key_size, field_index, pos = 0;
    char *key = NULL;
    PyObject *res, *val = NULL;

    res = Struct_alloc((PyTypeObject *)(st_type));
    if (res == NULL) return NULL;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(res);
        return NULL;
    }
    for (i = 0; i < size; i++) {
        PathNode key_path = {path, PATH_KEY, NULL};
        key_size = mpack_decode_cstr(self, &key, &key_path);
        if (MS_UNLIKELY(key_size < 0)) goto error;

        field_index = StructMeta_get_field_index(st_type, key, key_size, &pos);
        if (field_index < 0) {
            /* Skip unknown fields */
            if (mpack_skip(self) < 0) goto error;
        }
        else {
            PathNode field_path = {path, field_index, st_type};
            val = mpack_decode(
                self, st_type->struct_types[field_index], &field_path, is_key
            );
            if (val == NULL) goto error;
            Struct_set_index(res, field_index, val);
        }
    }

    if (Struct_fill_in_defaults(st_type, res, path) < 0) goto error;
    Py_LeaveRecursiveCall();
    return res;
error:
    Py_LeaveRecursiveCall();
    Py_DECREF(res);
    return NULL;
}

static PyObject *
mpack_decode_map(
    DecoderState *self, Py_ssize_t size, TypeNode *type,
    PathNode *path, bool is_key
) {
    if (type->types & MS_TYPE_ANY) {
        TypeNode type_any = {MS_TYPE_ANY};
        return mpack_decode_dict(self, size, &type_any, &type_any, path);
    }
    else if (type->types & MS_TYPE_DICT) {
        TypeNode *key, *val;
        TypeNode_get_dict(type, &key, &val);
        return mpack_decode_dict(self, size, key, val, path);
    }
    else if (type->types & MS_TYPE_STRUCT) {
        StructMetaObject *struct_type = TypeNode_get_struct(type);
        if (struct_type->asarray != OPT_TRUE) {
            return mpack_decode_struct_map(self, size, struct_type, type, path, is_key);
        }
    }
    return ms_validation_error("object", type, path);
}

static PyObject *
mpack_decode_ext(
    DecoderState *self, Py_ssize_t size, TypeNode *type, PathNode *path
) {
    Py_buffer *buffer;
    char code = 0, *data_buf = NULL;
    PyObject *data, *pycode = NULL, *view = NULL, *out = NULL;

    if (size < 0) return NULL;
    if (mpack_read1(self, &code) < 0) return NULL;
    if (mpack_read(self, &data_buf, size) < 0) return NULL;

    if (type->types & MS_TYPE_DATETIME && code == -1) {
        return mpack_decode_datetime(self, data_buf, size, path);
    }
    else if (type->types & MS_TYPE_EXT) {
        data = PyBytes_FromStringAndSize(data_buf, size);
        if (data == NULL) return NULL;
        return Ext_New(code, data);
    }
    else if (!(type->types & MS_TYPE_ANY)) {
        return ms_validation_error("ext", type, path);
    }

    /* Decode Any.
     * - datetime if code == -1
     * - call ext_hook if available
     * - otherwise return Ext object
     * */
    if (code == -1) {
        return mpack_decode_datetime(self, data_buf, size, path);
    }
    else if (self->ext_hook == NULL) {
        data = PyBytes_FromStringAndSize(data_buf, size);
        if (data == NULL) return NULL;
        return Ext_New(code, data);
    }
    else {
        pycode = PyLong_FromLong(code);
        if (pycode == NULL) goto done;
    }
    view = PyMemoryView_GetContiguous(self->buffer_obj, PyBUF_READ, 'C');
    if (view == NULL) goto done;
    buffer = PyMemoryView_GET_BUFFER(view);
    buffer->buf = data_buf;
    buffer->len = size;

    out = PyObject_CallFunctionObjArgs(self->ext_hook, pycode, view, NULL);
done:
    Py_XDECREF(pycode);
    Py_XDECREF(view);
    return out;
}

static PyObject *
mpack_decode_nocustom(
    DecoderState *self, TypeNode *type, PathNode *path, bool is_key
) {
    char op = 0;
    char *s = NULL;

    if (mpack_read1(self, &op) < 0) {
        return NULL;
    }

    if (-32 <= op && op <= 127) {
        return mpack_decode_int(self, op, type, path);
    }
    else if ('\xa0' <= op && op <= '\xbf') {
        return mpack_decode_str(self, op & 0x1f, type, path);
    }
    else if ('\x90' <= op && op <= '\x9f') {
        return mpack_decode_array(self, op & 0x0f, type, path, is_key);
    }
    else if ('\x80' <= op && op <= '\x8f') {
        return mpack_decode_map(self, op & 0x0f, type, path, is_key);
    }
    switch ((enum mpack_code)op) {
        case MP_NIL:
            return mpack_decode_none(self, type, path);
        case MP_TRUE:
            return mpack_decode_bool(self, Py_True, type, path);
        case MP_FALSE:
            return mpack_decode_bool(self, Py_False, type, path);
        case MP_UINT8:
            if (MS_UNLIKELY(mpack_read(self, &s, 1) < 0)) return NULL;
            return mpack_decode_uint(self, *(uint8_t *)s, type, path);
        case MP_UINT16:
            if (MS_UNLIKELY(mpack_read(self, &s, 2) < 0)) return NULL;
            return mpack_decode_uint(self, _msgspec_load16(uint16_t, s), type, path);
        case MP_UINT32:
            if (MS_UNLIKELY(mpack_read(self, &s, 4) < 0)) return NULL;
            return mpack_decode_uint(self, _msgspec_load32(uint32_t, s), type, path);
        case MP_UINT64:
            if (MS_UNLIKELY(mpack_read(self, &s, 8) < 0)) return NULL;
            return mpack_decode_uint(self, _msgspec_load64(uint64_t, s), type, path);
        case MP_INT8:
            if (MS_UNLIKELY(mpack_read(self, &s, 1) < 0)) return NULL;
            return mpack_decode_int(self, *(int8_t *)s, type, path);
        case MP_INT16:
            if (MS_UNLIKELY(mpack_read(self, &s, 2) < 0)) return NULL;
            return mpack_decode_int(self, _msgspec_load16(int16_t, s), type, path);
        case MP_INT32:
            if (MS_UNLIKELY(mpack_read(self, &s, 4) < 0)) return NULL;
            return mpack_decode_int(self, _msgspec_load32(int32_t, s), type, path);
        case MP_INT64:
            if (MS_UNLIKELY(mpack_read(self, &s, 8) < 0)) return NULL;
            return mpack_decode_int(self, _msgspec_load64(int64_t, s), type, path);
        case MP_FLOAT32: {
            float f = 0;
            uint32_t uf;
            if (mpack_read(self, &s, 4) < 0) return NULL;
            uf = _msgspec_load32(uint32_t, s);
            memcpy(&f, &uf, 4);
            return mpack_decode_float(self, f, type, path);
        }
        case MP_FLOAT64: {
            double f = 0;
            uint64_t uf;
            if (mpack_read(self, &s, 8) < 0) return NULL;
            uf = _msgspec_load64(uint64_t, s);
            memcpy(&f, &uf, 8);
            return mpack_decode_float(self, f, type, path);
        }
        case MP_STR8: {
            Py_ssize_t size = mpack_decode_size1(self);
            if (MS_UNLIKELY(size < 0)) return NULL;
            return mpack_decode_str(self, size, type, path);
        }
        case MP_STR16: {
            Py_ssize_t size = mpack_decode_size2(self);
            if (MS_UNLIKELY(size < 0)) return NULL;
            return mpack_decode_str(self, size, type, path);
        }
        case MP_STR32: {
            Py_ssize_t size = mpack_decode_size4(self);
            if (MS_UNLIKELY(size < 0)) return NULL;
            return mpack_decode_str(self, size, type, path);
        }
        case MP_BIN8:
            return mpack_decode_bin(self, mpack_decode_size1(self), type, path);
        case MP_BIN16:
            return mpack_decode_bin(self, mpack_decode_size2(self), type, path);
        case MP_BIN32:
            return mpack_decode_bin(self, mpack_decode_size4(self), type, path);
        case MP_ARRAY16: {
            Py_ssize_t size = mpack_decode_size2(self);
            if (MS_UNLIKELY(size < 0)) return NULL;
            return mpack_decode_array(self, size, type, path, is_key);
        }
        case MP_ARRAY32: {
            Py_ssize_t size = mpack_decode_size4(self);
            if (MS_UNLIKELY(size < 0)) return NULL;
            return mpack_decode_array(self, size, type, path, is_key);
        }
        case MP_MAP16: {
            Py_ssize_t size = mpack_decode_size2(self);
            if (MS_UNLIKELY(size < 0)) return NULL;
            return mpack_decode_map(self, size, type, path, is_key);
        }
        case MP_MAP32: {
            Py_ssize_t size = mpack_decode_size4(self);
            if (MS_UNLIKELY(size < 0)) return NULL;
            return mpack_decode_map(self, size, type, path, is_key);
        }
        case MP_FIXEXT1:
            return mpack_decode_ext(self, 1, type, path);
        case MP_FIXEXT2:
            return mpack_decode_ext(self, 2, type, path);
        case MP_FIXEXT4:
            return mpack_decode_ext(self, 4, type, path);
        case MP_FIXEXT8:
            return mpack_decode_ext(self, 8, type, path);
        case MP_FIXEXT16:
            return mpack_decode_ext(self, 16, type, path);
        case MP_EXT8:
            return mpack_decode_ext(self, mpack_decode_size1(self), type, path);
        case MP_EXT16:
            return mpack_decode_ext(self, mpack_decode_size2(self), type, path);
        case MP_EXT32:
            return mpack_decode_ext(self, mpack_decode_size4(self), type, path);
        default:
            PyErr_Format(
                msgspec_get_global_state()->DecodeError,
                "MessagePack data is malformed: invalid opcode '\\x%02x' (byte %zd)",
                op,
                (Py_ssize_t)(self->input_pos - self->input_start - 1)
            );
            return NULL;
    }
}

static PyObject *
mpack_decode(
    DecoderState *self, TypeNode *type, PathNode *path, bool is_key
) {
    PyObject *obj = mpack_decode_nocustom(self, type, path, is_key);
    if (MS_UNLIKELY(type->types & (MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC))) {
        return ms_decode_custom(
            obj, self->dec_hook, type->types & MS_TYPE_CUSTOM_GENERIC, type, path
        );
    }
    return obj;
}

PyDoc_STRVAR(Decoder_decode__doc__,
"decode(self, buf)\n"
"--\n"
"\n"
"Deserialize an object from bytes.\n"
"\n"
"Parameters\n"
"----------\n"
"buf : bytes-like\n"
"    The message to decode.\n"
"\n"
"Returns\n"
"-------\n"
"obj : Any\n"
"    The deserialized object\n"
);
static PyObject*
Decoder_decode(Decoder *self, PyObject *const *args, Py_ssize_t nargs)
{
    PyObject *res = NULL;
    Py_buffer buffer;
    buffer.buf = NULL;

    if (!check_positional_nargs(nargs, 1, 1)) {
        return NULL;
    }

    if (PyObject_GetBuffer(args[0], &buffer, PyBUF_CONTIG_RO) >= 0) {
        self->state.buffer_obj = args[0];
        self->state.input_start = buffer.buf;
        self->state.input_pos = buffer.buf;
        self->state.input_end = self->state.input_pos + buffer.len;

        res = mpack_decode(&(self->state), self->state.type, NULL, false);

        if (res != NULL && mpack_has_trailing_characters(&self->state)) {
            res = NULL;
        }

        PyBuffer_Release(&buffer);
        self->state.buffer_obj = NULL;
        self->state.input_start = NULL;
        self->state.input_pos = NULL;
        self->state.input_end = NULL;
    }
    return res;
}

static struct PyMethodDef Decoder_methods[] = {
    {
        "decode", (PyCFunction) Decoder_decode, METH_FASTCALL,
        Decoder_decode__doc__,
    },
    {NULL, NULL}                /* sentinel */
};

static PyMemberDef Decoder_members[] = {
    {"type", T_OBJECT_EX, offsetof(Decoder, orig_type), READONLY, "The Decoder type"},
    {"dec_hook", T_OBJECT, offsetof(Decoder, state.dec_hook), READONLY, "The Decoder dec_hook"},
    {"ext_hook", T_OBJECT, offsetof(Decoder, state.ext_hook), READONLY, "The Decoder ext_hook"},
    {NULL},
};

static PyTypeObject Decoder_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec.msgpack.Decoder",
    .tp_doc = Decoder__doc__,
    .tp_basicsize = sizeof(Decoder),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc)Decoder_init,
    .tp_traverse = (traverseproc)Decoder_traverse,
    .tp_dealloc = (destructor)Decoder_dealloc,
    .tp_repr = (reprfunc)Decoder_repr,
    .tp_methods = Decoder_methods,
    .tp_members = Decoder_members,
};


PyDoc_STRVAR(msgspec_msgpack_decode__doc__,
"msgpack_decode(buf, *, type='Any', dec_hook=None, ext_hook=None)\n"
"--\n"
"\n"
"Deserialize an object from bytes.\n"
"\n"
"Parameters\n"
"----------\n"
"buf : bytes-like\n"
"    The message to decode.\n"
"type : Type, optional\n"
"    A Python type (in type annotation form) to decode the object as. If\n"
"    provided, the message will be type checked and decoded as the specified\n"
"    type. Defaults to `Any`, in which case the message will be decoded using\n"
"    the default MessagePack types.\n"
"dec_hook : Callable, optional\n"
"    An optional callback for handling decoding custom types. Should have the\n"
"    signature ``dec_hook(type: Type, obj: Any) -> Any``, where ``type`` is the\n"
"    expected message type, and ``obj`` is the decoded representation composed\n"
"    of only basic MessagePack types. This hook should transform ``obj`` into\n"
"    type ``type``, or raise a ``TypeError`` if unsupported.\n"
"ext_hook : Callable, optional\n"
"    An optional callback for decoding MessagePack extensions. Should have the\n"
"    signature ``ext_hook(code: int, data: memoryview) -> Any``. If provided,\n"
"    this will be called to deserialize all extension types found in the\n"
"    message. Note that ``data`` is a memoryview into the larger message\n"
"    buffer - any references created to the underlying buffer without copying\n"
"    the data out will cause the full message buffer to persist in memory.\n"
"    If not provided, extension types will decode as ``msgspec.Ext`` objects.\n"
"\n"
"Returns\n"
"-------\n"
"obj : Any\n"
"    The deserialized object\n"
"\n"
"See Also\n"
"--------\n"
"Decoder.decode"
);
static PyObject*
msgspec_msgpack_decode(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PyObject *res = NULL, *buf = NULL, *type = NULL, *dec_hook = NULL, *ext_hook = NULL;
    DecoderState state;
    Py_buffer buffer;
    MsgspecState *st = msgspec_get_global_state();

    /* Parse arguments */
    if (!check_positional_nargs(nargs, 1, 1)) return NULL;
    buf = args[0];
    if (kwnames != NULL) {
        Py_ssize_t nkwargs = PyTuple_GET_SIZE(kwnames);
        if ((type = find_keyword(kwnames, args + nargs, st->str_type)) != NULL) nkwargs--;
        if ((dec_hook = find_keyword(kwnames, args + nargs, st->str_dec_hook)) != NULL) nkwargs--;
        if ((ext_hook = find_keyword(kwnames, args + nargs, st->str_ext_hook)) != NULL) nkwargs--;
        if (nkwargs > 0) {
            PyErr_SetString(
                PyExc_TypeError,
                "Extra keyword arguments provided"
            );
            return NULL;
        }
    }

    /* Handle dec_hook */
    if (dec_hook == Py_None) {
        dec_hook = NULL;
    }
    if (dec_hook != NULL) {
        if (!PyCallable_Check(dec_hook)) {
            PyErr_SetString(PyExc_TypeError, "dec_hook must be callable");
            return NULL;
        }
    }
    state.dec_hook = dec_hook;

    /* Handle ext_hook */
    if (ext_hook == Py_None) {
        ext_hook = NULL;
    }
    if (ext_hook != NULL) {
        if (!PyCallable_Check(ext_hook)) {
            PyErr_SetString(PyExc_TypeError, "ext_hook must be callable");
            return NULL;
        }
    }
    state.ext_hook = ext_hook;

    /* Only build TypeNode if required */
    state.type = NULL;
    if (type != NULL && type != st->typing_any) {
        state.type = TypeNode_Convert(type, false, NULL);
        if (state.type == NULL) return NULL;
    }

    buffer.buf = NULL;
    if (PyObject_GetBuffer(buf, &buffer, PyBUF_CONTIG_RO) >= 0) {
        state.buffer_obj = buf;
        state.input_start = buffer.buf;
        state.input_pos = buffer.buf;
        state.input_end = state.input_pos + buffer.len;
        if (state.type != NULL) {
            res = mpack_decode(&state, state.type, NULL, false);
        } else {
            TypeNode type_any = {MS_TYPE_ANY};
            res = mpack_decode(&state, &type_any, NULL, false);
        }
        PyBuffer_Release(&buffer);
        if (res != NULL && mpack_has_trailing_characters(&state)) {
            res = NULL;
        }
    }

    if (state.type != NULL) {
        TypeNode_Free(state.type);
    }
    return res;
}

/*************************************************************************
 * JSON Decoder                                                          *
 *************************************************************************/

typedef struct JSONDecoderState {
    /* Configuration */
    TypeNode *type;
    PyObject *dec_hook;

    /* Temporary scratch space */
    unsigned char *scratch;
    Py_ssize_t scratch_capacity;
    Py_ssize_t scratch_len;

    /* Per-message attributes */
    PyObject *buffer_obj;
    unsigned char *input_start;
    unsigned char *input_pos;
    unsigned char *input_end;
} JSONDecoderState;

typedef struct JSONDecoder {
    PyObject_HEAD
    PyObject *orig_type;
    JSONDecoderState state;
} JSONDecoder;

PyDoc_STRVAR(JSONDecoder__doc__,
"Decoder(type='Any', *, dec_hook=None)\n"
"--\n"
"\n"
"A JSON decoder.\n"
"\n"
"Parameters\n"
"----------\n"
"type : Type, optional\n"
"    A Python type (in type annotation form) to decode the object as. If\n"
"    provided, the message will be type checked and decoded as the specified\n"
"    type. Defaults to `Any`, in which case the message will be decoded using\n"
"    the default JSON types.\n"
"dec_hook : Callable, optional\n"
"    An optional callback for handling decoding custom types. Should have the\n"
"    signature ``dec_hook(type: Type, obj: Any) -> Any``, where ``type`` is the\n"
"    expected message type, and ``obj`` is the decoded representation composed\n"
"    of only basic JSON types. This hook should transform ``obj`` into type\n"
"    ``type``, or raise a ``TypeError`` if unsupported."
);
static int
JSONDecoder_init(JSONDecoder *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"type", "dec_hook", NULL};
    MsgspecState *st = msgspec_get_global_state();
    PyObject *type = st->typing_any;
    PyObject *dec_hook = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O$O", kwlist, &type, &dec_hook)) {
        return -1;
    }

    /* Handle dec_hook */
    if (dec_hook == Py_None) {
        dec_hook = NULL;
    }
    if (dec_hook != NULL) {
        if (!PyCallable_Check(dec_hook)) {
            PyErr_SetString(PyExc_TypeError, "dec_hook must be callable");
            return -1;
        }
        Py_INCREF(dec_hook);
    }
    self->state.dec_hook = dec_hook;

    /* Handle type */
    self->state.type = TypeNode_Convert(type, true, NULL);
    if (self->state.type == NULL) return -1;
    Py_INCREF(type);
    self->orig_type = type;

    /* Init scratch space */
    self->state.scratch = NULL;
    self->state.scratch_capacity = 0;
    self->state.scratch_len = 0;

    return 0;
}

static int
JSONDecoder_traverse(JSONDecoder *self, visitproc visit, void *arg)
{
    int out = TypeNode_traverse(self->state.type, visit, arg);
    if (out != 0) return out;
    Py_VISIT(self->orig_type);
    Py_VISIT(self->state.dec_hook);
    return 0;
}

static void
JSONDecoder_dealloc(JSONDecoder *self)
{
    TypeNode_Free(self->state.type);
    Py_XDECREF(self->orig_type);
    Py_XDECREF(self->state.dec_hook);
    PyMem_Free(self->state.scratch);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
JSONDecoder_repr(JSONDecoder *self) {
    int recursive;
    PyObject *typstr, *out = NULL;

    recursive = Py_ReprEnter((PyObject *)self);
    if (recursive != 0) {
        return (recursive < 0) ? NULL : PyUnicode_FromString("...");
    }
    typstr = PyObject_Repr(self->orig_type);
    if (typstr != NULL) {
        out = PyUnicode_FromFormat("msgspec.json.Decoder(%U)", typstr);
    }
    Py_XDECREF(typstr);
    Py_ReprLeave((PyObject *)self);
    return out;
}

static MS_INLINE bool
json_read1(JSONDecoderState *self, unsigned char *c)
{
    if (MS_UNLIKELY(self->input_pos == self->input_end)) {
        ms_err_truncated();
        return false;
    }
    *c = *self->input_pos;
    self->input_pos += 1;
    return true;
}

static MS_INLINE char
json_peek_or_null(JSONDecoderState *self) {
    if (MS_UNLIKELY(self->input_pos == self->input_end)) return '\0';
    return *self->input_pos;
}

static MS_INLINE bool
json_peek_skip_ws(JSONDecoderState *self, unsigned char *s)
{
    while (true) {
        if (MS_UNLIKELY(self->input_pos == self->input_end)) {
            ms_err_truncated();
            return false;
        }
        unsigned char c = *self->input_pos;
        if (!(c == ' ' || c == '\n' || c == '\t' || c == '\r')) {
            *s = c;
            return true;
        }
        self->input_pos++;
    }
}

static MS_INLINE bool
json_remaining(JSONDecoderState *self, ptrdiff_t remaining)
{
    return self->input_end - self->input_pos >= remaining;
}

static PyObject *
json_err_invalid(JSONDecoderState *self, const char *msg)
{
    PyErr_Format(
        msgspec_get_global_state()->DecodeError,
        "JSON is malformed: %s (byte %zd)",
        msg,
        (Py_ssize_t)(self->input_pos - self->input_start)
    );
    return NULL;
}

static MS_INLINE bool
json_has_trailing_characters(JSONDecoderState *self)
{
    while (self->input_pos != self->input_end) {
        unsigned char c = *self->input_pos++;
        if (MS_UNLIKELY(!(c == ' ' || c == '\n' || c == '\t' || c == '\r'))) {
            json_err_invalid(self, "trailing characters");
            return true;
        }
    }
    return false;
}

static int json_skip(JSONDecoderState *self);

static PyObject * json_decode(
    JSONDecoderState *self, TypeNode *type, PathNode *path
);

static PyObject *
json_decode_none(JSONDecoderState *self, TypeNode *type, PathNode *path) {
    self->input_pos++;  /* Already checked as 'n' */
    if (MS_UNLIKELY(!json_remaining(self, 3))) {
        ms_err_truncated();
        return NULL;
    }
    unsigned char c1 = *self->input_pos++;
    unsigned char c2 = *self->input_pos++;
    unsigned char c3 = *self->input_pos++;
    if (MS_UNLIKELY(c1 != 'u' || c2 != 'l' || c3 != 'l')) {
        return json_err_invalid(self, "invalid character");
    }
    if (type->types & (MS_TYPE_ANY | MS_TYPE_NONE)) {
        Py_INCREF(Py_None);
        return Py_None;
    }
    return ms_validation_error("null", type, path);
}

static PyObject *
json_decode_true(JSONDecoderState *self, TypeNode *type, PathNode *path) {
    self->input_pos++;  /* Already checked as 't' */
    if (MS_UNLIKELY(!json_remaining(self, 3))) {
        ms_err_truncated();
        return NULL;
    }
    unsigned char c1 = *self->input_pos++;
    unsigned char c2 = *self->input_pos++;
    unsigned char c3 = *self->input_pos++;
    if (MS_UNLIKELY(c1 != 'r' || c2 != 'u' || c3 != 'e')) {
        return json_err_invalid(self, "invalid character");
    }
    if (type->types & (MS_TYPE_ANY | MS_TYPE_BOOL)) {
        Py_INCREF(Py_True);
        return Py_True;
    }
    return ms_validation_error("bool", type, path);
}

static PyObject *
json_decode_false(JSONDecoderState *self, TypeNode *type, PathNode *path) {
    self->input_pos++;  /* Already checked as 'f' */
    if (MS_UNLIKELY(!json_remaining(self, 4))) {
        ms_err_truncated();
        return NULL;
    }
    unsigned char c1 = *self->input_pos++;
    unsigned char c2 = *self->input_pos++;
    unsigned char c3 = *self->input_pos++;
    unsigned char c4 = *self->input_pos++;
    if (MS_UNLIKELY(c1 != 'a' || c2 != 'l' || c3 != 's' || c4 != 'e')) {
        return json_err_invalid(self, "invalid character");
    }
    if (type->types & (MS_TYPE_ANY | MS_TYPE_BOOL)) {
        Py_INCREF(Py_False);
        return Py_False;
    }
    return ms_validation_error("bool", type, path);
}

static PyObject *
json_decode_list(JSONDecoderState *self, TypeNode *el_type, PathNode *path) {
    PyObject *out, *item = NULL;
    unsigned char c;
    bool first = true;
    PathNode el_path = {path, 0, NULL};

    self->input_pos++; /* Skip '[' */

    out = PyList_New(0);
    if (out == NULL) return NULL;
    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL;
    }
    while (true) {
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        /* Parse ']' or ',', then peek the next character */
        if (c == ']') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or ']'");
            goto error;
        }

        if (MS_UNLIKELY(c == ']')) {
            json_err_invalid(self, "trailing comma in array");
            goto error;
        }

        /* Parse item */
        item = json_decode(self, el_type, &el_path);
        if (item == NULL) goto error;
        el_path.index++;

        /* Append item to list */
        if (PyList_Append(out, item) < 0) goto error;
        Py_CLEAR(item);
    }
    Py_LeaveRecursiveCall();
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_DECREF(out);
    Py_XDECREF(item);
    return NULL;
}

static PyObject *
json_decode_set(JSONDecoderState *self, TypeNode *el_type, PathNode *path) {
    PyObject *out, *item = NULL;
    unsigned char c;
    bool first = true;
    PathNode el_path = {path, 0, NULL};

    self->input_pos++; /* Skip '[' */

    out = PySet_New(NULL);
    if (out == NULL) return NULL;
    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL;
    }
    while (true) {
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        /* Parse ']' or ',', then peek the next character */
        if (c == ']') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or ']'");
            goto error;
        }

        if (MS_UNLIKELY(c == ']')) {
            json_err_invalid(self, "trailing comma in array");
            goto error;
        }

        /* Parse item */
        item = json_decode(self, el_type, &el_path);
        if (item == NULL) goto error;
        el_path.index++;

        /* Append item to set */
        if (PySet_Add(out, item) < 0) goto error;
        Py_CLEAR(item);
    }
    Py_LeaveRecursiveCall();
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_DECREF(out);
    Py_XDECREF(item);
    return NULL;
}

static PyObject *
json_decode_vartuple(JSONDecoderState *self, TypeNode *el_type, PathNode *path) {
    PyObject *list, *item, *out = NULL;
    Py_ssize_t size, i;

    list = json_decode_list(self, el_type, path);
    if (list == NULL) return NULL;

    size = PyList_GET_SIZE(list);
    out = PyTuple_New(size);
    if (out != NULL) {
        for (i = 0; i < size; i++) {
            item = PyList_GET_ITEM(list, i);
            PyTuple_SET_ITEM(out, i, item);
            PyList_SET_ITEM(list, i, NULL);  /* Drop reference in old list */
        }
    }
    Py_DECREF(list);
    return out;
}

static PyObject *
json_decode_fixtuple(JSONDecoderState *self, TypeNode *type, PathNode *path) {
    PyObject *out, *item;
    Py_ssize_t i = 0, offset;
    unsigned char c;
    bool first = true;
    TypeNodeExtra *tex = (TypeNodeExtra *)type;
    PathNode el_path = {path, 0, NULL};

    offset = TypeNode_get_array_offset(type);

    self->input_pos++; /* Skip '[' */

    out = PyTuple_New(tex->fixtuple_size);
    if (out == NULL) return NULL;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL;
    }

    while (true) {
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        /* Parse ']' or ',', then peek the next character */
        if (c == ']') {
            self->input_pos++;
            if (MS_UNLIKELY(i < tex->fixtuple_size)) goto size_error;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or ']'");
            goto error;
        }

        if (MS_UNLIKELY(c == ']')) {
            json_err_invalid(self, "trailing comma in array");
            goto error;
        }

        /* Check we don't have too many elements */
        if (MS_UNLIKELY(i >= tex->fixtuple_size)) goto size_error;

        /* Parse item */
        item = json_decode(self, tex->extra[offset + i], &el_path);
        if (item == NULL) goto error;
        el_path.index++;

        /* Add item to tuple */
        PyTuple_SET_ITEM(out, i, item);
        i++;
    }
    Py_LeaveRecursiveCall();
    return out;

size_error:
    ms_raise_validation_error(
        path,
        "Expected `array` of length %zd",
        tex->fixtuple_size
    );
error:
    Py_LeaveRecursiveCall();
    Py_DECREF(out);
    return NULL;
}

static PyObject *
json_decode_struct_array(
    JSONDecoderState *self, StructMetaObject *st_type, TypeNode *type,
    PathNode *path
) {
    Py_ssize_t nfields, ndefaults, npos, i = 0;
    PyObject *out, *item = NULL;
    unsigned char c;
    bool should_untrack, first = true;

    self->input_pos++; /* Skip '[' */

    out = Struct_alloc((PyTypeObject *)(st_type));
    if (out == NULL) return NULL;

    nfields = PyTuple_GET_SIZE(st_type->struct_fields);
    ndefaults = PyTuple_GET_SIZE(st_type->struct_defaults);
    npos = nfields - ndefaults;
    should_untrack = PyObject_IS_GC(out);

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL;
    }
    while (true) {
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        /* Parse ']' or ',', then peek the next character */
        if (c == ']') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or ']'");
            goto error;
        }

        if (MS_UNLIKELY(c == ']')) {
            json_err_invalid(self, "trailing comma in array");
            goto error;
        }

        if (MS_LIKELY(i < nfields)) {
            /* Parse item */
            PathNode el_path = {path, i};
            item = json_decode(self, st_type->struct_types[i], &el_path);
            if (MS_UNLIKELY(item == NULL)) goto error;
            Struct_set_index(out, i, item);
            if (should_untrack) {
                should_untrack = !OBJ_IS_GC(item);
            }
            i++;
        }
        else {
            /* Skip trailing fields */
            if (json_skip(self) < 0) goto error;
        }
    }

    /* Check for missing required fields */
    if (i < npos) {
        ms_raise_validation_error(
            path,
            "Object missing required field `%U`%U",
            PyTuple_GET_ITEM(st_type->struct_fields, i)
        );
        goto error;
    }
    /* Fill in missing fields with defaults */
    for (; i < nfields; i++) {
        item = maybe_deepcopy_default(
            PyTuple_GET_ITEM(st_type->struct_defaults, i - npos)
        );
        if (item == NULL) goto error;
        Struct_set_index(out, i, item);
        if (should_untrack) {
            should_untrack = !OBJ_IS_GC(item);
        }
    }
    Py_LeaveRecursiveCall();
    if (should_untrack)
        PyObject_GC_UnTrack(out);
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_DECREF(out);
    return NULL;
}

static PyObject *
json_decode_array(
    JSONDecoderState *self, TypeNode *type, PathNode *path
) {
    if (type->types & MS_TYPE_ANY) {
        TypeNode type_any = {MS_TYPE_ANY};
        return json_decode_list(self, &type_any, path);
    }
    else if (type->types & MS_TYPE_LIST) {
        return json_decode_list(self, TypeNode_get_array(type), path);
    }
    else if (type->types & MS_TYPE_SET) {
        return json_decode_set(self, TypeNode_get_array(type), path);
    }
    else if (type->types & MS_TYPE_VARTUPLE) {
        return json_decode_vartuple(self, TypeNode_get_array(type), path);
    }
    else if (type->types & MS_TYPE_FIXTUPLE) {
        return json_decode_fixtuple(self, type, path);
    }
    else if (type->types & MS_TYPE_STRUCT) {
        StructMetaObject *struct_type = TypeNode_get_struct(type);
        if (struct_type->asarray == OPT_TRUE) {
            return json_decode_struct_array(self, struct_type, type, path);
        }
    }
    return ms_validation_error("array", type, path);
}

#define JS_SCRATCH_MAX_SIZE 1024

static int
json_scratch_resize(JSONDecoderState *state, Py_ssize_t size) {
    unsigned char *temp = PyMem_Realloc(state->scratch, size);
    if (MS_UNLIKELY(temp == NULL)) {
        PyErr_NoMemory();
        return -1;
    }
    state->scratch = temp;
    state->scratch_capacity = size;
    return 0;
}

static int
json_scratch_ensure_space(JSONDecoderState *state, Py_ssize_t size) {
    Py_ssize_t required = state->scratch_len + size;
    if (required >= state->scratch_capacity) {
        size_t new_size = Py_MAX(8, 1.5 * required);
        if (json_scratch_resize(state, new_size) < 0) return -1;
    }
    return 0;
}

static int
json_scratch_reset(JSONDecoderState *state) {
    state->scratch_len = 0;
    if (state->scratch_capacity > JS_SCRATCH_MAX_SIZE) {
        if (json_scratch_resize(state, JS_SCRATCH_MAX_SIZE) < 0) return -1;
    }
    return 0;
}

static int
json_scratch_extend(JSONDecoderState *state, const void *buf, Py_ssize_t size) {
    if (json_scratch_ensure_space(state, size) < 0) return -1;
    memcpy(state->scratch + state->scratch_len, buf, size);
    state->scratch_len += size;
    return 0;
}

static int
json_scratch_push(JSONDecoderState *state, unsigned char c) {
    return json_scratch_extend(state, &c, 1);
}

static int
json_read_codepoint(JSONDecoderState *self, unsigned int *out) {
    unsigned char c;
    unsigned int cp = 0;
    if (!json_remaining(self, 4)) return ms_err_truncated();
    for (int i = 0; i < 4; i++) {
        c = *self->input_pos++;
        if (c >= '0' && c <= '9') {
            c -= '0';
        }
        else if (c >= 'a' && c <= 'f') {
            c = c - 'a' + 10;
        }
        else if (c >= 'A' && c <= 'F') {
            c = c - 'A' + 10;
        }
        else {
            json_err_invalid(self, "invalid character in unicode escape");
            return -1;
        }
        cp = (cp << 4) + c;
    }
    *out = cp;
    return 0;
}

static int
json_parse_escape(JSONDecoderState *self) {
    unsigned char c;
    if (!json_read1(self, &c)) return -1;

    switch (c) {
        case '"': return json_scratch_push(self, '"');
        case '\\': return json_scratch_push(self, '\\');
        case '/': return json_scratch_push(self, '/');
        case 'b': return json_scratch_push(self, '\b');
        case 'f': return json_scratch_push(self, '\f');
        case 'n': return json_scratch_push(self, '\n');
        case 'r': return json_scratch_push(self, '\r');
        case 't': return json_scratch_push(self, '\t');
        case 'u': {
            unsigned int cp;
            if (json_read_codepoint(self, &cp) < 0) return -1;

            if (0xDC00 <= cp && cp <= 0xDFFF) {
                json_err_invalid(self, "invalid utf-16 surrogate pair");
                return -1;
            }
            else if (0xD800 <= cp && cp <= 0xDBFF) {
                /* utf-16 pair, parse 2nd pair */
                unsigned int cp2;
                if (!json_remaining(self, 6)) return ms_err_truncated();
                if (self->input_pos[0] != '\\' || self->input_pos[1] != 'u') {
                    json_err_invalid(self, "unexpected end of escaped utf-16 surrogate pair");
                    return -1;
                }
                self->input_pos += 2;
                if (json_read_codepoint(self, &cp2) < 0) return -1;
                if (cp2 < 0xDC00 || cp2 > 0xDFFF) {
                    json_err_invalid(self, "invalid utf-16 surrogate pair");
                    return -1;
                }
                cp = 0x10000 + (((cp - 0xD800) << 10) | (cp2 - 0xDC00));
            }

            /* Encode the codepoint as utf-8 */
            if (json_scratch_ensure_space(self, 4) < 0) return -1;
            unsigned char *p = self->scratch + self->scratch_len;
            if (cp < 0x80) {
                *p++ = cp;
                self->scratch_len += 1;
            } else if (cp < 0x800) {
                *p++ = 0xC0 | (cp >> 6);
                *p++ = 0x80 | (cp & 0x3F);
                self->scratch_len += 2;
            } else if (cp < 0x10000) {
                *p++ = 0xE0 | (cp >> 12);
                *p++ = 0x80 | ((cp >> 6) & 0x3F);
                *p++ = 0x80 | (cp & 0x3F);
                self->scratch_len += 3;
            } else {
                *p++ = 0xF0 | (cp >> 18);
                *p++ = 0x80 | ((cp >> 12) & 0x3F);
                *p++ = 0x80 | ((cp >> 6) & 0x3F);
                *p++ = 0x80 | (cp & 0x3F);
                self->scratch_len += 4;
            }
            return 0;
        }
        default:
            json_err_invalid(self, "invalid escape character in string");
            return -1;
    }
    return 0;
}

static Py_ssize_t
json_decode_string_view(JSONDecoderState *self, char **out) {
    Py_ssize_t size;
    self->scratch_len = 0;
    self->input_pos++; /* Skip '"' */
    unsigned char *start = self->input_pos;
    while (true) {
        while (self->input_pos < self->input_end && !escape_table[*(self->input_pos)]) {
            self->input_pos += 1;
        }
        if (MS_UNLIKELY(self->input_pos == self->input_end)) {
            return ms_err_truncated();
        }
        switch (*(self->input_pos)) {
            case '"': {
                if (self->scratch_len == 0) {
                    *out = (char *)start;
                    size = self->input_pos - start;
                }
                else {
                    if (json_scratch_extend(self, start, self->input_pos - start) < 0) return -1;
                    *out = (char *)(self->scratch);
                    size = self->scratch_len;
                }
                self->input_pos += 1;
                return size;
            }
            case '\\': {
                if (json_scratch_extend(self, start, self->input_pos - start) < 0) return -1;
                self->input_pos += 1;
                if (json_parse_escape(self) < 0) return -1;
                start = self->input_pos;
                break;
            }
            default: {
                self->input_pos += 1;
                json_err_invalid(self, "invalid character");
                return -1;
            }
        }
    }
}

/* A table of the corresponding base64 value for each character, or -1 if an
 * invalid character in the base64 alphabet (note the padding char '=' is
 * handled elsewhere, so is marked as invalid here as well) */
static const uint8_t base64_decode_table[] = {
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,62, -1,-1,-1,63,
    52,53,54,55, 56,57,58,59, 60,61,-1,-1, -1,-1,-1,-1,
    -1, 0, 1, 2,  3, 4, 5, 6,  7, 8, 9,10, 11,12,13,14,
    15,16,17,18, 19,20,21,22, 23,24,25,-1, -1,-1,-1,-1,
    -1,26,27,28, 29,30,31,32, 33,34,35,36, 37,38,39,40,
    41,42,43,44, 45,46,47,48, 49,50,51,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
};

static PyObject *
json_decode_binary(
    JSONDecoderState *self, const char *buffer, Py_ssize_t size,
    TypeNode *type, PathNode *path
) {
    PyObject *out = NULL;
    char *bin_buffer;
    Py_ssize_t bin_size, i;

    if (size % 4 != 0) goto invalid;

    int npad = 0;
    if (size > 0 && buffer[size - 1] == '=') npad++;
    if (size > 1 && buffer[size - 2] == '=') npad++;

    bin_size = (size / 4) * 3 - npad;
    if (type->types & MS_TYPE_BYTES) {
        out = PyBytes_FromStringAndSize(NULL, bin_size);
        if (out == NULL) return NULL;
        bin_buffer = PyBytes_AS_STRING(out);
    }
    else {
        out = PyByteArray_FromStringAndSize(NULL, bin_size);
        if (out == NULL) return NULL;
        bin_buffer = PyByteArray_AS_STRING(out);
    }

    int quad = 0;
    uint8_t left_c = 0;
    for (i = 0; i < size - npad; i++) {
        uint8_t c = base64_decode_table[(uint8_t)(buffer[i])];
        if (c >= 64) goto invalid;

        switch (quad) {
            case 0:
                quad = 1;
                left_c = c;
                break;
            case 1:
                quad = 2;
                *bin_buffer++ = (left_c << 2) | (c >> 4);
                left_c = c & 0x0f;
                break;
            case 2:
                quad = 3;
                *bin_buffer++ = (left_c << 4) | (c >> 2);
                left_c = c & 0x03;
                break;
            case 3:
                quad = 0;
                *bin_buffer++ = (left_c << 6) | c;
                left_c = 0;
                break;
        }
    }
    return out;

invalid:
    Py_XDECREF(out);
    MsgspecState *st = msgspec_get_global_state();
    PyObject *suffix = PathNode_ErrSuffix(path);
    if (suffix != NULL) {
        PyErr_Format(st->DecodeError, "Invalid base64 encoded string%U", suffix);
        Py_DECREF(suffix);
    }
    return NULL;
}

static inline const char *
json_read_fixint(const char *buf, int width, int *out) {
    int x = 0;
    for (int i = 0; i < width; i++) {
        char c = *buf++;
        if (!is_digit(c)) return NULL;
        x = x * 10 + (c - '0');
    }
    *out = x;
    return buf;
}

static PyObject *
json_decode_datetime(
    JSONDecoderState *self, const char *buf, Py_ssize_t size, PathNode *path
) {
    int year, month, day, hour, minute, second, microsecond = 0, offset = 0;
    const char *buf_end = buf + size;
    char c;
    MsgspecState *st;
    PyObject *suffix;

    /* A valid datetime is at least 20 characters in length */
    if (size < 20) goto invalid;

    /* Parse date */
    if ((buf = json_read_fixint(buf, 4, &year)) == NULL) goto invalid;
    if (*buf++ != '-') goto invalid;
    if ((buf = json_read_fixint(buf, 2, &month)) == NULL) goto invalid;
    if (*buf++ != '-') goto invalid;
    if ((buf = json_read_fixint(buf, 2, &day)) == NULL) goto invalid;

    /* Date/time separator can be T or t */
    c = *buf++;
    if (!(c == 'T' || c == 't')) goto invalid;

    /* Parse time */
    if ((buf = json_read_fixint(buf, 2, &hour)) == NULL) goto invalid;
    if (*buf++ != ':') goto invalid;
    if ((buf = json_read_fixint(buf, 2, &minute)) == NULL) goto invalid;
    if (*buf++ != ':') goto invalid;
    if ((buf = json_read_fixint(buf, 2, &second)) == NULL) goto invalid;

    /* This is the last read that doesn't need a bounds check */
    c = *buf++;

    /* Parse decimal if present.
     *
     * We accept up to 6 decimal digits and error if more are present. We
     * *could* instead drop the excessive digits and round, but that's more
     * complicated. Other systems commonly accept 3 or 6 digits, but RFC3339
     * doesn't specify a decimal precision.  */
    if (c == '.') {
        int ndigits = 0;
        while (buf < buf_end) {
            c = *buf++;
            if (!is_digit(c)) break;
            ndigits++;
            /* if decimal precision is higher than we support, error */
            if (ndigits > 6) goto invalid;
            microsecond = microsecond * 10 + (c - '0');
        }
        /* Error if no digits after decimal */
        if (ndigits == 0) goto invalid;
        int pow10[6] = {100000, 10000, 1000, 100, 10, 1};
        /* Scale microseconds appropriately */
        microsecond *= pow10[ndigits - 1];
    }

    /* Parse timezone */
    if (!(c == 'Z' || c == 'z')) {
        int offset_hour, offset_min;
        if (c == '-') {
            offset = -1;
        }
        else if (c == '+') {
            offset = 1;
        }
        else {
            goto invalid;
        }

        if (buf_end - buf < 5) goto invalid;

        if ((buf = json_read_fixint(buf, 2, &offset_hour)) == NULL) goto invalid;
        if (*buf++ != ':') goto invalid;
        if ((buf = json_read_fixint(buf, 2, &offset_min)) == NULL) goto invalid;
        if (offset_hour > 23 || offset_min > 59) goto invalid;
        offset *= (offset_hour * 60 + offset_min);
    }

    /* Check for trailing characters */
    if (buf != buf_end) goto invalid;

    /* Ensure all numbers are valid */
    if (year == 0) goto invalid;
    if (day == 0 || day > days_in_month(year, month)) goto invalid;
    if (month == 0 || month > 12) goto invalid;
    if (hour > 23) goto invalid;
    if (minute > 59) goto invalid;
    if (second > 59) goto invalid;

    if (offset) {
        if (datetime_apply_tz_offset(&year, &month, &day, &hour, &minute, offset) < 0) {
            goto invalid;
        }
    }
    return PyDateTimeAPI->DateTime_FromDateAndTime(
        year, month, day, hour, minute, second, microsecond,
        PyDateTime_TimeZone_UTC, PyDateTimeAPI->DateTimeType
    );

invalid:
    st = msgspec_get_global_state();
    suffix = PathNode_ErrSuffix(path);
    if (suffix != NULL) {
        PyErr_Format(st->DecodeError, "Invalid RFC3339 encoded datetime%U", suffix);
        Py_DECREF(suffix);
    }
    return NULL;
}

static PyObject *
json_decode_string(JSONDecoderState *self, TypeNode *type, PathNode *path) {
    if (
        MS_LIKELY(
            type->types & (
                MS_TYPE_ANY | MS_TYPE_STR | MS_TYPE_ENUM |
                MS_TYPE_BYTES | MS_TYPE_BYTEARRAY | MS_TYPE_DATETIME
            )
        )
    ) {
        char *view = NULL;
        Py_ssize_t size = json_decode_string_view(self, &view);
        if (size < 0) return NULL;
        if (MS_UNLIKELY(type->types & (MS_TYPE_BYTES | MS_TYPE_BYTEARRAY))) {
            return json_decode_binary(self, view, size, type, path);
        }
        else if (MS_UNLIKELY(type->types & MS_TYPE_DATETIME)) {
            return json_decode_datetime(self, view, size, path);
        }
        PyObject *val = PyUnicode_DecodeUTF8(view, size, NULL);
        if (val == NULL) return NULL;
        if (MS_UNLIKELY(type->types & MS_TYPE_ENUM)) {
            return ms_decode_enum(val, type, path);
        }
        return val;
    }
    return ms_validation_error("str", type, path);
}

static PyObject *
json_decode_dict(
    JSONDecoderState *self, TypeNode *key_type, TypeNode *val_type, PathNode *path
) {
    PyObject *out, *key = NULL, *val = NULL;
    unsigned char c;
    bool first = true;
    PathNode key_path = {path, PATH_KEY, NULL};
    PathNode val_path = {path, PATH_ELLIPSIS, NULL};

    self->input_pos++; /* Skip '{' */

    out = PyDict_New();
    if (out == NULL) return NULL;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL;
    }
    while (true) {
        /* Parse '}' or ',', then peek the next character */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        if (c == '}') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or '}'");
            goto error;
        }

        /* Parse a string key */
        if (c == '"') {
            key = json_decode_string(self, key_type, &key_path);
            if (key == NULL) goto error;
        }
        else if (c == '}') {
            json_err_invalid(self, "trailing comma in object");
            goto error;
        }
        else {
            json_err_invalid(self, "object keys must be strings");
            goto error;
        }

        /* Parse colon */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        if (c != ':') {
            json_err_invalid(self, "expected ':'");
            goto error;
        }
        self->input_pos++;

        /* Parse value */
        val = json_decode(self, val_type, &val_path);
        if (val == NULL) goto error;

        /* Add item to dict */
        if (MS_UNLIKELY(PyDict_SetItem(out, key, val) < 0))
            goto error;
        Py_CLEAR(key);
        Py_CLEAR(val);
    }
    Py_LeaveRecursiveCall();
    return out;

error:
    Py_LeaveRecursiveCall();
    Py_XDECREF(key);
    Py_XDECREF(val);
    Py_DECREF(out);
    return NULL;
}

static PyObject *
json_decode_struct_map(
    JSONDecoderState *self, StructMetaObject *st_type, TypeNode *type,
    PathNode *path
) {
    PyObject *out, *val = NULL;
    Py_ssize_t key_size, field_index, pos = 0;
    unsigned char c;
    char *key = NULL;
    bool first = true;
    PathNode field_path = {path, 0, st_type};

    self->input_pos++; /* Skip '{' */

    out = Struct_alloc((PyTypeObject *)(st_type));
    if (out == NULL) return NULL;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL;
    }
    while (true) {
        /* Parse '}' or ',', then peek the next character */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        if (c == '}') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or '}'");
            goto error;
        }

        /* Parse a string key */
        if (c == '"') {
            key_size = json_decode_string_view(self, &key);
            if (key_size < 0) goto error;
        }
        else if (c == '}') {
            json_err_invalid(self, "trailing comma in object");
            goto error;
        }
        else {
            json_err_invalid(self, "object keys must be strings");
            goto error;
        }

        /* Parse colon */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        if (c != ':') {
            json_err_invalid(self, "expected ':'");
            goto error;
        }
        self->input_pos++;

        /* Parse value */
        field_index = StructMeta_get_field_index(st_type, key, key_size, &pos);
        if (field_index < 0) {
            /* Skip unknown fields */
            if (json_skip(self) < 0) goto error;
        }
        else {
            field_path.index = field_index;
            val = json_decode(
                self, st_type->struct_types[field_index], &field_path
            );
            if (val == NULL) goto error;
            Struct_set_index(out, field_index, val);
        }
    }
    if (Struct_fill_in_defaults(st_type, out, path) < 0) goto error;
    Py_LeaveRecursiveCall();
    return out;

error:
    Py_LeaveRecursiveCall();
    Py_DECREF(out);
    return NULL;
}

static PyObject *
json_decode_object(
    JSONDecoderState *self, TypeNode *type, PathNode *path
) {
    if (type->types & MS_TYPE_ANY) {
        TypeNode type_any = {MS_TYPE_ANY};
        return json_decode_dict(self, &type_any, type, path);
    }
    else if (type->types & MS_TYPE_DICT) {
        TypeNode *key, *val;
        TypeNode_get_dict(type, &key, &val);
        return json_decode_dict(self, key, val, path);
    }
    else if (type->types & MS_TYPE_STRUCT) {
        StructMetaObject *struct_type = TypeNode_get_struct(type);
        if (struct_type->asarray != OPT_TRUE) {
            return json_decode_struct_map(self, struct_type, type, path);
        }
    }
    return ms_validation_error("object", type, path);
}

static MS_NOINLINE PyObject *
json_decode_extended_float(JSONDecoderState *self) {
    uint32_t nd = 0;
    int32_t dp = 0;

    ms_hpd dec;
    dec.num_digits = 0;
    dec.decimal_point = 0;
    dec.negative = false;
    dec.truncated = false;

    /* We know there is at least one byte available when this function is
     * called */
    char c = *self->input_pos;

    /* Parse minus sign (if present) */
    if (c == '-') {
        self->input_pos++;
        c = json_peek_or_null(self);
        dec.negative = true;
    }

    /* Parse integer */
    if (MS_UNLIKELY(c == '0')) {
        /* Ensure at most one leading zero */
        self->input_pos++;
        c = json_peek_or_null(self);
        /* This _can't_ happen, since it would have been caught in the fast routine first */
        /*if (MS_UNLIKELY(is_digit(c))) return json_err_invalid(self, "invalid number");*/
    }
    else {
        /* Parse the integer part of the number. */
        while (self->input_pos < self->input_end && is_digit(*self->input_pos)) {
            c = *self->input_pos++;
            if (MS_LIKELY(nd < MS_HPD_MAX_DIGITS)) {
                dec.digits[nd++] = (uint8_t)(c - '0');
            }
            else if (c != '0') {
                dec.truncated = true;
            }
            dp++;
        }
        /* This _can't_ happen, since it would have been caught in the fast routine first */
        /*if (MS_UNLIKELY(nd == 0)) return json_err_invalid(self, "invalid character");*/
    }

    c = json_peek_or_null(self);
    if (c == '.') {
        self->input_pos++;
        /* Parse remaining digits until invalid/unknown character */
        unsigned char *cur_pos = self->input_pos;
        while (self->input_pos < self->input_end && is_digit(*self->input_pos)) {
            c = *self->input_pos++;
            if (c == '0') {
                if (nd == 0) {
                    /* Track leading zeros implicitly */
                    dp--;
                }
                else if (nd < MS_HPD_MAX_DIGITS) {
                    dec.digits[nd++] = (uint8_t)(c - '0');
                }
            }
            else if ('1' <= c && c <= '9') {
                if (nd < MS_HPD_MAX_DIGITS) {
                    dec.digits[nd++] = (uint8_t)(c - '0');
                }
                else {
                    dec.truncated = true;
                }
            }
        }
        /* Error if no digits after decimal */
        if (MS_UNLIKELY(cur_pos == self->input_pos)) return json_err_invalid(self, "invalid number");

        c = json_peek_or_null(self);
    }
    if (c == 'e' || c == 'E') {
        self->input_pos++;

        int64_t exp_sign = 1, exp_part = 0;

        c = json_peek_or_null(self);
        /* Parse exponent sign (if any) */
        if (c == '+') {
            self->input_pos++;
        }
        else if (c == '-') {
            self->input_pos++;
            exp_sign = -1;
        }

        /* Parse exponent digits */
        unsigned char *cur_pos = self->input_pos;
        while (self->input_pos < self->input_end && is_digit(*self->input_pos)) {
            c = *self->input_pos++;
            if (MS_LIKELY(exp_part < 922337203685477580)) {  /* won't overflow int64_t */
                exp_part = (int64_t)(exp_part * 10) + (c - '0');
            }
        }

        /* Error if no digits in exponent */
        if (MS_UNLIKELY(cur_pos == self->input_pos)) return json_err_invalid(self, "invalid number");

        dp += exp_sign * exp_part;
    }

    dec.num_digits = nd;
    if (dp < -MS_HPD_DP_RANGE) {
        dec.decimal_point = -(MS_HPD_DP_RANGE + 1);
    }
    else if (dp > MS_HPD_DP_RANGE) {
        dec.decimal_point = (MS_HPD_DP_RANGE + 1);
    }
    else {
        dec.decimal_point = dp;
    }
    ms_hpd_trim(&dec);
    double res = ms_hpd_to_double(&dec);
    if (Py_IS_INFINITY(res)) return json_err_invalid(self, "number out of range");
    return PyFloat_FromDouble(res);
}

static PyObject *
json_maybe_decode_number(JSONDecoderState *self, TypeNode *type, PathNode *path) {
    uint64_t mantissa = 0;
    int32_t exponent = 0;
    bool is_negative = false;
    bool is_float = false;

    unsigned char *initial_pos = self->input_pos;

    /* We know there is at least one byte available when this function is
     * called */
    char c = *self->input_pos;

    /* Parse minus sign (if present) */
    if (c == '-') {
        self->input_pos++;
        c = json_peek_or_null(self);
        is_negative = true;
    }

    unsigned char *first_digit_pos = self->input_pos;

    /* Parse integer */
    if (MS_UNLIKELY(c == '0')) {
        /* Ensure at most one leading zero */
        self->input_pos++;
        c = json_peek_or_null(self);
        if (MS_UNLIKELY(is_digit(c))) return json_err_invalid(self, "invalid number");
    }
    else {
        /* Parse the integer part of the number.
         *
         * We can read the first 19 digits safely into a uint64 without
         * checking for overflow. Removing overflow checks from the loop gives
         * a measurable performance boost. */
        size_t n_safe = Py_MIN(19, self->input_end - self->input_pos);
        size_t n_safe_orig = n_safe;
        while (n_safe) {
            c = *self->input_pos;
            if (!is_digit(c)) goto end_integer;
            self->input_pos++;
            n_safe--;
            mantissa = mantissa * 10 + (uint64_t)(c - '0');
        }
        if (n_safe_orig == 19) {
            /* Reading a 20th digit may or may not cause overflow. Any
             * additional digits definitely will. Read the 20th digit (and
             * check for a 21st), taking the slow path upon overflow. */
            c = json_peek_or_null(self);
            if (MS_UNLIKELY(is_digit(c))) {
                self->input_pos++;
                uint64_t mantissa2 = mantissa * 10 + (uint64_t)(c - '0');
                bool overflowed = (mantissa2 < mantissa) || ((mantissa2 - (uint64_t)(c - '0')) / 10) != mantissa;
                if (MS_UNLIKELY(overflowed || is_digit(json_peek_or_null(self)))) {
                    goto fallback_extended;
                }
                mantissa = mantissa2;
            }
        }

end_integer:
        /* There must be at least one digit */
        if (MS_UNLIKELY(n_safe == n_safe_orig)) return json_err_invalid(self, "invalid character");
    }

    c = json_peek_or_null(self);
    if (c == '.') {
        self->input_pos++;
        is_float = true;
        /* Parse remaining digits until invalid/unknown character */
        unsigned char *first_dec_digit = self->input_pos;
        while (self->input_pos < self->input_end && is_digit(*self->input_pos)) {
            c = *self->input_pos++;
            mantissa = mantissa * 10 + (uint64_t)(c - '0');
        }
        /* Error if no digits after decimal */
        if (MS_UNLIKELY(first_dec_digit == self->input_pos)) return json_err_invalid(self, "invalid number");
        exponent = first_dec_digit - self->input_pos;

        c = json_peek_or_null(self);

        /* This is may be an overestimate of significant digits, only fix if it matters */
        uint32_t ndigits = self->input_pos - first_digit_pos;
        if (MS_UNLIKELY(ndigits > 19)) {
            /* Fix ndigits estimate by trimming leading zeros/decimal */
            const uint8_t* p = first_digit_pos;
            while (*p == '0' || *p == '.') p++;
            ndigits -= (p - first_digit_pos);
            if (ndigits > 19) {
                goto fallback_extended;
            }
        }
    }

    if (c == 'e' || c == 'E') {
        int32_t exp_sign = 1, exp_part = 0;
        self->input_pos++;
        is_float = true;

        c = json_peek_or_null(self);
        /* Parse exponent sign (if any) */
        if (c == '+') {
            self->input_pos++;
        }
        else if (c == '-') {
            self->input_pos++;
            exp_sign = -1;
        }

        /* Parse exponent digits */
        unsigned char *cur_pos = self->input_pos;
        while (self->input_pos < self->input_end && is_digit(*self->input_pos)) {
            c = *self->input_pos++;
            if (MS_LIKELY(exp_part < 10000)) {
                exp_part = (int32_t)(exp_part * 10) + (uint32_t)(c - '0');
            }
        }

        /* Error if no digits in exponent */
        if (MS_UNLIKELY(cur_pos == self->input_pos)) return json_err_invalid(self, "invalid number");

        exponent += exp_sign * exp_part;
    }

    if (MS_UNLIKELY(is_negative && mantissa > 1ull << 63)) {
        is_float = true;
    }

    if (!is_float && (type->types & (MS_TYPE_ANY | MS_TYPE_INT | MS_TYPE_INTENUM))) {
        PyObject *val = (
            is_negative ?
            PyLong_FromLongLong(-1 * (int64_t)mantissa) :
            PyLong_FromUnsignedLongLong(mantissa)
        );
        if (type->types & MS_TYPE_INTENUM) {
            return ms_decode_intenum(val, type, path);
        }
        return val;
    }
    else if (type->types & (MS_TYPE_ANY | MS_TYPE_FLOAT)) {
        double val;
        if (!reconstruct_double(mantissa, exponent, is_negative, &val)) {
            goto fallback_extended;
        }
        return PyFloat_FromDouble(val);
    }
    if (!is_float) {
        return ms_validation_error("int", type, path);
    }
    return ms_validation_error("float", type, path);

fallback_extended:
    self->input_pos = initial_pos;
    if (MS_UNLIKELY(!(type->types & (MS_TYPE_ANY | MS_TYPE_FLOAT)))) {
        return ms_validation_error("float", type, path);
    }
    return json_decode_extended_float(self);
}

static PyObject *
json_decode_nocustom(
    JSONDecoderState *self, TypeNode *type, PathNode *path
) {
    unsigned char c;

    if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) return NULL;

    switch (c) {
        case 'n': return json_decode_none(self, type, path);
        case 't': return json_decode_true(self, type, path);
        case 'f': return json_decode_false(self, type, path);
        case '[': return json_decode_array(self, type, path);
        case '{': return json_decode_object(self, type, path);
        case '"': return json_decode_string(self, type, path);
        default: return json_maybe_decode_number(self, type, path);
    }
}

static PyObject *
json_decode(
    JSONDecoderState *self, TypeNode *type, PathNode *path
) {
    PyObject *obj = json_decode_nocustom(self, type, path);
    if (MS_UNLIKELY(type->types & (MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC))) {
        return ms_decode_custom(
            obj, self->dec_hook, type->types & MS_TYPE_CUSTOM_GENERIC, type, path
        );
    }
    return obj;
}

static int
json_skip_ident(JSONDecoderState *self, const char *ident, size_t len) {
    self->input_pos++;  /* Already checked first char */
    if (MS_UNLIKELY(!json_remaining(self, len))) return ms_err_truncated();
    if (memcmp(self->input_pos, ident, len) != 0) {
        json_err_invalid(self, "invalid character");
        return -1;
    }
    self->input_pos += len;
    return 0;
}

static int
json_skip_string_escape(JSONDecoderState *self) {
    unsigned char c;
    if (!json_read1(self, &c)) return -1;

    switch (c) {
        case '"':
        case '\\':
        case '/':
        case 'b':
        case 'f':
        case 'n':
        case 'r':
        case 't':
            return 0;
        case 'u': {
            unsigned int cp;
            if (json_read_codepoint(self, &cp) < 0) return -1;

            if (0xDC00 <= cp && cp <= 0xDFFF) {
                json_err_invalid(self, "invalid utf-16 surrogate pair");
                return -1;
            }
            else if (0xD800 <= cp && cp <= 0xDBFF) {
                /* utf-16 pair, parse 2nd pair */
                unsigned int cp2;
                if (!json_remaining(self, 6)) return ms_err_truncated();
                if (self->input_pos[0] != '\\' || self->input_pos[1] != 'u') {
                    json_err_invalid(self, "unexpected end of hex escape");
                    return -1;
                }
                self->input_pos += 2;
                if (json_read_codepoint(self, &cp2) < 0) return -1;
                if (cp2 < 0xDC00 || cp2 > 0xDFFF) {
                    json_err_invalid(self, "invalid utf-16 surrogate pair");
                    return -1;
                }
                cp = 0x10000 + (((cp - 0xD800) << 10) | (cp2 - 0xDC00));
            }
            return 0;
        }
        default: {
            json_err_invalid(self, "invalid escaped character");
            return -1;
        }
    }
}

static int
json_skip_string(JSONDecoderState *self) {
    unsigned char c;
    self->input_pos++; /* Skip '"' */
    while (true) {
        if (!json_read1(self, &c)) return -1;
        if (MS_UNLIKELY(escape_table[c])) {
            if (c == '"') {
                return 0;
            }
            else if (c == '\\') {
                if (json_skip_string_escape(self) < 0) return -1;
            }
            else {
                json_err_invalid(self, "invalid character");
                return -1;
            }
        }
    }
}

static int
json_skip_array(JSONDecoderState *self) {
    unsigned char c;
    bool first = true;
    int out = -1;

    self->input_pos++; /* Skip '[' */

    if (Py_EnterRecursiveCall(" while deserializing an object")) return -1;
    while (true) {
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) break;
        if (c == ']') {
            self->input_pos++;
            out = 0;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) break;
        }
        else if (first) {
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or ']'");
            break;
        }
        if (MS_UNLIKELY(c == ']')) {
            json_err_invalid(self, "trailing comma in array");
            break;
        }

        if (json_skip(self) < 0) break;
    }
    Py_LeaveRecursiveCall();
    return out;
}

static int
json_skip_object(JSONDecoderState *self) {
    unsigned char c;
    bool first = true;
    int out = -1;

    self->input_pos++; /* Skip '{' */

    if (Py_EnterRecursiveCall(" while deserializing an object")) return -1;
    while (true) {
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) break;
        if (c == '}') {
            self->input_pos++;
            out = 0;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) break;
        }
        else if (first) {
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or '}'");
            break;
        }

        /* Skip key */
        if (c == '"') {
            if (json_skip_string(self) < 0) break;
        }
        else if (c == '}') {
            json_err_invalid(self, "trailing comma in object");
            break;
        }
        else {
            json_err_invalid(self, "expected '\"'");
            break;
        }

        /* Parse colon */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) break;
        if (c != ':') {
            json_err_invalid(self, "expected ':'");
            break;
        }
        self->input_pos++;

        /* Skip value */
        if (json_skip(self) < 0) break;
    }
    Py_LeaveRecursiveCall();
    return out;
}

static int
json_maybe_skip_number(JSONDecoderState *self) {
    /* We know there is at least one byte available when this function is
     * called */
    char c = *self->input_pos;

    /* Parse minus sign (if present) */
    if (c == '-') {
        self->input_pos++;
        c = json_peek_or_null(self);
    }

    /* Parse integer */
    if (MS_UNLIKELY(c == '0')) {
        /* Ensure at most one leading zero */
        self->input_pos++;
        c = json_peek_or_null(self);
        if (MS_UNLIKELY(is_digit(c))) {
            json_err_invalid(self, "invalid number");
            return -1;
        }
    }
    else {
        /* Skip the integer part of the number. */
        unsigned char *cur_pos = self->input_pos;
        while (self->input_pos < self->input_end && is_digit(*self->input_pos)) {
            self->input_pos++;
        }
        /* There must be at least one digit */
        if (MS_UNLIKELY(cur_pos == self->input_pos)) {
            json_err_invalid(self, "invalid character");
            return -1;
        }
    }

    c = json_peek_or_null(self);
    if (c == '.') {
        self->input_pos++;
        /* Skip remaining digits until invalid/unknown character */
        unsigned char *cur_pos = self->input_pos;
        while (self->input_pos < self->input_end && is_digit(*self->input_pos)) {
            self->input_pos++;
        }
        /* Error if no digits after decimal */
        if (MS_UNLIKELY(cur_pos == self->input_pos)) {
            json_err_invalid(self, "invalid number");
            return -1;
        }

        c = json_peek_or_null(self);
    }
    if (c == 'e' || c == 'E') {
        self->input_pos++;

        /* Parse exponent sign (if any) */
        c = json_peek_or_null(self);
        if (c == '+' || c == '-') {
            self->input_pos++;
        }

        /* Parse exponent digits */
        unsigned char *cur_pos = self->input_pos;
        while (self->input_pos < self->input_end && is_digit(*self->input_pos)) {
            self->input_pos++;
        }
        /* Error if no digits in exponent */
        if (MS_UNLIKELY(cur_pos == self->input_pos)) {
            json_err_invalid(self, "invalid number");
            return -1;
        }
    }
    return 0;
}

static int
json_skip(JSONDecoderState *self)
{
    unsigned char c;

    if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) return -1;

    switch (c) {
        case 'n': return json_skip_ident(self, "ull", 3);
        case 't': return json_skip_ident(self, "rue", 3);
        case 'f': return json_skip_ident(self, "alse", 4);
        case '"': return json_skip_string(self);
        case '[': return json_skip_array(self);
        case '{': return json_skip_object(self);
        default: return json_maybe_skip_number(self);
    }
}


PyDoc_STRVAR(JSONDecoder_decode__doc__,
"decode(self, buf)\n"
"--\n"
"\n"
"Deserialize an object from bytes.\n"
"\n"
"Parameters\n"
"----------\n"
"buf : bytes-like\n"
"    The message to decode.\n"
"\n"
"Returns\n"
"-------\n"
"obj : Any\n"
"    The deserialized object\n"
);
static PyObject*
JSONDecoder_decode(JSONDecoder *self, PyObject *const *args, Py_ssize_t nargs)
{
    PyObject *res = NULL;
    Py_buffer buffer;
    buffer.buf = NULL;

    if (!check_positional_nargs(nargs, 1, 1)) {
        return NULL;
    }

    if (PyObject_GetBuffer(args[0], &buffer, PyBUF_CONTIG_RO) >= 0) {
        self->state.buffer_obj = args[0];
        self->state.input_start = buffer.buf;
        self->state.input_pos = buffer.buf;
        self->state.input_end = self->state.input_pos + buffer.len;

        res = json_decode(&(self->state), self->state.type, NULL);

        if (res != NULL && json_has_trailing_characters(&self->state)) {
            res = NULL;
        }

        PyBuffer_Release(&buffer);
        self->state.buffer_obj = NULL;
        self->state.input_start = NULL;
        self->state.input_pos = NULL;
        self->state.input_end = NULL;
        json_scratch_reset(&(self->state));
    }

    return res;
}

static struct PyMethodDef JSONDecoder_methods[] = {
    {
        "decode", (PyCFunction) JSONDecoder_decode, METH_FASTCALL,
        JSONDecoder_decode__doc__,
    },
    {NULL, NULL}                /* sentinel */
};

static PyMemberDef JSONDecoder_members[] = {
    {"type", T_OBJECT_EX, offsetof(JSONDecoder, orig_type), READONLY, "The Decoder type"},
    {"dec_hook", T_OBJECT, offsetof(JSONDecoder, state.dec_hook), READONLY, "The Decoder dec_hook"},
    {NULL},
};

static PyTypeObject JSONDecoder_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec.json.Decoder",
    .tp_doc = JSONDecoder__doc__,
    .tp_basicsize = sizeof(JSONDecoder),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc)JSONDecoder_init,
    .tp_traverse = (traverseproc)JSONDecoder_traverse,
    .tp_dealloc = (destructor)JSONDecoder_dealloc,
    .tp_repr = (reprfunc)JSONDecoder_repr,
    .tp_methods = JSONDecoder_methods,
    .tp_members = JSONDecoder_members,
};

PyDoc_STRVAR(msgspec_json_decode__doc__,
"json_decode(buf, *, type='Any', dec_hook=None)\n"
"--\n"
"\n"
"Deserialize an object from bytes.\n"
"\n"
"Parameters\n"
"----------\n"
"buf : bytes-like\n"
"    The message to decode.\n"
"type : Type, optional\n"
"    A Python type (in type annotation form) to decode the object as. If\n"
"    provided, the message will be type checked and decoded as the specified\n"
"    type. Defaults to `Any`, in which case the message will be decoded using\n"
"    the default JSON types.\n"
"dec_hook : Callable, optional\n"
"    An optional callback for handling decoding custom types. Should have the\n"
"    signature ``dec_hook(type: Type, obj: Any) -> Any``, where ``type`` is the\n"
"    expected message type, and ``obj`` is the decoded representation composed\n"
"    of only basic JSON types. This hook should transform ``obj`` into type\n"
"    ``type``, or raise a ``TypeError`` if unsupported.\n"
"\n"
"Returns\n"
"-------\n"
"obj : Any\n"
"    The deserialized object\n"
"\n"
"See Also\n"
"--------\n"
"Decoder.decode"
);
static PyObject*
msgspec_json_decode(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PyObject *res = NULL, *buf = NULL, *type = NULL, *dec_hook = NULL;
    JSONDecoderState state;
    Py_buffer buffer;
    MsgspecState *st = msgspec_get_global_state();

    /* Parse arguments */
    if (!check_positional_nargs(nargs, 1, 1)) return NULL;
    buf = args[0];
    if (kwnames != NULL) {
        Py_ssize_t nkwargs = PyTuple_GET_SIZE(kwnames);
        if ((type = find_keyword(kwnames, args + nargs, st->str_type)) != NULL) nkwargs--;
        if ((dec_hook = find_keyword(kwnames, args + nargs, st->str_dec_hook)) != NULL) nkwargs--;
        if (nkwargs > 0) {
            PyErr_SetString(
                PyExc_TypeError,
                "Extra keyword arguments provided"
            );
            return NULL;
        }
    }

    /* Handle dec_hook */
    if (dec_hook == Py_None) {
        dec_hook = NULL;
    }
    if (dec_hook != NULL) {
        if (!PyCallable_Check(dec_hook)) {
            PyErr_SetString(PyExc_TypeError, "dec_hook must be callable");
            return NULL;
        }
    }
    state.dec_hook = dec_hook;

    /* Init scratch space */
    state.scratch = NULL;
    state.scratch_capacity = 0;
    state.scratch_len = 0;

    /* Only build TypeNode if required */
    state.type = NULL;
    if (type != NULL && type != st->typing_any) {
        state.type = TypeNode_Convert(type, true, NULL);
        if (state.type == NULL) return NULL;
    }

    buffer.buf = NULL;
    if (PyObject_GetBuffer(buf, &buffer, PyBUF_CONTIG_RO) >= 0) {
        state.buffer_obj = buf;
        state.input_start = buffer.buf;
        state.input_pos = buffer.buf;
        state.input_end = state.input_pos + buffer.len;

        if (state.type != NULL) {
            res = json_decode(&state, state.type, NULL);
        } else {
            TypeNode type_any = {MS_TYPE_ANY};
            res = json_decode(&state, &type_any, NULL);
        }

        if (res != NULL && json_has_trailing_characters(&state)) {
            res = NULL;
        }

        PyBuffer_Release(&buffer);
    }

    PyMem_Free(state.scratch);

    if (state.type != NULL) {
        TypeNode_Free(state.type);
    }

    return res;
}


/*************************************************************************
 * Module Setup                                                          *
 *************************************************************************/

static struct PyMethodDef msgspec_methods[] = {
    {
        "msgpack_encode", (PyCFunction) msgspec_msgpack_encode, METH_FASTCALL | METH_KEYWORDS,
        msgspec_msgpack_encode__doc__,
    },
    {
        "msgpack_decode", (PyCFunction) msgspec_msgpack_decode, METH_FASTCALL | METH_KEYWORDS,
        msgspec_msgpack_decode__doc__,
    },
    {
        "json_encode", (PyCFunction) msgspec_json_encode, METH_FASTCALL | METH_KEYWORDS,
        msgspec_json_encode__doc__,
    },
    {
        "json_decode", (PyCFunction) msgspec_json_decode, METH_FASTCALL | METH_KEYWORDS,
        msgspec_json_decode__doc__,
    },
    {NULL, NULL} /* sentinel */
};

static int
msgspec_clear(PyObject *m)
{
    MsgspecState *st = msgspec_get_state(m);
    Py_CLEAR(st->MsgspecError);
    Py_CLEAR(st->EncodeError);
    Py_CLEAR(st->DecodeError);
    Py_CLEAR(st->StructType);
    Py_CLEAR(st->EnumType);
    Py_CLEAR(st->str__name_);
    Py_CLEAR(st->str__value2member_map_);
    Py_CLEAR(st->str_name);
    Py_CLEAR(st->str_type);
    Py_CLEAR(st->str_enc_hook);
    Py_CLEAR(st->str_dec_hook);
    Py_CLEAR(st->str_ext_hook);
    Py_CLEAR(st->str_utcoffset);
    Py_CLEAR(st->str___origin__);
    Py_CLEAR(st->str___args__);
    Py_CLEAR(st->typing_dict);
    Py_CLEAR(st->typing_list);
    Py_CLEAR(st->typing_set);
    Py_CLEAR(st->typing_tuple);
    Py_CLEAR(st->typing_union);
    Py_CLEAR(st->typing_any);
    Py_CLEAR(st->get_type_hints);
    Py_CLEAR(st->astimezone);
    return 0;
}

static void
msgspec_free(PyObject *m)
{
    msgspec_clear(m);
}

static int
msgspec_traverse(PyObject *m, visitproc visit, void *arg)
{

#if STRUCT_FREELIST_MAX_SIZE > 0
    /* Since module objects tend to persist throughout a program's execution,
     * this should only be called during major GC collections (i.e. rarely).
     *
     * We want to clear the freelist periodically to free up old pages and
     * reduce fragementation. But we don't want to do it too often, or the
     * freelist will rarely be used. Hence clearing the freelist here. This may
     * change in future releases.
     */
    Struct_freelist_clear();
#endif

    MsgspecState *st = msgspec_get_state(m);
    Py_VISIT(st->MsgspecError);
    Py_VISIT(st->EncodeError);
    Py_VISIT(st->DecodeError);
    Py_VISIT(st->StructType);
    Py_VISIT(st->EnumType);
    Py_VISIT(st->typing_dict);
    Py_VISIT(st->typing_list);
    Py_VISIT(st->typing_set);
    Py_VISIT(st->typing_tuple);
    Py_VISIT(st->typing_union);
    Py_VISIT(st->typing_any);
    Py_VISIT(st->get_type_hints);
    Py_VISIT(st->astimezone);
    return 0;
}

static struct PyModuleDef msgspecmodule = {
    PyModuleDef_HEAD_INIT,
    .m_name = "msgspec._core",
    .m_size = sizeof(MsgspecState),
    .m_methods = msgspec_methods,
    .m_traverse = msgspec_traverse,
    .m_clear = msgspec_clear,
    .m_free =(freefunc)msgspec_free
};

PyMODINIT_FUNC
PyInit__core(void)
{
    PyObject *m, *temp_module, *temp_obj;
    MsgspecState *st;

    PyDateTime_IMPORT;

    m = PyState_FindModule(&msgspecmodule);
    if (m) {
        Py_INCREF(m);
        return m;
    }

    StructMetaType.tp_base = &PyType_Type;
    if (PyType_Ready(&StructMetaType) < 0)
        return NULL;
    if (PyType_Ready(&StructMixinType) < 0)
        return NULL;
    if (PyType_Ready(&Encoder_Type) < 0)
        return NULL;
    if (PyType_Ready(&Decoder_Type) < 0)
        return NULL;
    if (PyType_Ready(&Ext_Type) < 0)
        return NULL;
    if (PyType_Ready(&JSONEncoder_Type) < 0)
        return NULL;
    if (PyType_Ready(&JSONDecoder_Type) < 0)
        return NULL;

    /* Create the module */
    m = PyModule_Create(&msgspecmodule);
    if (m == NULL)
        return NULL;

    /* Add types */
    Py_INCREF(&Encoder_Type);
    if (PyModule_AddObject(m, "MsgpackEncoder", (PyObject *)&Encoder_Type) < 0)
        return NULL;
    Py_INCREF(&Decoder_Type);
    if (PyModule_AddObject(m, "MsgpackDecoder", (PyObject *)&Decoder_Type) < 0)
        return NULL;
    Py_INCREF(&Ext_Type);
    if (PyModule_AddObject(m, "Ext", (PyObject *)&Ext_Type) < 0)
        return NULL;
    Py_INCREF(&JSONEncoder_Type);
    if (PyModule_AddObject(m, "JSONEncoder", (PyObject *)&JSONEncoder_Type) < 0)
        return NULL;
    if (PyModule_AddObject(m, "JSONDecoder", (PyObject *)&JSONDecoder_Type) < 0)
        return NULL;

    st = msgspec_get_state(m);

    /* Initialize the Struct Type */
    st->StructType = PyObject_CallFunction(
        (PyObject *)&StructMetaType, "s(O){ssss}", "Struct", &StructMixinType,
        "__module__", "msgspec", "__doc__", Struct__doc__
    );
    if (st->StructType == NULL)
        return NULL;
    Py_INCREF(st->StructType);
    if (PyModule_AddObject(m, "Struct", st->StructType) < 0)
        return NULL;

    /* Initialize the exceptions. */
    st->MsgspecError = PyErr_NewExceptionWithDoc(
        "msgspec.MsgspecError",
        "Base class for all Msgspec exceptions",
        NULL, NULL
    );
    if (st->MsgspecError == NULL)
        return NULL;
    st->EncodeError = PyErr_NewExceptionWithDoc(
        "msgspec.EncodeError",
        "An error occurred while encoding an object",
        st->MsgspecError, NULL
    );
    if (st->EncodeError == NULL)
        return NULL;
    st->DecodeError = PyErr_NewExceptionWithDoc(
        "msgspec.DecodeError",
        "An error occurred while decoding an object",
        st->MsgspecError, NULL
    );
    if (st->DecodeError == NULL)
        return NULL;

    Py_INCREF(st->MsgspecError);
    if (PyModule_AddObject(m, "MsgspecError", st->MsgspecError) < 0)
        return NULL;
    Py_INCREF(st->EncodeError);
    if (PyModule_AddObject(m, "EncodeError", st->EncodeError) < 0)
        return NULL;
    Py_INCREF(st->DecodeError);
    if (PyModule_AddObject(m, "DecodeError", st->DecodeError) < 0)
        return NULL;

#define SET_REF(attr, name) \
    do { \
    st->attr = PyObject_GetAttrString(temp_module, name); \
    if (st->attr == NULL) return NULL; \
    } while (0)

    /* Get all imports from the typing module */
    temp_module = PyImport_ImportModule("typing");
    if (temp_module == NULL) return NULL;
    SET_REF(typing_list, "List");
    SET_REF(typing_set, "Set");
    SET_REF(typing_tuple, "Tuple");
    SET_REF(typing_dict, "Dict");
    SET_REF(typing_union, "Union");
    SET_REF(typing_any, "Any");
    SET_REF(get_type_hints, "get_type_hints");
    Py_DECREF(temp_module);

    /* Get the EnumType */
    temp_module = PyImport_ImportModule("enum");
    if (temp_module == NULL)
        return NULL;
    temp_obj = PyObject_GetAttrString(temp_module, "Enum");
    Py_DECREF(temp_module);
    if (temp_obj == NULL)
        return NULL;
    if (!PyType_Check(temp_obj)) {
        Py_DECREF(temp_obj);
        PyErr_SetString(PyExc_TypeError, "enum.Enum should be a type");
        return NULL;
    }
    st->EnumType = (PyTypeObject *)temp_obj;

    /* Get the datetime.datetime.astimezone method */
    temp_module = PyImport_ImportModule("datetime");
    if (temp_module == NULL)
        return NULL;
    temp_obj = PyObject_GetAttrString(temp_module, "datetime");
    Py_DECREF(temp_module);
    if (temp_obj == NULL)
        return NULL;
    st->astimezone = PyObject_GetAttrString(temp_obj, "astimezone");
    Py_DECREF(temp_obj);
    if (st->astimezone == NULL)
        return NULL;

    /* Initialize cached constant strings */
#define CACHED_STRING(attr, str) \
    if ((st->attr = PyUnicode_InternFromString(str)) == NULL) return NULL
    CACHED_STRING(str__name_, "_name_");
    CACHED_STRING(str__value2member_map_, "_value2member_map_");
    CACHED_STRING(str_name, "name");
    CACHED_STRING(str_type, "type");
    CACHED_STRING(str_enc_hook, "enc_hook");
    CACHED_STRING(str_dec_hook, "dec_hook");
    CACHED_STRING(str_ext_hook, "ext_hook");
    CACHED_STRING(str_utcoffset, "utcoffset");
    CACHED_STRING(str___origin__, "__origin__");
    CACHED_STRING(str___args__, "__args__");

    return m;
}
