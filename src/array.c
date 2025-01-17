#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "value.h"
#include "dict.h"
#include "log.h"
#include "functions.h"
#include "operators.h"
#include "util.h"
#include "vm.h"

static struct value
array_drop_mut(struct value *array, int argc, struct value *kwargs);

static struct value
array_drop(struct value *array, int argc, struct value *kwargs);

static struct value
array_min_by(struct value *array, int argc, struct value *kwargs);

static struct value
array_max_by(struct value *array, int argc, struct value *kwargs);

static struct value
array_reverse(struct value *array, int argc, struct value *kwargs);

static _Thread_local struct value *comparison_fn;

static int
compare_by(void const *v1, void const *v2)
{
        struct value k1 = value_apply_callable(comparison_fn, (struct value *)v1);
        gc_push(&k1);

        struct value k2 = value_apply_callable(comparison_fn, (struct value *)v2);
        gc_push(&k2);

        int result = value_compare(&k1, &k2);

        gc_pop();
        gc_pop();

        return result;
}

static int
compare_by2(void const *v1, void const *v2)
{
        struct value v = vm_eval_function(comparison_fn, v1, v2, NULL);
        gc_push(&v);

        int result;

        if (v.type == VALUE_INTEGER)
                result = v.integer;
        else
                result = value_truthy(&v) ? 1 : -1;

        gc_pop();

        return result;
}

inline static void
shrink(struct value *array)
{
        if (array->array->capacity > 8 * array->array->count || (array->array->capacity - array->array->count) > 1000) {
                array->array->capacity = array->array->count;
                if (array->array->count == 0)
                        gc_free(array->array->items), array->array->items = NULL;
                else
                        resize(array->array->items, array->array->count * sizeof (struct value));
        }
}

static struct value
array_push(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("the push method on arrays expects 1 argument but got %d", argc);

        value_array_push(array->array, ARG(0));

        return NIL;
}

static struct value
array_insert(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 2)
                vm_panic("the insert method on arrays expects 2 arguments but got %d", argc);

        struct value i = ARG(0);
        struct value v = ARG(1);

        if (i.type != VALUE_INTEGER)
                vm_panic("non-integer passed as the index to the insert method on array");

        int index = i.integer;

        if (index < 0)
                index += array->array->count + 1;
        if (index < 0 || index > array->array->count)
                vm_panic("array index passed to insert is out of range: %d", index);

        value_array_push(array->array, NIL);

        memmove(array->array->items + index + 1, array->array->items + index, (array->array->count - index - 1) * sizeof (struct value));
        array->array->items[index] = v;

        return *array;
}

static struct value
array_pop(struct value *array, int argc, struct value *kwargs)
{
        struct value result;

        if (argc == 0) {
                if (array->array->count == 0)
                        vm_panic("attempt to pop from an empty array");
                result = array->array->items[--array->array->count];
        } else if (argc == 1) {
                struct value arg = ARG(0);
                if (arg.type != VALUE_INTEGER)
                        vm_panic("the argument to pop must be an integer");
                if (arg.integer < 0)
                        arg.integer += array->array->count;
                if (arg.integer < 0 || arg.integer >= array->array->count)
                        vm_panic("array index passed to pop is out of range");
                result = array->array->items[arg.integer];
                vec_pop_ith(*array->array, arg.integer);
        } else {
                vm_panic("the pop method on arrays expects 0 or 1 argument(s) but got %d", argc);
        }

        shrink(array);

        return result;
}

static struct value
array_swap(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 2)
                vm_panic("array.swap() expects 2 arguments but got %d", argc);

        struct value i = ARG(0);
        struct value j = ARG(1);

        if (i.type != VALUE_INTEGER || j.type != VALUE_INTEGER)
                vm_panic("the arguments to array.swap() must be integers");

        if (i.integer < 0)
                i.integer += array->array->count;

        if (j.integer < 0)
                j.integer += array->array->count;

        if (i.integer < 0 || i.integer >= array->array->count
         || j.integer < 0 || j.integer >= array->array->count) {
                vm_panic("invalid indices passed to array.swap(): (%d, %d)", (int) i.integer, (int) j.integer);
           }

        struct value tmp = array->array->items[i.integer];
        array->array->items[i.integer] = array->array->items[j.integer];
        array->array->items[j.integer] = tmp;

        return *array;
}

static struct value
array_slice_mut(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1 && argc != 2)
                vm_panic("array.slice!() expects 1 or 2 arguments but got %d", argc);

        struct value start = ARG(0);

        if (start.type != VALUE_INTEGER)
                vm_panic("non-integer passed as first argument to array.slice!()");

        int s = start.integer;
        int n;

        if (argc == 2) {
                struct value count = ARG(1);
                if (count.type != VALUE_INTEGER)
                        vm_panic("non-integer passed as second argument to array.slice!()");
                n = count.integer;
        } else {
                n = array->array->count;
        }


        if (s < 0)
                s += array->array->count;
        if (s < 0)
                vm_panic("start index passed to array.slice!() is out of range");

        if (n < 0)
                n += array->array->count;
        if (n < 0)
                vm_panic("negative count passed to array.slice!()");

        s = min(s, array->array->count);
        n = min(n, array->array->count - s);

        struct array *slice = value_array_new();
        NOGC(slice);

        vec_push_n(*slice, array->array->items + s, n);
        memmove(array->array->items + s, array->array->items + s + n, sizeof (struct value[array->array->count - (s + n)]));

        array->array->count -= n;
        shrink(array);

        OKGC(slice);

        return ARRAY(slice);
}

static struct value
array_zip(struct value *array, int argc, struct value *kwargs)
{
        if (argc == 0 || (argc == 1 && ARG(0).type != VALUE_ARRAY)) {
                vm_panic("array.zip() expects at least one array argument");
        }

        int ac = argc;

        struct value f = NIL;
        if (CALLABLE(ARG(ac - 1))) {
                f = ARG(ac - 1);
                ac -= 1;
        }

        int n = array->array->count;
        for (int i = 0; i < ac; ++i) {
                if (ARG(i).type != VALUE_ARRAY) {
                        vm_panic("non-array passed to array.zip()");
                }
                n = min(n, ARG(i).array->count);
        }

        for (int i = 0; i < n; ++i) {
                if (f.type == VALUE_NIL) {
                        struct value t = value_tuple(ac + 1);
                        t.items[0] = array->array->items[i];
                        for (int j = 0; j < ac; ++j) {
                                t.items[j + 1] = ARG(j).array->items[i];
                        }
                        array->array->items[i] = t;
                } else {
                        vm_push(&array->array->items[i]);
                        for (int j = 0; j < ac; ++j) {
                                vm_push(&ARG(-1).array->items[i]);
                        }
                        array->array->items[i] = vm_call(&f, argc);
                }
        }

        array->array->count = n;
        shrink(array);

        return *array;
}

static struct value
array_window(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1 && argc != 2)
                vm_panic("array.window() expects 1 or 2 arguments but got %d", argc);

        struct value k = ARG(0);
        if (k.type != VALUE_INTEGER)
                vm_panic("the first argument to array.window() must be an integer");

        if (k.integer <= 0)
                vm_panic("the first argument to array.window() must be positive");

        int n = array->array->count - k.integer + 1;

        if (argc == 2) {
                struct value f = ARG(1);
                if (!CALLABLE(f))
                        vm_panic("the second argument to array.window() must be callable");

                for (int i = 0; i < n; ++i) {
                        for (int j = i; j < i + k.integer; ++j)
                                vm_push(&array->array->items[j]);
                        array->array->items[i] = vm_call(&f, k.integer);
                }

        } else {
                for (int i = 0; i < n; ++i) {
                        struct array *w = value_array_new();
                        NOGC(w);
                        for (int j = i; j < i + k.integer; ++j)
                                value_array_push(w, array->array->items[j]);
                        OKGC(w);
                        array->array->items[i] = ARRAY(w);
                }
        }

        array->array->count = n;
        shrink(array);

        return *array;
}

static struct value
array_slice(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1 && argc != 2)
                vm_panic("array.slice() expects 1 or 2 arguments but got %d", argc);

        struct value start = ARG(0);
        if (start.type != VALUE_INTEGER)
                vm_panic("non-integer passed as first argument to array.slice()");

        int s = start.integer;
        int n;

        if (argc == 2) {
                struct value count = ARG(1);
                if (count.type != VALUE_INTEGER)
                        vm_panic("non-integer passed as second argument to array.slice()");
                n = count.integer;
        } else {
                n = array->array->count;
        }

        if (s < 0)
                s += array->array->count;
        if (s < 0)
                vm_panic("start index passed to array.slice() is out of range");

        if (n < 0)
                n += array->array->count;
        if (n < 0)
                vm_panic("negative count passed to array.slice()");

        s = min(s, array->array->count);
        n = min(n, array->array->count - s);

        struct value result = ARRAY(value_array_new());
        NOGC(result.array);
        value_array_reserve(result.array, n);
        OKGC(result.array);
        memmove(result.array->items, array->array->items + s, n * sizeof (struct value));
        result.array->count = n;

        return result;
}

static struct value
array_sort(struct value *array, int argc, struct value *kwargs)
{
        int i;
        int n;

        switch (argc) {
        case 0:
                i = 0;
                n = array->array->count;
                break;
        case 2:
                if (ARG(1).type != VALUE_INTEGER)
                        vm_panic("the second argument to array.sort() must be an integer");
                n = ARG(1).integer;
        case 1:
                if (ARG(0).type != VALUE_INTEGER)
                        vm_panic("the first argument to array.sort() must be an integer");
                i = ARG(0).integer;
                break;
        default:
                vm_panic("array.sort() expects 0, 1, or 2 arguments but got %d", argc);
        }

        if (i < 0)
                i += array->array->count;

        if (argc == 1)
                n = array->array->count - i;

        if (n < 0 || i < 0 || i + n > array->array->count)
                vm_panic("invalid index passed to array.sort()");

        struct value *by = NAMED("by");
        struct value *cmp = NAMED("cmp");

        if (by != NULL && cmp != NULL) {
                vm_panic("ambiguous call to Array.sort(): by and cmp both specified");
        }

        if (by != NULL) {
                if (!CALLABLE(*by)) {
                        vm_panic("Array.sort(): `by` is not callable");
                }
                comparison_fn = by;
                qsort(array->array->items + i, n, sizeof (struct value), compare_by);
        } else if (cmp != NULL) {
                if (!CALLABLE(*cmp)) {
                        vm_panic("Array.sort(): `cmp` is not callable");
                }
                comparison_fn = cmp;
                qsort(array->array->items + i, n, sizeof (struct value), compare_by2);
        } else {
                qsort(array->array->items + i, n, sizeof (struct value), value_compare);
        }

        struct value *desc = NAMED("desc");

        if (desc != NULL && value_truthy(desc)) {
                array_reverse(array, argc, NULL);
        }

        return *array;
}

static struct value
array_next_permutation(struct value *array, int argc, struct value *kwargs)
{
#define CMP(i, j) value_compare(&array->array->items[i], &array->array->items[j])
        if (argc != 0)
                vm_panic("array.nextPermutation() expects no arguments but got %d", argc);

        for (int i = array->array->count - 1; i > 0; --i) {
                if (CMP(i - 1, i) < 0) {
                        int j = i;
                        for (int k = i + 1; k < array->array->count; ++k)
                                if (CMP(k, j) < 0 && CMP(k, i - 1) > 0)
                                        j = k;

                        struct value t = array->array->items[i - 1];
                        array->array->items[i - 1] = array->array->items[j];
                        array->array->items[j] = t;

                        vm_push(&INTEGER(i));
                        array_sort(array, 1, kwargs);
                        vm_pop();

                        return *array;
                }
        }

        return NIL;
#undef CMP
}

static struct value
array_take_while_mut(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("array.takeWhile!() expects 1 argument but got %d", argc);

        struct value f = ARG(0);

        if (!CALLABLE(f))
                vm_panic("non-callable predicate passed to array.takeWhile!()");

        int keep = 0;
        for (int i = 0; i < array->array->count; ++i) {
                if (value_apply_predicate(&f, &array->array->items[i]))
                        ++keep;
                else
                        break;
        }

        array->array->count = keep;
        shrink(array);

        return *array;
}

static struct value
array_take_while(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("array.takeWhile!() expects 1 argument but got %d", argc);

        struct value f = ARG(0);

        if (!CALLABLE(f))
                vm_panic("non-callable predicate passed to array.takeWhile!()");

        int keep = 0;
        for (int i = 0; i < array->array->count; ++i)
                if (value_apply_predicate(&f, &array->array->items[i]))
                        ++keep;
                else
                        break;

        struct value result = ARRAY(value_array_new());
        NOGC(result.array);
        value_array_reserve(result.array, keep);
        OKGC(result.array);
        memmove(result.array->items, array->array->items, keep * sizeof (struct value));
        result.array->count = keep;

        return result;
}

static struct value
array_drop_while_mut(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("array.dropWhile!() expects 1 argument but got %d", argc);

        struct value f = ARG(0);

        if (!CALLABLE(f))
                vm_panic("non-callable predicate passed to array.dropWhile!()");

        int drop = 0;
        for (int i = 0; i < array->array->count; ++i)
                if (value_apply_predicate(&f, &array->array->items[i]))
                        ++drop;
                else
                        break;

        memmove(array->array->items, array->array->items + drop, (array->array->count - drop) * sizeof (struct value));
        array->array->count -= drop;
        shrink(array);

        return *array;
}

static struct value
array_drop_while(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("array.dropWhile() expects 1 argument but got %d", argc);

        struct value f = ARG(0);

        if (!CALLABLE(f))
                vm_panic("non-callable predicate passed to array.dropWhile()");

        int drop = 0;
        for (int i = 0; i < array->array->count; ++i)
                if (value_apply_predicate(&f, &array->array->items[i]))
                        ++drop;
                else
                        break;

        int n = array->array->count - drop;
        struct value result = ARRAY(value_array_new());
        NOGC(result.array);
        value_array_reserve(result.array, n);
        OKGC(result.array);
        memmove(result.array->items, array->array->items + drop, n * sizeof (struct value));
        result.array->count = n;

        return result;
}

static struct value
array_uniq(struct value *array, int argc, struct value *kwargs)
{
        struct value *f = NULL;

        if (argc == 1)
                f = &ARG(0);
        else if (argc != 0)
                vm_panic("array.uniq() expects 0 or 1 arguments but got %d", argc);

        if (f != NULL && !CALLABLE(*f))
                vm_panic("the argument to array.uniq() must be callable");

        struct value d = DICT(dict_new());
        gc_push(&d);

        int n = 0;
        for (int i = 0; i < array->array->count; ++i) {
                struct value e = array->array->items[i];
                struct value k = (f == NULL) ? e : vm_eval_function(f, &e, NULL);
                struct value *v = dict_put_key_if_not_exists(d.dict, k);
                if (v->type == VALUE_NIL) {
                        *v = e;
                        array->array->items[n++] = e;
                }
        }

        gc_pop();
        array->array->count = n;

        return *array;
}

static struct value
array_take_mut(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("array.take!() expects 1 argument but got %d", argc);

        struct value n = ARG(0);

        if (n.type != VALUE_INTEGER)
                vm_panic("non-integer passed to array.take!()");

        array->array->count = min(array->array->count, n.integer);
        shrink(array);

        return *array;
}

static struct value
array_take(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("array.take() expects 1 argument but got %d", argc);

        struct value n = ARG(0);

        if (n.type != VALUE_INTEGER)
                vm_panic("non-integer passed to array.take!()");

        struct value result = ARRAY(value_array_new());

        int count = min(n.integer, array->array->count);

        NOGC(result.array);
        value_array_reserve(result.array, count);
        OKGC(result.array);

        memmove(result.array->items, array->array->items, count * sizeof (struct value));
        result.array->count = count;

        return result;
}

static struct value
array_drop_mut(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("array.drop!() expects 1 argument but got %d", argc);

        struct value n = ARG(0);

        if (n.type != VALUE_INTEGER)
                vm_panic("non-integer passed to array.drop!()");

        int d = min(array->array->count, max(n.integer, 0));

        memmove(array->array->items, array->array->items + d, (array->array->count - d) * sizeof (struct value));
        array->array->count -= d;
        shrink(array);

        return *array;
}

static struct value
array_drop(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("array.drop() expects 1 argument but got %d", argc);

        struct value n = ARG(0);

        if (n.type != VALUE_INTEGER)
                vm_panic("non-integer passed to array.drop()");

        struct value result = ARRAY(value_array_new());

        int d = min(max(n.integer, 0), array->array->count);
        int count = array->array->count - d;

        NOGC(result.array);
        value_array_reserve(result.array, count);
        OKGC(result.array);

        memcpy(result.array->items, array->array->items + d, count * sizeof (struct value));
        result.array->count = count;

        return result;
}

static struct value
array_sum(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 0)
                vm_panic("the sum method on arrays expects no arguments but got %d", argc);

        if (array->array->count == 0)
                return NIL;

        struct value sum, v;
        sum = array->array->items[0];

        gc_push(&sum);

        for (int i = 1; i < array->array->count; ++i) {
                v = array->array->items[i];
                sum = binary_operator_addition(&sum, &v);
        }

        gc_pop();

        return sum;
}

static struct value
array_join(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("array.join() expects 1 argument but got %d", argc);

        if (array->array->count == 0)
                return NIL;

        struct value sep = ARG(0);
        if (sep.type != VALUE_STRING)
                vm_panic("the argument to array.join() must be a string");

        vm_push(&array->array->items[0]);
        struct value sum = builtin_str(1, NULL);
        vm_pop();
        struct value v = NIL;

        gc_push(&sum);
        gc_push(&v);

        for (int i = 1; i < array->array->count; ++i) {
                vm_push(&array->array->items[i]);
                v = builtin_str(1, NULL);
                vm_pop();
                sum = binary_operator_addition(&sum, &sep);
                sum = binary_operator_addition(&sum, &v);
        }

        gc_pop();
        gc_pop();

        return sum;
}

static struct value
array_consume_while(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 2)
                vm_panic("array.consumeWhile() expects 2 arguments but got %d", argc);

        struct value f = ARG(0);
        struct value p = ARG(1);

        if (!CALLABLE(f))
                vm_panic("invalid source passed to array.consumeWhile()");

        if (!CALLABLE(p))
                vm_panic("invalid predicate passed to array.consumeWhile()");

        struct value v = NIL;
        gc_push(&v);

        for (;;) {
                v = vm_eval_function(&f, NULL);
                if (value_apply_predicate(&p, &v))
                        value_array_push(array->array, v);
                else
                        break;
        }

        gc_pop();

        return *array;
}

static struct value
array_groups_of(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1 && argc != 2)
                vm_panic("array.groupsOf() expects 1 or 2 arguments but got %d", argc);

        struct value size = ARG(0);
        if (size.type != VALUE_INTEGER)
                vm_panic("the argument to array.groupsOf() must be an integer");

        if (size.integer <= 0)
                vm_panic("the argument to array.groupsOf() must be positive");

        bool keep_short = true;

        if (argc == 2) {
                if (ARG(1).type != VALUE_BOOLEAN) {
                        vm_panic("the second argument to array.groupsOf() must be a boolean");
                }
                keep_short = ARG(1).boolean;
        }

        int n = 0;
        int i = 0;
        while (i + size.integer < array->array->count) {
                struct array *group = value_array_new();
                NOGC(group);
                vec_push_n(*group, array->array->items + i, size.integer);
                OKGC(group);
                array->array->items[n++] = ARRAY(group);
                i += size.integer;
        }

        if (keep_short && i != array->array->count) {
                struct array *last = value_array_new();
                NOGC(last);
                vec_push_n(*last, array->array->items + i, array->array->count - i);
                OKGC(last);
                array->array->items[n++] = ARRAY(last);
        }

        array->array->count = n;
        shrink(array);

        return *array;
}

static struct value
array_group_by(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("array.groupBy() expects 1 argument but got %d", argc);

        struct value f = ARG(0);

        if (!CALLABLE(f))
                vm_panic("the argument to array.groupBy() must be callable");

        struct value v1, v2;
        v1 = v2 = NIL;

        gc_push(&v1);
        gc_push(&v2);

        int len = 0;
        for (int i = 0; i < array->array->count; ++i) {
                struct value group = ARRAY(value_array_new());
                NOGC(group.array);
                struct value e = array->array->items[i];
                v1 = value_apply_callable(&f, &e);
                value_array_push(group.array, e);
                while (i + 1 < array->array->count) {
                        v2 = value_apply_callable(&f, &array->array->items[i + 1]);
                        if (value_test_equality(&v1, &v2))
                                value_array_push(group.array, array->array->items[++i]);
                        else
                                break;
                }
                OKGC(group.array);
                array->array->items[len++] = group;
        }

        gc_pop();
        gc_pop();

        array->array->count = len;
        shrink(array);

        return *array;
}

static struct value
array_group(struct value *array, int argc, struct value *kwargs)
{
        if (argc == 1)
                return array_group_by(array, argc, kwargs);

        if (argc != 0)
                vm_panic("array.group() expects 0 or 1 arguments but got %d", argc);

        int len = 0;
        for (int i = 0; i < array->array->count; ++i) {
                struct value group = ARRAY(value_array_new());
                NOGC(group.array);
                value_array_push(group.array, array->array->items[i]);
                while (i + 1 < array->array->count && value_test_equality(&array->array->items[i], &array->array->items[i + 1]))
                        value_array_push(group.array, array->array->items[++i]);
                OKGC(group.array);
                array->array->items[len++] = group;
        }

        array->array->count = len;
        shrink(array);

        return *array;
}

static struct value
array_intersperse(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("the intersperse method on arrays expects 1 argument but got %d", argc);

        struct value v = ARG(0);

        int n = array->array->count - 1;
        if (n < 1)
                return *array;

        int newcount = 2 * n + 1;
        value_array_reserve(array->array, newcount);
        memcpy(array->array->items + n + 1, array->array->items + 1, n * sizeof (struct value));

        int lo = 1;
        int hi = n + 1;
        for (int i = 0; i < n; ++i) {
                array->array->items[lo++] = v;
                array->array->items[lo++] = array->array->items[hi++];
        }

        array->array->count = newcount;
        return *array;
}

static struct value
array_min(struct value *array, int argc, struct value *kwargs)
{
        if (argc == 1)
                return array_min_by(array, argc, kwargs);

        if (argc != 0)
                vm_panic("the min method on arrays expects no arguments but got %d", argc);

        if (array->array->count == 0)
                return NIL;

        struct value min, v;
        min = array->array->items[0];

        for (int i = 1; i < array->array->count; ++i) {
                v = array->array->items[i];
                if (value_compare(&v, &min) < 0)
                        min = v;
        }

        return min;
}

static struct value
array_min_by(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("the minBy method on arrays expects 1 argument but got %d", argc);

        if (array->array->count == 0)
                return NIL;

        struct value f = ARG(0);
        if (!CALLABLE(f))
                vm_panic("non-function passed to the minBy method on array");

        struct value min, v, k, r;
        min = array->array->items[0];

        gc_push(&k);
        gc_push(&r);

        r = k = NIL;

        if (f.type == VALUE_FUNCTION && f.info[2] > 1) {
                for (int i = 1; i < array->array->count; ++i) {
                        v = array->array->items[i];
                        r = vm_eval_function(&f, &v, &min, NULL);
                        if ((r.type != VALUE_INTEGER && !value_truthy(&r)) || r.integer < 0)
                                min = v;

                }
        } else {
                k = vm_eval_function(&f, &min, NULL);
                for (int i = 1; i < array->array->count; ++i) {
                        v = array->array->items[i];
                        r = vm_eval_function(&f, &v, NULL);
                        if (value_compare(&r, &k) < 0) {
                                min = v;
                                k = r;
                        }
                }
        }

        gc_pop();
        gc_pop();

        return min;
}

static struct value
array_max(struct value *array, int argc, struct value *kwargs)
{
        if (argc == 1)
                return array_max_by(array, argc, kwargs);

        if (argc != 0)
                vm_panic("the max method on arrays expects no arguments but got %d", argc);

        if (array->array->count == 0)
                return NIL;

        struct value max, v;
        max = array->array->items[0];

        for (int i = 1; i < array->array->count; ++i) {
                v = array->array->items[i];
                if (value_compare(&v, &max) > 0)
                        max = v;
        }

        return max;
}

static struct value
array_max_by(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("the maxBy method on arrays expects 1 argument but got %d", argc);

        if (array->array->count == 0)
                return NIL;

        struct value f = ARG(0);
        if (!CALLABLE(f))
                vm_panic("non-function passed to the maxBy method on array");

        struct value max, v, k, r;
        max = array->array->items[0];

        gc_push(&k);
        gc_push(&r);

        k = r = NIL;

        if (f.type == VALUE_FUNCTION && f.info[2] > 1) {
                for (int i = 1; i < array->array->count; ++i) {
                        v = array->array->items[i];
                        r = vm_eval_function(&f, &v, &max, NULL);
                        if ((r.type != VALUE_INTEGER && value_truthy(&r)) || r.integer > 0)
                                max = v;

                }
        } else {
                k = vm_eval_function(&f, &max, NULL);
                for (int i = 1; i < array->array->count; ++i) {
                        v = array->array->items[i];
                        r = vm_eval_function(&f, &v, NULL);
                        if (value_compare(&r, &k) > 0) {
                                max = v;
                                k = r;
                        }
                }
        }

        gc_pop();
        gc_pop();

        return max;
}

static struct value
array_length(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 0)
                vm_panic("array.len() expects no arguments but got %d", argc);

        return INTEGER(array->array->count);
}

static struct value
array_shuffle(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 0)
                vm_panic("the shuffle! method on arrays expects no arguments but got %d", argc);

        struct value t;
        int n = array->array->count;
        for (int i = n - 1; i > 0; --i) {
                int j = rand() % (i + 1);
                t = array->array->items[i];
                array->array->items[i] = array->array->items[j];
                array->array->items[j] = t;
        }

        return *array;
}

static struct value
array_map(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("the map method on arrays expects 1 argument but got %d", argc);

        struct value f = ARG(0);

        if (!CALLABLE(f))
                vm_panic("non-function passed to the map method on array");

        int n = array->array->count;
        for (int i = 0; i < n; ++i)
                array->array->items[i] = value_apply_callable(&f, &array->array->items[i]);

        return *array;
}

static struct value
array_enumerate(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 0)
                vm_panic("the enumerate method on arrays expects no arguments but got %d", argc);

        int n = array->array->count;

        for (int i = 0; i < n; ++i) {
                struct value entry = value_tuple(2);
                entry.items[0] = INTEGER(i);
                entry.items[1] = array->array->items[i];
                array->array->items[i] =  entry;
        }

        return *array;
}

static struct value
array_remove(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("the remove method on arrays expects 1 argument but got %d", argc);

        struct value v = ARG(0);

        int n = array->array->count;
        int j = 0;
        for (int i = 0; i < n; ++i)
                if (!value_test_equality(&v, &array->array->items[i]))
                        array->array->items[j++] = array->array->items[i];

        array->array->count = j;
        shrink(array);

        return *array;
}

static struct value
array_filter(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("the filter method on arrays expects 1 argument but got %d", argc);

        struct value pred = ARG(0);

        if (!CALLABLE(pred))
                vm_panic("non-predicate passed to the filter method on array");

        int n = array->array->count;
        int j = 0;
        for (int i = 0; i < n; ++i)
                if (value_apply_predicate(&pred, &array->array->items[i]))
                        array->array->items[j++] = array->array->items[i];

        array->array->count = j;
        shrink(array);

        return *array;
}

static struct value
array_find(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("the find method on arrays expects 1 argument but got %d", argc);

        struct value pred = ARG(0);

        if (!CALLABLE(pred))
                vm_panic("non-predicate passed to the find method on array");

        int n = array->array->count;
        for (int i = 0; i < n; ++i)
                if (value_apply_predicate(&pred, &array->array->items[i]))
                        return array->array->items[i];

        return NIL;
}

static struct value
array_findr(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("the findr method on arrays expects 1 argument but got %d", argc);

        struct value pred = ARG(0);

        if (!CALLABLE(pred))
                vm_panic("non-predicate passed to the findr method on array");

        int n = array->array->count;
        for (int i = n - 1; i >= 0; --i)
                if (value_apply_predicate(&pred, &array->array->items[i]))
                        return array->array->items[i];

        return NIL;
}

static struct value
array_bsearch(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("the bsearch? method on array expects 1 argument but got %d", argc);

        struct value v = ARG(0);

        int i = 0,
            lo = 0,
            hi = array->array->count - 1;

        while (lo <= hi) {
                int m = (lo + hi) / 2;
                int c = value_compare(&v, &array->array->items[m]);
                if      (c < 0) { hi = m - 1; i = m;  }
                else if (c > 0) { lo = m + 1; i = lo; }
                else            { return INTEGER(m);  }
        }

        return INTEGER(i);
}

static struct value
array_bsearch_strict(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("the bsearch method on array expects 1 argument but got %d", argc);

        struct value v = ARG(0);

        int lo = 0,
            hi = array->array->count - 1;

        while (lo <= hi) {
                int m = (lo + hi) / 2;
                int c = value_compare(&v, &array->array->items[m]);
                if      (c < 0) hi = m - 1;
                else if (c > 0) lo = m + 1;
                else            return INTEGER(m);
        }

        return NIL;
}

static struct value
array_search_by(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("the searchBy method on arrays expects 1 argument but got %d", argc);

        struct value pred = ARG(0);

        if (!CALLABLE(pred))
                vm_panic("non-predicate passed to the searchBy method on array");

        int n = array->array->count;
        for (int i = 0; i < n; ++i)
                if (value_apply_predicate(&pred, &array->array->items[i]))
                        return INTEGER(i);

        return NIL;
}

static struct value
array_searchr_by(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("the searchrBy method on arrays expects 1 argument but got %d", argc);

        struct value pred = ARG(0);

        if (!CALLABLE(pred))
                vm_panic("non-predicate passed to the searchBy method on array");

        int n = array->array->count;
        for (int i = n - 1; i >= 0; --i)
                if (value_apply_predicate(&pred, &array->array->items[i]))
                        return INTEGER(i);

        return NIL;
}

static struct value
array_set(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 0)
                vm_panic("array.set() expects 0 arguments but got %d", argc);

        struct dict *d = dict_new();
        NOGC(d);

        for (int i = 0; i < array->array->count; ++i) {
                dict_put_key_if_not_exists(d, array->array->items[i]);
        }

        OKGC(d);
        return DICT(d);
}

static struct value
array_partition(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("the partition method on arrays expects 1 argument but got %d", argc);

        struct value pred = ARG(0);

        if (!CALLABLE(pred))
                vm_panic("non-predicate passed to the partition method on array");

        int n = array->array->count;
        int j = 0;
        struct array *yes = value_array_new();
        struct array *no = value_array_new();

        NOGC(yes);
        NOGC(no);

        for (int i = 0; i < n; ++i) {
                if (value_apply_predicate(&pred, &array->array->items[i])) {
                        array->array->items[j++] = array->array->items[i];
                } else {
                        value_array_push(no, array->array->items[i]);
                }
        }

        array->array->count = j;
        shrink(array);

        yes->items = array->array->items;
        yes->count = array->array->count;
        yes->capacity = array->array->capacity;

        vec_init(*array->array);

        value_array_push(array->array, ARRAY(yes));
        value_array_push(array->array, ARRAY(no));

        OKGC(yes);
        OKGC(no);

        return *array;
}

static struct value
array_split_at(struct value *array, int argc, struct value *kargs)
{
        if (argc != 1) {
                vm_panic("array.split()  expects 1 argument but got %d", argc);
        }

        if (ARG(0).type != VALUE_INTEGER) {
                vm_panic(
                        "array.split() expected integer but got %s%s%s%s",
                        TERM(96),
                        TERM(1),
                        value_show(&ARG(0)),
                        TERM(0)
                );
        }

        int i = ARG(0).integer;

        if (i < 0)
                i += array->array->count;

        if (i < 0 || i > array->array->count) {
                vm_panic("array.split(): index %s%d%s out of range", TERM(96), i, TERM(0));
        }

        struct array *front = value_array_new();
        NOGC(front);

        struct array *back = value_array_new();
        NOGC(back);

        vec_push_n(*front, array->array->items, i);
        vec_push_n(*back, array->array->items + i, array->array->count - i);

        struct value pair = value_tuple(2);
        pair.items[0] = ARRAY(front);
        pair.items[1] = ARRAY(back);

        OKGC(front);
        OKGC(back);

        return pair;
}

static struct value
array_partition_no_mut(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("the partition method on arrays expects 1 argument but got %d", argc);

        struct value pred = ARG(0);

        if (!CALLABLE(pred))
                vm_panic("non-predicate passed to the partition method on array");

        int n = array->array->count;
        struct array *yes = value_array_new();
        struct array *no = value_array_new();

        NOGC(yes);
        NOGC(no);

        for (int i = 0; i < n; ++i) {
                if (value_apply_predicate(&pred, &array->array->items[i])) {
                        value_array_push(yes, array->array->items[i]);
                } else {
                        value_array_push(no, array->array->items[i]);
                }
        }

        struct array *result = value_array_new();
        NOGC(result);

        value_array_push(result, ARRAY(yes));
        value_array_push(result, ARRAY(no));


        OKGC(yes);
        OKGC(no);
        OKGC(result);

        return ARRAY(result);
}

static struct value
array_contains(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("array.contains?() expects 1 argument but got %d", argc);

        struct value v = ARG(0);

        int n = array->array->count;
        for (int i = 0; i < n; ++i)
                if (value_test_equality(&v, &array->array->items[i]))
                        return BOOLEAN(true);

        return BOOLEAN(false);
}

static struct value
array_tuple(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 0) {
                vm_panic("array.tuple() expects 0 arguments but got %d", argc);
        }

        int n = array->array->count;

        struct value v = value_tuple(n);
        memcpy(v.items, array->array->items, sizeof (struct value [n]));

        return v;
}

static struct value
array_tally(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 0 && argc != 1)
                vm_panic("array.tally() expects 0 or 1 argument(s) but got %d", argc);

        struct value d = DICT(dict_new());
        gc_push(&d);

        if (argc == 0) {
                for (int i = 0; i < array->array->count; ++i) {
                        struct value *c = dict_get_value(d.dict, &array->array->items[i]);
                        if (c == NULL) {
                                dict_put_value(d.dict, array->array->items[i], INTEGER(1));
                        } else {
                                c->integer += 1;
                        }
                }
        } else {
                struct value f = ARG(0);
                if (!CALLABLE(f))
                        vm_panic("non-callable passed to array.tally()");

                for (int i = 0; i < array->array->count; ++i) {
                        struct value v = value_apply_callable(&f, &array->array->items[i]);
                        struct value *c = dict_get_value(d.dict, &v);
                        if (c == NULL) {
                                dict_put_value(d.dict, v, INTEGER(1));
                        } else {
                                c->integer += 1;
                        }
                }
        }

        gc_pop();

        return d;
}

static struct value
array_search(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("array.search() expects 1 argument but got %d", argc);

        struct value v = ARG(0);

        int n = array->array->count;
        for (int i = 0; i < n; ++i)
                if (value_test_equality(&v, &array->array->items[i]))
                        return INTEGER(i);

        return NIL;
}

static struct value
array_searchr(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("array.searchr() expects 1 argument but got %d", argc);

        struct value v = ARG(0);

        int n = array->array->count;
        for (int i = n - 1; i >= 0; --i)
                if (value_test_equality(&v, &array->array->items[i]))
                        return INTEGER(i);

        return NIL;
}

static struct value
array_flat(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 0 && argc != 1) {
                vm_panic("array.flat() expects 0 or 1 arguments but got %d", argc);
        }

        struct array *r = value_array_new();

        vec(struct value *) stack = {0};
        vec(int) dstack = {0};

        int maxdepth;

        if (argc == 1) {
                if (ARG(0).type != VALUE_INTEGER) {
                        vm_panic("the argument to array.flat() must be an integer");
                }
                maxdepth = ARG(0).integer;
        } else {
                maxdepth = INT_MAX;
        }

        NOGC(r);

        for (int i = 0; i < array->array->count; ++i) {
                vec_push(stack, &array->array->items[i]);
                vec_push(dstack, 1);
                while (stack.count > 0) {
                        struct value *v = *vec_pop(stack);
                        int d = *vec_pop(dstack);
                        if (v->type != VALUE_ARRAY || d > maxdepth) {
                                value_array_push(r, *v);
                        } else {
                                for (int i = v->array->count - 1; i >= 0; --i) {
                                        vec_push(stack, &v->array->items[i]);
                                        vec_push(dstack, d + 1);
                                }
                        }
                }
        }

        vec_empty(stack);
        OKGC(r);

        return ARRAY(r);

}

static struct value
array_each(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1 && argc != 2)
                vm_panic("the each method on arrays expects 1 or 2 arguments but got %d", argc);

        if (argc == 1) {
                struct value f = ARG(0);

                if (f.type != VALUE_FUNCTION && f.type != VALUE_BUILTIN_FUNCTION && f.type != VALUE_METHOD && f.type != VALUE_BUILTIN_METHOD)
                        vm_panic("non-function passed to the each method on array");

                int n = array->array->count;

                for (int i = 0; i < n; ++i)
                        vm_eval_function(&f, &array->array->items[i], &INTEGER(i), NULL);

                return *array;
        } else {
                struct value v = ARG(0);
                struct value f = ARG(1);

                if (f.type != VALUE_FUNCTION && f.type != VALUE_BUILTIN_FUNCTION && f.type != VALUE_METHOD && f.type != VALUE_BUILTIN_METHOD)
                        vm_panic("non-function passed to the each method on array");

                int n = array->array->count;

                for (int i = 0; i < n; ++i) {
                        vm_eval_function(&f, &v, &array->array->items[i], &INTEGER(i), NULL);
                }

                return v;
        }

}

static struct value
array_all(struct value *array, int argc, struct value *kwargs)
{
        int n = array->array->count;

        if (argc == 0) {
                for (int i = 0; i < n; ++i) {
                        if (!value_truthy(&array->array->items[i]))
                                return BOOLEAN(false);
                }
        } else if (argc == 1) {
                struct value pred = ARG(0);

                if (!CALLABLE(pred))
                        vm_panic("non-predicate passed to the all? method on array");

                for (int i = 0; i < n; ++i) {
                        if (!value_apply_predicate(&pred, &array->array->items[i]))
                                return BOOLEAN(false);
                }
        } else {
                vm_panic("the all? method on arrays expects 0 or 1 argument(s) but got %d", argc);
        }

        return BOOLEAN(true);
}

static struct value
array_any(struct value *array, int argc, struct value *kwargs)
{
        int n = array->array->count;

        if (argc == 0) {
                for (int i = 0; i < n; ++i)
                        if (value_truthy(&array->array->items[i]))
                                return BOOLEAN(true);
        } else if (argc == 1) {
                struct value pred = ARG(0);

                if (!CALLABLE(pred))
                        vm_panic("non-predicate passed to the any? method on array");

                for (int i = 0; i < n; ++i)
                        if (value_apply_predicate(&pred, &array->array->items[i]))
                                return BOOLEAN(true);
        } else {
                vm_panic("the any? method on arrays expects 0 or 1 argument(s) but got %d", argc);
        }

        return BOOLEAN(false);
}

static struct value
array_count(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("the count method on arrays expects 1 argument but got %d", argc);

        struct value v = ARG(0);

        int n = array->array->count;
        int k = 0;
        for (int i = 0; i < n; ++i)
                if (value_test_equality(&v, &array->array->items[i]))
                        k += 1;

        return INTEGER(k);
}

static struct value
array_count_by(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("the count method on arrays expects 1 argument but got %d", argc);

        struct value pred = ARG(0);

        if (!CALLABLE(pred))
                vm_panic("non-predicate passed to the count method on array");

        int n = array->array->count;
        int k = 0;
        for (int i = 0; i < n; ++i)
                if (value_apply_predicate(&pred, &array->array->items[i]))
                        k += 1;

        return INTEGER(k);
}

static struct value
array_fold_left(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1 && argc != 2)
                vm_panic("the foldLeft method on arrays expects 1 or 2 arguments but got %d", argc);

        int start;
        struct value f, v;

        if (argc == 1) {
                start = 1;
                f = ARG(0);
                if (array->array->count == 0)
                        vm_panic("foldLeft called on empty array with 1 argument");
                v = array->array->items[0];
        } else {
                start = 0;
                f = ARG(1);
                v = ARG(0);
        }

        if (!CALLABLE(f))
                vm_panic("non-function passed to the foldLeft method on array");

        gc_push(&v);

        int n = array->array->count;
        for (int i = start; i < n; ++i)
                v = vm_eval_function(&f, &v, &array->array->items[i], NULL);

        gc_pop();

        return v;
}

/* TODO: fix this */
static struct value
array_fold_right(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1 && argc != 2)
                vm_panic("the foldRight method on arrays expects 1 or 2 arguments but got %d", argc);

        int start;
        struct value f, v;

        if (argc == 1) {
                start = array->array->count - 2;
                f = ARG(0);
                if (array->array->count == 0)
                        vm_panic("foldRight called on empty array with 1 argument");
                v = array->array->items[start + 1];
        } else {
                start = array->array->count - 1;
                f = ARG(1);
                v = ARG(0);
        }

        if (!CALLABLE(f))
                vm_panic("non-function passed to the foldRight method on array");

        gc_push(&v);

        for (int i = start; i >= 0; --i)
                v = vm_eval_function(&f, &array->array->items[i], &v, NULL);

        gc_pop();

        return v;
}

static struct value
array_scan_left(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1 && argc != 2)
                vm_panic("the scanLeft method on arrays expects 1 or 2 arguments but got %d", argc);

        int start;
        struct value f, v;

        if (argc == 1) {
                start = 1;
                f = ARG(0);
                if (array->array->count == 0)
                        vm_panic("scanLeft called on empty array with 1 argument");
                v = array->array->items[0];
        } else {
                start = 0;
                f = ARG(1);
                v = ARG(0);
        }

        if (!CALLABLE(f))
                vm_panic("non-function passed to the scanLeft method on array");

        int n = array->array->count;
        for (int i = start; i < n; ++i) {
                v = vm_eval_function(&f, &v, &array->array->items[i], NULL);
                array->array->items[i] = v;
        }

        return *array;
}

static struct value
array_scan_right(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1 && argc != 2)
                vm_panic("the scanRight method on arrays expects 1 or 2 arguments but got %d", argc);

        int start;
        struct value f, v;

        if (argc == 1) {
                start = array->array->count - 2;
                f = ARG(0);
                if (array->array->count == 0)
                        vm_panic("scanRight called on empty array with 1 argument");
                v = array->array->items[start + 1];
        } else {
                start = array->array->count - 1;
                f = ARG(1);
                v = ARG(0);
        }

        if (!CALLABLE(f))
                vm_panic("non-function passed to the scanRight method on array");

        for (int i = start; i >= 0; --i) {
                v = vm_eval_function(&f, &array->array->items[i], &v, NULL);
                array->array->items[i] = v;
        }

        return *array;
}

static struct value
array_reverse(struct value *array, int argc, struct value *kwargs)
{
        int lo;
        int n;

        if (argc > 0 && ARG(0).type != VALUE_INTEGER) {
                vm_panic("array.reverse(): expected integer as first argument but got: %s", value_show(&ARG(0)));
        }

        if (argc > 1 && ARG(1).type != VALUE_INTEGER) {
                vm_panic("array.reverse(): expected integer as second argument but got: %s", value_show(&ARG(1)));
        }

        if (argc > 0) {
                lo = ARG(0).integer;
                if (lo < 0) { lo += array->array->count; }
        } else {
                lo = 0;
        }

        if (lo < 0 || lo > array->array->count) {
                vm_panic("array.reverse(): invalid start index %d for array with size %zu", lo, array->array->count);
        }

        if (argc > 1) {
                n = ARG(1).integer;
        } else {
                n = array->array->count - lo;
        }

        if (n == 0) {
                return *array;
        }

        int hi = lo + n - 1;

        if (hi > array->array->count) {
                vm_panic(
                        "array.reverse(): invalid count %d for start index %d and array with size %zu",
                        n, lo, array->array->count
                );
        }

        struct value t;

        while (lo < hi) {
                t = array->array->items[lo];
                array->array->items[lo] = array->array->items[hi];
                array->array->items[hi] = t;

                ++lo;
                --hi;
        }

        return *array;
}

static struct value
array_rotate(struct value *array, int argc, struct value *kwargs)
{
        int d = 1;
        int n = array->array->count;

        if (argc == 1) {
                struct value amount = ARG(0);
                if (amount.type != VALUE_INTEGER)
                        vm_panic("the argument to array.rotate() must be an integer");
                d = amount.integer;
        } else if (argc != 0) {
                vm_panic("the rotate method on arrays expects 0 or 1 arguments but got %d", argc);
        }

        d %= n;
        if (d < 0)
                d += n;

        int N = gcd(n, d);
        int i, j, k;
        for (i = 0; i < N; ++i) {
                struct value t = array->array->items[i];
                j = i;
                for (;;) {
                        k = j + d;
                        if (k >= n)
                                k = k - n;
                        if (k == i)
                                break;
                        array->array->items[j] = array->array->items[k];
                        j = k;

                }
                array->array->items[j] = t;
        }

        return *array;
}

static struct value
array_sort_on(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("Array.sortOn() expects 1 argument but got %d", argc);

        struct value f = ARG(0);
        if (!CALLABLE(f))
                vm_panic("non-function passed to the Array.sortOn()");

        if (array->array->count == 0)
                return *array;

        comparison_fn = &f;

        qsort(array->array->items, array->array->count, sizeof (struct value), compare_by);

        return *array;
}

static struct value
array_sort_by(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 1)
                vm_panic("Array.sortBy() expects 1 argument but got %d", argc);

        struct value f = ARG(0);
        if (!CALLABLE(f))
                vm_panic("non-function passed to the Array.sortBy()");

        if (array->array->count == 0)
                return *array;

        comparison_fn = &f;

        qsort(array->array->items, array->array->count, sizeof (struct value), compare_by2);

        return *array;
}

static struct value
array_clone(struct value *array, int argc, struct value *kwargs)
{
        if (argc != 0)
                vm_panic("the clone method on arrays expects no arguments but got %d", argc);

        struct value v = *array;
        v.array = value_array_clone(v.array);

        return v;
}

#define DEFINE_NO_MUT(name) \
        static struct value \
        array_ ## name ## _no_mut(struct value *array, int argc, struct value *kwargs) \
        { \
                struct value clone = array_clone(array, 0, NULL); \
                gc_push(&clone); \
                struct value result = array_ ## name(&clone, argc, kwargs); \
                gc_pop(); \
                return result; \
        }

DEFINE_NO_MUT(enumerate);
DEFINE_NO_MUT(filter);
DEFINE_NO_MUT(remove);
DEFINE_NO_MUT(group);
DEFINE_NO_MUT(group_by);
DEFINE_NO_MUT(groups_of);
DEFINE_NO_MUT(intersperse);
DEFINE_NO_MUT(map);
DEFINE_NO_MUT(window);
DEFINE_NO_MUT(reverse);
DEFINE_NO_MUT(rotate);
DEFINE_NO_MUT(scan_left);
DEFINE_NO_MUT(scan_right);
DEFINE_NO_MUT(shuffle);
DEFINE_NO_MUT(sort);
DEFINE_NO_MUT(sort_by);
DEFINE_NO_MUT(sort_on);
DEFINE_NO_MUT(uniq);
DEFINE_NO_MUT(zip);
DEFINE_NO_MUT(next_permutation);

DEFINE_METHOD_TABLE(
        { .name = "all?",              .func = array_all                     },
        { .name = "any?",              .func = array_any                     },
        { .name = "bsearch",           .func = array_bsearch_strict          },
        { .name = "bsearch?",          .func = array_bsearch                 },
        { .name = "clone",             .func = array_clone                   },
        { .name = "consumeWhile",      .func = array_consume_while           },
        { .name = "contains?",         .func = array_contains                },
        { .name = "count",             .func = array_count                   },
        { .name = "countBy",           .func = array_count_by                },
        { .name = "drop",              .func = array_drop                    },
        { .name = "drop!",             .func = array_drop_mut                },
        { .name = "dropWhile",         .func = array_drop_while              },
        { .name = "dropWhile!",        .func = array_drop_while_mut          },
        { .name = "each",              .func = array_each                    },
        { .name = "enumerate",         .func = array_enumerate_no_mut        },
        { .name = "enumerate!",        .func = array_enumerate               },
        { .name = "filter",            .func = array_filter_no_mut           },
        { .name = "filter!",           .func = array_filter                  },
        { .name = "find",              .func = array_find                    },
        { .name = "findr",             .func = array_findr                   },
        { .name = "flat",              .func = array_flat                    },
        { .name = "fold",              .func = array_fold_left               },
        { .name = "foldr",             .func = array_fold_right              },
        { .name = "group",             .func = array_group_no_mut            },
        { .name = "group!",            .func = array_group                   },
        { .name = "groupBy",           .func = array_group_by_no_mut         },
        { .name = "groupBy!",          .func = array_group_by                },
        { .name = "groupsOf",          .func = array_groups_of_no_mut        },
        { .name = "groupsOf!",         .func = array_groups_of               },
        { .name = "has?",              .func = array_contains                },
        { .name = "insert",            .func = array_insert                  },
        { .name = "intersperse",       .func = array_intersperse_no_mut      },
        { .name = "intersperse!",      .func = array_intersperse             },
        { .name = "join",              .func = array_join                    },
        { .name = "len",               .func = array_length                  },
        { .name = "map",               .func = array_map_no_mut              },
        { .name = "map!",              .func = array_map                     },
        { .name = "max",               .func = array_max                     },
        { .name = "maxBy",             .func = array_max_by                  },
        { .name = "min",               .func = array_min                     },
        { .name = "minBy",             .func = array_min_by                  },
        { .name = "nextPermutation",   .func = array_next_permutation_no_mut },
        { .name = "nextPermutation!",  .func = array_next_permutation        },
        { .name = "partition",         .func = array_partition_no_mut        },
        { .name = "partition!",        .func = array_partition               },
        { .name = "pop",               .func = array_pop                     },
        { .name = "push",              .func = array_push                    },
        { .name = "remove",            .func = array_remove_no_mut           },
        { .name = "remove!",           .func = array_remove                  },
        { .name = "reverse",           .func = array_reverse_no_mut          },
        { .name = "reverse!",          .func = array_reverse                 },
        { .name = "rotate",            .func = array_rotate_no_mut           },
        { .name = "rotate!",           .func = array_rotate                  },
        { .name = "scan",              .func = array_scan_left_no_mut        },
        { .name = "scan!",             .func = array_scan_left               },
        { .name = "scanr",             .func = array_scan_right_no_mut       },
        { .name = "scanr!",            .func = array_scan_right              },
        { .name = "search",            .func = array_search                  },
        { .name = "searchBy",          .func = array_search_by               },
        { .name = "searchr",           .func = array_searchr                 },
        { .name = "searchrBy",         .func = array_searchr_by              },
        { .name = "set",               .func = array_set                     },
        { .name = "shuffle",           .func = array_shuffle_no_mut          },
        { .name = "shuffle!",          .func = array_shuffle                 },
        { .name = "slice",             .func = array_slice                   },
        { .name = "slice!",            .func = array_slice_mut               },
        { .name = "sort",              .func = array_sort_no_mut             },
        { .name = "sort!",             .func = array_sort                    },
        { .name = "sortBy",            .func = array_sort_by_no_mut          },
        { .name = "sortBy!",           .func = array_sort_by                 },
        { .name = "sortOn",            .func = array_sort_on_no_mut          },
        { .name = "sortOn!",           .func = array_sort_on                 },
        { .name = "split",             .func = array_split_at                },
        { .name = "sum",               .func = array_sum                     },
        { .name = "swap",              .func = array_swap                    },
        { .name = "take",              .func = array_take                    },
        { .name = "take!",             .func = array_take_mut                },
        { .name = "takeWhile",         .func = array_take_while              },
        { .name = "takeWhile!",        .func = array_take_while_mut          },
        { .name = "tally",             .func = array_tally                   },
        { .name = "tuple",             .func = array_tuple                   },
        { .name = "uniq",              .func = array_uniq_no_mut             },
        { .name = "uniq!",             .func = array_uniq                    },
        { .name = "window",            .func = array_window_no_mut           },
        { .name = "window!",           .func = array_window                  },
        { .name = "zip",               .func = array_zip_no_mut              },
        { .name = "zip!",              .func = array_zip                     },
);

DEFINE_METHOD_LOOKUP(array)

DEFINE_METHOD_COMPLETER(array)
