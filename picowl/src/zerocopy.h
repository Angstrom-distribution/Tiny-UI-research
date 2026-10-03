/* zerocopy.h - picowl-buffer-v1, dmabuf and copy-type output handling. */
#ifndef PW_ZEROCOPY_H
#define PW_ZEROCOPY_H

#include <stdbool.h>

struct pw_server;
struct pw_output;

bool pw_zerocopy_init(struct pw_server *s);
void pw_zerocopy_finish(struct pw_server *s);
void pw_zerocopy_output_added(struct pw_output *o);
void pw_zerocopy_output_removed(struct pw_output *o);
/* did_commit=false: the frame handler ran but no output commit happened
 * (needs_frame false or output disabled). */
void pw_zerocopy_output_committed(struct pw_output *o, bool did_commit);
void pw_zerocopy_output_presented(struct pw_output *o);
bool pw_zerocopy_active(const struct pw_server *s);

#endif
