/* GPRS Subscriber Update Protocol client */

/* (C) 2014 by Sysmocom s.f.m.c. GmbH
 * All Rights Reserved
 *
 * Author: Jacob Erlbeck
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#pragma once

#include <osmocom/core/timer.h>
#include <osmocom/gsm/oap_client.h>

/* a loss of GSUP between MSC and HLR is considered quite serious, let's try to recover as quickly as
 * possible.  Even one new connection attempt per second should be quite acceptable until the link is
 * re-established */
#define GSUP_CLIENT_RECONNECT_INTERVAL 1
#define GSUP_CLIENT_PING_INTERVAL 20

struct msgb;
struct ipa_client_conn;
struct gsup_client;

/* Expects message in msg->l2h */
typedef int (*gsup_client_read_cb_t)(struct gsup_client *gsupc,
				     struct msgb *msg);

struct gsup_client {
	const char *unit_name;

	struct ipa_client_conn *link;
	gsup_client_read_cb_t read_cb;
	void *data;

	struct osmo_oap_client_state oap_state;

	struct osmo_timer_list ping_timer;
	struct osmo_timer_list connect_timer;
	int is_connected;
	int got_ipa_pong;
};

struct gsup_client *gsup_client_create(void *talloc_ctx,
				       const char *unit_name,
				       const char *ip_addr,
				       unsigned int tcp_port,
				       gsup_client_read_cb_t read_cb,
				       struct osmo_oap_client_config *oapc_config);

void gsup_client_destroy(struct gsup_client *gsupc);
int gsup_client_send(struct gsup_client *gsupc, struct msgb *msg);
struct msgb *gsup_client_msgb_alloc(void);

