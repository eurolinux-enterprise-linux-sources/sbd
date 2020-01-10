/* 
 * Copyright (C) 2013 Lars Marowsky-Bree <lmb@suse.com>
 * 
 * Based on crm_mon.c, which was:
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/* TODO list:
 *
 * - Trying to shutdown a node if no devices are up will fail, since SBD
 * currently uses a message via the disk to achieve this.
 *
 * - Shutting down cluster nodes while the majority of devices is down
 * will eventually take the cluster below the quorum threshold, at which
 * time the remaining cluster nodes will all immediately suicide.
 *
 */

#include "sbd.h"

#include <sys/param.h>

#include <crm/crm.h>

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <sys/utsname.h>

#include <config.h>

#include <crm_config.h>
#include <crm/msg_xml.h>
#include <crm/common/util.h>
#include <crm/common/xml.h>
#include <crm/common/ipc.h>
#include <crm/common/mainloop.h>
#ifdef SUPPORT_PLUGIN
#  include <crm/cluster.h>
#endif
#include <crm/cib.h>
#include <crm/pengine/status.h>


enum pcmk_health 
{
    pcmk_health_unknown,
    pcmk_health_pending,
    pcmk_health_transient,
    pcmk_health_unclean,
    pcmk_health_shutdown,
    pcmk_health_online,
    pcmk_health_noquorum,
};

extern int servant_count;

static void clean_up(int rc);
static void crm_diff_update(const char *event, xmlNode * msg);
static int cib_connect(gboolean full);
static void set_pcmk_health(enum pcmk_health healthy);
static void notify_parent(void);
static void compute_status(pe_working_set_t * data_set);
static gboolean mon_refresh_state(gpointer user_data);

static GMainLoop *mainloop = NULL;
static guint timer_id_reconnect = 0;
static guint timer_id_notify = 0;
static int reconnect_msec = 1000;
static enum pcmk_health pcmk_healthy = 0;
static int cib_connected = 0;

#ifdef SUPPORT_PLUGIN
static guint timer_id_ais = 0;
static enum cluster_type_e cluster_stack = pcmk_cluster_unknown;
static struct timespec t_last_quorum;
static int check_ais = 0;
#endif



#define LOGONCE(state, lvl, fmt, args...) do {	\
	if (last_state != state) {		\
		cl_log(lvl, fmt, ##args);	\
		last_state = state;		\
	}					\
	} while(0)

static cib_t *cib = NULL;
static xmlNode *current_cib = NULL;

static long last_refresh = 0;



static gboolean
mon_timer_reconnect(gpointer data)
{
	int rc = 0;

	if (timer_id_reconnect > 0) {
		g_source_remove(timer_id_reconnect);
	}

	rc = cib_connect(TRUE);
	if (rc != 0) {
		cl_log(LOG_WARNING, "CIB reconnect failed: %d", rc);
		timer_id_reconnect = g_timeout_add(reconnect_msec, mon_timer_reconnect, NULL);
	} else {
		cl_log(LOG_INFO, "CIB reconnect successful");
	}

	return FALSE;
}

static void
mon_cib_connection_destroy(gpointer user_data)
{
	if (cib) {
		cl_log(LOG_WARNING, "Disconnected from CIB");
		cib->cmds->signoff(cib);
		set_pcmk_health(pcmk_health_transient);
		timer_id_reconnect = g_timeout_add(reconnect_msec, mon_timer_reconnect, NULL);
	}
	cib_connected = 0;
	return;
}

static gboolean
mon_timer_notify(gpointer data)
{
	static int counter = 0;
	int counter_max = timeout_watchdog / timeout_loop;

	if (timer_id_notify > 0) {
		g_source_remove(timer_id_notify);
	}

	if (cib_connected) {
		if (counter == counter_max) {
			free_xml(current_cib);
			current_cib = get_cib_copy(cib);
			mon_refresh_state(NULL);
			counter = 0;
		} else {
			cib->cmds->noop(cib, 0);
			notify_parent();
			counter++;
		}
	}
	timer_id_notify = g_timeout_add(timeout_loop * 1000, mon_timer_notify, NULL);
	return FALSE;
}

/*
 * Mainloop signal handler.
 */
static void
mon_shutdown(int nsig)
{
	clean_up(0);
}

static int
cib_connect(gboolean full)
{
	int rc = 0;

	CRM_CHECK(cib != NULL, return -EINVAL);

	cib_connected = 0;

	crm_xml_init();

	if (cib->state != cib_connected_query && cib->state != cib_connected_command) {

		rc = cib->cmds->signon(cib, crm_system_name, cib_query);

		if (rc != 0) {
			return rc;
		}

		current_cib = get_cib_copy(cib);
		mon_refresh_state(NULL);

		if (full) {
			if (rc == 0) {
				rc = cib->cmds->set_connection_dnotify(cib, mon_cib_connection_destroy);
				if (rc == -EPROTONOSUPPORT) {
					/* Notification setup failed, won't be able to reconnect after failure */
					rc = 0;
				}
			}

			if (rc == 0) {
				cib->cmds->del_notify_callback(cib, T_CIB_DIFF_NOTIFY, crm_diff_update);
				rc = cib->cmds->add_notify_callback(cib, T_CIB_DIFF_NOTIFY, crm_diff_update);
			}

			if (rc != 0) {
				/* Notification setup failed, could not monitor CIB actions */
				clean_up(-rc);
			}
		}
	}
	
	if (!rc) {
		cib_connected = 1;
	}
	return rc;
}

#ifdef SUPPORT_PLUGIN
static gboolean
mon_timer_ais(gpointer data)
{
	if (timer_id_ais > 0) {
		g_source_remove(timer_id_ais);
	}

	send_cluster_text(crm_class_quorum, NULL, TRUE, NULL, crm_msg_ais);

	/* The timer is set in the response processing */
	return FALSE;
}

static void
ais_membership_destroy(gpointer user_data)
{
	cl_log(LOG_ERR, "AIS connection terminated - corosync down?");
#if SUPPORT_PLUGIN
	ais_fd_sync = -1;
#endif
	/* TODO: Is recovery even worth it here? After all, this means
	 * that corosync died ... */
	exit(1);
}

static void
ais_membership_dispatch(cpg_handle_t handle,
                          const struct cpg_name *groupName,
                          uint32_t nodeid, uint32_t pid, void *msg, size_t msg_len)
{
	uint32_t kind = 0;
	const char *from = NULL;
	char *data = pcmk_message_common_cs(handle, nodeid, pid, msg, &kind, &from);

	if (!data) {
		return;
	}
	free(data);
	data = NULL;

	if (kind != crm_class_quorum) {
		return;
	}

	DBGLOG(LOG_INFO, "AIS quorum state: %d", (int)crm_have_quorum);
	clock_gettime(CLOCK_MONOTONIC, &t_last_quorum);

	timer_id_ais = g_timeout_add(timeout_loop * 1000, mon_timer_ais, NULL);

	return;
}
#endif

static void
compute_status(pe_working_set_t * data_set)
{
    static int updates = 0;
    static int last_state = 0;
    static int ever_had_quorum = FALSE;

    int healthy = 0;
    struct timespec	t_now;
    node_t *node = pe_find_node(data_set->nodes, local_uname);

    updates++;
    clock_gettime(CLOCK_MONOTONIC, &t_now);

    if (data_set->dc_node == NULL) {
        LOGONCE(pcmk_health_transient, LOG_INFO, "We don't have a DC right now.");
        healthy = 2;
        goto out;
    }


    if (node == NULL || node->details->online == FALSE) {
        LOGONCE(pcmk_health_unknown, LOG_WARNING, "Node state: UNKNOWN");

    } else if (node->details->unclean) {
        LOGONCE(pcmk_health_unclean, LOG_WARNING, "Node state: UNCLEAN");

    } else if (node->details->pending) {
        LOGONCE(pcmk_health_pending, LOG_WARNING, "Node state: pending");

#if 0
    } else if (node->details->shutdown) {
        LOGONCE(pcmk_health_shutdown, LOG_WARNING, "Node state: shutting down");
#endif

    } else {

        if (data_set->flags & pe_flag_have_quorum) {
            LOGONCE(pcmk_health_online, LOG_INFO, "Node state: online");
            ever_had_quorum = TRUE;

        } else if(ever_had_quorum == FALSE) {
            LOGONCE(pcmk_health_transient, LOG_INFO, "We do not have quorum yet");

        } else if(servant_count > 0) {
            LOGONCE(pcmk_health_noquorum, LOG_WARNING, "Quorum lost");
            goto out;

        } else {
            /* We lost quorum, and there are no disks present
             * Setting healthy > 2 here will result in us self-fencing
             */
            switch (data_set->no_quorum_policy) {
                case no_quorum_freeze:
                    LOGONCE(pcmk_health_transient, LOG_INFO, "Quorum lost: Freeze resources");
                    break;
                case no_quorum_stop:
                    LOGONCE(pcmk_health_transient, LOG_INFO, "Quorum lost: Stop ALL resources");
                    break;
                case no_quorum_ignore:
                    LOGONCE(pcmk_health_transient, LOG_INFO, "Quorum lost: Ignore");
                    break;
                case no_quorum_suicide:
                    LOGONCE(pcmk_health_unclean, LOG_INFO, "Quorum lost: Self-fence");
                    break;
            }
        }
    }

#ifdef SUPPORT_PLUGIN
    if (check_ais) {
        int quorum_age = t_now.tv_sec - t_last_quorum.tv_sec;

        if (quorum_age > (int)(timeout_io+timeout_loop)) {
            if (t_last_quorum.tv_sec != 0)
                LOGONCE(pcmk_health_transient, LOG_WARNING, "AIS: Quorum outdated");

        } else if (crm_have_quorum) {
            LOGONCE(pcmk_health_online, LOG_INFO, "AIS: We have quorum");

        } else {
            LOGONCE(pcmk_health_unclean, LOG_WARNING, "AIS: We do NOT have quorum");
        }
    }
#endif

  out:
    set_pcmk_health(healthy);

    return;
}

static void
set_pcmk_health(enum pcmk_health healthy)
{
	pcmk_healthy = healthy;
	notify_parent();
}


static void
notify_parent(void)
{
	pid_t		ppid;
	union sigval	signal_value;

	memset(&signal_value, 0, sizeof(signal_value));
	ppid = getppid();

	if (ppid == 1) {
		/* Our parent died unexpectedly. Triggering
		* self-fence. */
		cl_log(LOG_WARNING, "Our parent is dead.");
		do_reset();
	}

	switch (pcmk_healthy) {
            case pcmk_health_pending:
            case pcmk_health_shutdown:
            case pcmk_health_transient:
                DBGLOG(LOG_INFO, "Not notifying parent: state transient (%d)", pcmk_healthy);
                break;

            case pcmk_health_unknown:
            case pcmk_health_unclean:
            case pcmk_health_noquorum:
                DBGLOG(LOG_WARNING, "Notifying parent: UNHEALTHY (%d)", pcmk_healthy);
                sigqueue(ppid, SIG_PCMK_UNHEALTHY, signal_value);
                break;

            case pcmk_health_online:
                DBGLOG(LOG_INFO, "Notifying parent: healthy");
                sigqueue(ppid, SIG_LIVENESS, signal_value);
                break;

            default:
                DBGLOG(LOG_WARNING, "Notifying parent: UNHEALTHY %d", pcmk_healthy);
                sigqueue(ppid, SIG_PCMK_UNHEALTHY, signal_value);
                break;
	}
}

static crm_trigger_t *refresh_trigger = NULL;

static gboolean
mon_trigger_refresh(gpointer user_data)
{
    mainloop_set_trigger(refresh_trigger);
    mon_refresh_state(NULL);
    return FALSE;
}

static void
crm_diff_update(const char *event, xmlNode * msg)
{
	int rc = -1;
	const char *op = NULL;
        long now = time(NULL);
        static int updates = 0;
        static mainloop_timer_t *refresh_timer = NULL;

        if(refresh_timer == NULL) {
            refresh_timer = mainloop_timer_add("refresh", 2000, FALSE, mon_trigger_refresh, NULL);
            refresh_trigger = mainloop_add_trigger(G_PRIORITY_LOW, mon_refresh_state, refresh_timer);
        }

        if (current_cib != NULL) {
		xmlNode *cib_last = current_cib;
		current_cib = NULL;

		rc = cib_apply_patch_event(msg, cib_last, &current_cib, LOG_DEBUG);
		free_xml(cib_last);

		switch(rc) {
			case -pcmk_err_diff_resync:
			case -pcmk_err_diff_failed:
                            crm_warn("[%s] %s Patch aborted: %s (%d)", event, op, pcmk_strerror(rc), rc);
                            break;
			case pcmk_ok:
                            updates++;
                            break;
			default:
                            crm_notice("[%s] %s ABORTED: %s (%d)", event, op, pcmk_strerror(rc), rc);
                            break;
		}
	}

	if (current_cib == NULL) {
		current_cib = get_cib_copy(cib);
	}

    /* Refresh
     * - immediately if the last update was more than 5s ago
     * - every 10 updates
     * - at most 2s after the last update
     */
    if (updates > 10 || (now - last_refresh) > (reconnect_msec / 1000)) {
        mon_refresh_state(refresh_timer);
        updates = 0;

    } else {
        mainloop_set_trigger(refresh_trigger);
        mainloop_timer_start(refresh_timer);
    }
}

static gboolean
mon_refresh_state(gpointer user_data)
{
    xmlNode *cib_copy = copy_xml(current_cib);
    pe_working_set_t data_set;

    last_refresh = time(NULL);
    if(user_data) {
        mainloop_timer_t *timer = user_data;

        mainloop_timer_stop(timer);
    }

    if (cli_config_update(&cib_copy, NULL, FALSE) == FALSE) {
        cl_log(LOG_WARNING, "cli_config_update() failed - forcing reconnect to CIB");
        if (cib) {
            cib->cmds->signoff(cib);
        }

    } else {
        set_working_set_defaults(&data_set);
        data_set.input = cib_copy;
        data_set.flags |= pe_flag_have_stonith_resource;
        cluster_status(&data_set);

        compute_status(&data_set);

        cleanup_calculations(&data_set);
    }

    return FALSE;
}

static void
clean_up(int rc)
{
	if (cib != NULL) {
		cib->cmds->signoff(cib);
		cib_delete(cib);
		cib = NULL;
	}

	if (rc >= 0) {
		exit(rc);
	}
	return;
}

int
servant_pcmk(const char *diskname, int mode, const void* argp)
{
	int exit_code = 0;
	crm_cluster_t crm_cluster;

	cl_log(LOG_INFO, "Monitoring Pacemaker health");
	set_proc_title("sbd: watcher: Pacemaker");
        setenv("PCMK_watchdog", "true", 1);

        if(debug == 0) {
            /* We don't want any noisy crm messages */
            set_crm_log_level(LOG_CRIT);
        }

#ifdef SUPPORT_PLUGIN
	cluster_stack = get_cluster_type();

	if (cluster_stack != pcmk_cluster_classic_ais) {
		check_ais = 0;
	} else {
		check_ais = 1;
		cl_log(LOG_INFO, "Legacy plug-in detected, AIS quorum check enabled");
		if(is_openais_cluster()) {
		    crm_cluster.destroy = ais_membership_destroy;
		    crm_cluster.cpg.cpg_deliver_fn = ais_membership_dispatch;
		    /* crm_cluster.cpg.cpg_confchg_fn = pcmk_cpg_membership; TODO? */
		    crm_cluster.cpg.cpg_confchg_fn = NULL;
		}

		while (!crm_cluster_connect(&crm_cluster)) {
			cl_log(LOG_INFO, "Waiting to sign in with cluster ...");
			sleep(reconnect_msec / 1000);
		}
	}
#endif

	if (current_cib == NULL) {
		cib = cib_new();

		do {
			exit_code = cib_connect(TRUE);

			if (exit_code != 0) {
				sleep(reconnect_msec / 1000);
			}
		} while (exit_code == -ENOTCONN);

		if (exit_code != 0) {
			clean_up(-exit_code);
		}
	}

	mainloop = g_main_new(FALSE);

	mainloop_add_signal(SIGTERM, mon_shutdown);
	mainloop_add_signal(SIGINT, mon_shutdown);
	timer_id_notify = g_timeout_add(timeout_loop * 1000, mon_timer_notify, NULL);
#ifdef SUPPORT_PLUGIN
	if (check_ais) {
		timer_id_ais = g_timeout_add(timeout_loop * 1000, mon_timer_ais, NULL);
	}
#endif

	g_main_run(mainloop);
	g_main_destroy(mainloop);

	clean_up(0);
	return 0;                   /* never reached */
}


