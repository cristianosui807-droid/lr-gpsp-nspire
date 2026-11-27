/* The small part of libndls.h used by nspire_input.c in the host tests. */
#ifndef HOST_LIBNDLS_H
#define HOST_LIBNDLS_H

#include "os.h"

int unit_is_touchpad(void);
#define is_touchpad unit_is_touchpad()

#endif
