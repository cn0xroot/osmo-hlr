/* (C) 2015 by Harald Welte <laforge@gnumonks.org>
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

#include <string.h>
#include <errno.h>
#include <inttypes.h>

#include <osmocom/core/utils.h>
#include <osmocom/crypt/auth.h>
#include <osmocom/gsm/gsm23003.h>

#include <sqlite3.h>

#include "logging.h"
#include "db.h"

#define LOGHLR(imsi, level, fmt, args ...)	LOGP(DAUC, level, "IMSI='%s': " fmt, imsi, ## args)

#define SL3_TXT(x, stmt, idx) 					\
	do {							\
		const char *_txt = (const char *) sqlite3_column_text(stmt, idx);\
		if (_txt)					\
			strncpy(x, _txt, sizeof(x));		\
		x[sizeof(x)-1] = '\0';				\
	} while (0)

int db_subscr_create(struct db_context *dbc, const char *imsi)
{
	sqlite3_stmt *stmt;
	int rc;

	if (!osmo_imsi_str_valid(imsi)) {
		LOGP(DAUC, LOGL_ERROR, "Cannot create subscriber: invalid IMSI: '%s'\n",
		     imsi);
		return -EINVAL;
	}

	stmt = dbc->stmt[DB_STMT_SUBSCR_CREATE];

	if (!db_bind_text(stmt, "$imsi", imsi))
		return -EIO;

	/* execute the statement */
	rc = sqlite3_step(stmt);
	db_remove_reset(stmt);
	if (rc != SQLITE_DONE) {
		LOGHLR(imsi, LOGL_ERROR, "Cannot create subscriber: SQL error: (%d) %s\n",
		       rc, sqlite3_errmsg(dbc->db));
		return -EIO;
	}

	return 0;
}

int db_subscr_delete_by_id(struct db_context *dbc, int64_t subscr_id)
{
	int rc;
	struct sub_auth_data_str aud;
	int ret = 0;

	sqlite3_stmt *stmt = dbc->stmt[DB_STMT_DEL_BY_ID];

	if (!db_bind_int64(stmt, "$subscriber_id", subscr_id))
		return -EIO;

	/* execute the statement */
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE) {
		LOGP(DAUC, LOGL_ERROR,
		       "Cannot delete subscriber ID=%"PRId64": SQL error: (%d) %s\n",
		       subscr_id, rc, sqlite3_errmsg(dbc->db));
		db_remove_reset(stmt);
		return -EIO;
	}

	/* verify execution result */
	rc = sqlite3_changes(dbc->db);
	if (!rc) {
		LOGP(DAUC, LOGL_ERROR, "Cannot delete: no such subscriber: ID=%"PRId64"\n",
		     subscr_id);
		ret = -ENOENT;
	} else if (rc != 1) {
		LOGP(DAUC, LOGL_ERROR, "Delete subscriber ID=%"PRId64
		     ": SQL modified %d rows (expected 1)\n", subscr_id, rc);
		ret = -EIO;
	}
	db_remove_reset(stmt);

	/* make sure to remove authentication data for this subscriber id, for
	 * both 2G and 3G. */

	aud = (struct sub_auth_data_str){
		.type = OSMO_AUTH_TYPE_GSM,
		.algo = OSMO_AUTH_ALG_NONE,
	};
	rc = db_subscr_update_aud_by_id(dbc, subscr_id, &aud);
	if (ret == -ENOENT && !rc)
		ret = 0;

	aud = (struct sub_auth_data_str){
		.type = OSMO_AUTH_TYPE_UMTS,
		.algo = OSMO_AUTH_ALG_NONE,
	};
	rc = db_subscr_update_aud_by_id(dbc, subscr_id, &aud);
	if (ret == -ENOENT && !rc)
		ret = 0;

	return ret;
}

int db_subscr_update_msisdn_by_imsi(struct db_context *dbc, const char *imsi,
				    const char *msisdn)
{
	int rc;
	int ret = 0;

	if (!osmo_msisdn_str_valid(msisdn)) {
		LOGHLR(imsi, LOGL_ERROR,
		       "Cannot update subscriber: invalid MSISDN: '%s'\n",
		       msisdn);
		return -EINVAL;
	}

	sqlite3_stmt *stmt = dbc->stmt[DB_STMT_SET_MSISDN_BY_IMSI];

	if (!db_bind_text(stmt, "$imsi", imsi))
		return -EIO;
	if (!db_bind_text(stmt, "$msisdn", msisdn))
		return -EIO;

	/* execute the statement */
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE) {
		LOGHLR(imsi, LOGL_ERROR,
		       "Cannot update subscriber's MSISDN: SQL error: (%d) %s\n",
		       rc, sqlite3_errmsg(dbc->db));
		ret = -EIO;
		goto out;
	}

	/* verify execution result */
	rc = sqlite3_changes(dbc->db);
	if (!rc) {
		LOGP(DAUC, LOGL_ERROR, "Cannot update MSISDN: no such subscriber: IMSI='%s'\n",
		     imsi);
		ret = -ENOENT;
		goto out;
	} else if (rc != 1) {
		LOGHLR(imsi, LOGL_ERROR, "Update MSISDN: SQL modified %d rows (expected 1)\n", rc);
		ret = -EIO;
	}

out:
	db_remove_reset(stmt);
	return ret;

}

/* Insert or update 2G or 3G authentication tokens in the database.
 * If aud->type is OSMO_AUTH_TYPE_GSM, the auc_2g table entry for the
 * subscriber will be added or modified; if aud->algo is OSMO_AUTH_ALG_NONE,
 * however, the auc_2g entry for the subscriber is deleted. If aud->type is
 * OSMO_AUTH_TYPE_UMTS, the auc_3g table is updated; again, if aud->algo is
 * OSMO_AUTH_ALG_NONE, the auc_3g entry is deleted.
 * Returns 0 if successful, -EINVAL for unknown aud->type, -ENOENT for unknown
 * subscr_id, -EIO for SQL errors.
 */
int db_subscr_update_aud_by_id(struct db_context *dbc, int64_t subscr_id,
			       const struct sub_auth_data_str *aud)
{
	sqlite3_stmt *stmt_del;
	sqlite3_stmt *stmt_ins;
	sqlite3_stmt *stmt;
	const char *label;
	int rc;
	int ret = 0;

	switch (aud->type) {
	case OSMO_AUTH_TYPE_GSM:
		label = "auc_2g";
		stmt_del = dbc->stmt[DB_STMT_AUC_2G_DELETE];
		stmt_ins = dbc->stmt[DB_STMT_AUC_2G_INSERT];

		switch (aud->algo) {
		case OSMO_AUTH_ALG_NONE:
		case OSMO_AUTH_ALG_COMP128v1:
		case OSMO_AUTH_ALG_COMP128v2:
		case OSMO_AUTH_ALG_COMP128v3:
		case OSMO_AUTH_ALG_XOR:
			break;
		case OSMO_AUTH_ALG_MILENAGE:
			LOGP(DAUC, LOGL_ERROR, "Cannot update auth tokens:"
			     " auth algo not suited for 2G: %s\n",
			     osmo_auth_alg_name(aud->algo));
			return -EINVAL;
		default:
			LOGP(DAUC, LOGL_ERROR, "Cannot update auth tokens:"
			     " Unknown auth algo: %d\n", aud->algo);
			return -EINVAL;
		}

		if (aud->algo == OSMO_AUTH_ALG_NONE)
			break;
		if (!osmo_is_hexstr(aud->u.gsm.ki, 32, 32, true)) {
			LOGP(DAUC, LOGL_ERROR, "Cannot update auth tokens:"
			     " Invalid KI: '%s'\n", aud->u.gsm.ki);
			return -EINVAL;
		}
		break;

	case OSMO_AUTH_TYPE_UMTS:
		label = "auc_3g";
		stmt_del = dbc->stmt[DB_STMT_AUC_3G_DELETE];
		stmt_ins = dbc->stmt[DB_STMT_AUC_3G_INSERT];
		switch (aud->algo) {
		case OSMO_AUTH_ALG_NONE:
		case OSMO_AUTH_ALG_MILENAGE:
			break;
		case OSMO_AUTH_ALG_COMP128v1:
		case OSMO_AUTH_ALG_COMP128v2:
		case OSMO_AUTH_ALG_COMP128v3:
		case OSMO_AUTH_ALG_XOR:
			LOGP(DAUC, LOGL_ERROR, "Cannot update auth tokens:"
			     " auth algo not suited for 3G: %s\n",
			     osmo_auth_alg_name(aud->algo));
			return -EINVAL;
		default:
			LOGP(DAUC, LOGL_ERROR, "Cannot update auth tokens:"
			     " Unknown auth algo: %d\n", aud->algo);
			return -EINVAL;
		}

		if (aud->algo == OSMO_AUTH_ALG_NONE)
			break;
		if (!osmo_is_hexstr(aud->u.umts.k, 32, 32, true)) {
			LOGP(DAUC, LOGL_ERROR, "Cannot update auth tokens:"
			     " Invalid K: '%s'\n", aud->u.umts.k);
			return -EINVAL;
		}
		if (!osmo_is_hexstr(aud->u.umts.opc, 32, 32, true)) {
			LOGP(DAUC, LOGL_ERROR, "Cannot update auth tokens:"
			     " Invalid OP/OPC: '%s'\n", aud->u.umts.opc);
			return -EINVAL;
		}
		if (aud->u.umts.ind_bitlen > OSMO_MILENAGE_IND_BITLEN_MAX) {
			LOGP(DAUC, LOGL_ERROR, "Cannot update auth tokens:"
			     " Invalid ind_bitlen: %d\n", aud->u.umts.ind_bitlen);
			return -EINVAL;
		}
		break;
	default:
		LOGP(DAUC, LOGL_ERROR, "Cannot update auth tokens:"
		     " unknown auth type: %d\n", aud->type);
		return -EINVAL;
	}

	stmt = stmt_del;

	if (!db_bind_int64(stmt, "$subscriber_id", subscr_id))
		return -EIO;

	/* execute the statement */
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE) {
		LOGP(DAUC, LOGL_ERROR,
		     "Cannot delete %s row: SQL error: (%d) %s\n",
		     label, rc, sqlite3_errmsg(dbc->db));
		ret = -EIO;
		goto out;
	}

	/* verify execution result */
	rc = sqlite3_changes(dbc->db);
	if (!rc)
		/* Leave "no such entry" logging to the caller -- during
		 * db_subscr_delete_by_id(), we call this to make sure it is
		 * empty, and no entry is not an error then.*/
		ret = -ENOENT;
	else if (rc != 1) {
		LOGP(DAUC, LOGL_ERROR, "Delete subscriber ID=%"PRId64
		     " from %s: SQL modified %d rows (expected 1)\n",
		     subscr_id, label, rc);
		ret = -EIO;
	}

	db_remove_reset(stmt);

	/* Error situation? Return now. */
	if (ret && ret != -ENOENT)
		return ret;

	/* Just delete requested? */
	if (aud->algo == OSMO_AUTH_ALG_NONE)
		return ret;

	/* Don't return -ENOENT if inserting new data. */
	ret = 0;

	/* Insert new row */
	stmt = stmt_ins;

	if (!db_bind_int64(stmt, "$subscriber_id", subscr_id))
		return -EIO;

	switch (aud->type) {
	case OSMO_AUTH_TYPE_GSM:
		if (!db_bind_int(stmt, "$algo_id_2g", aud->algo))
			return -EIO;
		if (!db_bind_text(stmt, "$ki", aud->u.gsm.ki))
			return -EIO;
		break;
	case OSMO_AUTH_TYPE_UMTS:
		if (!db_bind_int(stmt, "$algo_id_3g", aud->algo))
			return -EIO;
		if (!db_bind_text(stmt, "$k", aud->u.umts.k))
			return -EIO;
		if (!db_bind_text(stmt, "$op",
				  aud->u.umts.opc_is_op ? aud->u.umts.opc : NULL))
			return -EIO;
		if (!db_bind_text(stmt, "$opc",
				  aud->u.umts.opc_is_op ? NULL : aud->u.umts.opc))
			return -EIO;
		if (!db_bind_int(stmt, "$ind_bitlen", aud->u.umts.ind_bitlen))
			return -EIO;
		break;
	default:
		OSMO_ASSERT(false);
	}

	/* execute the statement */
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE) {
		LOGP(DAUC, LOGL_ERROR,
		     "Cannot insert %s row: SQL error: (%d) %s\n",
		     label, rc, sqlite3_errmsg(dbc->db));
		ret = -EIO;
		goto out;
	}

out:
	db_remove_reset(stmt);
	return ret;
}

/* Common code for db_subscr_get_by_*() functions. */
static int db_sel(struct db_context *dbc, sqlite3_stmt *stmt, struct hlr_subscriber *subscr,
		  const char **err)
{
	int rc;
	int ret = 0;

	/* execute the statement */
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_DONE) {
		ret = -ENOENT;
		goto out;
	}
	if (rc != SQLITE_ROW) {
		ret = -EIO;
		goto out;
	}

	if (!subscr)
		goto out;

	/* obtain the various columns */
	subscr->id = sqlite3_column_int64(stmt, 0);
	SL3_TXT(subscr->imsi, stmt, 1);
	SL3_TXT(subscr->msisdn, stmt, 2);
	/* FIXME: These should all be BLOBs as they might contain NUL */
	SL3_TXT(subscr->vlr_number, stmt, 3);
	SL3_TXT(subscr->sgsn_number, stmt, 4);
	SL3_TXT(subscr->sgsn_address, stmt, 5);
	subscr->periodic_lu_timer = sqlite3_column_int(stmt, 6);
	subscr->periodic_rau_tau_timer = sqlite3_column_int(stmt, 7);
	subscr->nam_cs = sqlite3_column_int(stmt, 8);
	subscr->nam_ps = sqlite3_column_int(stmt, 9);
	subscr->lmsi = sqlite3_column_int(stmt, 10);
	subscr->ms_purged_cs = sqlite3_column_int(stmt, 11);
	subscr->ms_purged_ps = sqlite3_column_int(stmt, 12);

out:
	db_remove_reset(stmt);

	switch (ret) {
	case 0:
		*err = NULL;
		break;
	case -ENOENT:
		*err = "No such subscriber";
		break;
	default:
		*err = sqlite3_errmsg(dbc->db);
		break;
	}
	return ret;
}

int db_subscr_get_by_imsi(struct db_context *dbc, const char *imsi,
			  struct hlr_subscriber *subscr)
{
	sqlite3_stmt *stmt = dbc->stmt[DB_STMT_SEL_BY_IMSI];
	const char *err;
	int rc;

	if (!db_bind_text(stmt, NULL, imsi))
		return -EIO;

	rc = db_sel(dbc, stmt, subscr, &err);
	if (rc)
		LOGP(DAUC, LOGL_ERROR, "Cannot read subscriber from db: IMSI='%s': %s\n",
		     imsi, err);
	return rc;
}

int db_subscr_get_by_msisdn(struct db_context *dbc, const char *msisdn,
			    struct hlr_subscriber *subscr)
{
	sqlite3_stmt *stmt = dbc->stmt[DB_STMT_SEL_BY_MSISDN];
	const char *err;
	int rc;

	if (!db_bind_text(stmt, NULL, msisdn))
		return -EIO;

	rc = db_sel(dbc, stmt, subscr, &err);
	if (rc)
		LOGP(DAUC, LOGL_ERROR, "Cannot read subscriber from db: MSISDN='%s': %s\n",
		     msisdn, err);
	return rc;
}

int db_subscr_get_by_id(struct db_context *dbc, int64_t id,
			struct hlr_subscriber *subscr)
{
	sqlite3_stmt *stmt = dbc->stmt[DB_STMT_SEL_BY_ID];
	const char *err;
	int rc;

	if (!db_bind_int64(stmt, NULL, id))
		return -EIO;

	rc = db_sel(dbc, stmt, subscr, &err);
	if (rc)
		LOGP(DAUC, LOGL_ERROR, "Cannot read subscriber from db: ID=%"PRId64": %s\n",
		     id, err);
	return rc;
}

/* Enable or disable PS or CS for a subscriber.
 * For the subscriber with the given imsi, set nam_ps (when is_ps == true) or
 * nam_cs (when is_ps == false) to nam_val in the database.
 * Returns 0 on success, -ENOENT when the given IMSI does not exist, -EINVAL if
 * the SQL statement could not be composed, -ENOEXEC if running the SQL
 * statement failed, -EIO if the amount of rows modified is unexpected.
 */
int db_subscr_nam(struct db_context *dbc, const char *imsi, bool nam_val, bool is_ps)
{
	sqlite3_stmt *stmt;
	int rc;
	int ret = 0;

	stmt = dbc->stmt[is_ps ? DB_STMT_UPD_NAM_PS_BY_IMSI
			       : DB_STMT_UPD_NAM_CS_BY_IMSI];

	if (!db_bind_text(stmt, "$imsi", imsi))
		return -EIO;
	if (!db_bind_int(stmt, "$val", nam_val ? 1 : 0))
		return -EIO;

	/* execute the statement */
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE) {
		LOGHLR(imsi, LOGL_ERROR, "%s %s: SQL error: %s\n",
		       nam_val ? "enable" : "disable",
		       is_ps ? "PS" : "CS",
		       sqlite3_errmsg(dbc->db));
		ret = -EIO;
		goto out;
	}

	/* verify execution result */
	rc = sqlite3_changes(dbc->db);
	if (!rc) {
		LOGP(DAUC, LOGL_ERROR, "Cannot %s %s: no such subscriber: IMSI='%s'\n",
		     nam_val ? "enable" : "disable",
		     is_ps ? "PS" : "CS",
		     imsi);
		ret = -ENOENT;
		goto out;
	} else if (rc != 1) {
		LOGHLR(imsi, LOGL_ERROR, "%s %s: SQL modified %d rows (expected 1)\n",
		       nam_val ? "enable" : "disable",
		       is_ps ? "PS" : "CS",
		       rc);
		ret = -EIO;
	}

out:
	db_remove_reset(stmt);
	return ret;
}

int db_subscr_lu(struct db_context *dbc, int64_t subscr_id,
		 const char *vlr_or_sgsn_number, bool is_ps)
{
	sqlite3_stmt *stmt;
	int rc, ret = 0;

	stmt = dbc->stmt[is_ps ? DB_STMT_UPD_SGSN_BY_ID
			       : DB_STMT_UPD_VLR_BY_ID];

	if (!db_bind_int64(stmt, "$subscriber_id", subscr_id))
		return -EIO;

	if (!db_bind_text(stmt, "$number", vlr_or_sgsn_number))
		return -EIO;

	/* execute the statement */
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE) {
		LOGP(DAUC, LOGL_ERROR, "Update %s number for subscriber ID=%"PRId64": SQL Error: %s\n",
		     is_ps? "SGSN" : "VLR", subscr_id, sqlite3_errmsg(dbc->db));
		ret = -EIO;
		goto out;
	}

	/* verify execution result */
	rc = sqlite3_changes(dbc->db);
	if (!rc) {
		LOGP(DAUC, LOGL_ERROR, "Cannot update %s number for subscriber ID=%"PRId64
		     ": no such subscriber\n",
		     is_ps? "SGSN" : "VLR", subscr_id);
		ret = -ENOENT;
	} else if (rc != 1) {
		LOGP(DAUC, LOGL_ERROR, "Update %s number for subscriber ID=%"PRId64
		       ": SQL modified %d rows (expected 1)\n",
		       is_ps? "SGSN" : "VLR", subscr_id, rc);
		ret = -EIO;
	}

out:
	db_remove_reset(stmt);
	return ret;
}

int db_subscr_purge(struct db_context *dbc, const char *by_imsi,
		    bool purge_val, bool is_ps)
{
	sqlite3_stmt *stmt;
	int rc, ret = 0;

	stmt = dbc->stmt[is_ps ? DB_STMT_UPD_PURGE_PS_BY_IMSI
			       : DB_STMT_UPD_PURGE_CS_BY_IMSI];

	if (!db_bind_text(stmt, "$imsi", by_imsi))
		return -EIO;
	if (!db_bind_int(stmt, "$val", purge_val ? 1 : 0))
		return -EIO;

	/* execute the statement */
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE) {
		LOGP(DAUC, LOGL_ERROR, "%s %s: SQL error: %s\n",
		     purge_val ? "purge" : "un-purge",
		     is_ps ? "PS" : "CS",
		     sqlite3_errmsg(dbc->db));
		ret = -EIO;
		goto out;
	}

	/* verify execution result */
	rc = sqlite3_changes(dbc->db);
	if (!rc) {
		LOGP(DAUC, LOGL_ERROR, "Cannot %s %s: no such subscriber: IMSI='%s'\n",
		     purge_val ? "purge" : "un-purge",
		     is_ps ? "PS" : "CS",
		     by_imsi);
		ret = -ENOENT;
		goto out;
	} else if (rc != 1) {
		LOGHLR(by_imsi, LOGL_ERROR, "%s %s: SQL modified %d rows (expected 1)\n",
		       purge_val ? "purge" : "un-purge",
		       is_ps ? "PS" : "CS",
		       rc);
		ret = -EIO;
	}

out:
	db_remove_reset(stmt);

	return ret;
}
