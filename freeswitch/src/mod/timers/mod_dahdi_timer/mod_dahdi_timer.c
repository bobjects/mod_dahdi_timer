/*
 *
 * questions?  Ask Bob Hartwig, bob@bobjectsinc.com
 *
 * mod_dahdi_timer.c -- timer synchronized to the dahdi driver's clock source
 *
 */
#include <switch.h>
#include <string.h>     /* strerror() */
#include <sys/ioctl.h>
#include "dahdi-definitions.h"

#define MAX_ELEMENTS 3600
#define MAX_TICK UINT32_MAX - 1024


static struct {
	int32_t RUNNING;
	int32_t STARTED;
	switch_mutex_t *mutex;
} globals;

struct timer_matrix {
	switch_size_t tick;
	uint32_t count;
	uint32_t roll;
	switch_mutex_t *mutex;
	switch_thread_cond_t *cond;
	switch_thread_rwlock_t *rwlock;
};
typedef struct timer_matrix timer_matrix_t;

struct timer_private {
	switch_size_t reference;
	switch_size_t start;
	uint32_t roll;
	uint32_t ready;
};
typedef struct timer_private timer_private_t;

static switch_memory_pool_t *module_pool = NULL;

static timer_matrix_t TIMER_MATRIX[MAX_ELEMENTS + 1];

SWITCH_MODULE_LOAD_FUNCTION(mod_dahdi_timer_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_dahdi_timer_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_dahdi_timer_runtime);
SWITCH_MODULE_DEFINITION(mod_dahdi_timer, mod_dahdi_timer_load, mod_dahdi_timer_shutdown, mod_dahdi_timer_runtime);

SWITCH_DECLARE(void) switch_os_yield(void)
{
	sched_yield();
}

static switch_status_t mod_dahdi_timer_init(switch_timer_t *timer)
{
	timer_private_t *private_info;
	while (globals.STARTED == 0) {
		switch_os_yield();
	}

	if ((private_info = switch_core_alloc(timer->memory_pool, sizeof(*private_info)))) {
		switch_mutex_lock(globals.mutex);
		if (!TIMER_MATRIX[timer->interval].mutex) {
			switch_mutex_init(&TIMER_MATRIX[timer->interval].mutex, SWITCH_MUTEX_NESTED, module_pool);
			switch_thread_cond_create(&TIMER_MATRIX[timer->interval].cond, module_pool);
		}
		TIMER_MATRIX[timer->interval].count++;
		switch_mutex_unlock(globals.mutex);
		timer->private_info = private_info;
		private_info->start = private_info->reference = TIMER_MATRIX[timer->interval].tick;
		private_info->start -= 2; /* switch_core_timer_init sets samplecount to samples, this makes first next() step once */
		private_info->roll = TIMER_MATRIX[timer->interval].roll;
		private_info->ready = 1;

		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_MEMERR;
}

#define check_roll() if (private_info->roll < TIMER_MATRIX[timer->interval].roll) {	\
		private_info->roll++;											\
		private_info->reference = private_info->start = TIMER_MATRIX[timer->interval].tick;	\
		private_info->start--; /* Must have a diff */					\
	}																	\

static switch_status_t mod_dahdi_timer_step(switch_timer_t *timer)
{
	timer_private_t *private_info = timer->private_info;
	uint64_t samples;

	if (globals.RUNNING != 1 || private_info->ready == 0) {
		return SWITCH_STATUS_FALSE;
	}

	check_roll();
	samples = timer->samples * (private_info->reference - private_info->start);

	if (samples > UINT32_MAX) {
	 	private_info->start = private_info->reference - 1; /* Must have a diff */
	 	samples = timer->samples;
	}

	timer->samplecount = (uint32_t) samples;
	private_info->reference++;

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t mod_dahdi_timer_next(switch_timer_t *timer)
{
	timer_private_t *private_info = timer->private_info;

#ifdef DISABLE_1MS_COND
	int cond_index = timer->interval;
#else
	int cond_index = 1;
#endif
	int delta = (int) (private_info->reference - TIMER_MATRIX[timer->interval].tick);

	/* sync up timer if it's not been called for a while otherwise it will return instantly several times until it catches up */
	if (delta < -1) {
		private_info->reference = timer->tick = TIMER_MATRIX[timer->interval].tick;
	}
	mod_dahdi_timer_step(timer);

	while (globals.RUNNING == 1 && private_info->ready && TIMER_MATRIX[timer->interval].tick < private_info->reference) {
		check_roll();

		switch_os_yield();


		switch_mutex_lock(TIMER_MATRIX[cond_index].mutex);
		if (TIMER_MATRIX[timer->interval].tick < private_info->reference) {
			switch_thread_cond_wait(TIMER_MATRIX[cond_index].cond, TIMER_MATRIX[cond_index].mutex);
		}
		switch_mutex_unlock(TIMER_MATRIX[cond_index].mutex);
	}

	return globals.RUNNING == 1 ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

static switch_status_t mod_dahdi_timer_sync(switch_timer_t *timer)
{
	timer_private_t *private_info = timer->private_info;

	if (globals.RUNNING != 1 || private_info->ready == 0) {
		return SWITCH_STATUS_FALSE;
	}

	/* sync the clock */
	private_info->reference = timer->tick = TIMER_MATRIX[timer->interval].tick;

	/* apply timestamp */
	mod_dahdi_timer_step(timer);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t mod_dahdi_timer_check(switch_timer_t *timer, switch_bool_t step)
{
	timer_private_t *private_info = timer->private_info;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (globals.RUNNING != 1 || !private_info->ready) {
		return SWITCH_STATUS_SUCCESS;
	}

	check_roll();

	timer->tick = TIMER_MATRIX[timer->interval].tick;

	if (timer->tick < private_info->reference) {
		timer->diff = private_info->reference - timer->tick;
	} else {
		timer->diff = 0;
	}

	if (timer->diff) {
		status = SWITCH_STATUS_FALSE;
	} else if (step) {
		mod_dahdi_timer_step(timer);
	}
	return status;
}

static switch_status_t mod_dahdi_timer_destroy(switch_timer_t *timer)
{
	timer_private_t *private_info = timer->private_info;
	if (timer->interval < MAX_ELEMENTS) {
		switch_mutex_lock(globals.mutex);
		TIMER_MATRIX[timer->interval].count--;
		if (TIMER_MATRIX[timer->interval].count == 0) {
			TIMER_MATRIX[timer->interval].tick = 0;
		}
		switch_mutex_unlock(globals.mutex);
	}
	if (private_info) {
		private_info->ready = 0;
	}

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_dahdi_timer_load)
{
	switch_timer_interface_t *timer_interface;

	memset(&globals, 0, sizeof(globals));


	module_pool = pool;

	switch_mutex_init(&globals.mutex, SWITCH_MUTEX_NESTED, module_pool);

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);
	timer_interface = switch_loadable_module_create_interface(*module_interface, SWITCH_TIMER_INTERFACE);
	timer_interface->interface_name = "dahdi";
	timer_interface->timer_init = mod_dahdi_timer_init;
	timer_interface->timer_next = mod_dahdi_timer_next;
	timer_interface->timer_step = mod_dahdi_timer_step;
	timer_interface->timer_sync = mod_dahdi_timer_sync;
	timer_interface->timer_check = mod_dahdi_timer_check;
	timer_interface->timer_destroy = mod_dahdi_timer_destroy;

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_RUNTIME_FUNCTION(mod_dahdi_timer_runtime)
{
	uint32_t current_ms = 0;
	uint32_t x;
	int fd;
	int dahdi_samples_per_interval = 8;  // Dahdi gives 8000 samples per second.  We're interested in 1 mS interval, so that's 8 samples.
	int res;
	fd_set fds;

	fd = open("/dev/dahdi/timer", O_RDWR);
	if (fd < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Unable to open Dahdi timer.\n");
		// TODO - Deal with error.
		//exit(1);
	}
	if (ioctl(fd, DAHDI_TIMERCONFIG, &dahdi_samples_per_interval)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Unable to set Dahdi timer.\n");
		// TODO - Deal with error.
		//exit(1);
	}

	switch_mutex_init(&TIMER_MATRIX[1].mutex, SWITCH_MUTEX_NESTED, module_pool);
	switch_thread_cond_create(&TIMER_MATRIX[1].cond, module_pool);
	globals.STARTED = globals.RUNNING = 1;

	while (globals.RUNNING == 1) { //******************************************** Big loop - 1mS ******
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		res = select(fd + 1, NULL, NULL, &fds, NULL);
		if (res != 1) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Unexpected result from Dahdi select.\n");
			// TODO - Deal with error.
			//exit(1);
		}
		dahdi_samples_per_interval = -1;
		if (ioctl(fd, DAHDI_TIMERACK, &dahdi_samples_per_interval)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Unable to ack Dahdi timer.\n");
			// TODO - Deal with error.
			//exit(1);
		}

		current_ms++;

		TIMER_MATRIX[1].tick++;
		if (switch_mutex_trylock(TIMER_MATRIX[1].mutex) == SWITCH_STATUS_SUCCESS) {
			switch_thread_cond_broadcast(TIMER_MATRIX[1].cond);
			switch_mutex_unlock(TIMER_MATRIX[1].mutex);
		}
		if (TIMER_MATRIX[1].tick == MAX_TICK) {
			TIMER_MATRIX[1].tick = 0;
			TIMER_MATRIX[1].roll++;
		}

		for (x = 2; x <= MAX_ELEMENTS; x++) {  // BOB: should we start x at 2? We already did 1 above.
			if ((current_ms % x) == 0) {       // EVERY N mS FOR EACH TIMER.
				if (TIMER_MATRIX[x].count) {
					TIMER_MATRIX[x].tick++;
					if (TIMER_MATRIX[x].tick == MAX_TICK) {
						TIMER_MATRIX[x].tick = 0;
						TIMER_MATRIX[x].roll++;
					}
				}
			}
		}
		if (current_ms == MAX_ELEMENTS) {
			current_ms = 0;
		}
	} //******************************************* end of big loop - 1mS ******

	close(fd);

	for (x = 1; x <= MAX_ELEMENTS; x++) {
		if (TIMER_MATRIX[x].mutex && switch_mutex_trylock(TIMER_MATRIX[x].mutex) == SWITCH_STATUS_SUCCESS) {
			switch_thread_cond_broadcast(TIMER_MATRIX[x].cond);
			switch_mutex_unlock(TIMER_MATRIX[x].mutex);
		}
	}

	switch_mutex_lock(globals.mutex);
	globals.RUNNING = 0;
	switch_mutex_unlock(globals.mutex);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "Dahdi timer thread exiting.\n");

	return SWITCH_STATUS_TERM;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_dahdi_timer_shutdown)
{
	if (globals.RUNNING == 1) {
		switch_mutex_lock(globals.mutex);
		globals.RUNNING = -1;
		switch_mutex_unlock(globals.mutex);

		while (globals.RUNNING == -1) {
			//do_sleep(10000);
			switch_os_yield();
		}
	}
	return SWITCH_STATUS_SUCCESS;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
