#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <minix/syslib.h>
#include <minix/drivers.h>
#include <minix/chardriver.h>
#include <minix/ds.h>
#include <minix/ioctl.h>
#include <sys/ioc_dfa.h>

#define NUM_STATES 256
#define STATES_MATRIX_SIZE 65536

static ssize_t dfa_read(devminor_t minor, u64_t position, endpoint_t endpt,
                          cp_grant_id_t grant, size_t size, int flags, cdev_id_t id);
static ssize_t dfa_write(devminor_t minor, u64_t position,
                             endpoint_t endpt, cp_grant_id_t grant, size_t size, int flags,
                             cdev_id_t id);
static int dfa_ioctl(devminor_t UNUSED(minor), unsigned long request, endpoint_t endpt,
                     cp_grant_id_t grant, int UNUSED(flags), endpoint_t user_endpt, cdev_id_t UNUSED(id));

static struct chardriver dfa_tab = {
  .cdr_read	= dfa_read,
  .cdr_write = dfa_write,
  .cdr_ioctl  = dfa_ioctl
};

static unsigned char current_state = 0;
static char accepting_states[NUM_STATES];
static unsigned char transition[NUM_STATES*NUM_STATES];

static int sef_cb_lu_state_save(int UNUSED(state)) {
  ds_publish_u32("current_state", current_state, DSF_OVERWRITE);
  ds_publish_mem("accepting_states", accepting_states, NUM_STATES, DSF_OVERWRITE);
  ds_publish_mem("transition", transition, STATES_MATRIX_SIZE, DSF_OVERWRITE);

  return OK;
}

static int lu_state_restore() {
  u32_t value;

  ds_retrieve_u32("current_state", &value);
  ds_delete_u32("current_state");
  current_state = (char) value;

  size_t len = NUM_STATES;
  ds_retrieve_mem("accepting_states", accepting_states, &len);
  ds_delete_mem("accepting_states");

  len = STATES_MATRIX_SIZE;
  ds_retrieve_mem("transition", transition, &len);
  ds_delete_mem("transition");

  return OK;
}

static ssize_t dfa_read(devminor_t UNUSED(minor), u64_t position,
                          endpoint_t endpt, cp_grant_id_t grant, size_t size, int UNUSED(flags),
                          cdev_id_t UNUSED(id)) {

  char result;
  if (accepting_states[current_state] == 0) {
    result = 'N';
  } else result = 'Y';

  int ret;

  for (size_t s = 0; s < size; s += 1024) {
    size_t chunk = size - s;
    if (chunk > 1024) chunk = 1024;

    char* buf = malloc(chunk);
    memset(buf, result, chunk);
    ret = sys_safecopyto(endpt, grant, s, (vir_bytes) buf, chunk);
    free(buf);

    if (ret != OK) return ret;
  }

  return size;
}

static ssize_t dfa_write(devminor_t minor, u64_t position,
                         endpoint_t endpt, cp_grant_id_t grant, size_t size, int flags,
                         cdev_id_t id) {
  unsigned char* buf = malloc(size);

  int ret = sys_safecopyfrom(endpt, grant, position, (vir_bytes) buf, size);
  if (ret != OK) {
    free(buf);
    return ret;
  }

  for (size_t i = 0; i < size; i++) {
    int index = current_state * NUM_STATES + buf[i];
    int next_state = transition[index];
    current_state = next_state;
  }

  free(buf);
  return size;
}

static int dfa_ioctl(devminor_t UNUSED(minor), unsigned long request, endpoint_t endpt,
                       cp_grant_id_t grant, int UNUSED(flags), endpoint_t user_endpt, cdev_id_t UNUSED(id)) {
  int rc = OK;

  unsigned char buf[3];

  switch (request) {
    case DFAIOCRESET:
      current_state = 0;
      break;

    case DFAIOCADD:
      rc = sys_safecopyfrom(endpt, grant, 0, (vir_bytes) buf, 3);
      if (rc == OK) {
        int index = buf[0] * NUM_STATES + buf[1];
        transition[index] = buf[2];
        current_state = 0;
      }
      break;

    case DFAIOCACCEPT:
      rc = sys_safecopyfrom(endpt, grant, 0, (vir_bytes) buf, 1);
      if (rc == OK) {
        accepting_states[buf[0]] = 1;
      }
      break;

    case DFAIOCREJECT:
      rc = sys_safecopyfrom(endpt, grant, 0, (vir_bytes) buf, 1);
      if (rc == OK) {
        accepting_states[buf[0]] = 0;
      }
      break;

    default:
      rc = ENOTTY;
  }

  return rc;
}

static int sef_cb_init(int type, sef_init_info_t *UNUSED(info))
{
  int do_announce_driver = TRUE;
  if (type == SEF_INIT_LU) {
    do_announce_driver = FALSE;
  }

  current_state = 0;
  memset(transition, 0, NUM_STATES*NUM_STATES);
  memset(accepting_states, 0, NUM_STATES);

  lu_state_restore();

  if (do_announce_driver) {
    chardriver_announce();
  }

  return OK;
}

static void sef_local_startup()
{
  sef_setcb_init_fresh(sef_cb_init);
  sef_setcb_init_lu(sef_cb_init);
  sef_setcb_init_restart(sef_cb_init);

  sef_setcb_lu_prepare(sef_cb_lu_prepare_always_ready);
  sef_setcb_lu_state_isvalid(sef_cb_lu_state_isvalid_standard);
  sef_setcb_lu_state_save(sef_cb_lu_state_save);

  sef_startup();
}

int main(int argc, char **argv) {
  sef_local_startup();
  chardriver_task(&dfa_tab);
  return OK;
}
