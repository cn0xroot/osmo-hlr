/* OsmoHLR VTY implementation */

/* (C) 2016 sysmocom s.f.m.c. GmbH <info@sysmocom.de>
 * Author: Neels Hofmeyr <nhofmeyr@sysmocom.de>
 * (C) 2018 Harald Welte <laforge@gnumonks.org>
 *
 * All Rights Reserved
 *
 * (C) 2018 Harald Welte <laforge@gnumonks.org>
 *
 * All Rights Reserved
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

#include <osmocom/core/talloc.h>
#include <osmocom/vty/vty.h>
#include <osmocom/vty/command.h>
#include <osmocom/vty/logging.h>
#include <osmocom/vty/misc.h>
#include <osmocom/abis/ipa.h>

#include "hlr_vty.h"
#include "hlr_vty_subscr.h"
#include "gsup_server.h"

struct cmd_node hlr_node = {
	HLR_NODE,
	"%s(config-hlr)# ",
	1,
};

DEFUN(cfg_hlr,
      cfg_hlr_cmd,
      "hlr",
      "Configure the HLR")
{
	vty->node = HLR_NODE;
	return CMD_SUCCESS;
}

struct cmd_node gsup_node = {
	GSUP_NODE,
	"%s(config-hlr-gsup)# ",
	1,
};

DEFUN(cfg_gsup,
      cfg_gsup_cmd,
      "gsup",
      "Configure GSUP options")
{
	vty->node = GSUP_NODE;
	return CMD_SUCCESS;
}

static int config_write_hlr(struct vty *vty)
{
	vty_out(vty, "hlr%s", VTY_NEWLINE);
	return CMD_SUCCESS;
}

static int config_write_hlr_gsup(struct vty *vty)
{
	vty_out(vty, " gsup%s", VTY_NEWLINE);
	if (g_hlr->gsup_bind_addr)
		vty_out(vty, "  bind ip %s%s", g_hlr->gsup_bind_addr, VTY_NEWLINE);
	return CMD_SUCCESS;
}

static void show_one_conn(struct vty *vty, const struct osmo_gsup_conn *conn)
{
	const struct ipa_server_conn *isc = conn->conn;
	char *name;
	int rc;

	rc = osmo_gsup_conn_ccm_get(conn, (uint8_t **) &name, IPAC_IDTAG_SERNR);
	OSMO_ASSERT(rc);

	vty_out(vty, " '%s' from %s:%5u, CS=%u, PS=%u, 3G_IND=%u%s",
		name, isc->addr, isc->port, conn->supports_cs, conn->supports_ps, conn->auc_3g_ind,
		VTY_NEWLINE);
}

DEFUN(show_gsup_conn, show_gsup_conn_cmd,
	"show gsup-connections",
	SHOW_STR "GSUP Connections from VLRs, SGSNs, EUSEs\n")
{
	struct osmo_gsup_server *gs = g_hlr->gs;
	struct osmo_gsup_conn *conn;

	llist_for_each_entry(conn, &gs->clients, list)
		show_one_conn(vty, conn);

	return CMD_SUCCESS;
}

DEFUN(cfg_hlr_gsup_bind_ip,
      cfg_hlr_gsup_bind_ip_cmd,
      "bind ip A.B.C.D",
      "Listen/Bind related socket option\n"
      IP_STR
      "IPv4 Address to bind the GSUP interface to\n")
{
	if(g_hlr->gsup_bind_addr)
		talloc_free(g_hlr->gsup_bind_addr);
	g_hlr->gsup_bind_addr = talloc_strdup(g_hlr, argv[0]);

	return CMD_SUCCESS;
}

/***********************************************************************
 * External USSD Entity
 ***********************************************************************/

#include "hlr_ussd.h"

DEFUN(cfg_euse_route_pfx, cfg_euse_route_pfx_cmd,
	"route prefix PREFIX",
	"")
{
	struct hlr_euse *euse = vty->index;
	struct hlr_euse_route *rt = euse_route_find(euse, argv[0]);

	if (rt) {
		vty_out(vty, "%% Cannot add [another?] route for prefix %s%s", argv[0], VTY_NEWLINE);
		return CMD_WARNING;
	}
	euse_route_prefix_alloc(euse, argv[0]);

	return CMD_SUCCESS;
}

DEFUN(cfg_euse_no_route_pfx, cfg_euse_no_route_pfx_cmd,
	"no route prefix PREFIX",
	NO_STR "")
{
	struct hlr_euse *euse = vty->index;
	struct hlr_euse_route *rt = euse_route_find(euse, argv[0]);
	if (!rt) {
		vty_out(vty, "%% Cannot find route for prefix %s%s", argv[0], VTY_NEWLINE);
		return CMD_WARNING;
	}
	euse_route_del(rt);

	return CMD_SUCCESS;
}

DEFUN(cfg_euse_defaultroute, cfg_euse_defaultroute_cmd,
	"default-route",
	"Set this EUSE as default-route for all USSD to unknown destinations\n")
{
	struct hlr_euse *euse = vty->index;

	if (g_hlr->euse_default != euse) {
		vty_out(vty, "Switching default route from %s to %s%s",
			g_hlr->euse_default->name, euse->name, VTY_NEWLINE);
		g_hlr->euse_default = euse;
	}

	return CMD_SUCCESS;
}

DEFUN(cfg_euse_no_defaultroute, cfg_euse_no_defaultroute_cmd,
	"no default-route",
	NO_STR "Remove this EUSE as default-route for all USSD to unknown destinations\n")
{
	struct hlr_euse *euse = vty->index;

	if (g_hlr->euse_default != euse) {
		vty_out(vty, "%% Current EUSE is no default route, cannot delete it%s", VTY_NEWLINE);
		return CMD_WARNING;
	}
	g_hlr->euse_default = NULL;

	return CMD_SUCCESS;
}

struct cmd_node euse_node = {
	EUSE_NODE,
	"%s(config-hlr-euse)# ",
	1,
};

DEFUN(cfg_euse, cfg_euse_cmd,
	"euse NAME",
	"Configure a particular External USSD Entity\n"
	"Alphanumeric name of the External USSD Entity\n")
{
	struct hlr_euse *euse;
	const char *id = argv[0];

	euse = euse_find(g_hlr, id);
	if (!euse) {
		euse = euse_alloc(g_hlr, id);
		if (!euse)
			return CMD_WARNING;
	}
	vty->index = euse;
	vty->index_sub = &euse->description;
	vty->node = EUSE_NODE;

	return CMD_SUCCESS;
}

DEFUN(cfg_no_euse, cfg_no_euse_cmd,
	"no euse NAME",
	NO_STR "Remove a particular External USSD Entity\n"
	"Alphanumeric name of the External USSD Entity\n")
{
	struct hlr_euse *euse = euse_find(g_hlr, argv[0]);
	if (!euse) {
		vty_out(vty, "%% Cannot remove non-existant EUSE %s%s", argv[0], VTY_NEWLINE);
		return CMD_WARNING;
	}
	if (g_hlr->euse_default == euse) {
		vty_out(vty, "%% Cannot remove EUSE %s, it is the default route%s", argv[0], VTY_NEWLINE);
		return CMD_WARNING;
	}
	euse_del(euse);
	return CMD_SUCCESS;
}

static void dump_one_euse(struct vty *vty, struct hlr_euse *euse)
{
	struct hlr_euse_route *er;

	vty_out(vty, " euse %s%s", euse->name, VTY_NEWLINE);

	llist_for_each_entry(er, &euse->routes, list)
		vty_out(vty, "  route prefix %s%s", er->prefix, VTY_NEWLINE);

	if (g_hlr->euse_default == euse)
		vty_out(vty, "  default-route%s", VTY_NEWLINE);
}

static int config_write_euse(struct vty *vty)
{
	struct hlr_euse *euse;

	llist_for_each_entry(euse, &g_hlr->euse_list, list)
		dump_one_euse(vty, euse);

	return 0;
}

/***********************************************************************
 * Common Code
 ***********************************************************************/

int hlr_vty_go_parent(struct vty *vty)
{
	switch (vty->node) {
	case GSUP_NODE:
	case EUSE_NODE:
		vty->node = HLR_NODE;
		vty->index = NULL;
		vty->index_sub = NULL;
		break;
	default:
	case HLR_NODE:
		vty->node = CONFIG_NODE;
		vty->index = NULL;
		break;
	case CONFIG_NODE:
		vty->node = ENABLE_NODE;
		vty->index = NULL;
		break;
	}

	return vty->node;
}

int hlr_vty_is_config_node(struct vty *vty, int node)
{
	switch (node) {
	/* add items that are not config */
	case CONFIG_NODE:
		return 0;

	default:
		return 1;
	}
}

void hlr_vty_init(struct hlr *hlr, const struct log_info *cat)
{
	logging_vty_add_cmds(cat);
	osmo_talloc_vty_add_cmds();

	install_element_ve(&show_gsup_conn_cmd);

	install_element(CONFIG_NODE, &cfg_hlr_cmd);
	install_node(&hlr_node, config_write_hlr);

	install_element(HLR_NODE, &cfg_gsup_cmd);
	install_node(&gsup_node, config_write_hlr_gsup);

	install_element(GSUP_NODE, &cfg_hlr_gsup_bind_ip_cmd);

	install_element(HLR_NODE, &cfg_euse_cmd);
	install_element(HLR_NODE, &cfg_no_euse_cmd);
	install_node(&euse_node, config_write_euse);
	install_element(EUSE_NODE, &cfg_euse_route_pfx_cmd);
	install_element(EUSE_NODE, &cfg_euse_no_route_pfx_cmd);
	install_element(EUSE_NODE, &cfg_euse_defaultroute_cmd);
	install_element(EUSE_NODE, &cfg_euse_no_defaultroute_cmd);

	hlr_vty_subscriber_init(hlr);
}
