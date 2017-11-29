
extern "C" {
#include <mach/mig_errors.h>

#include "mig-decls.h"
}

#include "pagemap.h"

#include "machMessage.h"

extern "C"
void _pager_dead_name_notify(mach_msg_header_t *inp, mach_msg_header_t *reply)
{
  pager_t memory_object;

  typedef struct {
    mach_msg_header_t Head;
    mach_msg_type_t RetCodeType;
    kern_return_t RetCode;
  } Reply;

  Reply *OutP = (Reply *) reply;

  if (MACH_MSGH_BITS_LOCAL (inp->msgh_bits) == MACH_MSG_TYPE_PROTECTED_PAYLOAD)
    memory_object = begin_using_pager_payload(inp->msgh_protected_payload);
  else
    memory_object = begin_using_pager(inp->msgh_local_port);

  memory_object->dead_name(machMessage(inp)[0][0]);

  OutP->RetCode = MIG_NO_REPLY;
  end_using_pager(memory_object);
}
