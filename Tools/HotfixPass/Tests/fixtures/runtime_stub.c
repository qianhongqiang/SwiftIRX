#include <stdbool.h>
#include <stdint.h>

bool ir_hotfix_invoke(uint64_t target_id, uint64_t signature_id,
                      const uint8_t *argument_kinds,
                      const uint64_t *argument_bits, int32_t argument_count,
                      const void *receiver, uint64_t *result_bits) {
  (void)target_id;
  (void)signature_id;
  (void)argument_kinds;
  (void)argument_bits;
  (void)argument_count;
  (void)receiver;
  (void)result_bits;
  return false;
}
