/*
 * Driver instance manager: Manages mapping between dynamic-db sections
 * in named.conf and driver instances.
 *
 * Instance name has to be unique.
 *
 * Copyright (C) 2009-2015  Red Hat ; see COPYING for license
 */

#include <config.h>

#include <isc/mem.h>
#include <isc/once.h>
#include <isc/result.h>
#include <isc/task.h>
#include <isc/timer.h>
#include <isc/boolean.h>
#include <isc/util.h>

#include <dns/dyndb.h>
#include <dns/view.h>
#include <dns/zone.h>

#include <string.h>
#include <unistd.h>

#include "instance.h"
#include "instance_manager.h"
#include "log.h"
#include "util.h"

struct db_instance {
	isc_mem_t		*mctx;
	char			*name;
	sample_instance_t	*inst;
	isc_timer_t		*timer;
	LINK(db_instance_t)	link;
};

static isc_once_t initialize_once = ISC_ONCE_INIT;
static isc_mutex_t instance_list_lock;
static LIST(db_instance_t) instance_list;

static void initialize_manager(void);
static void destroy_db_instance(db_instance_t **db_instp);
static isc_result_t find_db_instance(const char *name, db_instance_t **instance);


static void
initialize_manager(void) {
	INIT_LIST(instance_list);
	isc_mutex_init(&instance_list_lock);

	log_info("sample dyndb driver init ("
		 "compiled at " __TIME__ " " __DATE__
		 ", compiler " __VERSION__ ")");
}

/*
 * Destroy all driver instances.
 */
void
destroy_manager(void) {
	db_instance_t *db_inst;
	db_instance_t *next;

	isc_once_do(&initialize_once, initialize_manager);

	LOCK(&instance_list_lock);
	db_inst = HEAD(instance_list);
	while (db_inst != NULL) {
		next = NEXT(db_inst, link);
		UNLINK(instance_list, db_inst, link);
		destroy_db_instance(&db_inst);
		db_inst = next;
	}
	UNLOCK(&instance_list_lock);
}

static void
destroy_db_instance(db_instance_t **db_instp) {
	db_instance_t *db_inst;

	REQUIRE(db_instp != NULL && *db_instp != NULL);

	db_inst = *db_instp;

	if (db_inst->timer != NULL)
		isc_timer_detach(&db_inst->timer);
	if (db_inst->inst != NULL)
		destroy_sample_instance(&db_inst->inst);
	if (db_inst->name != NULL)
		isc_mem_free(db_inst->mctx, db_inst->name);

	MEM_PUT_AND_DETACH(db_inst);

	*db_instp = NULL;
}

/*
 * Create driver instance, parse configuration, and load zones.
 *
 * Only one instance with given name can be in named.conf at the same time.
 */
isc_result_t
manager_create_db_instance(isc_mem_t *mctx, const char *name,
			   int argc, char **argv,
			   const dns_dyndbctx_t *dctx)
{
	isc_result_t result;
	db_instance_t *db_inst = NULL;

	REQUIRE(name != NULL);
	REQUIRE(dctx != NULL);

	isc_once_do(&initialize_once, initialize_manager);

	result = find_db_instance(name, &db_inst);
	if (result == ISC_R_SUCCESS) {
		db_inst = NULL;
		log_error("sample dyndb instance '%s' already exists", name);
		CLEANUP_WITH(ISC_R_EXISTS);
	}

	CHECKED_MEM_GET_PTR(mctx, db_inst);
	ZERO_PTR(db_inst);

	isc_mem_attach(mctx, &db_inst->mctx);
	CHECKED_MEM_STRDUP(mctx, name, db_inst->name);
	CHECK(new_sample_instance(mctx, db_inst->name, argc, argv, dctx,
				  &db_inst->inst));

	/*
	 * instance must be in list before calling
	 * load_sample_instance_zones()
	 */
	LOCK(&instance_list_lock);
	APPEND(instance_list, db_inst, link);
	UNLOCK(&instance_list_lock);

	/*
	 * This is an example so we create and load zones
	 * right now.  This step can be arbitrarily postponed.
	 */
	CHECK(load_sample_instance_zones(db_inst->inst));

	return (ISC_R_SUCCESS);

cleanup:
	if (db_inst != NULL)
		destroy_db_instance(&db_inst);

	return (result);
}

/*
 * Auxiliary function to get driver instance coresponding
 * to a given instance name.
 */
static isc_result_t
find_db_instance(const char *name, db_instance_t **instance) {
	db_instance_t *iterator;

	REQUIRE(name != NULL);
	REQUIRE(instance != NULL && *instance == NULL);

	LOCK(&instance_list_lock);
	iterator = HEAD(instance_list);
	while (iterator != NULL) {
		if (strcmp(name, iterator->name) == 0)
			break;
		iterator = NEXT(iterator, link);
	}
	UNLOCK(&instance_list_lock);

	if (iterator != NULL) {
		*instance = iterator;
		return (ISC_R_SUCCESS);
	}

	return (ISC_R_NOTFOUND);
}

/*
 * Get driver instance coresponding to a given instance name.
 */
isc_result_t
manager_get_sample_instance(const char *name, sample_instance_t **inst) {
	isc_result_t result;
	db_instance_t *db_inst;

	REQUIRE(name != NULL);
	REQUIRE(inst != NULL && *inst == NULL);

	isc_once_do(&initialize_once, initialize_manager);

	db_inst = NULL;
	CHECK(find_db_instance(name, &db_inst));

	*inst = db_inst->inst;

cleanup:
	return (result);
}
