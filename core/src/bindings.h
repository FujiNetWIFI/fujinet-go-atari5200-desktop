/* The key/gamepad bindings table (bindings.c). */
#ifndef A52_BINDINGS_H
#define A52_BINDINGS_H

#include "a52session.h"

#ifdef __cplusplus
extern "C" {
#endif

void bindings_init(struct a52session *s);
int  session_binding_target_for_key(struct a52session *s, uint32_t keysym);

#ifdef __cplusplus
}
#endif

#endif
