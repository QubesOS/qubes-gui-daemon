#ifndef QUBES_GUI_UTIL_H
#define QUBES_GUI_UTIL_H QUBES_GUI_UTIL_H
/* Get the size of an array.  Error out on pointers. */
#define QUBES_ARRAY_SIZE(x) (0 * sizeof(struct { \
    int tried_to_compute_number_of_array_elements_in_a_pointer: \
        1 - 2*__builtin_types_compatible_p(__typeof__(x), __typeof__(&((x)[0]))); \
    }) + sizeof(x)/sizeof((x)[0]))

/* Exit if an XCB request fails */
static inline xcb_void_cookie_t check_xcb_void(
        xcb_void_cookie_t cookie,
        const char *msg) {
    if (!cookie.sequence) {
        perror(msg);
        exit(1);
    }
    return cookie;
}
#endif
