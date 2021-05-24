/* Get the size of an array.  Error out on pointers. */
#define QUBES_ARRAY_SIZE(x) (0 * sizeof(struct { \
    int tried_to_compute_number_of_array_elements_in_a_pointer: \
        1 - 2*__builtin_types_compatible_p(__typeof__(x), __typeof__(&((x)[0]))); \
    }) + sizeof(x)/sizeof((x)[0]))
