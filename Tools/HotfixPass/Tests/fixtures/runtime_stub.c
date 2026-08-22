#include "HFPatchFrame.h"

HFStatus hf_vm_invoke(HFPatchFrame *frame) {
  if (frame != NULL)
    frame->status = HFStatusNoPatch;
  return HFStatusNoPatch;
}
