#include "fpbinarysmall.h"
#include "fpbinarycommon.h"
#include <math.h>

/*
 * The value exposed to the user is 2's complement for signed representation.
 * Underlying scaled value uses an unsigned long int. We use the guaranteed
 * wrapping behavior of unsigned values to naturally give the correct 2's
 * complement answer for math operations (once the initial value is set
 * correctly).
 */

static int
check_new_bit_len_ok(FpBinarySmallObject *new_obj)
{
    if ((new_obj->int_bits + new_obj->frac_bits) > FP_UINT_NUM_BITS)
    {
        PyErr_SetString(PyExc_OverflowError,
                        "New FpBinary object has too many bits for this CPU.");
        return false;
    }

    return true;
}

static inline FP_UINT_TYPE
get_sign_bit(FP_UINT_TYPE total_bits)
{
    return (((FP_UINT_TYPE)1) << (total_bits - 1));
}

/*
* Returns an int that gives the largest value representable (after conversion to
* scaled
* int representation) given total_bits.
*/
static FP_UINT_TYPE
get_max_scaled_value(FP_UINT_TYPE total_bits, bool is_signed)
{
    if (is_signed)
    {
        return (((FP_UINT_TYPE)1) << (total_bits - 1)) - 1;
    }

    return FP_UINT_MAX_VAL >> (FP_UINT_NUM_BITS - total_bits);
}

/*
* Returns an int that gives the smallest value representable (after conversion
* to scaled
* int representation) given total_bits. Beware comparing with this directly.
* Usually, it
* needs to be compared via the compare_scaled_values function.
*/
static FP_UINT_TYPE
get_min_scaled_value(FP_UINT_TYPE total_bits, bool is_signed)
{
    if (is_signed)
    {
        return FP_UINT_MAX_VAL << (total_bits - 1);
    }

    return 0;
}

static FP_UINT_TYPE
get_mag_of_min_scaled_value(FP_UINT_TYPE total_bits, bool is_signed)
{
    if (is_signed)
    {
        return get_sign_bit(total_bits);
    }

    return 0;
}

/*
 * Interprets scaled_value as a 2's complement signed integer.
 */
static FP_INT_TYPE
scaled_value_to_int(FP_UINT_TYPE scaled_value)
{
    /* I'm not simply casting to a signed pointer because the C standard doesn't
     * guarantee signed integers are 2's complement.
     */
    if (scaled_value & FP_UINT_MAX_SIGN_BIT)
    {
        /* Negative. Convert to magnitude and multiply by -1. */
        return ((FP_INT_TYPE)(~scaled_value + 1)) * -1;
    }
    else
    {
        return (FP_INT_TYPE)scaled_value;
    }
}

static PyObject *
scaled_value_to_pylong(FP_UINT_TYPE scaled_value, bool is_signed)
{
    if (is_signed)
    {

        return fp_int_as_pylong(scaled_value_to_int(scaled_value));
    }

    return fp_uint_as_pylong(scaled_value);
}

/*
* Returns 1, 0 or -1 depending on whether op1 is larger, equal to or smaller
* than op2. It is assumed the ops are taken from fpbinarysmallobject
* scaled_value fields and that the total number of bits are the same for both
* ops.
*/
static inline int
compare_scaled_values(FP_UINT_TYPE total_bits, bool are_signed,
                      FP_UINT_TYPE op1, FP_UINT_TYPE op2)
{
    if (are_signed)
    {
        bool op1_negative = op1 & FP_UINT_MAX_SIGN_BIT;
        bool op2_negative = op2 & FP_UINT_MAX_SIGN_BIT;

        if ((op1_negative == op2_negative && op1 > op2) ||
            (!op1_negative && op2_negative))
        {
            return 1;
        }
        else if ((op1_negative == op2_negative && op1 < op2) ||
                 (op1_negative && !op2_negative))
        {
            return -1;
        }
        else
        {
            return 0;
        }
    }

    if (op1 == op2)
        return 0;
    return (op1 > op2) ? 1 : 0;
}

static FP_UINT_TYPE
get_total_bits_mask(FP_UINT_TYPE total_bits)
{
    return get_max_scaled_value(total_bits, false);
}

static FP_UINT_TYPE
get_frac_bits_mask(FP_UINT_TYPE frac_bits)
{
    if (frac_bits == 0)
    {
        return 0;
    }

    return get_total_bits_mask(frac_bits);
}

static FP_UINT_TYPE
apply_rshift(FP_UINT_TYPE value, FP_UINT_TYPE num_shifts, bool is_signed)
{
    /* Using unsigned integers to represent possible signed values, so need
     * to manually ensure sign is extended on shift.
     */
    if (is_signed)
    {
        if (value & FP_UINT_MAX_SIGN_BIT)
        {
            /* Need to shift with 1's. Calculate mask that will OR out the newly
             * shifted zeros. */
            return ((value >> num_shifts) | ~(FP_UINT_MAX_VAL >> num_shifts));
        }
    }

    return (value >> num_shifts);
}

static inline FP_UINT_TYPE
apply_overflow_wrap(FP_UINT_TYPE value, bool is_signed, FP_UINT_TYPE max_value,
                    FP_UINT_TYPE sign_bit)
{
    /* If we overflowed into the negative range, subtract the sign bit
     * from the magnitude-masked value. If we overflowed into the
     * positive range, just mask with the magnitude bits only. */

    if (is_signed && (value & sign_bit))
    {
        return (value & max_value) - sign_bit;
    }

    return (value & max_value);
}

static inline void
set_object_fields(FpBinarySmallObject *self, FP_UINT_TYPE scaled_value,
                  FP_UINT_TYPE int_bits, FP_UINT_TYPE frac_bits, bool is_signed)
{
    self->scaled_value = scaled_value;
    self->int_bits = int_bits;
    self->frac_bits = frac_bits;
    self->is_signed = is_signed;
}

static void
copy_fields(FpBinarySmallObject *from_obj, FpBinarySmallObject *to_obj)
{
    set_object_fields(to_obj, from_obj->scaled_value, from_obj->int_bits,
                      from_obj->frac_bits, from_obj->is_signed);
}

/*
 * Will check the fields in obj for overflow and will modify act on the
 * fields of obj or raise an exception depending on overflow_mode.
 * If there was no exception raised, non-zero is returned.
 * Otherwise, 0 is returned.
 */
static int
check_overflow(FpBinarySmallObject *self, fp_overflow_mode_t overflow_mode)
{
    FP_UINT_TYPE new_scaled_value = self->scaled_value;
    FP_UINT_TYPE min_value, max_value;
    FP_UINT_TYPE total_bits = self->int_bits + self->frac_bits;
    FP_UINT_TYPE sign_bit = get_sign_bit(total_bits);

    min_value = get_min_scaled_value(total_bits, self->is_signed);
    max_value = get_max_scaled_value(total_bits, self->is_signed);

    if (compare_scaled_values(total_bits, self->is_signed, new_scaled_value,
                              max_value) > 0)
    {
        if (overflow_mode == OVERFLOW_WRAP)
        {
            new_scaled_value = apply_overflow_wrap(
                new_scaled_value, self->is_signed, max_value, sign_bit);
        }
        else if (overflow_mode == OVERFLOW_SAT)
        {
            new_scaled_value = max_value;
        }
        else
        {
            PyErr_SetString(FpBinaryOverflowException,
                            "Fixed point resize overflow.");
            return false;
        }
    }
    else if (compare_scaled_values(total_bits, self->is_signed,
                                   new_scaled_value, min_value) < 0)
    {
        if (overflow_mode == OVERFLOW_WRAP)
        {
            new_scaled_value = apply_overflow_wrap(
                new_scaled_value, self->is_signed, max_value, sign_bit);
        }
        else if (overflow_mode == OVERFLOW_SAT)
        {
            new_scaled_value = min_value;
        }
        else
        {
            PyErr_SetString(FpBinaryOverflowException,
                            "Fixed point resize overflow.");
            return false;
        }
    }

    set_object_fields(self, new_scaled_value, self->int_bits, self->frac_bits,
                      self->is_signed);
    return true;
}

/*
 * Will convert the passed float to a fixed point object and apply
 * the result to output_obj.
 * Returns 0 if (there was an overflow AND round_mode is OVERFLOW_EXCEP) OR
 *              (the passed value can't be represented on this CPU using a
 * FP_UINT_TYPE).
 * Otherwise, non zero is returned.
 */
static bool
build_from_float(double value, FP_UINT_TYPE int_bits, FP_UINT_TYPE frac_bits,
                 bool is_signed, fp_overflow_mode_t overflow_mode,
                 fp_round_mode_t round_mode, FpBinarySmallObject *output_obj)
{
    FP_UINT_TYPE scaled_value = 0;
    FP_UINT_TYPE total_bits = int_bits + frac_bits;
    double scaled_value_dbl = value * (((FP_UINT_TYPE)1) << frac_bits);
    FP_UINT_TYPE max_scaled_value =
        get_max_scaled_value(FP_UINT_NUM_BITS, is_signed);
    FP_UINT_TYPE min_scaled_value_mag =
        get_mag_of_min_scaled_value(total_bits, is_signed);

    if (round_mode == ROUNDING_NEAR_POS_INF)
    {
        scaled_value_dbl += 0.5;
    }

    scaled_value_dbl = floor(scaled_value_dbl);

    /*
     * Check for architecture value limit - this should raise an
     * exception regardless of the overflow mode.
     */
    if (scaled_value_dbl > max_scaled_value ||
        scaled_value_dbl < (-1.0 * min_scaled_value_mag))
    {
        PyErr_SetString(
            PyExc_OverflowError,
            "Applied value is too large for int representation on this CPU.");
        return false;
    }

    /* Safe to cast to int */
    if (is_signed && scaled_value_dbl < 0.0)
    {
        double abs_scaled_value = -scaled_value_dbl;
        scaled_value = ~((FP_UINT_TYPE)abs_scaled_value) + 1;
    }
    else
    {
        scaled_value = (FP_UINT_TYPE)scaled_value_dbl;
    }

    set_object_fields(output_obj, scaled_value, int_bits, frac_bits, is_signed);
    return check_overflow(output_obj, overflow_mode);
}

/*
 * Will resize self to the format specified by new_int_bits and
 * new_frac_bits and take action based on overflow_mode and
 * round_mode.
 * If overflow_mode is OVERFLOW_EXCEP, an exception string will
 * be written and 0 will be returned if an overflow occurs.
 * Otherwise, non-zero is returned.
 */
static int
resize_object(FpBinarySmallObject *self, FP_UINT_TYPE new_int_bits,
              FP_UINT_TYPE new_frac_bits, fp_overflow_mode_t overflow_mode,
              fp_round_mode_t round_mode)
{
    FP_UINT_TYPE new_scaled_value = self->scaled_value;

    /* Rounding */
    if (new_frac_bits < self->frac_bits)
    {
        FP_UINT_TYPE right_shifts = self->frac_bits - new_frac_bits;

        if (round_mode == ROUNDING_NEAR_POS_INF)
        {
            /* Add "new value 0.5" to the old sized value and then truncate */
            new_scaled_value =
                self->scaled_value + (((FP_UINT_TYPE)1) << (right_shifts - 1));
        }
        else if (round_mode == ROUNDING_NEAR_ZERO)
        {
            /* This is "floor" functionality. So if we are positive, truncation
             * works.
             * If we are negative, need to add 1 to the new lowest int bit if
             * the old
             * frac bits are non zero, and then truncate.
             */
            if (self->is_signed &&
                (self->scaled_value &
                 get_sign_bit(self->int_bits + self->frac_bits)) &&
                (self->scaled_value & get_frac_bits_mask(right_shifts)))
            {
                new_scaled_value =
                    self->scaled_value + (((FP_UINT_TYPE)1) << right_shifts);
            }
        }
        /* else Default to truncate (ROUNDING_DIRECT_NEG_INF) */

        new_scaled_value =
            apply_rshift(new_scaled_value, right_shifts, self->is_signed);
    }
    else
    {
        new_scaled_value <<= (new_frac_bits - self->frac_bits);
    }

    set_object_fields(self, new_scaled_value, new_int_bits, new_frac_bits,
                      self->is_signed);
    return check_overflow(self, overflow_mode);
}

static double
fpbinarysmall_to_double(FpBinarySmallObject *obj)
{
    double result;

    if (obj->is_signed && (obj->scaled_value & FP_UINT_MAX_SIGN_BIT))
    {
        /* Negative - create double with magnitude and mult by -1.0 */
        double magnitude = (double)(~obj->scaled_value + 1);
        result = -magnitude / (((FP_UINT_TYPE)1) << obj->frac_bits);
    }
    else
    {
        result =
            ((double)obj->scaled_value) / (((FP_UINT_TYPE)1) << obj->frac_bits);
    }

    return result;
}

static void
fpsmall_format_as_pylongs(PyObject *self, PyObject **out_int_bits,
                          PyObject **out_frac_bits)
{
    *out_int_bits = PyLong_FromLong(((FpBinarySmallObject *)self)->int_bits);
    *out_frac_bits = PyLong_FromLong(((FpBinarySmallObject *)self)->frac_bits);
}

static FpBinarySmallObject *
fpbinarysmall_create_mem(PyTypeObject *type)
{
    FpBinarySmallObject *self = (FpBinarySmallObject *)type->tp_alloc(type, 0);

    if (self)
    {
        self->fpbinary_base.private_iface = &FpBinary_SmallPrvIface;
        set_object_fields(self, 0, 1, 0, true);
    }

    return self;
}

static PyObject *
fpbinarysmall_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    FpBinarySmallObject *self = NULL;
    long int_bits = 1, frac_bits = 0;
    bool is_signed = true;
    double value = 0.0;
    PyObject *bit_field = NULL, *format_instance = NULL;

    if (!fp_binary_new_params_parse(args, kwds, &int_bits, &frac_bits,
                                    &is_signed, &value, &bit_field,
                                    &format_instance))
    {
        return NULL;
    }

    if (format_instance)
    {
        if (!FpBinarySmall_Check(format_instance))
        {
            PyErr_SetString(
                PyExc_TypeError,
                "format_inst must be an instance of FpBinarySmall.");
            return NULL;
        }
    }

    if (format_instance)
    {
        int_bits = ((FpBinarySmallObject *)format_instance)->int_bits;
        frac_bits = ((FpBinarySmallObject *)format_instance)->frac_bits;
    }

    if (bit_field)
    {
        self = (FpBinarySmallObject *)FpBinarySmall_FromBitsPylong(
            bit_field, int_bits, frac_bits, is_signed);
    }
    else
    {
        self = (FpBinarySmallObject *)FpBinarySmall_FromDouble(
            value, int_bits, frac_bits, is_signed, OVERFLOW_SAT,
            ROUNDING_NEAR_POS_INF);
    }

    return (PyObject *)self;
}

static PyObject *
fpbinarysmall_copy(FpBinarySmallObject *self, PyObject *args)
{
    FpBinarySmallObject *new_obj =
        fpbinarysmall_create_mem(&FpBinary_SmallType);
    if (new_obj)
    {
        copy_fields(self, new_obj);
    }

    return (PyObject *)new_obj;
}

/*
 * Returns a new FpBinarySmall object where the value is the same
 * as obj but:
 *     if obj is unsigned, an extra bit is added to int_bits.
 *     if obj is signed, no change to value or format.
 */
static PyObject *
fpbinarysmall_to_signed(PyObject *obj, PyObject *args)
{
    FpBinarySmallObject *result = NULL;
    FpBinarySmallObject *cast_obj = (FpBinarySmallObject *)obj;

    if (!FpBinarySmall_Check(obj))
    {
        FPBINARY_RETURN_NOT_IMPLEMENTED;
    }

    if (cast_obj->is_signed)
    {
        return fpbinarysmall_copy(cast_obj, NULL);
    }

    /* Input is an unsigned FpBinarySmall object. */

    result = fpbinarysmall_create_mem(&FpBinary_SmallType);
    set_object_fields(result, cast_obj->scaled_value, cast_obj->int_bits,
                      cast_obj->frac_bits, true);

    return (PyObject *)result;
}

static PyObject *
fpbinarysmall_resize(FpBinarySmallObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *format;
    int overflow_mode = OVERFLOW_WRAP;
    int round_mode = ROUNDING_DIRECT_NEG_INF;
    static char *kwlist[] = {"format", "overflow_mode", "round_mode", NULL};
    FP_UINT_TYPE new_int_bits = self->int_bits, new_frac_bits = self->frac_bits;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|ii", kwlist, &format,
                                     &overflow_mode, &round_mode))
        return NULL;

    /* FP format is defined by a python tuple: (int_bits, frac_bits) */
    if (PyTuple_Check(format))
    {
        PyObject *int_bits_obj, *frac_bits_obj;

        if (!extract_fp_format_from_tuple(format, &int_bits_obj,
                                          &frac_bits_obj))
        {
            return NULL;
        }

        new_int_bits = (FP_UINT_TYPE)(long)PyLong_AsLong(int_bits_obj);
        new_frac_bits = (FP_UINT_TYPE)(long)PyLong_AsLong(frac_bits_obj);

        Py_DECREF(int_bits_obj);
        Py_DECREF(frac_bits_obj);
    }
    else if (FpBinarySmall_Check(format))
    {
        /* Format is an instance of FpBinary, so use its format */
        new_int_bits = ((FpBinarySmallObject *)format)->int_bits;
        new_frac_bits = ((FpBinarySmallObject *)format)->frac_bits;
    }
    else
    {
        PyErr_SetString(PyExc_TypeError,
                        "The format parameter type is not supported.");
        return NULL;
    }

    if (resize_object(self, new_int_bits, new_frac_bits, overflow_mode,
                      round_mode))
    {
        Py_INCREF(self);
        return (PyObject *)self;
    }

    return NULL;
}

/*
 * The bits represented in the passed fixed point object are interpreted as
 * a signed 2's complement integer and returned as a PyLong.
 * NOTE: if self is an unsigned object, the MSB, as defined by the int_bits
 * and frac_bits values, will be considered a sign bit.
 */
static PyObject *
fpbinarysmall_bits_to_signed(FpBinarySmallObject *self, PyObject *args)
{
    FP_UINT_TYPE scaled_value;

    if (self->is_signed)
    {
        scaled_value = self->scaled_value;
    }
    else
    {
        /*
         * If the MSB is one, need to interpret the bits as negative 2's
         * complement. This requires a sign extension.
         */
        FP_UINT_TYPE total_bits = self->int_bits + self->frac_bits;
        if (self->scaled_value & get_sign_bit(total_bits))
        {
            scaled_value =
                self->scaled_value | ~(get_total_bits_mask(total_bits));
        }
        else
        {
            scaled_value = self->scaled_value;
        }
    }

    return PyLong_FromLongLong(scaled_value_to_int(scaled_value));
}

/*
 * Convenience function to make sure the operands of a two operand operation
 * are FpBinarySmallObject instances and of the same signed type.
 */
static bool
check_binary_ops(PyObject *op1, PyObject *op2)
{
    if (!FpBinarySmall_Check(op1) || !FpBinarySmall_Check(op2))
    {
        return false;
    }

    if (((FpBinarySmallObject *)op1)->is_signed !=
        ((FpBinarySmallObject *)op2)->is_signed)
    {
        return false;
    }

    return true;
}

/*
 * NOTE: The calling function is expected to decrement the reference counters
 * of output_op1, output_op2.
 */
static void
make_binary_ops_same_frac_size(PyObject *op1, PyObject *op2,
                               FpBinarySmallObject **output_op1,
                               FpBinarySmallObject **output_op2)
{
    FpBinarySmallObject *cast_op1 = (FpBinarySmallObject *)op1;
    FpBinarySmallObject *cast_op2 = (FpBinarySmallObject *)op2;

    if (cast_op1->frac_bits > cast_op2->frac_bits)
    {
        FpBinarySmallObject *new_op =
            fpbinarysmall_create_mem(&FpBinary_SmallType);
        set_object_fields(
            new_op, cast_op2->scaled_value
                        << (cast_op1->frac_bits - cast_op2->frac_bits),
            cast_op2->int_bits, cast_op1->frac_bits, cast_op2->is_signed);

        *output_op2 = new_op;
        Py_INCREF(cast_op1);
        *output_op1 = cast_op1;
    }
    else if (cast_op2->frac_bits > cast_op1->frac_bits)
    {
        FpBinarySmallObject *new_op =
            fpbinarysmall_create_mem(&FpBinary_SmallType);
        set_object_fields(
            new_op, cast_op1->scaled_value
                        << (cast_op2->frac_bits - cast_op1->frac_bits),
            cast_op1->int_bits, cast_op2->frac_bits, cast_op1->is_signed);

        *output_op1 = new_op;
        Py_INCREF(cast_op2);
        *output_op2 = cast_op2;
    }
    else
    {
        Py_INCREF(cast_op1);
        Py_INCREF(cast_op2);
        *output_op1 = cast_op1;
        *output_op2 = cast_op2;
    }
}

/*
 *
 * Numeric methods implementation
 *
 */
static PyObject *
fpbinarysmall_add(PyObject *op1, PyObject *op2)
{
    FpBinarySmallObject *result = NULL;
    FpBinarySmallObject *cast_op1, *cast_op2;
    FP_UINT_TYPE result_int_bits;

    if (!check_binary_ops(op1, op2))
    {
        FPBINARY_RETURN_NOT_IMPLEMENTED;
    }

    /* Add requires the fractional bits to be lined up */
    make_binary_ops_same_frac_size(op1, op2, &cast_op1, &cast_op2);

    result_int_bits = (cast_op1->int_bits > cast_op2->int_bits)
                          ? cast_op1->int_bits
                          : cast_op2->int_bits;

    result =
        (FpBinarySmallObject *)fpbinarysmall_create_mem(&FpBinary_SmallType);

    /* Do add and add int_bit. */
    set_object_fields(result, cast_op1->scaled_value + cast_op2->scaled_value,
                      result_int_bits + 1, cast_op1->frac_bits,
                      cast_op1->is_signed);

    Py_DECREF(cast_op1);
    Py_DECREF(cast_op2);

    /* Check for overflow of our underlying word size. */
    if (!check_new_bit_len_ok(result))
    {
        Py_DECREF(result);
        return NULL;
    }

    return (PyObject *)result;
}

static PyObject *
fpbinarysmall_subtract(PyObject *op1, PyObject *op2)
{
    FpBinarySmallObject *result = NULL;
    FpBinarySmallObject *cast_op1, *cast_op2;
    FP_UINT_TYPE result_int_bits;

    if (!check_binary_ops(op1, op2))
    {
        FPBINARY_RETURN_NOT_IMPLEMENTED;
    }

    /* Add requires the fractional bits to be lined up */
    make_binary_ops_same_frac_size(op1, op2, &cast_op1, &cast_op2);

    result_int_bits = (cast_op1->int_bits > cast_op2->int_bits)
                          ? cast_op1->int_bits
                          : cast_op2->int_bits;

    result =
        (FpBinarySmallObject *)fpbinarysmall_create_mem(&FpBinary_SmallType);

    /* Do add and add int_bit. */
    set_object_fields(result, cast_op1->scaled_value - cast_op2->scaled_value,
                      result_int_bits + 1, cast_op1->frac_bits,
                      cast_op1->is_signed);

    /* Need to deal with negative numbers and wrapping if we are unsigned type.
     */
    if (!result->is_signed)
    {
        check_overflow(result, OVERFLOW_WRAP);
    }

    Py_DECREF(cast_op1);
    Py_DECREF(cast_op2);

    /* Check for overflow of our underlying word size. */
    if (!check_new_bit_len_ok(result))
    {
        Py_DECREF(result);
        return NULL;
    }

    return (PyObject *)result;
}

static PyObject *
fpbinarysmall_multiply(PyObject *op1, PyObject *op2)
{
    FpBinarySmallObject *result = NULL;
    FpBinarySmallObject *cast_op1, *cast_op2;

    if (!check_binary_ops(op1, op2))
    {
        FPBINARY_RETURN_NOT_IMPLEMENTED;
    }

    cast_op1 = (FpBinarySmallObject *)op1;
    cast_op2 = (FpBinarySmallObject *)op2;

    result =
        (FpBinarySmallObject *)fpbinarysmall_create_mem(&FpBinary_SmallType);
    /* Do multiply and add format bits. */
    set_object_fields(result, cast_op1->scaled_value * cast_op2->scaled_value,
                      cast_op1->int_bits + cast_op2->int_bits,
                      cast_op1->frac_bits + cast_op2->frac_bits,
                      cast_op1->is_signed);

    /* Check for overflow of our underlying word size. */
    if (!check_new_bit_len_ok(result))
    {
        Py_DECREF(result);
        return NULL;
    }

    return (PyObject *)result;
}

/*
 * Divide is a bit odd with fixed point. Currently convert to doubles,
 * do a divide and convert back to fixed point with the same format as
 * a multiply.
 */
static PyObject *
fpbinarysmall_divide(PyObject *op1, PyObject *op2)
{
    FpBinarySmallObject *result = NULL;
    FpBinarySmallObject *cast_op1, *cast_op2;
    double result_dbl;

    if (!check_binary_ops(op1, op2))
    {
        FPBINARY_RETURN_NOT_IMPLEMENTED;
    }

    cast_op1 = (FpBinarySmallObject *)op1;
    cast_op2 = (FpBinarySmallObject *)op2;

    result =
        (FpBinarySmallObject *)fpbinarysmall_create_mem(&FpBinary_SmallType);
    result_dbl =
        fpbinarysmall_to_double(cast_op1) / fpbinarysmall_to_double(cast_op2);
    build_from_float(result_dbl, cast_op1->int_bits + cast_op2->int_bits,
                     cast_op1->frac_bits + cast_op2->frac_bits,
                     cast_op1->is_signed, OVERFLOW_WRAP,
                     ROUNDING_DIRECT_NEG_INF, result);

    /* Check for overflow of our underlying word size. */
    if (!check_new_bit_len_ok(result))
    {
        Py_DECREF(result);
        return NULL;
    }

    return (PyObject *)result;
}

static PyObject *
fpbinarysmall_negative(PyObject *self)
{
    PyObject *result;
    PyObject *minus_one =
        (PyObject *)fpbinarysmall_create_mem(&FpBinary_SmallType);

    set_object_fields((FpBinarySmallObject *)minus_one, -1, 1, 0, true);
    result = fpbinarysmall_multiply(self, minus_one);

    Py_DECREF(minus_one);

    return result;
}

static PyObject *
fpbinarysmall_long(PyObject *self)
{
    FpBinarySmallObject *cast_self = (FpBinarySmallObject *)self;
    FpBinarySmallObject *result_fp =
        fpbinarysmall_create_mem(&FpBinary_SmallType);
    PyObject *result = NULL;
    copy_fields(cast_self, result_fp);
    resize_object(result_fp, cast_self->int_bits, 0, OVERFLOW_WRAP,
                  ROUNDING_NEAR_ZERO);

    result = PyLong_FromLong(scaled_value_to_int(result_fp->scaled_value));
    Py_DECREF(result_fp);

    return result;
}

static PyObject *
fpbinarysmall_int(PyObject *self)
{
    PyObject *result_pylong = fpbinarysmall_long(self);
    PyObject *result = NULL;

    result = FpBinary_IntFromLong(PyLong_AsLong(result_pylong));
    Py_DECREF(result_pylong);

    return result;
}

/*
 * Creating indexes from a fixed point number number is just returning
 * an unsigned int from the bits in the number.
 */
static PyObject *
fpbinarysmall_index(PyObject *self)
{
    FpBinarySmallObject *cast_self = (FpBinarySmallObject *)self;
    return fp_uint_as_pylong(
        cast_self->scaled_value &
        get_total_bits_mask(cast_self->int_bits + cast_self->frac_bits));
}

static PyObject *
fpbinarysmall_float(PyObject *self)
{
    return PyFloat_FromDouble(
        fpbinarysmall_to_double((FpBinarySmallObject *)self));
}

static PyObject *
fpbinarysmall_abs(PyObject *self)
{
    FpBinarySmallObject *cast_self = (FpBinarySmallObject *)self;

    if (!cast_self->is_signed ||
        !(cast_self->scaled_value & FP_UINT_MAX_SIGN_BIT))
    {
        /* Positive already */
        FpBinarySmallObject *result =
            (FpBinarySmallObject *)fpbinarysmall_create_mem(
                &FpBinary_SmallType);
        copy_fields(cast_self, result);
        return (PyObject *)result;
    }

    /* Negative value. Just negate */
    return FP_NUM_METHOD(self, nb_negative)(self);
}

static PyObject *
fpbinarysmall_lshift(PyObject *self, PyObject *pyobj_lshift)
{
    long lshift;
    FpBinarySmallObject *cast_self = (FpBinarySmallObject *)self;
    FP_UINT_TYPE total_bits = cast_self->int_bits + cast_self->frac_bits;
    FP_UINT_TYPE sign_bit = get_sign_bit(total_bits);
    FP_UINT_TYPE mask = get_total_bits_mask(total_bits);
    FP_UINT_TYPE shifted_value;
    FpBinarySmallObject *result = NULL;

    if (!PyLong_Check(pyobj_lshift))
    {
        FPBINARY_RETURN_NOT_IMPLEMENTED;
    }

    lshift = PyLong_AsLong(pyobj_lshift);
    shifted_value = cast_self->scaled_value << lshift;

    /*
     * For left shifting, we need to make sure the bits above our sign
     * bit are the correct value. I.e. zeros if the result is positive
     * and ones if the result is negative. This is because we rely on
     * the signed value of the underlying scaled_value integer.
     */
    if (cast_self->is_signed && (shifted_value & sign_bit))
    {
        shifted_value |= (~mask);
    }
    else
    {
        shifted_value &= (mask);
    }

    result =
        (FpBinarySmallObject *)fpbinarysmall_create_mem(&FpBinary_SmallType);
    set_object_fields(result, shifted_value, cast_self->int_bits,
                      cast_self->frac_bits, cast_self->is_signed);

    return (PyObject *)result;
}

static PyObject *
fpbinarysmall_rshift(PyObject *self, PyObject *pyobj_rshift)
{
    long rshift;
    FpBinarySmallObject *cast_self = ((FpBinarySmallObject *)self);
    FpBinarySmallObject *result = NULL;

    if (!PyLong_Check(pyobj_rshift))
    {
        FPBINARY_RETURN_NOT_IMPLEMENTED;
    }

    rshift = PyLong_AsLong(pyobj_rshift);

    result =
        (FpBinarySmallObject *)fpbinarysmall_create_mem(&FpBinary_SmallType);
    set_object_fields(
        result,
        apply_rshift(cast_self->scaled_value, rshift, cast_self->is_signed),
        cast_self->int_bits, cast_self->frac_bits, cast_self->is_signed);

    return (PyObject *)result;
}

static int
fpbinarysmall_nonzero(PyObject *self)
{
    return (self && ((FpBinarySmallObject *)self)->scaled_value != 0);
}

/*
 *
 * Sequence methods implementation
 *
 */

static Py_ssize_t
fpbinarysmall_sq_length(PyObject *self)
{
    return (Py_ssize_t)(((FpBinarySmallObject *)self)->int_bits +
                        ((FpBinarySmallObject *)self)->frac_bits);
}

/*
 * A get item on an fpbinarysmallobject returns a bool (True for 1, False for
 * 0).
 */
static PyObject *
fpbinarysmall_sq_item(PyObject *self, Py_ssize_t py_index)
{
    FpBinarySmallObject *cast_self = ((FpBinarySmallObject *)self);

    if (py_index < ((Py_ssize_t)(cast_self->int_bits + cast_self->frac_bits)))
    {
        if (cast_self->scaled_value & (((FP_UINT_TYPE)1) << py_index))
        {
            Py_RETURN_TRUE;
        }
        else
        {
            Py_RETURN_FALSE;
        }
    }

    return NULL;
}

/*
 * If slice notation is invoked on an fpbinarysmallobject, a new
 * fpbinarysmallobject is created as an unsigned integer where the value is the
 * value of the selected bits.
 *
 * This is useful for digital logic implementations of NCOs and trig lookup
 * tables.
 */
static PyObject *
fpbinarysmall_sq_slice(PyObject *self, Py_ssize_t index1, Py_ssize_t index2)
{
    FpBinarySmallObject *result = NULL;
    FpBinarySmallObject *cast_self = ((FpBinarySmallObject *)self);
    Py_ssize_t low_index, high_index;
    FP_UINT_TYPE mask;
    FP_INT_TYPE max_index = cast_self->int_bits + cast_self->frac_bits - 1;

    /* To allow for the (reasonably) common convention of "high-to-low" bit
     * array ordering in languages like VHDL, the user can have index 1 higher
     * than index 2 - we always just assume the highest value is the MSB
     * desired. */
    if (index1 < index2)
    {
        low_index = index1;
        high_index = index2;
    }
    else
    {
        low_index = index2;
        high_index = index1;
    }

    if (high_index > max_index)
        high_index = max_index;

    mask = (((FP_UINT_TYPE)1) << (high_index + 1)) - 1;
    result =
        (FpBinarySmallObject *)fpbinarysmall_create_mem(&FpBinary_SmallType);
    set_object_fields(result, (cast_self->scaled_value & mask) >> low_index,
                      high_index - low_index + 1, 0, false);

    return (PyObject *)result;
}

static PyObject *
fpbinarysmall_subscript(PyObject *self, PyObject *item)
{
    FpBinarySmallObject *cast_self = ((FpBinarySmallObject *)self);
    Py_ssize_t index, start, stop;

    if (fp_binary_subscript_get_item_index(item, &index))
    {
        return fpbinarysmall_sq_item(self, index);
    }
    else if (fp_binary_subscript_get_item_start_stop(item, &start, &stop,
                                                     cast_self->int_bits +
                                                         cast_self->frac_bits))
    {
        return fpbinarysmall_sq_slice(self, start, stop);
    }

    return NULL;
}

static PyObject *
fpbinarysmall_str(PyObject *obj)
{
    PyObject *result;
    PyObject *double_val = fpbinarysmall_float(obj);
    result = Py_TYPE(double_val)->tp_str(double_val);

    Py_DECREF(double_val);
    return result;
}

static PyObject *
fpbinarysmall_str_ex(PyObject *self)
{
    FpBinarySmallObject *cast_self = ((FpBinarySmallObject *)self);
    PyObject *scaled_value =
        scaled_value_to_pylong(cast_self->scaled_value, cast_self->is_signed);
    PyObject *result = scaled_long_to_float_str(
        scaled_value, fp_uint_as_pylong(cast_self->int_bits),
        fp_uint_as_pylong(cast_self->frac_bits));

    Py_DECREF(scaled_value);
    return result;
}

static bool
fpbinarysmall_signed_compare(FpBinarySmallObject *op1, FpBinarySmallObject *op2,
                             int operator)
{
    FP_INT_TYPE op1_int = scaled_value_to_int(op1->scaled_value);
    FP_INT_TYPE op2_int = scaled_value_to_int(op2->scaled_value);

    switch (operator)
    {
        case Py_LT: return (op1_int < op2_int);
        case Py_LE: return (op1_int <= op2_int);
        case Py_EQ: return (op1_int == op2_int);
        case Py_NE: return (op1_int != op2_int);
        case Py_GT: return (op1_int > op2_int);
        case Py_GE: return (op1_int >= op2_int);
        default: return false;
    }

    return false;
}

static bool
fpbinarysmall_unsigned_compare(FpBinarySmallObject *op1,
                               FpBinarySmallObject *op2, int operator)
{
    switch (operator)
    {
        case Py_LT: return (op1->scaled_value < op2->scaled_value);
        case Py_LE: return (op1->scaled_value <= op2->scaled_value);
        case Py_EQ: return (op1->scaled_value == op2->scaled_value);
        case Py_NE: return (op1->scaled_value != op2->scaled_value);
        case Py_GT: return (op1->scaled_value > op2->scaled_value);
        case Py_GE: return (op1->scaled_value >= op2->scaled_value);
        default: return false;
    }

    return false;
}

static PyObject *
fpbinarysmall_richcompare(PyObject *obj1, PyObject *obj2, int operator)
{
    bool eval = false;
    FpBinarySmallObject *cast_op1, *cast_op2;

    if (!check_binary_ops(obj1, obj2))
    {
        FPBINARY_RETURN_NOT_IMPLEMENTED;
    }

    make_binary_ops_same_frac_size(obj1, obj2, &cast_op1, &cast_op2);

    if (cast_op1->is_signed)
    {
        eval = fpbinarysmall_signed_compare(cast_op1, cast_op2, operator);
    }
    else
    {
        eval = fpbinarysmall_unsigned_compare(cast_op1, cast_op2, operator);
    }

    Py_DECREF(cast_op1);
    Py_DECREF(cast_op2);

    if (eval)
        Py_RETURN_TRUE;
    else
        Py_RETURN_FALSE;
}

static void
fpbinarysmall_dealloc(FpBinarySmallObject *self)
{
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
fpbinarysmall_getformat(PyObject *self, void *closure)
{
    PyObject *result_tuple = NULL;
    PyObject *pylong_int_bits;
    PyObject *pylong_frac_bits;

    fpsmall_format_as_pylongs(self, &pylong_int_bits, &pylong_frac_bits);

    if (pylong_int_bits && pylong_frac_bits)
    {
        result_tuple = PyTuple_Pack(2, pylong_int_bits, pylong_frac_bits);
    }

    if (!result_tuple)
    {
        Py_XDECREF(pylong_int_bits);
        Py_XDECREF(pylong_frac_bits);
    }

    return result_tuple;
}

static PyObject *
fpbinarysmall_is_signed(PyObject *self, void *closure)
{
    if (((FpBinarySmallObject *)self)->is_signed)
        Py_RETURN_TRUE;
    else
        Py_RETURN_FALSE;
}

/*
 * Returns the maximum bit width of fixed point numbers this object can
 * represent. If there is no limit, None is returned.
 */
static PyObject *
fpbinarysmall_get_max_bits(PyObject *cls)
{
    return PyLong_FromLong(FP_UINT_NUM_BITS);
}

/* Helper functions for use of top client object. */
static FP_UINT_TYPE
fpbinarysmall_get_total_bits(PyObject *obj)
{
    FpBinarySmallObject *cast_obj = (FpBinarySmallObject *)obj;
    return cast_obj->int_bits + cast_obj->frac_bits;
}

void
FpBinarySmall_FormatAsUints(PyObject *self, FP_UINT_TYPE *out_int_bits,
                            FP_UINT_TYPE *out_frac_bits)
{
    *out_int_bits = ((FpBinarySmallObject *)self)->int_bits;
    *out_frac_bits = ((FpBinarySmallObject *)self)->frac_bits;
}

/*
 * Returns a PyLong* who's bits are those of the underlying FpBinarySmallObject
 * instance. This means that if the object represents a negative value, the
 * sign bit (as defined by int_bits and frac_bits) will be 1 (i.e. the bits
 * will be in 2's complement format). However, don't assume the PyLong returned
 * will or won't be negative.
 *
 * No need to increment the reference counter on the returned object.
 */
PyObject *
FpBinarySmall_BitsAsPylong(PyObject *obj)
{
    return PyLong_FromUnsignedLongLong(
        ((FpBinarySmallObject *)obj)->scaled_value);
}

static bool
fpsmall_is_signed(PyObject *obj)
{
    return ((FpBinarySmallObject *)obj)->is_signed;
}

bool
FpBinarySmall_IsNegative(PyObject *obj)
{
    FpBinarySmallObject *cast_obj = ((FpBinarySmallObject *)obj);
    return (cast_obj->is_signed &&
            (cast_obj->scaled_value &
             get_sign_bit(cast_obj->int_bits + cast_obj->frac_bits)));
}

PyObject *
FpBinarySmall_FromDouble(double value, FP_UINT_TYPE int_bits,
                         FP_UINT_TYPE frac_bits, bool is_signed,
                         fp_overflow_mode_t overflow_mode,
                         fp_round_mode_t round_mode)
{
    FpBinarySmallObject *self = fpbinarysmall_create_mem(&FpBinary_SmallType);
    if (!build_from_float(value, int_bits, frac_bits, is_signed, overflow_mode,
                          round_mode, self))
    {
        Py_DECREF(self);
        self = NULL;
    }

    return (PyObject *)self;
}

/*
 * Will return a new FpBinarySmallObject with the underlying fixed point value
 * defined by bits, int_bits and frac_bits. Note that bits is
 * expected to be a group of bits that reflect the 2's complement representation
 * of the fixed point value * 2^frac_bits. I.e. the signed bit would be 1 for
 * a negative fixed point number. However, it is NOT assumed that bits
 * is negative (i.e. only int_bits + frac_bits bits will be used so sign
 * extension is not required).
 *
 * This function is useful for creating an object that has a large number of
 * fixed point bits and the double type is too small to represent the the
 * init value.
 *
 * NOTE: It is assumed that the inputs are correct such that the number of
 * format bits are enough to represent the size of bits.
 */
PyObject *
FpBinarySmall_FromBitsPylong(PyObject *scaled_value, FP_UINT_TYPE int_bits,
                             FP_UINT_TYPE frac_bits, bool is_signed)
{
    PyObject *result;
    FP_UINT_TYPE total_bits = int_bits + frac_bits;
    FP_UINT_TYPE sign_bit = get_sign_bit(total_bits);
    PyObject *mask =
        PyLong_FromUnsignedLongLong(get_total_bits_mask(int_bits + frac_bits));
    PyObject *masked_val =
        FP_NUM_METHOD(scaled_value, nb_and)(scaled_value, mask);
    FP_UINT_TYPE scaled_value_uint = pylong_as_fp_uint(masked_val);

    if (is_signed && (scaled_value_uint & sign_bit) &&
        sign_bit < FP_UINT_MAX_SIGN_BIT)
    {
        /* If underlying value is negative, ensure bits are sign extended. */
        scaled_value_uint -= (sign_bit << 1);
    }

    result = (PyObject *)fpbinarysmall_create_mem(&FpBinary_SmallType);
    set_object_fields((FpBinarySmallObject *)result, scaled_value_uint,
                      int_bits, frac_bits, is_signed);

    Py_DECREF(mask);
    Py_DECREF(masked_val);

    return result;
}

bool
FpBinary_IntCheck(PyObject *ob)
{
#if PY_MAJOR_VERSION >= 3

    return false;

#else

    return PyInt_Check(ob);

#endif
}

static PyMethodDef fpbinarysmall_methods[] = {
    {"resize", (PyCFunction)fpbinarysmall_resize, METH_VARARGS | METH_KEYWORDS,
     "Resize the fixed point binary object."},
    {"str_ex", (PyCFunction)fpbinarysmall_str_ex, METH_NOARGS,
     "Extended version of str that provides max precision."},
    {"to_signed", (PyCFunction)fpbinarysmall_to_signed, METH_NOARGS,
     "Copies the input value, adds an int bit and makes signed."},
    {"bits_to_signed", (PyCFunction)fpbinarysmall_bits_to_signed, METH_NOARGS,
     "Interpret the bits of the fixed point binary object as a 2's complement "
     "long integer."},
    {"__copy__", (PyCFunction)fpbinarysmall_copy, METH_NOARGS,
     "Shallow copy the fixed point binary object."},
    {"get_max_bits", (PyCFunction)fpbinarysmall_get_max_bits, METH_CLASS,
     "Returns max number of bits representable with this object."},

    {"__getitem__", (PyCFunction)fpbinarysmall_subscript, METH_O, NULL},

    {NULL} /* Sentinel */
};

static PyGetSetDef fpbinarysmall_getsetters[] = {
    {"format", (getter)fpbinarysmall_getformat, NULL, "Format tuple.", NULL},
    {"is_signed", (getter)fpbinarysmall_is_signed, NULL,
     "Returns True if signed.", NULL},
    {NULL} /* Sentinel */
};

static PyNumberMethods fpbinarysmall_as_number = {
    .nb_add = (binaryfunc)fpbinarysmall_add,
    .nb_subtract = (binaryfunc)fpbinarysmall_subtract,
    .nb_multiply = (binaryfunc)fpbinarysmall_multiply,
    .nb_true_divide = (binaryfunc)fpbinarysmall_divide,
    .nb_negative = (unaryfunc)fpbinarysmall_negative,
    .nb_int = (unaryfunc)fpbinarysmall_int,
    .nb_index = (unaryfunc)fpbinarysmall_index,

#if PY_MAJOR_VERSION < 3
    .nb_divide = (binaryfunc)fpbinarysmall_divide,
    .nb_long = (unaryfunc)fpbinarysmall_long,
#endif

    .nb_float = (unaryfunc)fpbinarysmall_float,
    .nb_absolute = (unaryfunc)fpbinarysmall_abs,
    .nb_lshift = (binaryfunc)fpbinarysmall_lshift,
    .nb_rshift = (binaryfunc)fpbinarysmall_rshift,
    .nb_nonzero = (inquiry)fpbinarysmall_nonzero,
};

static PySequenceMethods fpbinarysmall_as_sequence = {
    .sq_length = (lenfunc)fpbinarysmall_sq_length,
    .sq_item = (ssizeargfunc)fpbinarysmall_sq_item,

#if PY_MAJOR_VERSION < 3

    .sq_slice = (ssizessizeargfunc)fpbinarysmall_sq_slice,

#endif
};

static PyMappingMethods fpbinarysmall_as_mapping = {
    .mp_length = fpbinarysmall_sq_length,
    .mp_subscript = (binaryfunc)fpbinarysmall_subscript,
};

PyTypeObject FpBinary_SmallType = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "fpbinary.FpBinarySmall",
    .tp_doc = "Fixed point binary objects",
    .tp_basicsize = sizeof(FpBinarySmallObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_CHECKTYPES,
    .tp_methods = fpbinarysmall_methods,
    .tp_getset = fpbinarysmall_getsetters,
    .tp_as_number = &fpbinarysmall_as_number,
    .tp_as_sequence = &fpbinarysmall_as_sequence,
    .tp_as_mapping = &fpbinarysmall_as_mapping,
    .tp_new = (newfunc)fpbinarysmall_new,
    .tp_dealloc = (destructor)fpbinarysmall_dealloc,
    .tp_str = fpbinarysmall_str,
    .tp_repr = fpbinarysmall_str,
    .tp_richcompare = fpbinarysmall_richcompare,
};

fpbinary_private_iface_t FpBinary_SmallPrvIface = {
    .get_total_bits = fpbinarysmall_get_total_bits,
    .is_signed = fpsmall_is_signed,
    .resize = (PyCFunctionWithKeywords)fpbinarysmall_resize,
    .str_ex = fpbinarysmall_str_ex,
    .to_signed = fpbinarysmall_to_signed,
    .bits_to_signed = (PyCFunction)fpbinarysmall_bits_to_signed,
    .copy = (PyCFunction)fpbinarysmall_copy,
    .fp_getformat = fpbinarysmall_getformat,

    .fp_from_double = FpBinarySmall_FromDouble,
    .fp_from_bits_pylong = FpBinarySmall_FromBitsPylong,

    .getitem = fpbinarysmall_subscript,
};